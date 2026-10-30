#pragma once

// Stage 3 scheduling logic, free of Vulkan: the bypass gate, the fixed output clock, and the per-pair plan
// (which ticks to emit, and the fractional t of each). Everything is in CLOCK_MONOTONIC nanoseconds held as
// doubles (exact to well under a nanosecond for any realistic uptime). tests/test_scheduler.cpp replays the
// same frame-time traces as tools/clock_sim/clock_sim.py and compares the results.

#include <algorithm>
#include <cmath>
#include <cstdint>

// Interpolation on/off with hysteresis and dwell, so a game hovering near the target does not flap.
struct InterpolationGate {
    double enter{1.12}, exit{1.03};         // enter = VKIFRAME_MIN_GAIN; exit = 1 + (enter - 1) / 4
    int    dwell{8};
    bool   active{false};
    int    streak{0};
    bool Update(double ratio) {             // ratio = smoothed base frame time / target frame time
        const bool want = active ? ratio > exit : ratio > enter;
        streak = (want != active) ? streak + 1 : 0;
        if (streak >= dwell) { active = want; streak = 0; }
        return active;
    }
    void Reset() { active = false; streak = 0; }
};

struct PlannedTick {
    double time_ns;      // when the tick is to be presented
    double t;            // fractional position between the held frame A (0) and the new frame B (1)
    bool   native_a;     // t is so close to 0 that A itself is shown
};

class PairScheduler {
public:
    static constexpr int kMaxTicks = 16;
    enum class Kind { Bypass, Hold, Pair };

    struct Params {
        double target_ns{1e9 / 60.0};
        int    k_max{3};                    // most synthetic frames between two natives (the pair emits up to k_max + 1 ticks)
        double min_gain{1.12};              // VKIFRAME_MIN_GAIN
        double phase_offset{0.0};           // VKIFRAME_PHASE_OFFSET: where the locked ticks sit, in target frames after the native frame (0 = on it, 0.5 = halfway)
        bool   phase_lock{false};           // VKIFRAME_PHASE_LOCK: slew L so one tick per pair lands on the native frame (near-integer ratios)
    };

    struct Plan {
        Kind        kind{Kind::Bypass};
        bool        flush_held{false};      // Bypass: a frame is being held and must be presented now
        int         n{0};                   // Pair: number of ticks in `ticks`
        PlannedTick ticks[kMaxTicks]{};
        bool        carrier_only{false};    // Pair with no tick: present the held frame alone at carrier_time_ns
        double      carrier_time_ns{0};
        int         dropped{0};             // ticks discarded by the k_max clamp
    };

    bool Holding() const { return m_held; }
    bool GateActive() const { return m_gate.active; }
    double ContentDelayNs() const { return m_L; }

    // Call once per native frame, after TimingEngine::Update. `smoothed` and `jitter` come from the engine (exact
    // values), `raw` is the un-clamped interval to the previous native frame.
    Plan OnNativeFrame(double now, double smoothed, double jitter, double raw, const Params& p) {
        Plan plan;
        const bool stalled = raw > 100e6;                                   // pause / loading screen
        m_gate.enter = p.min_gain;
        m_gate.exit  = 1.0 + (p.min_gain - 1.0) / 4.0;
        const bool interpolate = !stalled && m_gate.Update(smoothed / p.target_ns);
        if (stalled) m_gate.Reset();

        if (!interpolate) {                                                 // bypass: flush the held frame, then pass through
            plan.kind = Kind::Bypass;
            plan.flush_held = m_held;
            m_held = false; m_running = false;
            return plan;
        }

        // Smoothed arrival times of A and B: advance on the smoothed cadence, slew 1/8 toward the real arrival.
        if (!m_running) m_v_cur = now;
        else { m_v_prev = m_v_cur; m_v_cur += smoothed; m_v_cur += (now - m_v_cur) / 8.0; }

        const double wanted = WantedDelay(smoothed, jitter);
        if (!m_held) {                                                      // first frame of a run: hold it, start the clock
            m_held = true; m_running = true;
            m_v_prev = m_v_cur - smoothed;
            m_L = wanted;
            m_anchor = m_v_cur + m_L;
            m_next = 0;
            plan.kind = Kind::Hold;
            return plan;
        }

        // L moves toward its wanted value by at most 2% of a target frame per pair: a hitch cannot run content backwards.
        const double step = 0.02 * p.target_ns;
        bool locking = false;
        if (p.phase_lock) {
            const double r = smoothed / p.target_ns, ri = std::round(r);
            if (ri >= 2.0 && std::fabs(r - ri) < 0.03) {
                const double T = p.target_ns;
                const double tick0 = m_anchor + double(m_next) * T;
                double d = std::fmod(tick0 - m_L - m_v_prev - p.phase_offset * T, T);
                if (d < 0) d += T;
                double delta = 0;
                if (d > 0.01 * T && d < 0.99 * T) {
                    const double up = d, down = -(T - d);                    // later or earlier content: both reach the same phase
                    const bool up_ok = m_L + up <= wanted + T, down_ok = m_L + down >= wanted;
                    if (up_ok && down_ok) delta = (d < 0.5 * T) ? up : down;
                    else if (up_ok) delta = up;
                    else if (down_ok) delta = down;                         // at most one extra target frame of latency
                }
                m_L += std::clamp(delta, -2.5 * step, 2.5 * step);
                locking = true;
            }
        }
        if (!locking) m_L += std::clamp(wanted - m_L, -step, step);
        else if (m_L < wanted) m_L += std::min(wanted - m_L, step);

        plan.kind = Kind::Pair;
        int emitted = 0;
        for (int guard = 0; guard < 256; ++guard) {
            const double tick = m_anchor + double(m_next) * p.target_ns;
            const double tau  = tick - m_L;                                 // content time of this tick
            if (tau >= m_v_cur) break;                                      // belongs to the next pair
            ++m_next;
            if (emitted > p.k_max) { ++plan.dropped; continue; }
            const double span = m_v_cur - m_v_prev;
            const double t = span > 0 ? std::clamp((tau - m_v_prev) / span, 0.0, 1.0) : 0.0;
            if (plan.n < kMaxTicks) plan.ticks[plan.n++] = {tick, t, t < 0.02};
            ++emitted;
        }
        if (plan.n == 0) {                                                  // the held frame must still go out
            plan.carrier_only = true;
            plan.carrier_time_ns = m_anchor + double(m_next) * p.target_ns;
            ++m_next;
        }
        return plan;
    }

