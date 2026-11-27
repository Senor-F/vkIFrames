[index](../STAGE2.md) | [next](02-device-creation-enable-what-the-driver-s.md)

# Stage 2: Precision Timing Engine (`VK_EXT_present_timing`)

> **Implementation note.** The shipped code differs from the sketches below in these ways: the work is split into `include/timing_engine.hpp` (EMA, jitter, clock helpers), `src/present_timing.cpp` (swapchain setup, time-domain choice and calibration, present chain, result draining, statistics) and the hooks in `src/layer_main.cpp`. Instance level: `VK_KHR_get_surface_capabilities2` is added when the app uses `VK_KHR_surface` on Vulkan ≥ 1.1. Device level: the three extensions and the `presentTiming`, `presentAtAbsoluteTime` and `presentId2` features are enabled only when all are offered and the app has not touched them itself. Swapchain level: the surface is queried first (`VkPresentTimingSurfaceCapabilitiesEXT`, `VkSurfaceCapabilitiesPresentId2KHR`); on failure creation is retried without the flags and with the app's own present mode. A present that already carries a present id (or timing info) from the app is passed through untouched. A full result queue triggers one drain-and-retry, then a plain present. A surface that supports timing but not absolute targets (seen on the custom machine's Wayland session) is used for feedback only: targets stay 0 and pacing stays vblank-driven. Tests: `tests/test_timing.cpp` (mock driver, no GPU; `ctest` name `mock_timing`). The first repository's `scripts/test_stage2.sh` (real app, three runs, report file) was not carried over; the three runs below are done by hand. Since then: a game that uses present timing or present id 2 itself is detected and the layer then does not use present timing on that device; the base-rate cap `VKIFRAME_BASE_FPS_CAP` is the layer's own (default 45), which is what the 30 FPS run uses; and the timing results of the desktop Wayland session and the Deck are in TARGETS.md §7 (feedback only, no absolute target times, so pacing is vblank-driven).

## 1. Overview & Objectives

Stage 2 gives the layer control over presentation timing. It will:

1. Detect `VK_EXT_present_timing` and `VK_KHR_present_id2` at device creation and enable them (with their feature structs) only if the driver supports them.
2. Create swapchains with the matching flags, force `FIFO` (target times apply to FIFO present modes only), and size the timing results queue.
3. Measure the base frame delta $\Delta T_{\text{base}}$ at every `vkQueuePresentKHR` using `CLOCK_MONOTONIC`, and smooth it with an EMA.
4. Chain `VkPresentId2KHR` and `VkPresentTimingsInfoEXT` into presents, and read the actual presentation times back to validate scheduling.
5. Provide a **vblank-paced fallback flag** for drivers without the extension (DESIGN §4.6).

> **API check.** Structure, enum and function names below were checked against Vulkan-Headers `cde2207` (October 2026, `VK_EXT_present_timing` spec version 3). The extension is new, so confirm against your own headers: notable details are `VkPresentTimingInfoEXT::flags` and `targetTimeDomainPresentStage`, the feature fields `presentTiming` / `presentAtAbsoluteTime` / `presentAtRelativeTime`, and the swapchain-local time domains `VK_TIME_DOMAIN_PRESENT_STAGE_LOCAL_EXT` / `VK_TIME_DOMAIN_SWAPCHAIN_LOCAL_EXT`. Support is also reported per surface (`VkPresentTimingSurfaceCapabilitiesEXT`, `VkSurfaceCapabilitiesPresentId2KHR`); the retry-without-flags fallback of §4.B covers surfaces that refuse.

Key rules from the specification that the code below follows:

