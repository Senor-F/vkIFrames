# Stage 6: Integration, Optimization & Edge Case Handling

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

    VkSwapchainCreateInfoKHR ci = *pCreateInfo;
    ApplyLayerSwapchainChanges(*dt, ci);                 // image count, usage, FIFO, timing flags (Stages 1-3)

    VkResult r = dt->CreateSwapchainKHR(device, &ci, pAllocator, pSwapchain);
    if (r != VK_SUCCESS) return r;

    SwapchainState& sc = RegisterSwapchain(*pSwapchain); // new state for the new handle
    sc.extent = ci.imageExtent;
    if (sc.active) {
        sc.ring.Initialize(*dt, ci.imageExtent, /*capacity=*/4, QueueFamilies(*dt));
        sc.synth.Initialize(*dt, ci.imageExtent, /*capacity=*/GetConfig().max_synthetic, QueueFamilies(*dt));
        sc.flow.RecreateForExtent(ci.imageExtent);
        sc.interpolation.RecreateForExtent(ci.imageExtent);
    }
    return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL Hook_DestroySwapchainKHR(VkDevice device, VkSwapchainKHR swapchain,
                                                    const VkAllocationCallbacks* pAllocator) {
    DeviceDispatch* dt = LookupDevice(GetKey(device));
    if (SwapchainState* sc = FindSwapchain(swapchain)) {
        dt->DeviceWaitIdle(device);
        // Present the held frame if the swapchain is still usable; otherwise drop it.
        sc->flow.Cleanup(); sc->interpolation.Cleanup(); sc->synth.Cleanup(); sc->ring.Cleanup();
        DestroySwapchainState(swapchain);
    }
    dt->DestroySwapchainKHR(device, swapchain, pAllocator);
}
```

### B. Forwarding present results

Hidden acquires and the extra presents can fail independently of the game's own frame. The worst result is returned to the game so that it recreates the swapchain when needed:

```cpp
static int Rank(VkResult r) { return r < 0 ? 2 : (r == VK_SUBOPTIMAL_KHR ? 1 : 0); }
static VkResult Worst(VkResult a, VkResult b) { return Rank(b) > Rank(a) ? b : a; }

