// All hooks: instance/device/swapchain create, present hook, presenter thread, hold-one.
#include "vk_iframes_layer.hpp"
#include "live_config.hpp"
#include "gpu_frames.hpp"
#include "present_timing.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <exception>
#include <mutex>
#include <new>
#include <thread>
#include <unistd.h>
#include <cstring>
#include <string>
#include <vector>

#include "layer_internal.hpp"
using namespace fg_layer;
// Set by the hook bodies for the guard below (see "host out of memory" at the end of this file): the next layer's destroy was already called; the game's frame
// was already consumed by the layer; the swapchain of the present in progress.
namespace fg_layer { std::atomic<bool> g_presenter_detached{false}; thread_local bool t_guard_forwarded = false, t_guard_present_consumed = false; thread_local SwapchainState* t_guard_present_sc = nullptr; }

void Impl_DestroySwapchainKHR(VkDevice device, VkSwapchainKHR swapchain,
                                                    const VkAllocationCallbacks* pAllocator) {
    DeviceDispatch* dt = LookupDevice(GetKey(device));
    if (!dt) return;
    StatsBenchAbort("the game closed its swapchain");           // a benchmark run stops and restores ENABLED
    FG_LOG("shutdown: vkDestroySwapchainKHR");
    const uint64_t t0 = NowNs();
    if (swapchain) {
        if (SwapchainState* sc = FindSwapchain(swapchain)) {
            if (sc->timing_enabled) { std::lock_guard<std::mutex> l(sc->mu); DrainPastTimings(*dt, *sc); }   // under mu like every other caller: the presenter thread may be in its own drain
            bool detached;
            {   // flow_mu: a thread that flushes the held frame for a fence or a present id (Hook_WaitForFences, Hook_GetFenceStatus, Hook_WaitForPresentKHR) holds it for the whole
                // flush. Destroy waits for that flush here; after it those threads find no GPU state (sc->gpu is null) and do nothing. The state itself stays allocated
                // for as long as a flush holds a reference to it (FlushForFences).
                std::lock_guard<std::mutex> fl(sc->flow_mu);
                detached = StopPresenter(*sc);                                    // no job may touch the swapchain while it is destroyed
                FG_LOG("shutdown: presenter stopped (%.0f ms)", (NowNs() - t0) / 1e6);
                HeldFencesDrop(*dt, sc);                                          // a later wait on the held frame's fence must not reach this state
                if (!detached && sc->gpu) {
                    GpuState* g = sc->gpu;
                    sc->gpu = nullptr;                                            // the held frame is simply dropped with its swapchain
                    GpuDestroy(*dt, g);
                    FG_LOG("shutdown: GPU resources destroyed (%.0f ms)", (NowNs() - t0) / 1e6);
                }
            }
            if (detached) {
                // The presenter thread is stuck in a driver call and still holds the swapchain state, its GPU state and the device dispatch table:
                // none of them is freed (this is shutdown, a small leak); vkDestroyDevice leaves the dispatch table alone too.
                FG_LOG("shutdown: the presenter thread is still running; its swapchain state, GPU state and dispatch table are left allocated");
                g_presenter_detached = true;
                LeakSwapchainState(swapchain);
            } else {
                DestroySwapchainState(swapchain);
            }
        } else DestroySwapchainState(swapchain);
    }
    t_guard_forwarded = true;
    dt->DestroySwapchainKHR(device, swapchain, pAllocator);
    FG_LOG("shutdown: swapchain destroyed (%.0f ms)", (NowNs() - t0) / 1e6);
}

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
    {
        const double nat = double(r.native) / secs, shown = double(r.shown) / secs;
        LiveWriteStatus(nat, shown, nat > 0.5 ? std::max(0, int(std::lround(shown / nat)) - 1) : 0, !Live().enabled.load() ? "off" : GetConfig().extrapolate ? "extrap" : sc.sched.GateActive() ? (holding ? "interp" : "passthru") : "bypass", sc.refresh_ns ? 1e9 / double(sc.refresh_ns) : 0.0, StatsBenchLabel());
    }
    FG_LOG_ROUTINE("native %.1f fps -> output %.1f fps (planned) | shown %.1f fps%s | %s | pairs %llu (synthetic %llu) bypass %llu carrier-only %llu dropped %llu | content delay %.1f ms",
           double(r.native) / secs, holding || !sc.sched.GateActive() ? double(r.output) / secs : double(r.native) / secs, double(r.shown) / secs, sc.gpu ? (GetConfig().extrapolate ? " (extrapolating)" : " (frame holding on)") : "",
           sc.sched.GateActive() ? (holding ? "generating" : "pass-through (frame holding off)") : "bypass",
           (unsigned long long)r.pair, (unsigned long long)r.synthetic, (unsigned long long)r.bypass, (unsigned long long)r.carrier, (unsigned long long)r.dropped,
           sc.sched.ContentDelayNs() / 1e6);
    if (GetConfig().extrapolate) {
        const uint64_t planned = sc.ex_planned.exchange(0), cancelled = sc.ex_cancelled.exchange(0), ln = sc.ex_late_n.exchange(0), lsum = sc.ex_late_sum_ns.exchange(0), lmax = sc.ex_late_max_ns.exchange(0);
        rec.late_mean_ms = ln ? double(lsum) / double(ln) / 1e6 : 0.0; rec.late_max_ms = double(lmax) / 1e6; rec.cancelled = cancelled;
        FG_LOG_ROUTINE("extrapolation: predicted frames not shown %llu of %llu | native frame late against its slot: mean %.2f ms, max %.2f ms",
               (unsigned long long)cancelled, (unsigned long long)planned, ln ? double(lsum) / double(ln) / 1e6 : 0.0, double(lmax) / 1e6);
    }
    {
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
        if (want_stats) {                                      // VKIFRAME_STATS / benchmark: this second as one record (stats.hpp)
            rec.secs = secs; rec.target_fps = EffectiveTargetFps(double(Live().target_fps.load()), sc.refresh_ns, sc.refresh_vrr); rec.refresh_hz = sc.refresh_ns ? 1e9 / double(sc.refresh_ns) : 0.0;
            rec.mode = !Live().enabled.load() ? "off" : GetConfig().extrapolate ? "extrap" : sc.sched.GateActive() ? (holding ? "interp" : "passthru") : "bypass";
            rec.native_fps = double(r.native) / secs; rec.shown_fps = double(r.shown) / secs;
            rec.gen_per_native = rec.native_fps > 0.5 ? std::max(0.0, rec.shown_fps / rec.native_fps - 1.0) : 0.0;
            rec.cpu_game_ms = cpu_hook; rec.presenter_core = cpu_pres;
            rec.wait_lock_ms = w_lock; rec.wait_ring_ms = w_ring; rec.wait_fence_ms = w_fence; rec.wait_acquire_ms = w_acq; rec.wait_present_ms = w_pres;
            rec.dropped = r.dropped;
            rec.cap_wait_ms = cap_wait_ms; rec.cap_hold_ms = cap_hold_ms;
            rec.pid = long(getpid());
            rec.content_delay_ms = sc.sched.ContentDelayNs() / 1e6;
            rec.flow_ms = flow_ms; rec.pairs_per_s = pairs_s; rec.interp_ms = interp_ms; rec.generated_per_s = gen_s;
            stats::FillDerived(rec, sc.stat_ft);
        }
        if (want_stats && GetConfig().stats_frames) {          // every frame time of this second, once, from the vector that already exists (no work in the present path)
            const float capv = (Live().enabled.load() || StatsBenchActive()) ? Live().base_fps_cap.load() : 0.0f;
            timespec wts; clock_gettime(CLOCK_REALTIME, &wts);
            (void)LiveFramesAppend(stats::FramesJson(sc.stat_ft, (long long)wts.tv_sec, capv >= 1.0f ? 1000.0 / double(capv) : 0.0, long(getpid())));
        }
        sc.stat_ft.Clear();
    }
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

