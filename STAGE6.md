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

Runtime configuration through environment variables (read once per process), plus the live settings. README.md has the complete tables with defaults; this page keeps the Stage 6 view. All of them are read by `Config` in `include/vk_iframes_layer.hpp` through `EnvVar("NAME")`, which tries `VKIFRAME_NAME`, then `FRAMEGEN_NAME`.

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

`scripts/install.sh` writes this file for the current user (no root; it points `library_path` at the built library and escapes the path), and `scripts/uninstall.sh` removes it. The template is `manifest/VkLayer_frame_gen.json.in`; the build also writes a copy next to the library for explicit loading. The function names and the environment switches are as above.

Steam launch option for a Proton game (a Deck OLED at 90 Hz with the game capped to 45):

```text
VKIFRAME_ENABLE=1 VKIFRAME_TARGET_FPS=90 VKIFRAME_BASE_FPS_CAP=45 VKIFRAME_INTERPOLATE=1 %command%
```

The build produces a 64-bit library only. 32-bit games run through Proton's 64-bit path with `PROTON_USE_WOW64=1` (Tomb Raider 2013 on the Deck); the Decky plugin has a copy button for that launch option. A separate 32-bit build is not made.

### Hide the presentation extensions from games

**Still open.** The layer owns `VK_EXT_present_timing` and `VK_KHR_present_id2` on the swapchains it manages. If a game also used them, present IDs and timing requests would collide. The plan: hook `vkEnumerateDeviceExtensionProperties` (and the `vkGetPhysicalDeviceFeatures2` outputs for the matching feature structs) and remove both extensions from what the game sees, while the layer itself still enables them in `vkCreateDevice` (Stage 2).

**What the code does today:** it detects an application that uses present timing or present id 2 itself and then does not use present timing on that device (`present timing: skipped (the application uses present timing / present id itself)`), and an application whose present chain carries present id 2 or timing structs is left in passthrough (`hold: off`). Applications with a plain `VK_KHR_present_id` are handled by carrying the id with the held frame (STAGE3.md addenda).

---

## 6. Performance Budget

| Resolution | Combined GPU budget (flow + masking + interpolation) |
| :--- | :--- |
| 1080p | ≤ 2.5 ms |
| 1440p | ≤ 3.8 ms |

These are **provisional** targets, and they are **not met on the Deck**: at 1280x720 the flow alone takes about 5 ms per pair and the interpolation 0.8 to 1.4 ms per synthetic frame (STATUS.md; the benchmark `bench.json` gives the layer's cost on the game's frame time). Stage 4 allots 1.5 ms of the 1080p budget to the flow chain; revise the split after measuring. Measure with timestamp queries around each pass (the log lines `flow stages` and `interpolation: GPU`, and the stats and benchmark records), not with wall-clock time. The first design named `OpticalFlowEngine::LastGpuMs()`; the code has no such class.

If the budget is missed, the cheapest levers, roughly in order:
1. Run the flow at a coarser level (`VKIFRAME_FLOW_LEVEL`: 1 = half resolution, 2 = quarter, the default, 3 = eighth). Already the default at quarter resolution; the sub-pixel fit and the colour-guided weighting recover part of the precision.
2. Skip the finest backward refine (`VKIFRAME_FLOW_BWD_SKIP`). Measured on the Deck: the refine at level 2 went from 1.75 to 0.91 ms and the flow from 4.85 to 4.02 ms per pair, but the smudging on thin lines got worse, so it stays off.
3. Reduce the coarse search radius or use a larger step.
4. Use shared-memory tiles in the SAD loops.
5. Skip the backward flow every other frame and reuse the previous occlusion mask.
6. Compute the pan check once per workgroup (done: the interpolation went from 2.35 to 0.77 ms on the Deck).
7. Lower `VKIFRAME_MAX_SYNTHETIC` or the target when the cost warning appears (`VKIFRAME_COST_WARN`).

---

## 7. Verification & Final Testing Protocol

### Testing Checklist Across Linux Environments

1. **Wayland + VRR (KWin / Hyprland):**
   * Enable variable refresh rate on the display and run a game that fluctuates between roughly 28 and 45 FPS.
   * **Pass:** cadence stays smooth, no tearing, no compositor crashes, output cadence stays even, and `t` varies smoothly without pops.
2. **X11 and fixed refresh:** same game, verify the vblank-paced fallback (DESIGN §4.6) and the timed path both produce even spacing.
3. **Steam Deck (Game Mode, Desktop Mode) and the RX 9070 SteamOS machine:** follow the test matrix in [TARGETS.md §5](TARGETS.md). Game Mode uses the vblank-paced fallback until gamescope exposes absolute target times (measured: present timing is offered for feedback only).
   **Gamescope session:** run under gamescope (SteamOS Game Mode); verify the layer loads and that gamescope's own swapchain layer does not reject the forced `FIFO` / extra image count.
4. **Window resize / Alt-Tab stress:**
   * Rapidly toggle fullscreen, change in-game resolution, resize the window.
   * **Pass:** no leaks, no validation errors, and no frame stuck on screen after a resize.
5. **Pause and stall:** pause a game for several seconds, and kill it mid-frame.
   * **Pass:** the last frame is flushed (not frozen on an old image) and shutdown is clean.
6. **Performance budget:** measure the combined GPU time of all layer passes.
   $$\text{Target} \le 2.5\text{ ms (1080p)} \quad \Big| \quad \le 3.8\text{ ms (1440p)}$$
7. **Bypass:** a game that already runs at or above the cap is in bypass and shows no measurable latency or frame-time change compared with the layer disabled. STATUS.md records a small measured cost in bypass seconds (about 1 ms at 18 fps, inside an interpolating run); a clean 60-to-60 run has not been measured. `ENABLED=0` (live) makes every frame a plain bypass and switches off the base-rate cap, but the present hook still runs; only `VKIFRAME_DISABLE=1` (or not enabling the layer) removes the layer entirely.

---

## 8. Project Completion

With Stage 6 complete, the layer would cover interception, hold-one presentation, `VK_EXT_present_timing` pacing, hierarchical optical flow, UI-safe motion interpolation and runtime configuration. The remaining open questions are collected in [DESIGN.md](DESIGN.md) §6.
