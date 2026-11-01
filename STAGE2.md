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

### A. Device creation: enable what the driver supports

The layer cannot assume the extensions exist. It checks the physical device, queries the feature bits, and enables only what is supported. Structures the application already put in the `pNext` chain are never duplicated.

```cpp
static bool DeviceHasExtension(InstanceDispatch& inst, VkPhysicalDevice pd, const char* name) {
    uint32_t n = 0;
    inst.EnumerateDeviceExtensionProperties(pd, nullptr, &n, nullptr);
    std::vector<VkExtensionProperties> props(n);
    inst.EnumerateDeviceExtensionProperties(pd, nullptr, &n, props.data());
    return std::any_of(props.begin(), props.end(), [&](auto& p) { return !strcmp(p.extensionName, name); });
}

static bool ChainHas(const void* pNext, VkStructureType type) {
    for (auto* s = static_cast<const VkBaseInStructure*>(pNext); s; s = s->pNext)
        if (s->sType == type) return true;
    return false;
}

VKAPI_ATTR VkResult VKAPI_CALL Hook_CreateDevice(VkPhysicalDevice pd, const VkDeviceCreateInfo* pCreateInfo,
                                                 const VkAllocationCallbacks* pAllocator, VkDevice* pDevice) {
    InstanceDispatch* inst = LookupInstance(GetKey(pd));
    auto* link = FindDeviceLink(pCreateInfo);
    PFN_vkGetDeviceProcAddr gdpa = link->u.pLayerInfo->pfnNextGetDeviceProcAddr;
    link->u.pLayerInfo = link->u.pLayerInfo->pNext;

    VkDeviceCreateInfo ci = *pCreateInfo;
    std::vector<const char*> exts(ci.ppEnabledExtensionNames, ci.ppEnabledExtensionNames + ci.enabledExtensionCount);

    // Query feature support for both extensions.
    VkPhysicalDevicePresentTimingFeaturesEXT supTiming{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRESENT_TIMING_FEATURES_EXT};
    VkPhysicalDevicePresentId2FeaturesKHR    supId2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRESENT_ID_2_FEATURES_KHR};
    supTiming.pNext = &supId2;
    VkPhysicalDeviceFeatures2 q{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, &supTiming};
    const bool have_exts = DeviceHasExtension(*inst, pd, VK_EXT_PRESENT_TIMING_EXTENSION_NAME) &&
                           DeviceHasExtension(*inst, pd, VK_KHR_PRESENT_ID_2_EXTENSION_NAME);
    if (have_exts) inst->GetPhysicalDeviceFeatures2(pd, &q);
    const bool timing_ok = have_exts && supTiming.presentTiming && supTiming.presentAtAbsoluteTime && supId2.presentId2
                           && !ChainHas(ci.pNext, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRESENT_TIMING_FEATURES_EXT);

    VkPhysicalDevicePresentTimingFeaturesEXT enTiming = supTiming;   // enable exactly what is supported
    VkPhysicalDevicePresentId2FeaturesKHR    enId2    = supId2;
    if (timing_ok) {
        enId2.pNext    = const_cast<void*>(ci.pNext);                // prepend to the app's chain
        enTiming.pNext = &enId2;
        ci.pNext       = &enTiming;
        for (const char* e : {VK_EXT_PRESENT_TIMING_EXTENSION_NAME, VK_KHR_PRESENT_ID_2_EXTENSION_NAME})
            if (std::none_of(exts.begin(), exts.end(), [&](auto* x) { return !strcmp(x, e); })) exts.push_back(e);
        ci.enabledExtensionCount   = uint32_t(exts.size());
        ci.ppEnabledExtensionNames = exts.data();
    }

    VkResult r = inst->CreateDevice(pd, &ci, pAllocator, pDevice);
    if (r != VK_SUCCESS) return r;

    DeviceDispatch* dt = RegisterDevice(GetKey(*pDevice), *pDevice, pd, gdpa, inst);
    dt->timing_supported = timing_ok;                               // new bool member of DeviceDispatch
    FG_LOG("present timing %s", timing_ok ? "ENABLED" : "unavailable (vblank-paced fallback)");
    return VK_SUCCESS;
}
```

(If the application itself already requests present timing, the layer leaves its chain alone and treats timing as unavailable; Stage 6 hides the extension from applications to avoid that case.)