// ---------------------------------------------------------------- presenter thread (Stage 3c-ii)

struct Presenter {
    std::thread th;
    std::mutex mu;
    std::condition_variable cv, idle_cv;
    std::deque<PresentJob> q;
    bool busy{false};
    std::atomic<bool> stop{false};
};

namespace fg_layer {
void NotePresentError(SwapchainState& sc, VkResult r) {
    if (r < 0) { int zero = 0; sc.pending_error.compare_exchange_strong(zero, int(r)); }
}

// Emits the planned ticks of a pair; A's real image is the carrier of the first one. Synthetic failures switch synthetic frames
// off, never the game. Runs on the presenter thread, or on the game thread with VKIFRAME_PRESENTER=0.
void ExecutePair(DeviceDispatch& dt, SwapchainState& sc, const PresentJob& j, Presenter* pr) {
    const Config& cfg = GetConfig();
    GpuState& g = *sc.gpu;
    const PairScheduler::Plan& plan = j.plan;
    const bool timed = pr != nullptr;                              // sleep until each tick only with the presenter thread
    auto wait_tick = [&](int i, const PlannedTick& tk) {
        if (!timed && !(cfg.pace_sleep && i > 0)) return;
        const double due = tk.time_ns - double(cfg.pace_lead_us) * 1e3, now = double(NowNs());
        if (due > now && due - now < 100e6) SleepUntilNs(uint64_t(due));    // a tick far in the future is a scheduler hiccup: do not stall for it
    };
    const bool synth = g.synth_ready && !sc.synth_failed && cfg.synth;
    bool carrier_done = false;
    if (synth && cfg.flow && !plan.carrier_only && plan.n > 0 && g.flow.ready && !g.flow.failed) GpuFlow(dt, j.queue, g, j.a, j.b);
    auto stopped = [&] { return pr && pr->stop.load(); };
    if (!plan.carrier_only) {
        for (int i = 0; i < plan.n && !stopped(); ++i) {
            const PlannedTick& tk = plan.ticks[i];
            wait_tick(i, tk);
            if (stopped()) break;
            const uint64_t now = NowNs();
            if (i == 0) {
                // the flow debug view replaces every frame, native ones included
                if ((tk.native_a && !(cfg.debug_flow && g.flow.pair_valid)) || !synth) { NotePresentError(sc, PresentFrame(dt, j.queue, sc, j.a, now, tk.time_ns)); carrier_done = true; continue; }
                const VkResult r = PresentSynthetic(dt, j.queue, sc, j.a, j.b, float(tk.t), /*hidden=*/false, now, tk.time_ns);
                if (r == VK_SUCCESS || r == VK_SUBOPTIMAL_KHR) { carrier_done = true; continue; }
                FG_LOG("synth: first synthetic frame failed (%d), synthetic frames are off for this swapchain", int(r));
                sc.synth_failed = true; NotePresentError(sc, PresentFrame(dt, j.queue, sc, j.a, now, tk.time_ns)); carrier_done = true; continue;
            }
            if (!synth || sc.synth_failed) break;
            const VkResult r = PresentSynthetic(dt, j.queue, sc, j.a, j.b, float(tk.t), /*hidden=*/true, now, tk.time_ns);
            if (r == VK_SUCCESS || r == VK_SUBOPTIMAL_KHR) continue;
            if (r == VK_ERROR_OUT_OF_DATE_KHR || r == VK_ERROR_SURFACE_LOST_KHR) { NotePresentError(sc, r); break; }
            FG_LOG("synth: synthetic frame failed (%d), synthetic frames are off for this swapchain", int(r));
            sc.synth_failed = true; break;
        }
    }
    if (!carrier_done && !stopped()) {                                           // carrier-only, or no tick at all
        PlannedTick tk{plan.carrier_time_ns, 0.0, true};
        if (plan.carrier_only) wait_tick(0, tk);
        NotePresentError(sc, PresentFrame(dt, j.queue, sc, j.a, NowNs(), plan.carrier_only ? plan.carrier_time_ns : 0.0));
    }
    GpuUnpin(g, j.a.slot); GpuUnpin(g, j.b.slot);
}

// Extrapolation job. Tick 0 is the native frame B (never skipped: the application waits on it); the other ticks are frames predicted from B,
// presented at their slots unless a newer frame has arrived (then they are stale and the new job takes over the clock).
void ExecuteExtrap(DeviceDispatch& dt, SwapchainState& sc, const PresentJob& j, Presenter* pr) {
    const Config& cfg = GetConfig();
    GpuState& g = *sc.gpu;
    const PairScheduler::Plan& plan = j.plan;
    auto stopped   = [&] { return pr && pr->stop.load(); };
    auto cancelled = [&] { return sc.job_gen.load() != j.gen; };
    // sleep until the tick is due; one wait on the presenter's condition variable, woken early by a cancel or a stop (no polling)
    auto wait_tick = [&](const PlannedTick& tk, bool cancellable) {
        if (!pr) return;
        const double due = tk.time_ns - double(cfg.pace_lead_us) * 1e3, now = double(NowNs());
        if (due <= now || due - now > 100e6) return;
        std::unique_lock<std::mutex> l(pr->mu);
        pr->cv.wait_until(l, std::chrono::steady_clock::time_point(std::chrono::nanoseconds((long long)due)), [&] { return stopped() || (cancellable && cancelled()); });
    };
    wait_tick(plan.ticks[0], false);
    NotePresentError(sc, PresentFrame(dt, j.queue, sc, j.b, NowNs(), plan.ticks[0].time_ns));
    {   // how late the native frame went out against its planned slot (the presenter is one serial queue: it may have been busy with the job before)
        const double late = std::max(0.0, double(NowNs()) - plan.ticks[0].time_ns);
        sc.ex_late_sum_ns += uint64_t(late); ++sc.ex_late_n;
        uint64_t m = sc.ex_late_max_ns.load(); while (uint64_t(late) > m && !sc.ex_late_max_ns.compare_exchange_weak(m, uint64_t(late))) {}
    }
    const bool synth = plan.n > 1 && g.synth_ready && !sc.synth_failed && cfg.synth && g.flow.ready && !g.flow.failed && !stopped() && !cancelled();
    if (synth) GpuFlow(dt, j.queue, g, j.a, j.b);
    int shown = 0;
    for (int i = 1; synth && i < plan.n; ++i) {
        wait_tick(plan.ticks[i], true);
        if (stopped() || cancelled()) break;
        const VkResult r = PresentSynthetic(dt, j.queue, sc, j.a, j.b, float(plan.ticks[i].t), /*hidden=*/true, NowNs(), plan.ticks[i].time_ns, /*extrap=*/true);
        if (r == VK_SUCCESS || r == VK_SUBOPTIMAL_KHR) { ++shown; continue; }
        if (r == VK_ERROR_FEATURE_NOT_PRESENT) break;                           // no flow for this pair: the native frame stays
        if (r == VK_ERROR_OUT_OF_DATE_KHR || r == VK_ERROR_SURFACE_LOST_KHR) { NotePresentError(sc, r); break; }
        FG_LOG("extrapolate: predicted frame failed (%d), synthetic frames are off for this swapchain", int(r));
        sc.synth_failed = true; break;
    }
    sc.ex_planned += uint64_t(plan.n - 1); sc.ex_cancelled += uint64_t(plan.n - 1 - shown);   // each one not shown leaves its output slot to repeat the last frame
    GpuUnpin(g, j.a.slot); GpuUnpin(g, j.b.slot);
}

void PresenterMain(DeviceDispatch* dt, SwapchainState* sc, Presenter* p) {
    uint64_t wd_logged = 0;
    for (;;) {
        PresentJob j;
        {
            std::unique_lock<std::mutex> l(p->mu);
            while (!p->cv.wait_for(l, std::chrono::milliseconds(50), [&] { return p->stop.load() || !p->q.empty(); })) {
                const uint64_t now = NowNs(), last = sc->last_present_ns.load();
                if (last && now - last > 200'000'000ull && sc->gpu && sc->last_queue) {   // idle flush: the game stopped presenting, show its last frame
                    l.unlock();
                    {
                        std::unique_lock<std::mutex> fl(sc->flow_mu, std::try_to_lock);
                        if (fl.owns_lock() && sc->gpu->held.valid && NowNs() - sc->last_present_ns.load() > 200'000'000ull) {
                            FG_LOG_ROUTINE("hold: no present for %.0f ms, showing the held frame", (NowNs() - last) / 1e6);
                            NotePresentError(*sc, PresentHeld(*dt, sc->last_queue, *sc, NowNs()));
                        }
                    }
                    l.lock();
                    if (p->stop.load() || !p->q.empty()) break;
                }
                if (!(last && now - last > 2'000'000'000ull)) continue;                      // watchdog: the game stopped presenting
                if (last && now - last > 2'000'000'000ull && now - wd_logged > 5'000'000'000ull) {
                    wd_logged = now;
                    const uint64_t aw = sc->acquire_wait_since_ns.load(), pw = sc->wait_present_since_ns.load();
                    // The frame count (sc->mu) and the held frame (flow_mu) belong to the game thread; this log line only looks at them when it can take the locks at once,
                    // and with the presenter's own mutex released (the game thread takes flow_mu and then that mutex, never the other way round).
                    unsigned long long frames = 0; int held = -2; unsigned long long held_id = 0; int held_fence = 0; bool known = false;
                    l.unlock();
                    {
                        std::unique_lock<std::mutex> ml(sc->mu, std::try_to_lock);
                        std::unique_lock<std::mutex> fl(sc->flow_mu, std::try_to_lock);
                        if (ml.owns_lock() && fl.owns_lock()) {
                            known = true; frames = sc->frames;
                            held = sc->gpu ? int(sc->gpu->held.valid) : -1; held_id = sc->gpu ? sc->gpu->held.present_id : 0; held_fence = sc->gpu ? int(sc->gpu->held.fence != VK_NULL_HANDLE) : 0;
                        }
                    }
                    l.lock();
                    if (known) FG_LOG_ROUTINE("watchdog: no present for %.1f s | frames %llu | held %d (id %llu fence %d) | in acquire %.1f s | in present-wait %.1f s (id %llu)",
                           (now - last) / 1e9, frames, held, held_id, held_fence, aw ? (now - aw) / 1e9 : 0.0, pw ? (now - pw) / 1e9 : 0.0, (unsigned long long)sc->wait_present_id.load());
                    else FG_LOG_ROUTINE("watchdog: no present for %.1f s | the game thread holds its locks (frame state not read) | in acquire %.1f s | in present-wait %.1f s (id %llu)",
                           (now - last) / 1e9, aw ? (now - aw) / 1e9 : 0.0, pw ? (now - pw) / 1e9 : 0.0, (unsigned long long)sc->wait_present_id.load());
                    if (p->stop.load() || !p->q.empty()) break;
                }
            }
            if (p->stop.load()) break;
            j = p->q.front(); p->q.pop_front(); p->busy = true;
        }
        const uint64_t cpu0 = ThreadCpuNs();
        try {
            if (j.extrap) ExecuteExtrap(*dt, *sc, j, p); else ExecutePair(*dt, *sc, j, p);
        } catch (...) {
            // Out of host memory (or any exception) in a job: it must not end the thread or the process, and the game thread must not wait for it for ever (busy is
            // cleared below). The job's references on its two ring slots are released here (they are released at the end of the job otherwise), the swapchain
            // goes back to plain presents for what follows, and the frames of this job are not shown (the next job starts fresh). No lock is held here.
            GuardNote("presenter thread", "a job failed");
            sc->synth_failed = true;
            if (sc->gpu) { GpuUnpin(*sc->gpu, j.a.slot); GpuUnpin(*sc->gpu, j.b.slot); }
        }
        sc->cpu_presenter_ns += ThreadCpuNs() - cpu0;
        { std::lock_guard<std::mutex> l(p->mu); p->busy = false; }
        p->idle_cv.notify_all();
    }
}

void StartPresenter(DeviceDispatch& dt, SwapchainState& sc) {
    auto* p = new Presenter;
    sc.presenter = p;
    p->th = std::thread(PresenterMain, &dt, &sc, p);
    FG_LOG("presenter: thread started (lead %d us)", GetConfig().pace_lead_us);
}

bool StopPresenter(SwapchainState& sc) {    // true: the thread was detached and still uses `sc`, its GPU state and the device dispatch table
    Presenter* p = sc.presenter;
    if (!p) return false;
    { std::lock_guard<std::mutex> l(p->mu); p->stop = true; p->q.clear(); }
    p->cv.notify_all();
    if (p->th.joinable()) {
        // a presenter that is stuck inside a driver call must not hang the game's exit: wait a few seconds, then leave the thread behind
        std::unique_lock<std::mutex> l(p->mu);
        if (!p->idle_cv.wait_for(l, std::chrono::seconds(3), [&] { return !p->busy; })) {
            l.unlock();
            FG_LOG("shutdown: the presenter thread is still busy after 3 s, detaching it");
            p->th.detach();
            sc.presenter = nullptr;
            return true;                                                      // the Presenter object is leaked on purpose: the thread still uses it
        }
        l.unlock();
        p->th.join();
    }
    sc.presenter = nullptr;
    delete p;
    return false;
}

// Waits until every queued tick has been presented. Needed before the game's own present goes out, to keep the order.
void DrainPresenter(SwapchainState& sc) {
    Presenter* p = sc.presenter;
    if (!p) return;
    std::unique_lock<std::mutex> l(p->mu);
    if (!p->idle_cv.wait_for(l, std::chrono::seconds(2), [&] { return p->q.empty() && !p->busy; })) FG_LOG("presenter: drain timed out");
}
} // namespace

VkResult Impl_QueuePresentKHR(VkQueue queue, const VkPresentInfoKHR* pPresentInfo) {
    DeviceDispatch* dt = LookupDevice(GetKey(queue));          // a queue shares its device's key
    if (!dt) return VK_ERROR_DEVICE_LOST;
    const Config& cfg = GetConfig();

    SwapchainState* sc = pPresentInfo->swapchainCount == 1 ? FindSwapchain(pPresentInfo->pSwapchains[0]) : nullptr;
    if (!sc) {                                                  // several swapchains in one present: pass through untouched
        std::mutex* qm = QueueMutex(queue);
        std::unique_lock<std::mutex> lk;
        if (qm) lk = std::unique_lock<std::mutex>(*qm);
        return dt->QueuePresentKHR(queue, pPresentInfo);
    }

    t_guard_present_sc = sc;
    struct PresentTimer { SwapchainState* s; uint64_t t0, c0; ~PresentTimer() { s->wait_present_ns += NowNs() - t0; s->cpu_hook_ns += ThreadCpuNs() - c0; } } present_timer{sc, NowNs(), ThreadCpuNs()};     // everything below is the game thread inside our present

    // Optional base-rate cap: space the game's presents 1/cap apart (it makes Game Mode testable). By default the wait happens AFTER the present, so the
    // game starts its next frame as late as it can and still presents on the grid: it samples input just before rendering and its frame is not stale.
    // (VKIFRAME_CAP_EARLY=1: wait before the present, which makes the intervals exact but adds the whole wait to the latency of every frame.)
    uint64_t cap_slot_ns = 0;
    LivePoll(NowNs());                                          // live.conf: one stat() a second
    const float cap_fps_raw = (Live().enabled.load() || StatsBenchActive()) ? Live().base_fps_cap.load() : 0.0f;
    const float cap_fps = cap_fps_raw >= 1.0f ? cap_fps_raw : 0.0f;   // below 1 fps is not a cap (the settings already turn it into 0; this is the last guard before the sleep)   // a benchmark keeps the cap in its "off" seconds, so both states pace the game the same way   // disabled = pure passthrough, no cap either
    if (cap_fps > 0.0f) {
        cap_slot_ns = uint64_t(1e9 / cap_fps);
        const uint64_t now0 = NowNs();
        ++sc->cap_n;
        if (cfg.cap_mode == Config::CapMode::Early) {
            if (sc->cap_next_slot_ns > now0) { SleepUntilNs(sc->cap_next_slot_ns); sc->cap_hold_ns += NowNs() - now0; }
            sc->cap_next_slot_ns = std::max(sc->cap_next_slot_ns + cap_slot_ns, now0);   // never accumulates debt after a stall
        } else if (cfg.cap_mode == Config::CapMode::Hybrid) {
            // The game's render time since we handed the thread back, its mean and its mean absolute deviation (frames longer than a slot count too, clamped at two slots:
            // a game that cannot reach the cap must not be given a lead based on its fast days).
            if (sc->cap_return_ns) {
                const double render = std::min(double(now0 - sc->cap_return_ns), 2.0 * double(cap_slot_ns));
                if (sc->cap_render_ema_ns <= 0.0) { sc->cap_render_ema_ns = render; sc->cap_dev_ns = 0.0; }
                else { const double err = render - sc->cap_render_ema_ns; sc->cap_render_ema_ns += 0.1 * err; sc->cap_dev_ns = 0.9 * sc->cap_dev_ns + 0.1 * std::fabs(err); }
            }
            uint64_t slot_t = sc->cap_next_slot_ns;
            if (slot_t == 0 || now0 > slot_t + cap_slot_ns) slot_t = now0;               // the first frame, or more than a slot late: the grid restarts here, no debt, no hold
            if (slot_t > now0) { SleepUntilNs(slot_t); sc->cap_hold_ns += NowNs() - now0; }   // early: the present lands on the grid
            sc->cap_next_slot_ns = slot_t + cap_slot_ns;
        } else {
            if (sc->cap_return_ns) {                                                       // how long the game took from our return to this present
                const double render = double(now0 - sc->cap_return_ns);
                if (render < double(cap_slot_ns)) sc->cap_render_ema_ns = sc->cap_render_ema_ns > 0.0 ? 0.9 * sc->cap_render_ema_ns + 0.1 * render : render;
            }
            if (sc->cap_next_slot_ns == 0 || now0 > sc->cap_next_slot_ns + cap_slot_ns) sc->cap_next_slot_ns = now0;      // first frame, or after a stall: no debt
            sc->cap_next_slot_ns += cap_slot_ns;
        }
    }

    const uint64_t now = NowNs();
    if (StatsOn()) { if (sc->stat_prev_ns) sc->stat_ft.Add(double(now - sc->stat_prev_ns) / 1e6); sc->stat_prev_ns = now; }   // present-to-present interval of the game
    else sc->stat_prev_ns = 0;
    PairScheduler::Plan plan;
    {
        std::lock_guard<std::mutex> l(sc->mu);
        sc->timing.Update(now);
        ++sc->frames;
        plan = PlanAndCount(*sc, now);
    }

    // ---- frame holding (Stage 3b) ----
    std::unique_lock<std::mutex> flow(sc->flow_mu);
    const AppChainInfo app = ClassifyPresentChain(pPresentInfo);
    const AppChain chain_kind = app.kind;
    sc->last_queue = queue;
    sc->last_present_ns = now;
    if (sc->frames == 1) {                                                              // what the game chains to its presents (once)
        std::string types;
        for (auto* p = static_cast<const VkBaseInStructure*>(pPresentInfo->pNext); p; p = p->pNext) types += " " + std::to_string(int(p->sType));
        FG_LOG("present chain of the first present: [%s ] (%s)", types.c_str(), chain_kind == AppChain::Unsupported ? "unsupported" : chain_kind == AppChain::Carried ? "carried with held frames" : "plain");
    }
    bool holding_possible = sc->hold_candidate && !sc->hold_failed && chain_kind != AppChain::Unsupported;
    if (sc->hold_candidate && chain_kind == AppChain::Unsupported && !sc->skip_logged) {
        sc->skip_logged = true;
        FG_LOG("hold: off, the application's present chain uses present id 2 / present timing (or a malformed id/fence/mode struct), which cannot be carried to a later present");
    }
    if (holding_possible && !sc->gpu) {
        const uint32_t family = QueueFamilyOf(queue);
        VkQueueFamilyProperties qf[16]{}; uint32_t nq = 16;
        dt->instance->GetPhysicalDeviceQueueFamilyProperties(dt->physical_device, &nq, qf);
        if (family == UINT32_MAX || family >= nq || !(qf[family].queueFlags & VK_QUEUE_GRAPHICS_BIT)) {
            FG_LOG("hold: present queue family %u is unknown or cannot blit, frame holding is off", family);
            sc->hold_failed = true;
        } else if (!(sc->gpu = GpuInit(*dt, *sc, family))) {
            sc->hold_failed = true;
        } else if (cfg.presenter && sc->gpu->synth_ready && cfg.synth) {
            StartPresenter(*dt, *sc);                          // only worth a thread when synthetic frames need pacing
        }
        holding_possible = !sc->hold_failed;
    }

    t_guard_present_consumed = true;                            // from here on the layer may have submitted or presented: a failure must not present the game's frame a second time
    VkResult r = VK_SUCCESS;
    auto plain_present = [&] {                                  // the game's own present, after everything queued has gone out
        DrainPresenter(*sc);
        return PresentWithTiming(*dt, queue, *sc, *pPresentInfo, now);
    };
    if (!holding_possible || !sc->gpu) {
        if (sc->gpu && sc->gpu->held.valid) {                    // holding just failed or became impossible: let the held frame go first
            DrainPresenter(*sc);
            (void)PresentHeld(*dt, queue, *sc, now);
        }
        r = plain_present();
    } else if (plan.kind == PairScheduler::Kind::Bypass) {
        if (cfg.extrapolate) { ++sc->job_gen; if (sc->presenter) { { std::lock_guard<std::mutex> l(sc->presenter.load()->mu); } sc->presenter.load()->cv.notify_all(); } }   // predicted frames still queued are stale now
        DrainPresenter(*sc);
        if (sc->gpu->last.valid) { GpuUnpin(*sc->gpu, sc->gpu->last.slot); sc->gpu->last.valid = false; }
        VkResult rf = plan.flush_held || sc->gpu->held.valid ? PresentHeld(*dt, queue, *sc, now) : VK_SUCCESS;
        r = plain_present();                                                            // the game's own present, untouched
        if (rf < 0) r = rf;
    } else {                                                                            // Hold or Pair: capture the new frame, show the old one
        HeldFrame b;
        const bool captured = GpuCapture(*dt, queue, *sc->gpu, *pPresentInfo, b);
        b.present_id = app.id; b.fence = app.fence; b.mode = app.mode; b.has_mode = app.has_mode;                                     // shown later, with the application's own id and fence
        if (!captured) {
            FG_LOG("hold: capture failed, back to passthrough for this swapchain");
            sc->hold_failed = true;
            DrainPresenter(*sc);
            if (sc->gpu->held.valid) (void)PresentHeld(*dt, queue, *sc, now);
            r = plain_present();
        } else if (cfg.extrapolate) {
            // nothing is held back: B goes out as tick 0 of its job, the earlier frame is only a ring copy for the motion estimate
            GpuState& g = *sc->gpu;
            const bool have_prev = g.last.valid;
            HeldFrame a = have_prev ? g.last : b;                                      // the previous frame's reference passes to the job
            if (!have_prev) GpuPin(g, b.slot);                                         // a == b: the job drops both references
            GpuPin(g, b.slot);                                                         // the reference of g.last; the capture's own goes to the job
            g.last = b;
            if (!have_prev) plan.n = std::min(plan.n, 1);
            PresentJob job{a, b, plan, queue, true, ++sc->job_gen};
            if (sc->presenter) {
                { std::lock_guard<std::mutex> l(sc->presenter.load()->mu); sc->presenter.load()->q.push_back(job); }
                sc->presenter.load()->cv.notify_all();
            } else {
                ExecuteExtrap(*dt, *sc, job, nullptr);                                  // synchronous (VKIFRAME_PRESENTER=0)
            }
        } else if (plan.kind == PairScheduler::Kind::Pair && sc->gpu->held.valid) {
            PresentJob job{sc->gpu->held, b, plan, queue};
            HeldFenceRemove(*dt, sc->gpu->held);
            sc->gpu->held.valid = false;                                                // A's reference now belongs to the job
            GpuPin(*sc->gpu, b.slot);                                                   // B stays held for the next pair and is used by this one
            sc->gpu->held = b; HeldFenceAdd(*dt, *sc, b);
            if (sc->presenter) {
                { std::lock_guard<std::mutex> l(sc->presenter.load()->mu); sc->presenter.load()->q.push_back(job); }
                sc->presenter.load()->cv.notify_all();
            } else {
                ExecutePair(*dt, *sc, job, nullptr);                                    // synchronous (VKIFRAME_PRESENTER=0)
            }
        } else {
            r = PresentHeld(*dt, queue, *sc, now);                                       // Hold: nothing to present yet
            sc->gpu->held = b; HeldFenceAdd(*dt, *sc, b);                                // frame B waits for the next call
        }
    }
    flow.unlock();
    if (const int e = sc->pending_error.exchange(0); e < 0 && r >= 0) r = VkResult(e);    // an error of a presenter-thread present
    if (pPresentInfo->pResults) pPresentInfo->pResults[0] = r;

    {
        std::lock_guard<std::mutex> l(sc->mu);
        if (sc->timing_enabled) DrainPastTimings(*dt, *sc);
        LogTimingStats(*dt, *sc, NowNs());
        LogRates(*sc, now);
    }
    if (sc->frames % 300 == 0) FG_LOG("vkQueuePresentKHR (frame %llu)", (unsigned long long)sc->frames);
    if (cap_slot_ns && cfg.cap_mode != Config::CapMode::Early) {  // hold the game back until it should start the frame that will be presented on the next slot
        // late: lead = 1.2 x the mean render time; hybrid: the mean plus two mean deviations (the present is then also held to the grid, so a small overshoot costs nothing)
        const double lead = cfg.cap_mode == Config::CapMode::Hybrid ? sc->cap_render_ema_ns + 2.0 * sc->cap_dev_ns : 1.2 * sc->cap_render_ema_ns;
        if (double(sc->cap_next_slot_ns) > lead) { const uint64_t until = uint64_t(double(sc->cap_next_slot_ns) - lead); if (until > NowNs()) { const uint64_t t0 = NowNs(); SleepUntilNs(until); sc->cap_wait_ns += NowNs() - t0; } }
        sc->cap_return_ns = NowNs();
    }
    return r;
}

// ---------------------------------------------------------------- queue locking (Stage 3c-ii)
// The presenter thread submits and presents on the game's queue. Vulkan requires external synchronization per queue, so every
// queue operation of the game takes the same mutex as ours. Image acquires are polled so that no lock is held while waiting.

namespace {
struct QueueLock {
    std::mutex* m;
    explicit QueueLock(VkQueue q) : m(QueueMutex(q)) { if (m) m->lock(); }
    ~QueueLock() { if (m) m->unlock(); }
};

template <class F> VkResult PolledAcquire(uint64_t timeout, F&& once) {
    const uint64_t start = NowNs();
    for (;;) {
        const VkResult r = once();
        if (r != VK_NOT_READY && r != VK_TIMEOUT) return r;
        if (timeout == 0) return VK_NOT_READY;
        if (timeout != UINT64_MAX && NowNs() - start >= timeout) return VK_TIMEOUT;
        timespec nap{0, 100'000}; nanosleep(&nap, nullptr);
    }
}
} // namespace

VKAPI_ATTR VkResult VKAPI_CALL Hook_QueueSubmit(VkQueue q, uint32_t n, const VkSubmitInfo* s, VkFence f) {
    DeviceDispatch* dt = LookupDevice(GetKey(q)); if (!dt) return VK_ERROR_DEVICE_LOST; QueueLock l(q); return dt->QueueSubmit(q, n, s, f);
}
VKAPI_ATTR VkResult VKAPI_CALL Hook_QueueSubmit2(VkQueue q, uint32_t n, const VkSubmitInfo2* s, VkFence f) {
    DeviceDispatch* dt = LookupDevice(GetKey(q)); if (!dt) return VK_ERROR_DEVICE_LOST; QueueLock l(q);
    const auto submit2 = dt->QueueSubmit2 ? dt->QueueSubmit2 : dt->QueueSubmit2KHR;      // the same fallback as Hook_QueueSubmit2KHR, the other way round
    return submit2 ? submit2(q, n, s, f) : VK_ERROR_FEATURE_NOT_PRESENT;
}
VKAPI_ATTR VkResult VKAPI_CALL Hook_QueueSubmit2KHR(VkQueue q, uint32_t n, const VkSubmitInfo2* s, VkFence f) {
    DeviceDispatch* dt = LookupDevice(GetKey(q)); if (!dt) return VK_ERROR_DEVICE_LOST; QueueLock l(q);
    const auto submit2 = dt->QueueSubmit2KHR ? dt->QueueSubmit2KHR : dt->QueueSubmit2;
    return submit2 ? submit2(q, n, s, f) : VK_ERROR_FEATURE_NOT_PRESENT;
}
VKAPI_ATTR VkResult VKAPI_CALL Hook_QueueBindSparse(VkQueue q, uint32_t n, const VkBindSparseInfo* s, VkFence f) {
    DeviceDispatch* dt = LookupDevice(GetKey(q)); if (!dt) return VK_ERROR_DEVICE_LOST; QueueLock l(q); return dt->QueueBindSparse(q, n, s, f);
}
VKAPI_ATTR VkResult VKAPI_CALL Hook_QueueWaitIdle(VkQueue q) {
    DeviceDispatch* dt = LookupDevice(GetKey(q));
    if (!dt) return VK_ERROR_DEVICE_LOST;
    const uint64_t t0 = NowNs();
    QueueLock l(q);
    const uint64_t t1 = NowNs();
    const VkResult r = dt->QueueWaitIdle(q);
    const uint64_t t2 = NowNs();
    if (t2 - t0 > 500'000'000ull) FG_LOG("shutdown: vkQueueWaitIdle took %.0f ms (queue lock %.0f ms, wait %.0f ms)", (t2 - t0) / 1e6, (t1 - t0) / 1e6, (t2 - t1) / 1e6);
    return r;
}
VKAPI_ATTR VkResult VKAPI_CALL Hook_DeviceWaitIdle(VkDevice device) {
    DeviceDispatch* dt = LookupDevice(GetKey(device));
    if (!dt) return VK_ERROR_DEVICE_LOST;
    const uint64_t t0 = NowNs();
    std::vector<std::unique_lock<std::mutex>> locks;
    for (std::mutex* m : DeviceQueueMutexes(GetKey(device))) locks.emplace_back(*m);        // all queues, in a fixed order
    const uint64_t t1 = NowNs();
    const VkResult r = dt->DeviceWaitIdle(device);
    const uint64_t t2 = NowNs();
    if (t2 - t0 > 500'000'000ull) FG_LOG("shutdown: vkDeviceWaitIdle took %.0f ms (queue locks %.0f ms, wait %.0f ms)", (t2 - t0) / 1e6, (t1 - t0) / 1e6, (t2 - t1) / 1e6);
    return r;
}
// About the raw SwapchainState pointers in the acquire and present-wait hooks below: the swapchain is externally synchronized in vkAcquireNextImageKHR,
// vkAcquireNextImage2KHR (VkAcquireNextImageInfoKHR::swapchain), vkWaitForPresentKHR and vkDestroySwapchainKHR (externsync in vk.xml), so the game cannot destroy a
// swapchain while another thread is inside one of these calls, and the pointer from FindSwapchain stays valid for the whole call. The fence hooks are different (a fence
// is not synchronized with its swapchain) and hold shared ownership, see FlushForFences.
VkResult Impl_AcquireNextImageKHR(VkDevice device, VkSwapchainKHR swapchain, uint64_t timeout,
                                                        VkSemaphore sem, VkFence fence, uint32_t* pIndex) {
    DeviceDispatch* dt = LookupDevice(GetKey(device));
    if (!dt) return VK_ERROR_DEVICE_LOST;
    SwapchainState* sc = FindSwapchain(swapchain);
    if (!sc || !sc->gpu || !sc->presenter) { t_guard_forwarded = true; return dt->AcquireNextImageKHR(device, swapchain, timeout, sem, fence, pIndex); }
    GpuState* g = sc->gpu;
    struct Mark { SwapchainState* s; uint64_t t0; explicit Mark(SwapchainState* x) : s(x), t0(NowNs()) { s->acquire_wait_since_ns = t0; } ~Mark() { s->acquire_wait_since_ns = 0; s->wait_acquire_ns += NowNs() - t0; } } mark(sc);
    return PolledAcquire(timeout, [&] {
        std::lock_guard<std::mutex> l(g->acquire_mu);           // the presenter acquires hidden images on the same swapchain
        const VkResult r = dt->AcquireNextImageKHR(device, swapchain, 0, sem, fence, pIndex);
        if (r == VK_SUCCESS || r == VK_SUBOPTIMAL_KHR) t_guard_forwarded = true;     // an image was acquired and the semaphore or fence will be signalled: the guard must not acquire again
        return r;
    });
}
VkResult Impl_AcquireNextImage2KHR(VkDevice device, const VkAcquireNextImageInfoKHR* info, uint32_t* pIndex) {
    DeviceDispatch* dt = LookupDevice(GetKey(device));
    if (!dt) return VK_ERROR_DEVICE_LOST;
    SwapchainState* sc = FindSwapchain(info->swapchain);
    if (!sc || !sc->gpu || !sc->presenter) { t_guard_forwarded = true; return dt->AcquireNextImage2KHR(device, info, pIndex); }
    GpuState* g = sc->gpu;
    struct Mark { SwapchainState* s; uint64_t t0; explicit Mark(SwapchainState* x) : s(x), t0(NowNs()) { s->acquire_wait_since_ns = t0; } ~Mark() { s->acquire_wait_since_ns = 0; s->wait_acquire_ns += NowNs() - t0; } } mark(sc);
    VkAcquireNextImageInfoKHR once = *info; once.timeout = 0;
    return PolledAcquire(info->timeout, [&] {
        std::lock_guard<std::mutex> l(g->acquire_mu);
        const VkResult r = dt->AcquireNextImage2KHR(device, &once, pIndex);
        if (r == VK_SUCCESS || r == VK_SUBOPTIMAL_KHR) t_guard_forwarded = true;
        return r;
    });
}

// Releases the held frame of a swapchain when the game waits for something that only that frame's presentation can complete:
// its present id (vkWaitForPresentKHR) or its present fence (vkWaitForFences / vkGetFenceStatus). The ticks of the pair after it
// are skipped; the next pair starts fresh.
namespace fg_layer {
VkResult FlushHeld(DeviceDispatch& dt, SwapchainState& sc, uint64_t present_id, VkFence fence, const char* why) {
    std::lock_guard<std::mutex> fl(sc.flow_mu);
    if (!sc.gpu || !sc.last_queue) return VK_SUCCESS;
    GpuState& g = *sc.gpu;
    DrainPresenter(sc);
    const bool hit = g.held.valid && ((fence && g.held.fence == fence) || (present_id && g.held.present_id != 0 && g.held.present_id <= present_id));
    if (!hit) return VK_SUCCESS;
    static std::atomic<int> logged{0};
    if (logged++ < 5) FG_LOG("hold: the game waits for the held frame (%s), presenting it now", why);
    return PresentHeld(dt, sc.last_queue, sc, NowNs());
}

// Swapchains with a held frame whose application fence is among fences[]
void FlushForFences(DeviceDispatch& dt, uint32_t n, const VkFence* fences, const char* why) {
    std::vector<std::shared_ptr<SwapchainState>> hit;       // owning references: a swapchain destroyed on another thread while we flush stays allocated until we are done
    {
        std::lock_guard<std::mutex> l(dt.fence_mu);
        if (dt.held_fences.empty()) return;
        for (uint32_t i = 0; i < n; ++i)
            for (auto& e : dt.held_fences)
                if (e.first == uint64_t(reinterpret_cast<uintptr_t>(fences[i]))) if (auto s = e.second.lock()) hit.push_back(std::move(s));
    }
    for (uint32_t i = 0; i < n; ++i)
        for (auto& sc : hit) (void)FlushHeld(dt, *sc, 0, fences[i], why);
}
} // namespace

VkResult Impl_WaitForFences(VkDevice device, uint32_t n, const VkFence* fences, VkBool32 all, uint64_t timeout) {
    DeviceDispatch* dt = LookupDevice(GetKey(device));
    if (!dt) return VK_ERROR_DEVICE_LOST;
    FlushForFences(*dt, n, fences, "vkWaitForFences");
    return dt->WaitForFences(device, n, fences, all, timeout);
}
VkResult Impl_GetFenceStatus(VkDevice device, VkFence fence) {
    DeviceDispatch* dt = LookupDevice(GetKey(device));
    if (!dt) return VK_ERROR_DEVICE_LOST;
    FlushForFences(*dt, 1, &fence, "vkGetFenceStatus");
    return dt->GetFenceStatus(device, fence);
}

// vkWaitForPresentKHR for the id of the frame we are holding would wait for a present that only the game's next present
// triggers (DXVK waits for every present it makes). The frame is not lost, only shown one frame late, so the wait is answered
// at once and the game goes on to render the next frame. Waits for older ids are real: those frames are on screen or queued
// on the presenter thread. A game that then stops presenting gets its held frame from the idle flush in the presenter thread.
VkResult Impl_WaitForPresentKHR(VkDevice device, VkSwapchainKHR swapchain, uint64_t presentId, uint64_t timeout) {
    DeviceDispatch* dt = LookupDevice(GetKey(device));
    if (!dt) return VK_ERROR_DEVICE_LOST;
    SwapchainState* sc = FindSwapchain(swapchain);
    if (!dt->WaitForPresentKHR) return VK_ERROR_EXTENSION_NOT_PRESENT;                 // the driver has no vkWaitForPresentKHR (a game cannot legally call it then): do not call a null pointer
    if (!sc) return dt->WaitForPresentKHR(device, swapchain, presentId, timeout);
    {
        std::lock_guard<std::mutex> fl(sc->flow_mu);
        if (sc->gpu && sc->gpu->held.valid && sc->gpu->held.present_id != 0 && presentId >= sc->gpu->held.present_id) {
            static std::atomic<int> logged{0};
            if (logged++ < 3) FG_LOG("hold: vkWaitForPresentKHR(%llu) is for the held frame, answered early (it is shown with the next present)", (unsigned long long)presentId);
            return VK_SUCCESS;
        }
    }
    sc->wait_present_id = presentId; sc->wait_present_since_ns = NowNs();
    struct Done { SwapchainState* s; ~Done() { s->wait_present_since_ns = 0; } } done{sc};
    return dt->WaitForPresentKHR(device, swapchain, presentId, timeout);
}

// ---------------------------------------------------------------- host out of memory
// An exception (std::bad_alloc from a vector, string or map, std::system_error from a thread or a lock) must never leave a hook: it would run into the game's C
// code and end the process. Every entry point that allocates catches it here, logs once (a few lines at most), and falls back: a create hook fails with
// VK_ERROR_OUT_OF_HOST_MEMORY, a destroy hook goes on to the next layer's destroy (unless that was already called), a wait or acquire hook calls the real function,
// and a present forwards the game's own present when the layer has not touched the frame yet and fails with VK_ERROR_OUT_OF_HOST_MEMORY otherwise (never two presents).
// Nothing is locked while a fallback runs: the locks of the failed body were released while the exception unwound. This changes only the failure path.
namespace fg_layer {
std::atomic<int> g_guard_notes{0};
void GuardNote(const char* hook, const char* what) noexcept {
    if (g_guard_notes.fetch_add(1) >= 5) return;
    try { FG_LOG("%s: %s: the layer steps aside for this call (out of host memory or an internal error)", hook, what); } catch (...) {}
}
// Runs body(); true when it returned normally. On an exception: logs and returns false.
template <class F> bool Run(const char* hook, F&& body) noexcept {
    try { body(); return true; }
    catch (const std::bad_alloc&) { GuardNote(hook, "std::bad_alloc"); }
    catch (const std::exception& e) { GuardNote(hook, e.what()); }
    catch (...) { GuardNote(hook, "unknown exception"); }
    return false;
}
// A fallback must not throw out of the hook either.
template <class F> void Quiet(F&& f) noexcept { try { f(); } catch (...) {} }
} // namespace

VKAPI_ATTR VkResult VKAPI_CALL Hook_CreateInstance(const VkInstanceCreateInfo* ci, const VkAllocationCallbacks* a, VkInstance* out) {
    VkResult r = VK_ERROR_OUT_OF_HOST_MEMORY;
    (void)Run("vkCreateInstance", [&] { r = Impl_CreateInstance(ci, a, out); });
    return r;
}
VKAPI_ATTR VkResult VKAPI_CALL Hook_CreateDevice(VkPhysicalDevice pd, const VkDeviceCreateInfo* ci, const VkAllocationCallbacks* a, VkDevice* out) {
    VkResult r = VK_ERROR_OUT_OF_HOST_MEMORY;
    (void)Run("vkCreateDevice", [&] { r = Impl_CreateDevice(pd, ci, a, out); });
    return r;
}
VKAPI_ATTR VkResult VKAPI_CALL Hook_CreateSwapchainKHR(VkDevice d, const VkSwapchainCreateInfoKHR* ci, const VkAllocationCallbacks* a, VkSwapchainKHR* out) {
    VkResult r = VK_ERROR_OUT_OF_HOST_MEMORY;
    const VkSwapchainKHR before = out ? *out : VK_NULL_HANDLE;
    if (Run("vkCreateSwapchainKHR", [&] { r = Impl_CreateSwapchainKHR(d, ci, a, out); })) return r;
    if (out && *out != before && *out != VK_NULL_HANDLE) {         // the driver had created a swapchain before the failure: the game gets none, so it must not stay alive
        const VkSwapchainKHR made = *out; *out = VK_NULL_HANDLE;
        Quiet([&] { Hook_DestroySwapchainKHR(d, made, a); });
    }
    return VK_ERROR_OUT_OF_HOST_MEMORY;
}

// The destroy fallbacks below are the plain driver destroy: if the body failed before its own call to the next layer, this skips the device idle wait, the
// layer's state is not cleaned (it is leaked) and a presenter thread is not stopped. That is acceptable on the out-of-memory path: the game is closing the object.
VKAPI_ATTR void VKAPI_CALL Hook_DestroyInstance(VkInstance instance, const VkAllocationCallbacks* a) {
    void* key = GetKey(instance);                                 // read before the handle dies
    t_guard_forwarded = false;
    if (Run("vkDestroyInstance", [&] { Impl_DestroyInstance(instance, a); })) return;
    if (!t_guard_forwarded) Quiet([&] { if (InstanceDispatch* id = LookupInstance(key)) { id->DestroyInstance(instance, a); } });
}
VKAPI_ATTR void VKAPI_CALL Hook_DestroyDevice(VkDevice device, const VkAllocationCallbacks* a) {
    void* key = GetKey(device);
    t_guard_forwarded = false;
    if (Run("vkDestroyDevice", [&] { Impl_DestroyDevice(device, a); })) return;
    if (!t_guard_forwarded) Quiet([&] { if (DeviceDispatch* dd = LookupDevice(key)) { dd->DestroyDevice(device, a); } });
}
VKAPI_ATTR void VKAPI_CALL Hook_DestroySwapchainKHR(VkDevice device, VkSwapchainKHR swapchain, const VkAllocationCallbacks* a) {
    void* key = GetKey(device);
    t_guard_forwarded = false;
    if (Run("vkDestroySwapchainKHR", [&] { Impl_DestroySwapchainKHR(device, swapchain, a); })) return;
    if (!t_guard_forwarded) Quiet([&] { if (DeviceDispatch* dt = LookupDevice(key)) { dt->DestroySwapchainKHR(device, swapchain, a); } });
}

VKAPI_ATTR VkResult VKAPI_CALL Hook_AcquireNextImageKHR(VkDevice device, VkSwapchainKHR swapchain, uint64_t timeout, VkSemaphore sem, VkFence fence, uint32_t* pIndex) {
    VkResult r = VK_ERROR_OUT_OF_HOST_MEMORY;
    t_guard_forwarded = false;
    if (Run("vkAcquireNextImageKHR", [&] { r = Impl_AcquireNextImageKHR(device, swapchain, timeout, sem, fence, pIndex); })) return r;
    if (t_guard_forwarded) return VK_ERROR_OUT_OF_HOST_MEMORY;          // the real acquire already ran and signalled: a second one would signal the semaphore twice
    Quiet([&] { if (DeviceDispatch* dt = LookupDevice(GetKey(device))) r = dt->AcquireNextImageKHR(device, swapchain, timeout, sem, fence, pIndex); });
    return r;
}
VKAPI_ATTR VkResult VKAPI_CALL Hook_AcquireNextImage2KHR(VkDevice device, const VkAcquireNextImageInfoKHR* info, uint32_t* pIndex) {
    VkResult r = VK_ERROR_OUT_OF_HOST_MEMORY;
    t_guard_forwarded = false;
    if (Run("vkAcquireNextImage2KHR", [&] { r = Impl_AcquireNextImage2KHR(device, info, pIndex); })) return r;
    if (t_guard_forwarded) return VK_ERROR_OUT_OF_HOST_MEMORY;
    Quiet([&] { if (DeviceDispatch* dt = LookupDevice(GetKey(device))) r = dt->AcquireNextImage2KHR(device, info, pIndex); });
    return r;
}
VKAPI_ATTR VkResult VKAPI_CALL Hook_WaitForFences(VkDevice device, uint32_t n, const VkFence* fences, VkBool32 all, uint64_t timeout) {
    VkResult r = VK_ERROR_OUT_OF_HOST_MEMORY;
    if (Run("vkWaitForFences", [&] { r = Impl_WaitForFences(device, n, fences, all, timeout); })) return r;
    Quiet([&] { if (DeviceDispatch* dt = LookupDevice(GetKey(device))) r = dt->WaitForFences(device, n, fences, all, timeout); });
    return r;
}
VKAPI_ATTR VkResult VKAPI_CALL Hook_GetFenceStatus(VkDevice device, VkFence fence) {
    VkResult r = VK_ERROR_OUT_OF_HOST_MEMORY;
    if (Run("vkGetFenceStatus", [&] { r = Impl_GetFenceStatus(device, fence); })) return r;
    Quiet([&] { if (DeviceDispatch* dt = LookupDevice(GetKey(device))) r = dt->GetFenceStatus(device, fence); });
    return r;
}
VKAPI_ATTR VkResult VKAPI_CALL Hook_WaitForPresentKHR(VkDevice device, VkSwapchainKHR swapchain, uint64_t presentId, uint64_t timeout) {
    VkResult r = VK_ERROR_OUT_OF_HOST_MEMORY;
    if (Run("vkWaitForPresentKHR", [&] { r = Impl_WaitForPresentKHR(device, swapchain, presentId, timeout); })) return r;
    Quiet([&] { if (DeviceDispatch* dt = LookupDevice(GetKey(device))) r = dt->WaitForPresentKHR ? dt->WaitForPresentKHR(device, swapchain, presentId, timeout) : VK_ERROR_EXTENSION_NOT_PRESENT; });
    return r;
}

VKAPI_ATTR VkResult VKAPI_CALL Hook_QueuePresentKHR(VkQueue queue, const VkPresentInfoKHR* pPresentInfo) {
    VkResult r = VK_ERROR_OUT_OF_HOST_MEMORY;
    t_guard_present_consumed = false; t_guard_present_sc = nullptr;
    if (Run("vkQueuePresentKHR", [&] { r = Impl_QueuePresentKHR(queue, pPresentInfo); })) return r;
    SwapchainState* sc = t_guard_present_sc;
    const bool consumed = t_guard_present_consumed;
    if (sc) Quiet([&] { std::lock_guard<std::mutex> fl(sc->flow_mu); sc->hold_failed = true; });   // no more holding on this swapchain; a held frame goes out with the next present
    if (!consumed) {                                              // the layer has not touched the frame: the game's own present, as if the layer were not there
        Quiet([&] {
            if (DeviceDispatch* dt = LookupDevice(GetKey(queue))) {
                std::mutex* qm = QueueMutex(queue);
                std::unique_lock<std::mutex> lk;
                if (qm) lk = std::unique_lock<std::mutex>(*qm);
                r = dt->QueuePresentKHR(queue, pPresentInfo);
            }
        });
        return r;
    }
    if (pPresentInfo && pPresentInfo->pResults && pPresentInfo->swapchainCount >= 1) pPresentInfo->pResults[0] = VK_ERROR_OUT_OF_HOST_MEMORY;
    return VK_ERROR_OUT_OF_HOST_MEMORY;                           // the frame may be held or queued already: presenting it again could show it twice
}
