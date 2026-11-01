# vkIFrames: Implementation Roadmap

This roadmap breaks the development of the Vulkan interpolation layer into distinct, verifiable stages. Each stage ends with a practical test so you can validate components incrementally before adding complexity. Architecture and math live in [DESIGN.md](DESIGN.md); each stage has its own `STAGEn.md` with code and procedures.

**Conventions used by every stage**

* Library: `libvkIFrames.so` (CMake target `vkIFrames`). Layer name: `VK_LAYER_CUSTOM_frame_gen`. Log prefix `[FrameGen]`, environment prefix `VKIFRAME_`.
* Stages are **incremental changes to one repository**, not separate copies. Headers go in `include/`, sources in `src/`, GLSL in `shaders/`.
* Inside the layer, Vulkan functions are called **through the dispatch tables**, never as bare `vk*` symbols.
* During development the layer is loaded as an explicit layer:
  ```bash
  export VK_LAYER_PATH=$PWD/build
  export VK_LOADER_LAYERS_ENABLE=VK_LAYER_CUSTOM_frame_gen
  ```
  Stage 6 adds an implicit-layer manifest for everyday use.

**Order of work now (owner, 2026-11-01, local time zone):** measurement and `bench.json` (done), refresh-rate clamp (done), the benchmark series on the saved-game scene (done), the code-audit fixes (done: all merged on `main` c5230b8, issues #21 to #34 and #41 closed), the margin-0 fix (#37, merged; rerun valid on a stationary scene), and the clock re-anchor on a live target change (#45, merged). Next: the b8-l2 baseline on the current `main` library (the regression check), then fresh frame captures on the Deck for the offline test of a one-sided fill (STATUS.md open item 1), the owner's A/B of the phase lock against the content delay (STATUS.md, findings), the Decky and `live.conf` design decision, the remaining open items in STATUS.md and the GPU and CPU load list in AUDIT.md, then the in-game overlay and `vkiframes-ctl`, and the async compute queue last, only if the measurements show it is worth it (the layer's cost is serial GPU time, so a second queue does not add capacity). A depth buffer from the game is parked.

`[x]` done, `[ ]` open, `[~]` partly done.

---

## Stage 1: Vulkan Layer Skeleton & Swapchain Interception
**Goal:** Establish the layer infrastructure and intercept the presentation path without breaking the target application.

### Tasks
- [x] CMake build (C++20) of `libvkIFrames.so`, with the layer manifest generated at configure time (`manifest/VkLayer_frame_gen.json.in`).
- [x] Export `vkNegotiateLoaderLayerInterfaceVersion`, `vkGetInstanceProcAddr` and `vkGetDeviceProcAddr`.
- [x] Implement correct layer chaining in `vkCreateInstance` / `vkCreateDevice` (`VkLayerInstanceCreateInfo` / `VkLayerDeviceCreateInfo` link info) and per-instance / per-device dispatch tables.
- [x] Intercept `vkDestroyInstance`, `vkDestroyDevice`, `vkCreateSwapchainKHR`, `vkDestroySwapchainKHR`, `vkQueuePresentKHR`.
- [x] In `vkCreateSwapchainKHR`, raise `minImageCount` toward 4, clamped to the surface's `maxImageCount`.

### Testing & Validation Milestone
- **Status:** done. Verified 2026-10-29 on the RX 9070 SteamOS machine (Mesa 26.2.0-devel, `vkcube`): build, mock-loader test, loader lists the layer, instance/device creation, swapchain image count 3 → 4, present interception and clean teardown all pass. Not yet run: the validation layer (not installed) and the Steam Deck.
- **Test:** Launch `vkcube` with the layer enabled, plus `VK_LOADER_DEBUG=layer` to confirm the layer loads.
- **Success Criteria:** The application runs without crashes or validation errors, and logs show swapchain creation and periodic present interception.

---

## Stage 2: Precision Timing Engine (`VK_EXT_present_timing`)
**Goal:** Take control of presentation cadence using the presentation timing extension, with a fallback if it is unavailable.

### Tasks
- [x] At device creation, detect `VK_EXT_present_timing` and `VK_KHR_present_id2`; enable them and their feature structs only if supported.
- [x] Add the swapchain flags for present timing and present id 2, force `FIFO`, and size the timing queue with `vkSetSwapchainPresentTimingQueueSizeEXT`.
- [x] Select a time domain matching CLOCK_MONOTONIC when the driver reports one.
- [x] Track $\Delta T_{\text{base}}$ in `vkQueuePresentKHR` and smooth it with the EMA filter.
- [x] Chain `VkPresentId2KHR` and `VkPresentTimingsInfoEXT` into presents; read back results with `vkGetPastPresentationTimingEXT`.
- [ ] If the extension is missing, mark the device as "vblank-paced" so later stages use the fallback of DESIGN §4.6. (Today the layer only logs `present timing off`; Stage 3 consumes the flag.)

> **Status:** implemented and verified on the RX 9070 machine (Wayland, Mesa 26.2.0-devel): 9 passed, 1 skipped. **Open:** aimed presents (absolute targets) are untested because this compositor does not offer them; check on the Deck and other compositors. The `present_stage_local` time domain with calibration is also still unexercised against absolute targets.

### Testing & Validation Milestone
- **Test:** Run an app limited to 30 FPS and log the smoothed base rate and the measured presentation timestamps.
- **Success Criteria:** Smoothed FPS converges to the cap, requested target times are honored within a small error, and there are no stalls or tearing.

---

## Stage 3: Hold-One Presentation, Ring Buffer & Passthrough Engine
**Goal:** Delay native presentation by one frame, capture frames, and drive a fixed output clock at the Target FPS with a fractional $t$ per tick, using a simple blend as a placeholder for interpolation.

### Tasks
- [x] Request `synchronization2` at device creation (3b; `timelineSemaphore` not needed yet, fences are used).
- [x] Raise the swapchain image count for the held and hidden images (DESIGN §4.5); add transfer usage bits. *(3b, verified 18/18 on the RX 9070 machine)*
- [x] Allocate the RGBA16F ring buffer (`CONCURRENT` when the device has several queue families) and implement capture by blit in the present hook. *(3b, awaiting hardware)*
- [x] Implement hold-one scheduling, the flush rules, and the bypass when the game is at or above the target. *(3b, awaiting hardware)*
- [x] **3a** *(verified on the RX 9070 machine, 7/7)* Output clock, per-tick $t$, synthetic-frame clamp and bypass gate as pure logic (`include/output_clock.hpp`), verified against the simulator and running as a dry run in the layer. *(Scheduling the actual presents per DESIGN §2.D belongs to 3b/3c.)*
- [x] **3b** *(verified on the RX 9070 machine, 18/18)* Hold one frame, ring buffer, passthrough engine (`src/gpu_frames.cpp`).
- [x] **3c-i** *(verified on the RX 9070 machine, 20/20; visual check pending)* Placeholder blend (`shaders/blend.comp`), hidden images, `scripts/test_stage3c.sh`.
- [x] **3c-ii** *(verified on the RX 9070 machine, 19/19)* Presenter thread plus per-queue mutexes for exact pacing (`scripts/test_stage3cii.sh`).
- [x] Write `shaders/blend.comp` (`mix(A, B, t)`) as the stand-in synthetic frame, plus a small `ComputePass` helper.
- [x] Insert `VkImageMemoryBarrier2` transitions between presentation, transfer and compute use.

### Testing & Validation Milestone
- **Test:** Run a 30 FPS-capped app with a 60 FPS target, then a 40 FPS-capped app (non-integer ratio).
- **Success Criteria:** Output rate measured below the layer is 60 FPS with even cadence; when the app is uncapped and at or above 60 FPS the layer is a pure passthrough with no added latency; no device loss or leaks; the validation layers report clean synchronization.

---

## Stage 4: Hierarchical Optical Flow Compute Engine
**Goal:** Compute dense pixel displacement vectors between frames on an async compute queue.

### Tasks
- [ ] *(not built: flow runs on the present queue; async compute is item 9 of AUDIT.md, last in the order of work)* At device creation, pick a compute-capable queue family (prefer one without graphics) and request a queue from it; fall back to the graphics queue.
- [x] Write the pyramid downsample shader (4–5 levels).
- [x] Write coarse-to-fine flow shaders (coarse search, per-level refine).
- [x] Write the forward-backward consistency shader that outputs the occlusion mask.
- [x] Build SPIR-V at build time and embed it in the library.
- [ ] *(not built, same reason)* Synchronize with the graphics queue using timeline semaphores.

### Testing & Validation Milestone
- **Test:** Feed two consecutive frames of a camera pan through the pipeline and dump the vector map.
- **Success Criteria:** A clean, plausible motion vector map and occlusion mask, with no GPU hangs, and flow cost within its budget (provisionally 1.5 ms at 1080p; see Stage 6).

---

## Stage 5: Static UI Masking & Dynamic Motion Interpolation
**Goal:** Combine motion vectors with temporal weights to synthesize clean intermediate frames with UI protection.

### Tasks
- [x] Static difference-threshold compute pass for invariant pixels (HUD, text, crosshairs). *(inside `flow_interp.comp`; `VKIFRAME_UI_MASK_THRESHOLD`)*
- [x] *(done as colour-guided vector weighting inside `flow_interp.comp`, not as a separate pass)* Bilateral edge filtering of the flow field guided by source colors and the UI mask.
- [x] *(done differently: the pan fallback measures the bad-match share on a fixed grid inside `flow_interp.comp`; there is no `flow_stats.comp`)* GPU reduction of the mean flow magnitude.
- [x] Backward-gather interpolation shader using the dynamic $t$, with occlusion fallback and smooth blend toward 50/50 when mean velocity exceeds the threshold.
- [x] Replace the Stage 3 blend with the interpolation pass.

### Testing & Validation Milestone
- **Test:** Play a fast-paced game with prominent HUD elements.
- **Success Criteria:** HUD text and crosshairs stay sharp and still, and fast pans degrade gracefully into soft blending instead of tearing geometry.

---

## Stage 6: Full Integration, Optimization & Edge Case Handling
**Goal:** Robustness, performance, packaging and configuration.

### Tasks
- [ ] Swapchain recreation, window resizing, dynamic resolution, and `VK_ERROR_OUT_OF_DATE_KHR` / `VK_SUBOPTIMAL_KHR` forwarding.
- [~] *(in progress: measurement logic and `bench.json` are in; Deck costs are in STATUS.md; flow tiling comes after the benchmark)* Profile and optimize the full chain to the budget: **≤ 2.5 ms at 1080p, ≤ 3.8 ms at 1440p** for flow + masking + interpolation combined (provisional).
- [ ] Test on Wayland compositors (KWin, Hyprland) and X11, with VRR and fixed-refresh displays.
- [x] Runtime configuration through environment variables, and an optional debug overlay. *(plus live settings through `live.conf`, the Decky panel, stats and a benchmark; the debug overlay is not built)*
- [x] Implicit-layer manifest for installation, and hiding `VK_EXT_present_timing` from games. *(manifest, install scripts and `VKIFRAME_ENABLE`/`VKIFRAME_DISABLE` are done; hiding `VK_EXT_present_timing` from games is still open)*
- [~] *(partly done: the layer loads in Steam games on the Deck, Cuphead, Cult of the Lamb and Tomb Raider 2013 with `PROTON_USE_WOW64=1` for the 32-bit game; the base-rate cap is `VKIFRAME_BASE_FPS_CAP`; building in the Steam Runtime SDK is not recorded)* Steam Deck and SteamOS: build in the Steam Runtime SDK, confirm the layer is visible inside the game container, decide how the base frame rate is capped (TARGETS.md §3), and add a Deck-sized flow setting (half-resolution flow).

### Testing & Validation Milestone
- **Test:** Playtest native Linux and Proton games with base rates from 25 FPS to 60+ FPS.
- **Success Criteria:** Smooth, low-overhead output matching the FPS cap, zero crashes on resize and fullscreen toggles, and transparent bypass when native performance reaches the limit.