    // Extrapolation (VKIFRAME_EXTRAPOLATE): no frame is held back. The new native frame B is shown on the next slot of the fixed output clock
    // (tick 0, t = 0), and the slots until the next native frame is due show frames predicted from B: tick j has t = j * target / smoothed, the
    // fraction of one native interval beyond B. The ticks beyond tick 0 are speculative: the caller cancels those that are still unpresented when
    // the next native frame arrives. The first frame of a run gets tick 0 only (there is no earlier frame to take motion from).
    Plan OnNativeFrameExtrap(double now, double smoothed, double raw, const Params& p) {
        Plan plan;
        const bool stalled = raw > 100e6;
        m_gate.enter = p.min_gain;
        m_gate.exit  = 1.0 + (p.min_gain - 1.0) / 4.0;
        const bool interpolate = !stalled && m_gate.Update(smoothed / p.target_ns);
        if (stalled) m_gate.Reset();
        if (!interpolate) { plan.kind = Kind::Bypass; m_running = false; m_held = false; return plan; }
        const double T = p.target_ns;
        const bool first = !m_running;
        if (first) { m_running = true; m_anchor = now; m_next = 0; }
        // the slot of B: the first one at or after `now` (a little early is fine), always after the previous B's slot
        long long n = (long long)std::ceil((now - m_anchor) / T - 0.1);
        if (!first && n <= (long long)m_next) n = (long long)m_next + 1;
        if (n < 0) n = 0;
        const double tb = m_anchor + double(n) * T;
        plan.kind = Kind::Pair;
        plan.ticks[plan.n++] = {tb, 0.0, true};
        // predicted ticks fill the slots between B's and the slot of the next native frame, which is expected `smoothed` after this one
        // (at a ratio of 1.5 that alternates between one predicted slot and none, so every slot shows a frame)
        const long long n_next = (long long)std::ceil((now + smoothed - m_anchor) / T - 0.1);
        int j_max = first ? 0 : std::clamp(int(n_next - n - 1), 0, p.k_max);
        for (int j = 1; j <= j_max && plan.n < kMaxTicks; ++j) plan.ticks[plan.n++] = {tb + double(j) * T, std::clamp(double(j) * T / smoothed, 0.0, 1.0), false};
        m_next = (uint64_t)n;
        m_L = tb - now;                                                      // what the grid adds to the native frame: the whole content delay in this mode
        return plan;
    }

private:
    static double WantedDelay(double smoothed, double jitter) {             // one frame to hold + a jitter buffer (2x, capped at half a frame)
        return smoothed + std::min(2.0 * jitter, 0.5 * smoothed);
    }

    InterpolationGate m_gate;
    bool   m_held{false}, m_running{false};
    double m_v_prev{0}, m_v_cur{0}, m_anchor{0}, m_L{0};
    uint64_t m_next{0};
};
