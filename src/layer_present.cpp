// The present hook and the waits that release a held frame (fences, present id).
#include "layer_internal.hpp"
#include "live_config.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <mutex>
#include <cstring>
#include <string>
#include <vector>

using namespace fg_layer;

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
