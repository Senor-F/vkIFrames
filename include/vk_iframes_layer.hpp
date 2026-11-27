#pragma once
// Shared declarations: dispatch tables, SwapchainState, hook list, log macros.

#include <vulkan/vulkan.h>
#include <vulkan/vk_layer.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <mutex>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <memory>
#include <string>
#include <vector>

#include "output_clock.hpp"
#include "stats.hpp"
#include "timing_engine.hpp"

#include "vk_iframes_config.hpp"
#include "vk_iframes_dispatch.hpp"

inline const Config& GetConfig() {
    static const Config c = [] {
        Config c;
        { const MachineDefaults md = MachineDefaultsFor(); c.target_fps = md.target_fps; c.base_fps_cap = md.base_fps_cap; }
        // VKIFRAME_LOG_FILE=<path>: append everything the layer prints (and the game's own stderr) to that file; for runs without the launch script
        if (const char* v = EnvVar("LOG_FILE"); v && *v) { FILE* r_ = std::freopen(v, "a", stderr); (void)r_; }
        else (void)LiveLogOpen();                     // default: one log, ~/.config/vkiframes/live.log, shared with the settings changes
        EnvNumber("TARGET_FPS", 1.0, 600.0, c.target_fps);
        EnvNumber("SYNTH_MARGIN", 0.0, 0.45, c.synth_margin);
        EnvNumber("PHASE_OFFSET", 0.0, 0.999, c.phase_offset);
        EnvFlag("PHASE_LOCK", c.phase_lock);
        EnvNumber("MIN_GAIN", 1.01, 2.0, c.min_gain);
        EnvInt("MAX_SYNTHETIC", 1, 10, c.max_synthetic);
        EnvNumber("BASE_FPS_CAP", 0.0, 240.0, c.base_fps_cap);
        if (c.base_fps_cap < 1.0f) c.base_fps_cap = 0.0f;                      // below 1 fps is not a cap anybody means: off (a 1/x second sleep per frame would freeze the game)
        EnvFlag("HOLD", c.hold);
        EnvFlag("SYNTH", c.synth);
        EnvFlag("PACE_SLEEP", c.pace_sleep);
        EnvFlag("PRESENTER", c.presenter);
        EnvInt("CMD_BUFFERS", 4, 32, c.cmd_buffers);
        EnvInt("ACQUIRE_WAIT_US", 0, 20000, c.acquire_wait_us);
        EnvInt("PACE_LEAD_US", 0, 20000, c.pace_lead_us);
        EnvFlag("ABSOLUTE", c.absolute);
        EnvFlag("FLOW", c.flow);
        EnvFlag("DEBUG_FLOW", c.debug_flow);
        EnvInt("TICKMARK", 0, 3, c.tickmark);
        EnvInt("FLOW_LEVEL", 1, 3, c.flow_level);
        if (const char* v = EnvVar2("INTERP_GUIDE", "WARP_GUIDE")) EnvNumberValue("INTERP_GUIDE", v, 0.0, 1.0, c.interp_guide);
        EnvNumber("CONF_LO", 0.0, 10.0, c.conf_lo);
        EnvNumber("CONF_HI", 0.0, 10.0, c.conf_hi);
        EnvNumber("PAN_LO", 0.0, 1.0, c.pan_lo);
        EnvNumber("PAN_HI", 0.0, 1.0, c.pan_hi);
        EnvNumber("UI_MASK_THRESHOLD", 0.0, 0.5, c.ui_mask);
        EnvFlag("CAP_EARLY", c.cap_early);
        EnvFlag("STATS_FRAMES", c.stats_frames);
        if (c.cap_early) c.cap_mode = Config::CapMode::Early;
        if (const char* v = EnvVar("CAP_MODE")) {
            if (c.cap_early && std::strcmp(v, "early")) FG_LOG("VKIFRAME_CAP_EARLY=1 and VKIFRAME_CAP_MODE=\"%s\" disagree: CAP_MODE wins", ShownValue(v).c_str());
            if (!std::strcmp(v, "late")) c.cap_mode = Config::CapMode::Late;
            else if (!std::strcmp(v, "early")) c.cap_mode = Config::CapMode::Early;
            else if (!std::strcmp(v, "hybrid")) c.cap_mode = Config::CapMode::Hybrid;
            else FG_LOG("VKIFRAME_CAP_MODE=\"%s\" is not late, early or hybrid: ignored", ShownValue(v).c_str());
        }
        EnvFlag("EXTRAPOLATE", c.extrapolate);
        if (const char* v = EnvVar2("INTERP_BIDIR", "WARP_BIDIR")) EnvFlagValue("INTERP_BIDIR", v, c.interp_bidir);
        if (const char* v = EnvVar2("INTERP_CAND", "WARP_CAND")) EnvNumberValue("INTERP_CAND", v, 0.0, 1.0, c.cand_thr);
        EnvInt("FLOW_BWD_SKIP", 0, 2, c.flow_bwd_skip);
        EnvFlag("FLOW_SUBPIXEL", c.flow_subpixel);
        if (const char* v = EnvVar("DUMP_DIR"))         { std::snprintf(c.dump_dir, sizeof c.dump_dir, "%s", v); }
        EnvInt("DUMP", 0, 1000000000, c.dump_pair);
        EnvInt("DUMP_FRAMES", 5, 60, c.dump_frames);
        if (const char* v = EnvVar3("OPTICAL_FLOW", "INTERPOLATE", "WARP")) EnvFlagValue("OPTICAL_FLOW", v, c.interpolate);
        EnvFlag("ONESIDED_FILL", c.onesided_fill);
        if (c.onesided_fill && LiveMaxSyntheticLimit() <= 4) { c.onesided_fill = false; FG_LOG("VKIFRAME_ONESIDED_FILL is experimental: it needs VKIFRAME_EXPERIMENTAL=1 as well; ignored"); }
        if (c.extrapolate) { c.interpolate = true; c.debug_flow = false; }
        if (c.debug_flow || c.interpolate) c.flow = true;
        EnvFlag("DEBUG_RING", c.debug_ring);
        EnvFlag("TIMING_TEST", c.timing_test);
        return c;
    }();
    return c;
}

