// The presenter thread: queued present jobs, their timing, start, drain and stop.
#include "layer_internal.hpp"
#include "live_config.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <exception>
#include <mutex>
#include <new>
#include <thread>
#include <cstring>
#include <string>
#include <vector>

using namespace fg_layer;

// ---------------------------------------------------------------- presenter thread (Stage 3c-ii)


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
