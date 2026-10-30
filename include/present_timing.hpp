#pragma once

#include "vk_iframes_layer.hpp"

// Storage that must outlive the vkQueuePresentKHR call it is chained into.
struct TimingChain {
    VkPresentId2KHR         id2{VK_STRUCTURE_TYPE_PRESENT_ID_2_KHR};
    VkPresentTimingsInfoEXT timings{VK_STRUCTURE_TYPE_PRESENT_TIMINGS_INFO_EXT};
    VkPresentTimingInfoEXT  info{};          // one swapchain
    uint64_t                present_id{0};
};

// Picks the present stage, time domain and result queue for a freshly created swapchain that has the
// present-timing flags. Returns false (and leaves sc.timing_enabled false) if anything needed is missing.
// `stage_mask` is VkPresentTimingSurfaceCapabilitiesEXT::presentStageQueries for the surface.
bool SetupSwapchainTiming(const DeviceDispatch& dt, SwapchainState& sc, VkPresentStageFlagsEXT stage_mask, uint32_t queue_size);

// CLOCK_MONOTONIC nanoseconds -> the swapchain's time domain. Returns 0 ("as soon as possible") if the domain
// cannot be converted.
uint64_t ToDomainTime(const DeviceDispatch& dt, SwapchainState& sc, uint64_t mono_ns);

// Fills `c` and returns the structure to put at the head of VkPresentInfoKHR::pNext.
// target_time 0 means "as soon as possible"; app_pnext is the application's own chain, appended behind ours.
const void* BuildTimingChain(TimingChain& c, SwapchainState& sc, uint64_t target_time, const void* app_pnext);

// Reads back finished presentation timings (vkGetPastPresentationTimingEXT) into the statistics.
void DrainPastTimings(const DeviceDispatch& dt, SwapchainState& sc);

// Logs the windowed statistics about once a second.
void LogTimingStats(const DeviceDispatch& dt, SwapchainState& sc, uint64_t now_ns);
