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