* Present timing info is a **`VkPresentTimingsInfoEXT`** (plural) holding an array of per-swapchain **`VkPresentTimingInfoEXT`** in the `VkPresentInfoKHR` `pNext` chain. The singular struct cannot be chained directly.
* Present IDs come from **`VK_KHR_present_id2`** (`VkPresentId2KHR`, swapchain flag `VK_SWAPCHAIN_CREATE_PRESENT_ID_2_BIT_KHR`). Non-zero IDs must increase per swapchain. The older `VkPresentIdKHR` does not apply.
* The swapchain needs `VK_SWAPCHAIN_CREATE_PRESENT_TIMING_BIT_EXT`, then `vkSetSwapchainPresentTimingQueueSizeEXT` to allocate the results queue.
* If results are requested (`presentStageQueries != 0`) and the queue is full, `vkQueuePresentKHR` fails with `VK_ERROR_PRESENT_TIMING_QUEUE_FULL_EXT`; drain it with `vkGetPastPresentationTimingEXT`.
* `targetTime` is in nanoseconds in the time domain named by `timeDomainId`; `0` means "as soon as possible".

---

## 2. Repository Changes

```text
include/
├── vk_iframes_layer.hpp     # + extension function pointers, per-swapchain state
└── timing_engine.hpp        # new: EMA filter
src/
└── layer_main.cpp           # CreateDevice / CreateSwapchain / QueuePresent updated
```

New entries for the X-macro in `vk_iframes_layer.hpp`:

```cpp
// FRAMEGEN_INSTANCE_FUNCS: + X(GetPhysicalDeviceFeatures2)
// FRAMEGEN_DEVICE_FUNCS:
//   + X(SetSwapchainPresentTimingQueueSizeEXT)
//   + X(GetSwapchainTimeDomainPropertiesEXT)
//   + X(GetSwapchainTimingPropertiesEXT)
//   + X(GetPastPresentationTimingEXT)
//   + X(GetCalibratedTimestampsKHR)

struct SwapchainState {
    bool     timing_enabled{false};
    uint64_t time_domain_id{0};       // chosen domain (see §4.C)
    uint64_t next_present_id{1};      // must increase, never 0
    uint32_t image_count{0};
};
```

---

## 3. Timing Engine

### A. EMA Frame-Time Filter (`include/timing_engine.hpp`)

```cpp
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <time.h>

inline uint64_t NowNs() {                       // CLOCK_MONOTONIC, same clock as std::chrono::steady_clock on Linux
    timespec ts{};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return uint64_t(ts.tv_sec) * 1'000'000'000ull + uint64_t(ts.tv_nsec);
}

class TimingEngine {
public:
    explicit TimingEngine(float alpha = 0.15f) : m_alpha(alpha) {}

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
        const float clamped = float(std::clamp<uint64_t>(raw, 4'166'666, 100'000'000));

        if (!m_seeded) {
            m_smoothed_ns = clamped;                  // seed from the first real interval: no 60 FPS assumption to converge away from
            m_seeded = true;
        } else {
            // Jitter estimate (slow EMA of |sample - average|) feeds the output clock's jitter buffer (STAGE3 §4.E).
            m_jitter_ns = 0.05f * std::fabs(clamped - m_smoothed_ns) + 0.95f * m_jitter_ns;
            // EMA = alpha * current + (1 - alpha) * previous
            m_smoothed_ns = m_alpha * clamped + (1.0f - m_alpha) * m_smoothed_ns;
        }
        return uint64_t(m_smoothed_ns);
    }

    uint64_t JitterNs()   const { return uint64_t(m_jitter_ns); }
    uint64_t SmoothedNs() const { return uint64_t(m_smoothed_ns); }
    uint64_t LastRawNs()  const { return m_last_raw_ns; }          // un-clamped, used for the 100 ms flush rule
    float    SmoothedFps() const { return 1e9f / m_smoothed_ns; }

private:
    float    m_alpha;
    bool     m_first{true};
    uint64_t m_last_ns{0};
    uint64_t m_last_raw_ns{0};
    bool     m_seeded{false};
    float    m_jitter_ns{0.0f};
    float    m_smoothed_ns{16'666'667.0f};
};
```

---

## 4. Device and Swapchain Changes (`src/layer_main.cpp`)