### B. Swapchain creation

```cpp
VKAPI_ATTR VkResult VKAPI_CALL Hook_CreateSwapchainKHR(VkDevice device, const VkSwapchainCreateInfoKHR* pCreateInfo,
                                                       const VkAllocationCallbacks* pAllocator, VkSwapchainKHR* pSwapchain) {
    DeviceDispatch* dt = LookupDevice(GetKey(device));
    VkSwapchainCreateInfoKHR ci = *pCreateInfo;

    // ... minImageCount handling from Stage 1 ...

    if (dt->timing_supported) {
        ci.flags |= VK_SWAPCHAIN_CREATE_PRESENT_TIMING_BIT_EXT | VK_SWAPCHAIN_CREATE_PRESENT_ID_2_BIT_KHR;
        if (ci.presentMode != VK_PRESENT_MODE_FIFO_KHR) {
            FG_LOG("forcing FIFO present mode (timing needs FIFO)");
            ci.presentMode = VK_PRESENT_MODE_FIFO_KHR;
        }
    }

    VkResult r = dt->CreateSwapchainKHR(device, &ci, pAllocator, pSwapchain);
    if (r == VK_SUCCESS && dt->timing_supported) {
        auto& sc = RegisterSwapchain(*pSwapchain);                 // per-swapchain state, keyed by handle
        sc.image_count = ci.minImageCount;
        dt->SetSwapchainPresentTimingQueueSizeEXT(device, *pSwapchain, ci.minImageCount * 4);
        sc.time_domain_id = PickTimeDomain(dt, device, *pSwapchain);
        sc.timing_enabled = true;
    }
    return r;
}
```

> A surface may refuse `FIFO`-only or the present-id2 swapchain flag. `FIFO` is mandatory for every surface, but if `vkCreateSwapchainKHR` fails with the extra flags, retry once without them and mark the device `timing_supported = false`.

### C. Choosing a time domain

Targets are expressed in a *time domain* identified by an ID. The layer needs one that it can convert from `CLOCK_MONOTONIC`:

```cpp
// Pseudocode of PickTimeDomain():
// 1. vkGetSwapchainTimeDomainPropertiesEXT(device, sc, &props, &counter) twice (count, then fill).
// 2. If a reported domain is VK_TIME_DOMAIN_CLOCK_MONOTONIC_KHR, use its timeDomainId directly:
//        target_ns = monotonic_ns
// 3. Otherwise use VK_TIME_DOMAIN_SWAPCHAIN_LOCAL_EXT (or PRESENT_STAGE_LOCAL_EXT, which every
//    implementation must support) and keep a calibration pair, refreshed about once a second:
//        vkGetCalibratedTimestampsKHR(...) with VkSwapchainCalibratedTimestampInfoEXT
//        offset = domain_ns - monotonic_ns
//    then target_ns = monotonic_ns + offset.
```

### D. Presenting with timing information

This version of `Hook_QueuePresentKHR` measures the base rate and attaches timing data. Stage 3 reuses `BuildTimingChain` for every output frame.