// in Hook_QueuePresentKHR: result = Worst(result, PresentSynthetic(...));
// If a hidden vkAcquireNextImageKHR returns VK_ERROR_OUT_OF_DATE_KHR, stop generating frames for this pair,
// present nothing more, and return VK_ERROR_OUT_OF_DATE_KHR to the game.
```

---

## 3. Asynchronous Timeline-Semaphore Synchronization

Flow runs on the compute queue while the game renders its next frame. Cross-queue ordering uses timeline semaphores and `vkQueueSubmit2`, which requires the `timelineSemaphore` and `synchronization2` features that Stage 3 enables.

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

Value bookkeeping: the graphics queue signals the *capture timeline*, the compute queue signals the *flow timeline*, and each value only ever increases. Command buffers and per-slot resources are reused only after the CPU has waited (`vkWaitSemaphores`) for the value recorded when they were last submitted.

---

## 4. Configuration

Runtime configuration through environment variables (read once per process):

| Variable | Default | Description |
| :--- | :--- | :--- |
| `VKIFRAME_ENABLE` | unset | Turns the implicit layer on (`1`). `VKIFRAME_DISABLE=1` forces it off. |
| `VKIFRAME_TARGET_FPS` | `60` | Output frame rate the layer targets exactly; at or above it the layer bypasses. |
| `VKIFRAME_MIN_GAIN` | `1.12` | Bypass unless base frame time / target frame time exceeds this (Stage 3 gate). |
| `VKIFRAME_MAX_SYNTHETIC` | `3` | $k_{\max}$: most synthetic frames per native frame. Provisional default. |
| `VKIFRAME_MAX_VELOCITY_FALLBACK` | `32.0` | Mean flow (pixels) above which output ramps to 50/50 blending. |
| `VKIFRAME_UI_MASK_THRESHOLD` | `0` (off) | Static-UI mask: pixels whose 3x3 mean of \|A-B\| (luma) stays below this cross-fade instead of interpolating. `0.03` suits film-grain content. |
| `VKIFRAME_DEBUG_OVERLAY` | `0` | `1` blends a color-coded flow thumbnail into the corner of synthetic frames. Timing numbers stay in the log. |
| `VKIFRAME_DUMP_FLOW` | `0` | `1` writes the first few flow fields to `/tmp` (Stage 4 debug path). |

Extend the Stage 3 `Config` struct accordingly:

```cpp
struct Config {
    float target_fps{60.0f};
    float min_gain{1.12f};
    int   max_synthetic{3};
    float max_velocity_fallback{32.0f};
    float ui_mask_threshold{0.02f};
    bool  debug_overlay{false};
};
```

---

## 5. Packaging as an Implicit Layer

For daily use the layer is installed and enabled per game with one environment variable, instead of exporting `VK_LAYER_PATH` and `VK_LOADER_LAYERS_ENABLE`.

`~/.local/share/vulkan/implicit_layer.d/VkLayer_frame_gen.json`:

```json
{
  "file_format_version": "1.1.2",
  "layer": {
    "name": "VK_LAYER_CUSTOM_frame_gen",
    "type": "GLOBAL",
    "library_path": "/home/YOU/.local/lib/libvkIFrames.so",
    "api_version": "1.3.0",
    "implementation_version": "1",
    "description": "vkIFrames dynamic frame interpolation layer",
    "functions": {
      "vkNegotiateLoaderLayerInterfaceVersion": "FrameGen_NegotiateLoaderLayerInterfaceVersion",
      "vkGetInstanceProcAddr": "FrameGen_GetInstanceProcAddr",
      "vkGetDeviceProcAddr": "FrameGen_GetDeviceProcAddr"
    },
    "enable_environment":  { "VKIFRAME_ENABLE": "1" },
    "disable_environment": { "VKIFRAME_DISABLE": "1" }
  }
}
```

Steam launch option for a Proton game:

```text
VKIFRAME_ENABLE=1 VKIFRAME_TARGET_FPS=60 %command%
```

The build produces a 64-bit library only, so 32-bit games need a separate 32-bit build and manifest.

### Hide the presentation extensions from games

The layer owns `VK_EXT_present_timing` and `VK_KHR_present_id2` on the swapchains it manages. If a game also used them, present IDs and timing requests would collide. Hook `vkEnumerateDeviceExtensionProperties` (and the `vkGetPhysicalDeviceFeatures2` outputs for the matching feature structs) and remove both extensions from what the game sees. The layer itself still enables them in `vkCreateDevice` (Stage 2).

---

## 6. Performance Budget

| Resolution | Combined GPU budget (flow + masking + interpolation) |
| :--- | :--- |
| 1080p | ≤ 2.5 ms |
| 1440p | ≤ 3.8 ms |

These are **provisional** targets. Stage 4 allots 1.5 ms of the 1080p budget to the flow chain; revise the split after measuring. Measure with timestamp queries around each pass on both queues (`OpticalFlowEngine::LastGpuMs()` plus the equivalent for masking and interpolation), not with wall-clock time.

If the budget is missed, the cheapest levers, roughly in order:
1. Run the flow at half resolution and upsample the vectors.
2. Skip the finest refine level and rely on the bilateral filter.
3. Reduce the coarse search radius or use a larger step.
4. Use shared-memory tiles in the SAD loops.
5. Skip the backward flow every other frame and reuse the previous occlusion mask.

---

## 7. Verification & Final Testing Protocol

### Testing Checklist Across Linux Environments

1. **Wayland + VRR (KWin / Hyprland):**
   * Enable variable refresh rate on the display and run a game that fluctuates between roughly 28 and 45 FPS.
   * **Pass:** cadence stays smooth, no tearing, no compositor crashes, output cadence stays even, and `t` varies smoothly without pops.
2. **X11 and fixed refresh:** same game, verify the vblank-paced fallback (DESIGN §4.6) and the timed path both produce even spacing.
3. **Steam Deck (Game Mode, Desktop Mode) and the RX 9070 SteamOS machine:** follow the test matrix in [TARGETS.md §5](TARGETS.md). Game Mode is expected to use the vblank-paced fallback until gamescope exposes `VK_EXT_present_timing`.
   **Gamescope session:** run under gamescope (SteamOS Game Mode); verify the layer loads and that gamescope's own swapchain layer does not reject the forced `FIFO` / extra image count.
4. **Window resize / Alt-Tab stress:**
   * Rapidly toggle fullscreen, change in-game resolution, resize the window.
   * **Pass:** no leaks, no validation errors, and no frame stuck on screen after a resize.
5. **Pause and stall:** pause a game for several seconds, and kill it mid-frame.
   * **Pass:** the last frame is flushed (not frozen on an old image) and shutdown is clean.
6. **Performance budget:** measure the combined GPU time of all layer passes.
   $$\text{Target} \le 2.5\text{ ms (1080p)} \quad \Big| \quad \le 3.8\text{ ms (1440p)}$$
7. **Bypass:** a game that already runs at or above the cap is in bypass and shows no measurable latency or frame-time change compared with the layer disabled.

---

## 8. Project Completion

With Stage 6 complete, the layer covers interception, hold-one presentation, `VK_EXT_present_timing` pacing, hierarchical optical flow, UI-safe motion interpolation and runtime configuration. The remaining open questions are collected in [DESIGN.md](DESIGN.md) §6.
