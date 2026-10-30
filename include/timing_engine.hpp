#pragma once

#include <algorithm>
#include <cmath>
#include <cerrno>
#include <cstdint>
#include <time.h>

inline uint64_t NowNs() {                       // CLOCK_MONOTONIC, same clock as std::chrono::steady_clock on Linux
    timespec ts{};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return uint64_t(ts.tv_sec) * 1'000'000'000ull + uint64_t(ts.tv_nsec);
}

inline uint64_t ThreadCpuNs() {                 // CPU time burned by the calling thread (waiting does not count)
    timespec ts{};
    clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts);
    return uint64_t(ts.tv_sec) * 1'000'000'000ull + uint64_t(ts.tv_nsec);
}

// Sleeps until an absolute CLOCK_MONOTONIC time (used by the optional base-rate cap).
inline void SleepUntilNs(uint64_t t_ns) {
    timespec ts{time_t(t_ns / 1'000'000'000ull), long(t_ns % 1'000'000'000ull)};
    while (clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &ts, nullptr) == EINTR) {}
}

class TimingEngine {
public:
    explicit TimingEngine(double alpha = 0.15) : m_alpha(alpha) {}

    // Call once per native present. Returns the smoothed frame time in ns.
    uint64_t Update(uint64_t now_ns) {
        if (m_first) {
            m_last_ns = now_ns;
            m_first = false;
            return uint64_t(m_smoothed_ns);
        }
        const uint64_t raw = now_ns - m_last_ns;
        m_last_ns = now_ns;
        m_last_raw_ns = raw;

        // Clamp to 240 FPS .. 10 FPS equivalents so one hitch cannot wreck the average.
        const double clamped = double(std::clamp<uint64_t>(raw, 4'166'666, 100'000'000));

        if (!m_seeded) {
            m_smoothed_ns = clamped;                  // seed from the first real interval: no 60 FPS assumption to converge away from
            m_seeded = true;
        } else {
            // Jitter estimate (slow EMA of |sample - average|) feeds the output clock's jitter buffer (Stage 3).
            m_jitter_ns = 0.05 * std::fabs(clamped - m_smoothed_ns) + 0.95 * m_jitter_ns;
            // EMA = alpha * current + (1 - alpha) * previous
            m_smoothed_ns = m_alpha * clamped + (1.0 - m_alpha) * m_smoothed_ns;
        }
        return uint64_t(m_smoothed_ns);
    }

    uint64_t JitterNs()   const { return uint64_t(m_jitter_ns); }
    uint64_t SmoothedNs() const { return uint64_t(m_smoothed_ns); }
    double   SmoothedExactNs() const { return m_smoothed_ns; }
    double   JitterExactNs()   const { return m_jitter_ns; }
    uint64_t LastRawNs()  const { return m_last_raw_ns; }          // un-clamped, used for the 100 ms flush rule
    double   SmoothedFps() const { return 1e9 / m_smoothed_ns; }

private:
    double   m_alpha;
    bool     m_first{true};
    bool     m_seeded{false};
    uint64_t m_last_ns{0};
    uint64_t m_last_raw_ns{0};
    double   m_jitter_ns{0.0};
    double   m_smoothed_ns{16'666'667.0};
};
