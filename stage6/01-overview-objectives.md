[index](../STAGE6.md) | [next](02-packaging-as-an-implicit-layer.md)

# Stage 6: Integration, Optimization & Edge Case Handling

> **Status: in progress.** This page keeps the first Stage 6 design and brings it up to the code on `main`. Done: runtime configuration (environment, `live.conf`, the Decky panel, stats and a benchmark), the implicit-layer manifest and the install scripts, and use in several Proton games on the Steam Deck. Open: swapchain recreation and resizing (never exercised; the sketch in §2 is a plan, not the code), the performance budget (not met on the Deck), hiding `VK_EXT_present_timing` from games, the compositor test matrix, and the Deck-specific flow setting. ROADMAP.md has the checklist and STATUS.md the measurements. Settings use the `VKIFRAME_*` names; the old `FRAMEGEN_*` names are still read as a fallback.

## 1. Overview & Objectives

Stage 6 makes the layer fit for daily use across games, compositors and hardware. The layer will:

1. Survive swapchain recreation, window resizing and fullscreen toggles, and forward `VK_ERROR_OUT_OF_DATE_KHR` / `VK_SUBOPTIMAL_KHR` correctly while a frame is held.
2. Meet a combined GPU budget for flow, masking and interpolation.
3. Work on Wayland (KWin, Hyprland, Mutter), X11 and gamescope, on fixed-refresh and VRR displays.
4. Be configurable at runtime, installable as an **implicit layer**, and not conflict with games that use the same presentation extensions.

---

## 2. Swapchain Recreation and Resizing

Dynamic resolution scaling (DRS) normally changes the *render* resolution inside a fixed-size swapchain, so the layer sees nothing and keeps running. Window resizes, fullscreen toggles and monitor changes recreate the swapchain, and with hold-one presentation a frame may be held (unpresented) at that moment.

### A. Creation, with `oldSwapchain`

**What the code does today.** `Hook_CreateSwapchainKHR` (`src/layer_main.cpp`) does not look at `oldSwapchain`. It applies the layer's changes to a copy of the create info (image count, transfer usage, present timing flags, forced `FIFO` where the layer uses present timing), creates the swapchain, and registers a fresh `SwapchainState` for the new handle. The GPU state (ring, flow and interpolation images) is created lazily at the first present of that swapchain. `Hook_DestroySwapchainKHR` stops the presenter thread, drops the held frame (it is not presented: its image goes away with the swapchain), destroys the GPU state, removes the swapchain's state and then calls down. A resize therefore ends one swapchain's life and starts another's; what has not been tested is the order the game uses (create the new one with `oldSwapchain` before destroying the old one, so both exist for a moment) under fullscreen toggles, monitor changes and Alt-Tab. ROADMAP.md Stage 6 lists this as open.

The first design's sketch is kept as the plan to check the code against:

```cpp
VKAPI_ATTR VkResult VKAPI_CALL Hook_CreateSwapchainKHR(VkDevice device, const VkSwapchainCreateInfoKHR* pCreateInfo,
                                                       const VkAllocationCallbacks* pAllocator, VkSwapchainKHR* pSwapchain) {
    DeviceDispatch* dt = LookupDevice(GetKey(device));

    // The retiring swapchain's held frame is dropped (its images are going away). Make sure no layer work still
    // references the old ring, flow and interpolation resources before anything is recreated.
    if (pCreateInfo->oldSwapchain != VK_NULL_HANDLE) {
        if (SwapchainState* old = FindSwapchain(pCreateInfo->oldSwapchain)) {
            dt->DeviceWaitIdle(device);                  // resizes are rare; simplicity beats a finer wait here
            old->held.valid = false;                     // not presented: the image belongs to the retired chain
        }
    }
    ...
}
```

In the code the equivalent of "no layer work references the old resources" is `StopPresenter` plus `GpuDestroy` (which waits for the device to be idle) in the destroy hook, and each swapchain owns its own `GpuState`, so the new swapchain never shares resources with the old one.

### B. Forwarding present results

Hidden acquires and the extra presents can fail independently of the game's own frame. The worst result is returned to the game so that it recreates the swapchain when needed. In the code the presenter thread notes the first error in `SwapchainState::pending_error` (`NotePresentError`), and the next `vkQueuePresentKHR` of the game returns it (`VK_ERROR_OUT_OF_DATE_KHR` and `VK_ERROR_SURFACE_LOST_KHR` from a hidden acquire stop the pair; any other failure of a synthetic frame switches synthetic frames off for that swapchain and never fails the game). The first design's helper, kept as the rule:

```cpp
static int Rank(VkResult r) { return r < 0 ? 2 : (r == VK_SUBOPTIMAL_KHR ? 1 : 0); }
static VkResult Worst(VkResult a, VkResult b) { return Rank(b) > Rank(a) ? b : a; }
```

---

## 3. Asynchronous Timeline-Semaphore Synchronization (not built)

**Not built.** Flow and interpolation run on the game's present queue, ordered by memory barriers inside the command buffers and by fences and binary semaphores between submits; `timelineSemaphore` is not requested (STAGE3.md §4.A, STAGE4.md §3). The asynchronous queue is item 9 of AUDIT.md and last in the order of work, only if the measurements show it is worth it (ASYNC.md: the layer's cost is serial GPU time, so a second queue does not add capacity). The design below is kept for that day.

