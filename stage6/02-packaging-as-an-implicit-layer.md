[index](../STAGE6.md) | [previous](01-overview-objectives.md)

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
3. **Steam Deck (Game Mode, Desktop Mode) and the RX 9070 SteamOS machine:** follow the test matrix in [TARGETS.md §5](../TARGETS.md). Game Mode uses the vblank-paced fallback until gamescope exposes absolute target times (measured: present timing is offered for feedback only).
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

With Stage 6 complete, the layer would cover interception, hold-one presentation, `VK_EXT_present_timing` pacing, hierarchical optical flow, UI-safe motion interpolation and runtime configuration. The remaining open questions are collected in [DESIGN.md](../DESIGN.md) §6.