// ---- Per-swapchain state ----
struct GpuState;
// A pointer that is written by one thread and read by others (#33): the same syntax as a plain pointer, loads and stores are atomic (acquire/release, so
// what the pointer leads to is visible to the thread that reads it).
template <class T> struct AtomicPtr {
    std::atomic<T*> p{nullptr};
    AtomicPtr& operator=(T* v) { p.store(v, std::memory_order_release); return *this; }
    operator T*() const { return p.load(std::memory_order_acquire); }
    T* operator->() const { return p.load(std::memory_order_acquire); }
    T& operator*() const { return *p.load(std::memory_order_acquire); }
};

struct SwapchainState : std::enable_shared_from_this<SwapchainState> {
    VkSwapchainKHR handle{};
    std::mutex flow_mu;                       // the hold/pair/flush logic of one swapchain (present hook, fence and present-wait hooks)
    std::atomic<uint64_t> last_present_ns{0}, acquire_wait_since_ns{0}, wait_present_since_ns{0}, wait_present_id{0};   // watchdog
    std::mutex mu;                            // guards rates and the timing state below (game thread and presenter thread both touch them)
    std::atomic<int> pending_error{0};        // first error of a presenter-thread present, returned to the game by its next present
    std::atomic<struct Presenter*> presenter{nullptr};   // Stage 3c-ii: thread that presents the planned ticks at their times; written by the game thread (flow_mu), read without it by the acquire hooks
    // present timing (Stage 2)
    bool     timing_enabled{false};
    VkPresentStageFlagsEXT stage{0};          // the one present stage we query and target
    uint64_t time_domain_id{0};
    VkTimeDomainKHR time_domain{VK_TIME_DOMAIN_CLOCK_MONOTONIC_KHR};
    bool     domain_monotonic{true};          // the chosen domain is CLOCK_MONOTONIC: no conversion needed
    bool     absolute_ok{false};              // the surface accepts absolute target times; if not, timing is feedback only
    bool     calib_ok{false};                 // calib_ok, domain_offset_ns and last_calib_ns: every access after the swapchain is created is under mu (PresentWithTiming)
    int64_t  domain_offset_ns{0};             // domain time - CLOCK_MONOTONIC, refreshed about once a second
    uint64_t last_calib_ns{0};
    uint64_t next_present_id{1};              // must increase, never 0
    uint32_t image_count{0};
    uint64_t refresh_ns{0};                   // refresh duration, 0 = unknown (with a variable-refresh display: the shortest refresh)
    bool     refresh_vrr{false};              // the display is variable-refresh (refreshInterval == UINT64_MAX): TARGET_FPS is not limited then
    float    clamp_logged_fps{0.0f};          // the limited target last written to the log, so the note appears once per change
    unsigned cost_over_windows{0};            // consecutive seconds in which the layer's GPU work was over its budget
    bool     cost_warned{false};              // the cost warning was written (once per swapchain)
    // timing and rate control
    TimingEngine timing;
    std::atomic<uint64_t> job_gen{0};         // VKIFRAME_EXTRAPOLATE: number of the newest presenter job; ticks of an older job are cancelled
    std::atomic<uint64_t> ex_planned{0}, ex_cancelled{0}, ex_late_sum_ns{0}, ex_late_max_ns{0}, ex_late_n{0};   // VKIFRAME_EXTRAPOLATE window counters: predicted ticks planned / not presented, lateness of tick 0
    PairScheduler sched;                      // Stage 3: gate, output clock, per-pair plan
    struct Rates { uint64_t synthetic{0}, shown{0}, native{0}, output{0}, bypass{0}, pair{0}, hold{0}, carrier{0}, dropped{0}, last_ns{0}; } rates;   // window counters
    uint64_t cap_next_slot_ns{0}, cap_return_ns{0};   // VKIFRAME_BASE_FPS_CAP: next slot of the grid, when the game was handed back to its thread
    double   cap_dev_ns{0.0};                 // hybrid cap: EWMA of the absolute deviation of the game's render time from cap_render_ema_ns
    double   cap_render_ema_ns{0.0};          // how long the game takes between our return and its next present
    std::atomic<uint64_t> cpu_hook_ns{0}, cpu_presenter_ns{0};   // CPU time (not waiting) of the game thread inside our present, and of the presenter thread (window totals)
    std::atomic<uint64_t> cap_wait_ns{0}, cap_hold_ns{0}, cap_n{0};   // base-rate cap (window totals): the sleep after a present (late wait), the sleep before it (early hold), presents with the cap on
    std::atomic<uint64_t> wait_acquire_ns{0}, wait_present_ns{0}, native_cnt{0};   // game-thread waits (window totals, see LogRates)
    stats::FrameTimes stat_ft;                // VKIFRAME_STATS: present-to-present intervals of this second (only filled while StatsOn())
    uint64_t stat_prev_ns{0};
    // frame holding (Stage 3b)
    bool     hold_candidate{false};           // surface, format, usage and image count allow it
    int      synth_capacity{10};              // most synthetic frames per native frame this swapchain can hold (images granted); k_max never exceeds it
    bool     synth_failed{false};             // creating a synthetic frame failed: held frames continue without synthetic ones
    bool     skip_logged{false};              // "frame holding skipped for this present chain" was logged once
    std::atomic<VkQueue> last_queue{};        // the queue the game presents on (for flushing a held frame from vkWaitForPresentKHR); written by the game thread, read by the presenter thread's idle flush
    bool     hold_failed{false};              // a GPU step failed: back to passthrough for this swapchain
    VkExtent2D extent{};
    VkFormat   format{VK_FORMAT_UNDEFINED};
    AtomicPtr<GpuState> gpu;                  // created at the first present (needs the queue family); written by the game thread, read by the presenter thread's idle flush
    uint64_t frames{0};
    uint64_t last_log_ns{0};
    // statistics, windowed over the log interval except for the totals
    struct Stats {
        uint64_t results_total{0}, queue_full_total{0};
        uint64_t n_err{0};  double sum_err_ms{0}, max_abs_err_ms{0};
        uint64_t n_int{0};  double sum_int_ms{0}, sum_int2_ms{0};
        uint64_t last_actual_ns{0};
    } st;
};

