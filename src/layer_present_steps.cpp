// Present path pieces: the rate gate plan, the per-second rate log, and the single presents (real, held, synthetic).
#include "layer_internal.hpp"
#include "live_config.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>
#include <time.h>
#include <unistd.h>

using namespace fg_layer;

// ---------------------------------------------------------------- present

namespace fg_layer {
// Stage 3a: runs the scheduler on the real arrival times and counts what it plans; 3b acts on the plan.
PairScheduler::Plan PlanAndCount(SwapchainState& sc, uint64_t now) {
    const Config& cfg = GetConfig();
    PairScheduler::Params p;
    const LiveSettings& live = Live();                       // the settings the plugin can change while a game runs
    const double want_fps = double(live.target_fps.load()), use_fps = EffectiveTargetFps(want_fps, sc.refresh_ns, sc.refresh_vrr);
    if (use_fps != want_fps && sc.clamp_logged_fps != float(want_fps)) { sc.clamp_logged_fps = float(want_fps); FG_LOG("TARGET_FPS %.1f is above the display's refresh rate: using %.2f fps (frames above it would only wait in the present queue)", want_fps, use_fps); }
    else if (use_fps == want_fps) sc.clamp_logged_fps = 0.0f;
    p.target_ns = 1e9 / use_fps; p.k_max = std::min(live.max_synthetic.load(), sc.synth_capacity); p.phase_lock = live.phase_lock.load() != 0; p.phase_offset = live.phase_offset.load();
    p.min_gain = live.enabled.load() ? live.min_gain.load() : 1e6f;   // disabled: the gain gate never opens, so every frame is a plain bypass (a held frame is flushed first)
    auto plan = cfg.extrapolate ? sc.sched.OnNativeFrameExtrap(double(now), sc.timing.SmoothedExactNs(), double(sc.timing.LastRawNs()), p)
                                : sc.sched.OnNativeFrame(double(now), sc.timing.SmoothedExactNs(), sc.timing.JitterExactNs(), double(sc.timing.LastRawNs()), p);
    if (plan.kind == PairScheduler::Kind::Pair && live.synth_margin.load() > 0.0f && !cfg.extrapolate) {   // only generated frames: no tick may sit on A (or B)
        const double m = double(live.synth_margin.load());
        for (int i = 0; i < plan.n; ++i) { plan.ticks[i].t = std::clamp(plan.ticks[i].t, m, 1.0 - m); plan.ticks[i].native_a = false; }
    }
    auto& r = sc.rates;
    ++r.native;
    switch (plan.kind) {
        case PairScheduler::Kind::Bypass: ++r.bypass; r.output += 1 + (plan.flush_held ? 1 : 0); break;
        case PairScheduler::Kind::Hold:   ++r.hold; break;
        case PairScheduler::Kind::Pair:   ++r.pair; r.output += plan.carrier_only ? 1 : plan.n; r.carrier += plan.carrier_only; r.dropped += plan.dropped; break;
    }
    return plan;
}

namespace {
// What one second of the layer measured besides the frame counts (filled by CollectWaits and CollectGpuTimes).
struct SecondTimes { double cpu_hook = 0, cpu_pres = 0, w_lock = 0, w_ring = 0, w_fence = 0, w_acq = 0, w_pres = 0, cap_wait_ms = 0, cap_hold_ms = 0, flow_ms = 0, pairs_s = 0, interp_ms = 0, gen_s = 0; };

// The mode word of status.json and of the stats record.
const char* RateMode(SwapchainState& sc, bool holding) {
    return !Live().enabled.load() ? "off" : GetConfig().extrapolate ? "extrap" : sc.sched.GateActive() ? (holding ? "interp" : "passthru") : "bypass";
}

// The status file of the panel and the one-line rate log of this second.
void LogRateLine(SwapchainState& sc, bool holding, double secs) {
    auto& r = sc.rates;
    {
        const double nat = double(r.native) / secs, shown = double(r.shown) / secs;
        LiveWriteStatus(nat, shown, nat > 0.5 ? std::max(0, int(std::lround(shown / nat)) - 1) : 0, RateMode(sc, holding), sc.refresh_ns ? 1e9 / double(sc.refresh_ns) : 0.0, StatsBenchLabel());
    }
    FG_LOG_ROUTINE("native %.1f fps -> output %.1f fps (planned) | shown %.1f fps%s | %s | pairs %llu (synthetic %llu) bypass %llu carrier-only %llu dropped %llu | content delay %.1f ms",
           double(r.native) / secs, holding || !sc.sched.GateActive() ? double(r.output) / secs : double(r.native) / secs, double(r.shown) / secs, sc.gpu ? (GetConfig().extrapolate ? " (extrapolating)" : " (frame holding on)") : "",
           sc.sched.GateActive() ? (holding ? "generating" : "pass-through (frame holding off)") : "bypass",
           (unsigned long long)r.pair, (unsigned long long)r.synthetic, (unsigned long long)r.bypass, (unsigned long long)r.carrier, (unsigned long long)r.dropped,
           sc.sched.ContentDelayNs() / 1e6);
}

void LogExtrapolation(SwapchainState& sc, stats::Record& rec) {
    if (GetConfig().extrapolate) {
        const uint64_t planned = sc.ex_planned.exchange(0), cancelled = sc.ex_cancelled.exchange(0), ln = sc.ex_late_n.exchange(0), lsum = sc.ex_late_sum_ns.exchange(0), lmax = sc.ex_late_max_ns.exchange(0);
        rec.late_mean_ms = ln ? double(lsum) / double(ln) / 1e6 : 0.0; rec.late_max_ms = double(lmax) / 1e6; rec.cancelled = cancelled;
        FG_LOG_ROUTINE("extrapolation: predicted frames not shown %llu of %llu | native frame late against its slot: mean %.2f ms, max %.2f ms",
               (unsigned long long)cancelled, (unsigned long long)planned, ln ? double(lsum) / double(ln) / 1e6 : 0.0, double(lmax) / 1e6);
    }
}

// Layer CPU time and the waits of the game thread, per frame; the cap's sleeps of this second.
void CollectWaits(SwapchainState& sc, double secs, SecondTimes& t) {
    auto& r = sc.rates;
    const double n = std::max<double>(1.0, double(r.native));
    const double cpu_hook = double(sc.cpu_hook_ns.exchange(0)) / 1e6 / n, cpu_pres = double(sc.cpu_presenter_ns.exchange(0)) / 1e9 / secs;
    const double w_acq = double(sc.wait_acquire_ns.exchange(0)) / 1e6 / n, w_pres = double(sc.wait_present_ns.exchange(0)) / 1e6 / n;
    double w_lock = 0, w_ring = 0, w_fence = 0;
    if (sc.gpu) {
        w_lock = double(sc.gpu->wait_lock_ns.exchange(0)) / 1e6 / n; w_ring = double(sc.gpu->wait_ring_ns.exchange(0)) / 1e6 / n; w_fence = double(sc.gpu->wait_fence_ns.exchange(0)) / 1e6 / n;
        FG_LOG_ROUTINE("layer CPU: game thread %.3f ms per frame inside present (CPU time, waits excluded) | presenter thread %.1f %% of one core", cpu_hook, cpu_pres * 100.0);
        FG_LOG_ROUTINE("game thread waits per frame: acquire %.2f ms | in present %.2f ms (gpu lock %.2f, ring slot %.2f, cmd fence %.2f)", w_acq, w_pres, w_lock, w_ring, w_fence);
    }
    // The cap's sleeps of this second per present with the cap on (late wait after the present, early hold before it). Taken every second, whether or not stats are on, so a row
    // written after STATS was switched on during the game covers one second and not the whole run.
    double cap_wait_ms = 0, cap_hold_ms = 0;
    { const double cn = std::max<double>(1.0, double(sc.cap_n.exchange(0)));
      cap_wait_ms = double(sc.cap_wait_ns.exchange(0)) / 1e6 / cn; cap_hold_ms = double(sc.cap_hold_ns.exchange(0)) / 1e6 / cn; }
    t.cpu_hook = cpu_hook; t.cpu_pres = cpu_pres; t.w_acq = w_acq; t.w_pres = w_pres; t.w_lock = w_lock; t.w_ring = w_ring; t.w_fence = w_fence;
    t.cap_wait_ms = cap_wait_ms; t.cap_hold_ms = cap_hold_ms;
}

// GPU time of the flow and of the interpolation in this second, and the warning when the layer's own GPU work is too long for the game's frame time.
void CollectGpuTimes(SwapchainState& sc, double secs, SecondTimes& t) {
    double flow_ms = 0, pairs_s = 0, interp_ms = 0, gen_s = 0;       // GPU time of the flow and of the interpolation in this second (copied while the readback runs)
    if (sc.gpu) {
        FlowState& fs = sc.gpu->flow;
        const uint64_t fn = fs.stat_flow_n.exchange(0), fns = fs.stat_flow_ns.exchange(0), in = fs.stat_interp_n.exchange(0), ins = fs.stat_interp_ns.exchange(0);
        flow_ms = fn ? double(fns) / 1e6 / double(fn) : 0.0; pairs_s = double(fn) / secs;
        interp_ms = in ? double(ins) / 1e6 / double(in) : 0.0; gen_s = double(in) / secs;
        // One warning per swapchain: the layer's own GPU work is longer than the game's frame time (3 seconds in a row), so the game cannot keep its frame rate.
        static const double kCostWarn = CostWarnFromEnv(EnvVar("COST_WARN"));
        const double ratio = LayerCostRatio(double(fns) / 1e6, double(ins) / 1e6, secs);
        sc.cost_over_windows = ratio > kCostWarn ? sc.cost_over_windows + 1 : 0;
        if (sc.cost_over_windows >= 3 && !sc.cost_warned) {
            sc.cost_warned = true;
            FG_LOG("WARNING: the layer's GPU work is %.0f %% of the game's frame time (flow %.2f ms x %.1f pairs/s + %s %.2f ms x %.1f frames/s = %.0f ms of GPU per second; the copies of the frames are not counted). The game cannot hold its frame rate with this: lower MAX_SYNTHETIC or FLOW_LEVEL, or the target.",
                   ratio * 100.0, flow_ms, pairs_s, GetConfig().interpolate ? "interpolation" : "cross-fade", interp_ms, gen_s, ratio * 1000.0);
        }
    }
    t.flow_ms = flow_ms; t.pairs_s = pairs_s; t.interp_ms = interp_ms; t.gen_s = gen_s;
}

// VKIFRAME_STATS / benchmark: this second as one record (stats.hpp).
void FillRecord(stats::Record& rec, SwapchainState& sc, bool holding, double secs, const SecondTimes& t) {
    auto& r = sc.rates;
    rec.secs = secs; rec.target_fps = EffectiveTargetFps(double(Live().target_fps.load()), sc.refresh_ns, sc.refresh_vrr); rec.refresh_hz = sc.refresh_ns ? 1e9 / double(sc.refresh_ns) : 0.0;
    rec.mode = RateMode(sc, holding);
    rec.native_fps = double(r.native) / secs; rec.shown_fps = double(r.shown) / secs;
    rec.gen_per_native = rec.native_fps > 0.5 ? std::max(0.0, rec.shown_fps / rec.native_fps - 1.0) : 0.0;
    rec.cpu_game_ms = t.cpu_hook; rec.presenter_core = t.cpu_pres;
    rec.wait_lock_ms = t.w_lock; rec.wait_ring_ms = t.w_ring; rec.wait_fence_ms = t.w_fence; rec.wait_acquire_ms = t.w_acq; rec.wait_present_ms = t.w_pres;
    rec.dropped = r.dropped;
    rec.cap_wait_ms = t.cap_wait_ms; rec.cap_hold_ms = t.cap_hold_ms;
    rec.pid = long(getpid());
    rec.content_delay_ms = sc.sched.ContentDelayNs() / 1e6;
    rec.flow_ms = t.flow_ms; rec.pairs_per_s = t.pairs_s; rec.interp_ms = t.interp_ms; rec.generated_per_s = t.gen_s;
    stats::FillDerived(rec, sc.stat_ft);
}

void AppendFrames(SwapchainState& sc, bool want_stats) {
    if (want_stats && GetConfig().stats_frames) {          // every frame time of this second, once, from the vector that already exists (no work in the present path)
        const float capv = (Live().enabled.load() || StatsBenchActive()) ? Live().base_fps_cap.load() : 0.0f;
        timespec wts; clock_gettime(CLOCK_REALTIME, &wts);
        (void)LiveFramesAppend(stats::FramesJson(sc.stat_ft, (long long)wts.tv_sec, capv >= 1.0f ? 1000.0 / double(capv) : 0.0, long(getpid())));
    }
}
} // namespace

void LogRates(SwapchainState& sc, uint64_t now) {
    auto& r = sc.rates;
    // The rate gate (the scheduler) works out a plan for every present, also on a swapchain where the layer cannot hold frames (an application present chain it cannot carry, or
    // a failed GPU step): there nothing is generated. "holding" is what is true; the planned output is only reported when it can happen.
    const bool holding = sc.gpu && !sc.hold_failed;
    if (r.last_ns == 0) { r = {.last_ns = now}; return; }
    if (now - r.last_ns < 1'000'000'000ull) return;
    const double secs = double(now - r.last_ns) / 1e9;
    LiveLogCap();                                                  // live.log stays under its cap while the game runs
    const bool want_stats = StatsWantsSecond();
    stats::Record rec;
    LogRateLine(sc, holding, secs);
    LogExtrapolation(sc, rec);
    SecondTimes t;
    CollectWaits(sc, secs, t);
    CollectGpuTimes(sc, secs, t);
    if (want_stats) FillRecord(rec, sc, holding, secs, t);                 // VKIFRAME_STATS / benchmark: this second as one record (stats.hpp)
    AppendFrames(sc, want_stats);
    sc.stat_ft.Clear();
    if (want_stats) StatsOnSecond(rec, now);
    r = {.last_ns = now};
}

// One present of the queue's mutex-protected queue, with the Stage 2 timing chain when the swapchain has it.
// tick_ns (CLOCK_MONOTONIC) is the time the frame is due; it becomes an absolute target only with VKIFRAME_ABSOLUTE=1.
VkResult PresentWithTiming(DeviceDispatch& dt, VkQueue queue, SwapchainState& sc, const VkPresentInfoKHR& in, uint64_t now, double tick_ns) {
    const Config& cfg = GetConfig();
    // Never mix with a present id from the application; our chain would clash with it.
    const bool app_has_ids = FindInChain(in.pNext, VK_STRUCTURE_TYPE_PRESENT_ID_KHR) ||
                             FindInChain(in.pNext, VK_STRUCTURE_TYPE_PRESENT_ID_2_KHR) ||
                             FindInChain(in.pNext, VK_STRUCTURE_TYPE_PRESENT_TIMINGS_INFO_EXT);
    { std::lock_guard<std::mutex> l(sc.mu); ++sc.rates.shown; }
    auto present = [&](const VkPresentInfoKHR* p) {
        std::mutex* qm = QueueMutex(queue);
        std::unique_lock<std::mutex> lk;
        if (qm) lk = std::unique_lock<std::mutex>(*qm);        // the game thread submits to this queue too
        return dt.QueuePresentKHR(queue, p);
    };
    if (!sc.timing_enabled || app_has_ids) return present(&in);
    for (int attempt = 0; attempt < 2; ++attempt) {
        TimingChain chain;
        VkPresentInfoKHR pi = in;
        {
            std::lock_guard<std::mutex> l(sc.mu);
            uint64_t target = 0;                                // 0: as soon as possible
            if (sc.absolute_ok && cfg.timing_test) target = ToDomainTime(dt, sc, now + 2 * sc.timing.SmoothedNs());
            else if (sc.absolute_ok && cfg.absolute && tick_ns > 0.0) target = ToDomainTime(dt, sc, uint64_t(tick_ns));
            pi.pNext = BuildTimingChain(chain, sc, target, in.pNext);
        }
        const VkResult r = present(&pi);
        if (r != VK_ERROR_PRESENT_TIMING_QUEUE_FULL_EXT) return r;
        std::lock_guard<std::mutex> l(sc.mu);
        ++sc.st.queue_full_total;
        DrainPastTimings(dt, sc);                               // free queue slots, then try once more
    }
    return present(&in);                                        // still full: present without timing rather than fail the game
}

AppChainInfo ClassifyPresentChain(const VkPresentInfoKHR* pi) {
    AppChainInfo r;
    for (auto* p = static_cast<const VkBaseInStructure*>(pi->pNext); p; p = p->pNext) {
        switch (int(p->sType)) {
            case VK_STRUCTURE_TYPE_PRESENT_ID_KHR: {
                auto* x = reinterpret_cast<const VkPresentIdKHR*>(p);
                if (x->swapchainCount != 1 || !x->pPresentIds) { r.kind = AppChain::Unsupported; return r; }
                r.id = x->pPresentIds[0]; r.kind = AppChain::Carried; break; }
            case VK_STRUCTURE_TYPE_SWAPCHAIN_PRESENT_FENCE_INFO_KHR: {
                auto* x = reinterpret_cast<const VkSwapchainPresentFenceInfoKHR*>(p);
                if (x->swapchainCount != 1 || !x->pFences) { r.kind = AppChain::Unsupported; return r; }
                r.fence = x->pFences[0]; r.kind = AppChain::Carried; break; }
            case VK_STRUCTURE_TYPE_SWAPCHAIN_PRESENT_MODE_INFO_KHR: {
                auto* x = reinterpret_cast<const VkSwapchainPresentModeInfoKHR*>(p);
                if (x->swapchainCount != 1 || !x->pPresentModes) { r.kind = AppChain::Unsupported; return r; }
                r.mode = x->pPresentModes[0]; r.has_mode = true; r.kind = AppChain::Carried; break; }
            case VK_STRUCTURE_TYPE_PRESENT_ID_2_KHR:
            case VK_STRUCTURE_TYPE_PRESENT_TIMINGS_INFO_EXT:
                r.kind = AppChain::Unsupported; return r;
            default: break;                                                      // other structures are dropped for a held frame
        }
    }
    return r;
}

// The carried part of an application's present chain, as a pNext chain for the layer's own present of that frame.
struct CarriedExt {
    VkPresentIdKHR pid{VK_STRUCTURE_TYPE_PRESENT_ID_KHR};
    VkSwapchainPresentFenceInfoKHR fi{VK_STRUCTURE_TYPE_SWAPCHAIN_PRESENT_FENCE_INFO_KHR};
    VkSwapchainPresentModeInfoKHR mi{VK_STRUCTURE_TYPE_SWAPCHAIN_PRESENT_MODE_INFO_KHR};
    uint64_t id{0}; VkFence fence{}; VkPresentModeKHR mode{}; bool has_mode{false};
    const void* Link() {
        const void* head = nullptr;
        if (has_mode) { mi.swapchainCount = 1; mi.pPresentModes = &mode; mi.pNext = head; head = &mi; }
        if (fence) { fi.swapchainCount = 1; fi.pFences = &fence; fi.pNext = head; head = &fi; }
        if (id)    { pid.swapchainCount = 1; pid.pPresentIds = &id; pid.pNext = head; head = &pid; }
        return head;
    }
};

// Frames the layer holds back keep their application fence registered, so a wait for it releases the frame (see the hooks below).
void HeldFenceAdd(DeviceDispatch& dt, SwapchainState& sc, const HeldFrame& h) {
    if (!h.fence) return;
    std::lock_guard<std::mutex> l(dt.fence_mu);
    dt.held_fences.emplace_back(uint64_t(reinterpret_cast<uintptr_t>(h.fence)), sc.weak_from_this());
}
void HeldFencesDrop(DeviceDispatch& dt, SwapchainState* sc) {      // every held fence that belongs to a swapchain that is being destroyed
    std::lock_guard<std::mutex> l(dt.fence_mu);
    dt.held_fences.erase(std::remove_if(dt.held_fences.begin(), dt.held_fences.end(), [&](auto& e) { auto s = e.second.lock(); return !s || s.get() == sc; }), dt.held_fences.end());
}
void HeldFenceRemove(DeviceDispatch& dt, const HeldFrame& h) {
    if (!h.fence) return;
    std::lock_guard<std::mutex> l(dt.fence_mu);
    const uint64_t k = uint64_t(reinterpret_cast<uintptr_t>(h.fence));
    dt.held_fences.erase(std::remove_if(dt.held_fences.begin(), dt.held_fences.end(), [&](auto& e) { return e.first == k; }), dt.held_fences.end());
}

// Presents the real image of a held frame (through the ring when VKIFRAME_DEBUG_RING=1). Does not touch the ring pins.
VkResult PresentFrame(DeviceDispatch& dt, VkQueue queue, SwapchainState& sc, const HeldFrame& h, uint64_t now, double tick_ns) {
    GpuState& g = *sc.gpu;
    VkSemaphore wait = VK_NULL_HANDLE;
    if (!GpuPrepareHeldPresent(dt, queue, g, h, GetConfig().debug_ring, wait)) {      // the debug round trip failed: show the real image
        GpuPrepareHeldPresent(dt, queue, g, h, /*via_ring=*/false, wait);
    }
    VkPresentInfoKHR pi{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
    pi.waitSemaphoreCount = 1; pi.pWaitSemaphores = &wait;
    pi.swapchainCount = 1; pi.pSwapchains = &sc.handle; pi.pImageIndices = &h.image_index;
    CarriedExt ext; ext.id = h.present_id; ext.fence = h.fence; ext.mode = h.mode; ext.has_mode = h.has_mode;                   // the frame's own application id and fence
    pi.pNext = ext.Link();
    return PresentWithTiming(dt, queue, sc, pi, now, tick_ns);
}

// Presents the frame being held (if any) and drops its ring reference. Returns VK_SUCCESS when nothing was held.
VkResult PresentHeld(DeviceDispatch& dt, VkQueue queue, SwapchainState& sc, uint64_t now) {
    GpuState& g = *sc.gpu;
    if (!g.held.valid) return VK_SUCCESS;
    const HeldFrame h = g.held;
    g.held.valid = false;
    HeldFenceRemove(dt, h);
    const VkResult r = PresentFrame(dt, queue, sc, h, now);
    GpuUnpin(g, h.slot);
    return r;
}

// Presents one synthetic frame: mix(A, B, t) written into the carrier's image (hidden == false) or a hidden image.
VkResult PresentSynthetic(DeviceDispatch& dt, VkQueue queue, SwapchainState& sc, const HeldFrame& a, const HeldFrame& b, float t, bool hidden, uint64_t now, double tick_ns, bool extrap) {
    uint32_t idx = 0; VkSemaphore wait = VK_NULL_HANDLE;
    const VkResult sr = GpuSynthesize(dt, queue, *sc.gpu, a, b, t, hidden, idx, wait, extrap);
    if (sr != VK_SUCCESS) return sr;
    VkPresentInfoKHR pi{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
    pi.waitSemaphoreCount = 1; pi.pWaitSemaphores = &wait;
    pi.swapchainCount = 1; pi.pSwapchains = &sc.handle; pi.pImageIndices = &idx;
    CarriedExt ext; if (!hidden) { ext.id = a.present_id; ext.fence = a.fence; ext.mode = a.mode; ext.has_mode = a.has_mode; }       // the carrier stands in for A: it keeps A's id and fence
    pi.pNext = ext.Link();
    { std::lock_guard<std::mutex> l(sc.mu); ++sc.rates.synthetic; }
    return PresentWithTiming(dt, queue, sc, pi, now, tick_ns);
}
} // namespace