```cpp
struct TimingChain {                       // storage that must outlive the vkQueuePresentKHR call
    VkPresentId2KHR        id2{VK_STRUCTURE_TYPE_PRESENT_ID_2_KHR};
    VkPresentTimingsInfoEXT timings{VK_STRUCTURE_TYPE_PRESENT_TIMINGS_INFO_EXT};
    VkPresentTimingInfoEXT info;           // one swapchain
    uint64_t               present_id;
};

static void BuildTimingChain(TimingChain& c, SwapchainState& sc, uint64_t target_time /*0 = ASAP*/,
                             const void* app_pnext) {
    c.present_id = sc.next_present_id++;
    c.id2 = {VK_STRUCTURE_TYPE_PRESENT_ID_2_KHR, const_cast<void*>(app_pnext), 1, &c.present_id};

    c.info = {};
    c.info.sType = VK_STRUCTURE_TYPE_PRESENT_TIMING_INFO_EXT;
    c.info.targetTime = target_time;
    c.info.timeDomainId = sc.time_domain_id;
    c.info.presentStageQueries = VK_PRESENT_STAGE_IMAGE_FIRST_PIXEL_VISIBLE_BIT_EXT;   // report when it hit the screen
    c.info.targetTimeDomainPresentStage = VK_PRESENT_STAGE_IMAGE_FIRST_PIXEL_VISIBLE_BIT_EXT;  // the stage targetTime refers to (required when targetTime != 0)
    c.info.flags = 0;                                                                  // absolute time; RELATIVE_TIME / NEAREST_REFRESH_CYCLE bits are not used

    c.timings = {VK_STRUCTURE_TYPE_PRESENT_TIMINGS_INFO_EXT, &c.id2, 1, &c.info};
}

VKAPI_ATTR VkResult VKAPI_CALL Hook_QueuePresentKHR(VkQueue queue, const VkPresentInfoKHR* pPresentInfo) {
    DeviceDispatch* dt = LookupDevice(GetKey(queue));
    static TimingEngine engine(0.15f);
    static uint64_t frame = 0;

    const uint64_t now_ns = NowNs();
    const uint64_t smoothed_ns = engine.Update(now_ns);

    VkPresentInfoKHR pi = *pPresentInfo;
    TimingChain chain;
    SwapchainState* sc = (pPresentInfo->swapchainCount == 1) ? FindSwapchain(pPresentInfo->pSwapchains[0]) : nullptr;
    if (sc && sc->timing_enabled && !ChainHas(pi.pNext, VK_STRUCTURE_TYPE_PRESENT_ID_2_KHR)) {
        // Test schedule: ask for exactly two smoothed frame times from now.
        BuildTimingChain(chain, *sc, ToDomainTime(*sc, now_ns + 2 * smoothed_ns), pPresentInfo->pNext);
        pi.pNext = &chain.timings;
    }

    VkResult r = dt->QueuePresentKHR(queue, &pi);

    if (sc && sc->timing_enabled) DrainPastTimings(dt, *sc);       // vkGetPastPresentationTimingEXT, keeps the queue from filling

    if (++frame % 120 == 0)
        FG_LOG("base FPS (smoothed) %.2f | delta %.2f ms", engine.SmoothedFps(), smoothed_ns / 1e6);
    return r;
}
```

`DrainPastTimings` calls `vkGetPastPresentationTimingEXT` with `VkPastPresentationTimingInfoEXT{swapchain}` in a loop until it returns no more entries, and logs `actual − target` for the entries it finds. A systematic offset is fine; a growing one is not.

---

## 5. Verification & Testing Procedure

### Quick path
The first repository's script `scripts/test_stage2.sh` (build, mock tests, three `vkcube` runs, a report file) was not carried over. The mock tests are `ctest --test-dir build` (`mock_timing`); the three runs are done by hand with the steps below: run 1 is plain, run 2 aims every present two frames ahead (`VKIFRAME_TIMING_TEST=1`) and reports the error against the request, run 3 caps the base rate at 30 (`VKIFRAME_BASE_FPS_CAP=30`) and checks that the smoothed FPS converges. On SteamOS build in the container and run on the host.

### Step 1: Build
```bash
cmake --build build -j"$(nproc)"
```

### Step 2: Enable the layer
```bash
export VK_LAYER_PATH=$PWD/build
export VK_LOADER_LAYERS_ENABLE=VK_LAYER_CUSTOM_frame_gen
```

### Step 3: Run an app at a known base rate
`DXVK_FRAME_RATE` only affects DXVK (Direct3D) games and does nothing for native Vulkan programs such as `vkcube`. Use a frame cap that the application sees *before* this layer:

```bash
# a DXVK / Proton game:      DXVK_FRAME_RATE=30 <game>
# anything else, via MangoHud's limiter (it must sit above this layer in the layer order):
MANGOHUD_CONFIG=fps_limit=30 mangohud vkcube
```
Or use the game's own frame limiter.

### Step 4: Success criteria
1. The device log reports `present timing ENABLED` (or `unavailable`, and the rest of this stage is skipped on that machine — confirm with `vulkaninfo | grep -i present_timing`).
2. Smoothed FPS converges to the cap:
   ```text
   [FrameGen] base FPS (smoothed) 30.01 | delta 33.32 ms
   ```
3. Measured `actual − target` presentation error stays within about one refresh interval and does not drift upward.
4. No `VK_ERROR_PRESENT_TIMING_QUEUE_FULL_EXT`, no stutter, and no validation errors (`VK_LAYER_KHRONOS_validation` enabled once).