// ---- Hooks (layer_main.cpp) ----
VKAPI_ATTR VkResult VKAPI_CALL Hook_CreateInstance(const VkInstanceCreateInfo*, const VkAllocationCallbacks*, VkInstance*);
VKAPI_ATTR void     VKAPI_CALL Hook_DestroyInstance(VkInstance, const VkAllocationCallbacks*);
VKAPI_ATTR VkResult VKAPI_CALL Hook_CreateDevice(VkPhysicalDevice, const VkDeviceCreateInfo*, const VkAllocationCallbacks*, VkDevice*);
VKAPI_ATTR void     VKAPI_CALL Hook_DestroyDevice(VkDevice, const VkAllocationCallbacks*);
VKAPI_ATTR VkResult VKAPI_CALL Hook_CreateSwapchainKHR(VkDevice, const VkSwapchainCreateInfoKHR*, const VkAllocationCallbacks*, VkSwapchainKHR*);
VKAPI_ATTR void     VKAPI_CALL Hook_DestroySwapchainKHR(VkDevice, VkSwapchainKHR, const VkAllocationCallbacks*);
VKAPI_ATTR VkResult VKAPI_CALL Hook_QueuePresentKHR(VkQueue, const VkPresentInfoKHR*);
VKAPI_ATTR VkResult VKAPI_CALL Hook_QueueSubmit(VkQueue, uint32_t, const VkSubmitInfo*, VkFence);
VKAPI_ATTR VkResult VKAPI_CALL Hook_QueueSubmit2(VkQueue, uint32_t, const VkSubmitInfo2*, VkFence);
VKAPI_ATTR VkResult VKAPI_CALL Hook_QueueSubmit2KHR(VkQueue, uint32_t, const VkSubmitInfo2*, VkFence);
VKAPI_ATTR VkResult VKAPI_CALL Hook_QueueBindSparse(VkQueue, uint32_t, const VkBindSparseInfo*, VkFence);
VKAPI_ATTR VkResult VKAPI_CALL Hook_QueueWaitIdle(VkQueue);
VKAPI_ATTR VkResult VKAPI_CALL Hook_DeviceWaitIdle(VkDevice);
VKAPI_ATTR VkResult VKAPI_CALL Hook_AcquireNextImageKHR(VkDevice, VkSwapchainKHR, uint64_t, VkSemaphore, VkFence, uint32_t*);
VKAPI_ATTR VkResult VKAPI_CALL Hook_WaitForFences(VkDevice, uint32_t, const VkFence*, VkBool32, uint64_t);
VKAPI_ATTR VkResult VKAPI_CALL Hook_GetFenceStatus(VkDevice, VkFence);
VKAPI_ATTR VkResult VKAPI_CALL Hook_WaitForPresentKHR(VkDevice, VkSwapchainKHR, uint64_t, uint64_t);
VKAPI_ATTR VkResult VKAPI_CALL Hook_AcquireNextImage2KHR(VkDevice, const VkAcquireNextImageInfoKHR*, uint32_t*);
VKAPI_ATTR void     VKAPI_CALL Hook_GetDeviceQueue(VkDevice, uint32_t, uint32_t, VkQueue*);
VKAPI_ATTR void     VKAPI_CALL Hook_GetDeviceQueue2(VkDevice, const VkDeviceQueueInfo2*, VkQueue*);

// ---- Exported entry points ----
FRAMEGEN_EXPORT VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL FrameGen_GetInstanceProcAddr(VkInstance, const char*);
FRAMEGEN_EXPORT VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL FrameGen_GetDeviceProcAddr(VkDevice, const char*);
FRAMEGEN_EXPORT VKAPI_ATTR VkResult VKAPI_CALL FrameGen_NegotiateLoaderLayerInterfaceVersion(VkNegotiateLayerInterface*);