Flow would run on the compute queue while the game renders its next frame. Cross-queue ordering uses timeline semaphores and `vkQueueSubmit2`, which requires the `timelineSemaphore` and `synchronization2` features (the second is already enabled by Stage 3).

```cpp
// Submit a command buffer that waits on one timeline and signals another (possibly on a different queue).
VkResult SubmitWithTimelines(const DeviceDispatch& dt, VkQueue queue, VkCommandBuffer cmd,
                             VkSemaphore wait_sem, uint64_t wait_value,
                             VkSemaphore signal_sem, uint64_t signal_value) {
    VkSemaphoreSubmitInfo wait{VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO};
    wait.semaphore = wait_sem;     wait.value = wait_value;     wait.stageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;

    VkSemaphoreSubmitInfo signal{VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO};
    signal.semaphore = signal_sem; signal.value = signal_value; signal.stageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;

    VkCommandBufferSubmitInfo cb{VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO};
    cb.commandBuffer = cmd;

    VkSubmitInfo2 si{VK_STRUCTURE_TYPE_SUBMIT_INFO_2};
    si.waitSemaphoreInfoCount   = 1; si.pWaitSemaphoreInfos   = &wait;
    si.commandBufferInfoCount   = 1; si.pCommandBufferInfos   = &cb;
    si.signalSemaphoreInfoCount = 1; si.pSignalSemaphoreInfos = &signal;
    return dt.QueueSubmit2(queue, 1, &si, VK_NULL_HANDLE);
}
```

Value bookkeeping: the graphics queue signals the *capture timeline*, the compute queue signals the *flow timeline*, and each value only ever increases. Command buffers and per-slot resources are reused only after the CPU has waited (`vkWaitSemaphores`) for the value recorded when they were last submitted. ASYNC.md has the fuller plan, including the write-after-read ordering between a pair's synthesis and the next pair's flow.

---

## 4. Configuration

Runtime configuration through environment variables (read once per process), plus the live settings. README.md has the complete tables with defaults; this page keeps the Stage 6 view. All of them are read by `Config` in `include/vk_iframes_layer.hpp` through `EnvVar("NAME")`, which tries `VKIFRAME_NAME`, then `FRAMEGEN_NAME` (for the five startup-only keys `EXTRAPOLATE`, `UI_MASK_THRESHOLD`, `FLOW_LEVEL`, `INTERPOLATE` and `EXPERIMENTAL`, `live.conf` is looked at first and wins over the environment).

| Variable | Default | Description |
| :--- | :--- | :--- |
| `VKIFRAME_ENABLE` | unset | Turns the implicit layer on (`1`, loader setting). `VKIFRAME_DISABLE=1` forces it off. |
| `VKIFRAME_TARGET_FPS` | `60` (`90` on the Steam Deck OLED) | Output frame rate the layer targets exactly; at or above it the layer bypasses. Limited to the display's refresh rate unless the display is variable-refresh or the rate is unknown. Live. |
| `VKIFRAME_MIN_GAIN` | `1.01` | Interpolate only above this base-time / target-time ratio (the Stage 3 gate; the simulation was tuned with 1.12). Live. |
| `VKIFRAME_MAX_SYNTHETIC` | `3` | $k_{\max}$: most synthetic frames per native frame (1 to 4; up to 10 with `VKIFRAME_EXPERIMENTAL=1`). Live. |
| `VKIFRAME_BASE_FPS_CAP` | `45` (`0` = off) | The layer limits the game's present rate itself (Game Mode). Live. |
| `VKIFRAME_INTERPOLATE` | `0` | `1`: build synthetic frames along the optical flow (implies `VKIFRAME_FLOW`); without it they are plain cross-fades. |
| `VKIFRAME_UI_MASK_THRESHOLD` | `0.08` (`0` = off) | Static-UI mask: pixels whose 3x3 mean of \|A-B\| (luma) stays below this cross-fade instead of interpolating. `0.03` suits film-grain content. |
| `VKIFRAME_PAN_LO` / `VKIFRAME_PAN_HI` | `0.15` / `0.35` | The whole-frame pan fallback to the cross-fade (replaces the first design's `VKIFRAME_MAX_VELOCITY_FALLBACK`, which was never built). |
| `VKIFRAME_DEBUG_FLOW`, `VKIFRAME_TICKMARK` | `0` | Debug views: the motion field instead of the frames; frame-position marks. The first design's `VKIFRAME_DEBUG_OVERLAY` (a flow thumbnail in the corner) was not built. |
| `VKIFRAME_DUMP`, `VKIFRAME_DUMP_DIR` | off | Write a capture of one pair (the first design's `VKIFRAME_DUMP_FLOW`). |
| `VKIFRAME_STATS`, live keys, `VKIFRAME_LIVE_CONF`, `VKIFRAME_LOG_FILE` | | Measurement and live settings: `decky/LIVECONF.md`, README.md. |

The runtime file settings (`~/.config/vkiframes/live.conf`, read about once a second) and the Decky panel are the part of Stage 6 that the first design did not have. The first design's `Config` struct (with `max_velocity_fallback`, `ui_mask_threshold` and `debug_overlay`) is replaced by the one in `include/vk_iframes_layer.hpp`.

---

