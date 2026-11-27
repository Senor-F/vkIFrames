[index](../STAGE2.md) | [previous](02-device-creation-enable-what-the-driver-s.md)

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
