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

**Order of work (owner, local time zone):** measurement and `bench.json` (done), refresh-rate clamp (done), the benchmark series on the saved-game scene (done), the code-audit fixes (done), the layer-cost baseline on the current library (done), capture and fill tests of the one-sided fill (done; it stays opt-in and experimental), packaging and the alpha release (in progress). Next after the alpha: reduce the layer's GPU load (plan below), the in-game overlay and `vkiframes-ctl`, and the async compute queue last, only if the measurements show it is worth it (the layer's cost is serial GPU time, so a second queue does not add capacity). The state of the build is in STATUS.md, section "Build state". A depth buffer from the game is parked; so is the hint-provider idea below.

`[x]` done, `[ ]` open, `[~]` partly done.

---

## Stage 1: Vulkan Layer Skeleton & Swapchain Interception
**Goal:** Establish the layer infrastructure and intercept the presentation path without breaking the target application.

### Tasks
- [x] CMake build (C++20) of `libvkIFrames.so`, with the layer manifest generated at configure time (`manifest/VkLayer_frame_gen.json.in`).
- [x] Export `vkNegotiateLoaderLayerInterfaceVersion`, `vkGetInstanceProcAddr` and `vkGetDeviceProcAddr`.
- [x] Implement correct layer chaining in `vkCreateInstance` / `vkCreateDevice` (`VkLayerInstanceCreateInfo` / `VkLayerDeviceCreateInfo` link info) and per-instance / per-device dispatch tables.
- [x] Intercept `vkDestroyInstance`, `vkDestroyDevice`, `vkCreateSwapchainKHR`, `vkDestroySwapchainKHR`, `vkQueuePresentKHR`.
- [x] In `vkCreateSwapchainKHR`, raise `minImageCount` to at least 4, clamped to the surface's `maxImageCount`. *(Stage 3 raises it further when frames are held: the game's count + `MAX_SYNTHETIC` + 2.)*

### Testing & Validation Milestone
- **Status:** done. Verified 2026-10-29 on the RX 9070 SteamOS machine (Mesa 26.2.0-devel, `vkcube`): build, mock-loader test, loader lists the layer, instance/device creation, swapchain image count 3 → 4 (without holding; with holding it is the game's count + `MAX_SYNTHETIC` + 2), present interception and clean teardown all pass. Not yet run: the validation layer (not installed) and the Steam Deck.
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
- [x] If the extension is missing, use the vblank-paced fallback of DESIGN §4.6. *(Done in effect, without a separate flag: the presenter thread paces the ticks by sleeping until `VKIFRAME_PACE_LEAD_US` before each tick and FIFO does the rest. The Steam Deck in Game Mode and the desktop Wayland session run this way, because their surfaces offer feedback but no absolute target times.)*

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
- [x] Allocate the RGBA16F ring buffer (6 reference-counted slots, `EXCLUSIVE` while everything runs on one queue family) and implement capture by blit in the present hook. *(3b, verified 18/18 on the RX 9070 machine)*
- [x] Implement hold-one scheduling, the flush rules, and the bypass when the game is at or above the target. *(3b, verified 18/18 on the RX 9070 machine)*
- [x] **3a** *(verified on the RX 9070 machine, 7/7)* Output clock, per-tick $t$, synthetic-frame clamp and bypass gate as pure logic (`include/output_clock.hpp`), verified against the simulator and running as a dry run in the layer. *(Scheduling the actual presents per DESIGN §2.D belongs to 3b/3c.)*
- [x] **3b** *(verified on the RX 9070 machine, 18/18)* Hold one frame, ring buffer, passthrough engine (`src/gpu_frames.cpp`).
- [x] **3c-i** *(verified on the RX 9070 machine, 20/20; visual check pending)* Placeholder blend (`shaders/blend.comp`), hidden images, `tests/test_synth.cpp` (the old repository's `scripts/test_stage3c.sh` was not carried over).
- [x] **3c-ii** *(verified on the RX 9070 machine, 19/19)* Presenter thread plus per-queue mutexes for exact pacing (`tests/test_pacer.cpp`; the old repository's `scripts/test_stage3cii.sh` was not carried over).
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
- [x] Write the pyramid downsample shaders (4 levels: `flow_luma.comp`, `flow_down.comp`).
- [x] Write coarse-to-fine flow shaders (coarse search, per-level refine).
- [x] Write the forward-backward consistency shader that outputs the occlusion mask.
- [x] Build SPIR-V at build time and embed it in the library.
- [ ] *(not built, same reason)* Synchronize with the graphics queue using timeline semaphores.

### Testing & Validation Milestone
- **Test:** Feed two consecutive frames of a camera pan through the pipeline and dump the vector map.
- **Success Criteria:** A clean, plausible motion vector map and occlusion mask, with no GPU hangs, and flow cost within its budget (provisionally 1.5 ms at 1080p; see Stage 6). *Not met on the Deck: about 5 ms per pair at 720p (STATUS.md).*

---

## Stage 5: Static UI Masking & Dynamic Motion Interpolation
**Goal:** Combine motion vectors with temporal weights to synthesize clean intermediate frames with UI protection.

### Tasks
- [x] Static difference-threshold compute pass for invariant pixels (HUD, text, crosshairs). *(inside `flow_interp.comp`; `VKIFRAME_UI_MASK_THRESHOLD`)*
- [x] *(done as colour-guided vector weighting inside `flow_interp.comp`, not as a separate pass)* Bilateral edge filtering of the flow field guided by source colors and the UI mask.
- [x] *(done differently: the pan fallback measures the bad-match share on a fixed grid inside `flow_interp.comp`; there is no `flow_stats.comp`)* GPU reduction of the mean flow magnitude.
- [x] Backward-gather interpolation shader using the dynamic $t$, with occlusion fallback and smooth fallbacks to the cross-fade (match cost, speed, and a whole-frame pan fallback; one-sided and two-sided forms).
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

---

## Next after the alpha: reduce the layer's GPU load (plan, owner, 2026-11-01, local time zone)
**Goal:** lower the GPU time the layer takes from the game (item 3 of the order of work). **Source:** the Auditor's feasibility check, from the code, `STATUS.md`, `ASYNC.md` and the `b8-l2` `bench.json`; nothing was run on a Deck for it. Labels: *measured*, *recomputed* (from measured numbers), *reasoned* (follows from the code, not tested), *not shown*.

* **What the layer spends** (recomputed, `b8-l2` on `c5230b8`, saved-game scene, level 2, two-sided interpolation): flow 3.12 ms per pair, interpolation 0.906 ms per pass, 2.08 passes per native frame, so 5.00 ms against the measured layer cost of 5.11 ms. On a GPU-bound game the cost is the sum of the layer's own passes (about 61 % flow, 37 % interpolation); nothing is overlapped today.
* **The flow time follows the number of image loads** (reasoned): the flow shaders have no shared-memory reuse, and the work per pixel is fixed by their loops. The three heavy passes read about 57 M (coarse, 1.25 ms), 82 M (refine at level 3, 1.45 ms) and 104 M (refine at level 2, 1.75 ms) texels per pair at similar loads per millisecond (`STATUS.md` split). Scene content therefore cannot explain the drift in flow time between runs (3.1 to 8.4 ms); the cause is *not shown*. The per-pass times are from the 2026-10-30 Deck log (flow 4.8 ms per pair, an older build and scene); the `b8-l2` total is 3.12 ms. The load counts come from the code and do not depend on the build; the split between passes in today's build is not measured (step 1 measures it).

**Order:**
1. **Measure first.** GPU clock and power from the `amdgpu` sysfs files, once a second into the stats records, layer on and off. A standalone pass benchmark on the Deck (the shader harness timing each flow pass 1000 times on a fixed input, with the clock logged). If possible a hardware profile of one pair. Look tests for `VKIFRAME_FLOW_LEVEL=3` and `VKIFRAME_INTERP_BIDIR=0` with the real shader.
2. **Decisions that need no code, after the owner's look tests.** `FLOW_LEVEL=3` saves 0.99 ms and `INTERP_BIDIR=0` 0.44 ms (together 1.39 ms; measured in the saved-game series, one scene; the level 3 and combined runs were taken in a later game session than the baseline). The two-sided form beat the one-sided form by only 0.9 % (all pixels) and 2.1 % (hard pixels) on the five Deck captures, in the CPU reference on 25 triplets (the real shader was not run). README guidance on the base rate: the flow runs once per native frame, so 45 to 90 costs about 140 ms of flow per second and 60 to 90 about 187 ms. `FLOW_BWD_SKIP=2` is not an option with the defaults (it smeared trees, and the two-sided form switches it off).
3. **Flow shader load reduction** (tiles in shared memory, or box-filtered per-candidate difference images). Reasoned saving 40 to 60 % of the 4.45 ms of the three heavy passes, uncertain by about a factor of two. Accepted by a tolerance test against the CPU reference and a look test, not by bit identity (summation order changes).
4. **Interpolation bandwidth:** a 32-bit ring and synthetic format instead of RGBA16F. Reasoned saving 0.3 to 0.5 ms per native frame; only if the pass benchmark shows the pass is bandwidth bound. Risks: banding, tests and offline tools that assume half floats.
5. **Near-static gate:** skip the flow for near-static pairs. No saving in motion; useful for power and heat in static scenes. Needs a reduction pass, indirect dispatch for the later passes, hysteresis, and a pacing check.
6. **Temporal predictor for the coarse search,** only if the coarse pass stays large after step 3.
7. **Async compute queue** (`ASYNC.md`, stage 1): last, probably never. The flow passes appear to saturate the texture path the game also uses, so a second queue adds no capacity.

**Not recommended:** deriving the backward flow from the forward flow (what `FLOW_BWD_SKIP` approximated; worse smearing, and the occlusion mask needs a true backward flow).

**Not shown:** the Deck's real texture throughput (the Auditor's 51 G texel/s is arithmetic), the size of the saving in step 3, and the cause of the cost drift. Every step needs Deck runs and the Auditor's review; shader changes also need the owner's look test.

---

## Later, after the alpha: fill pre-pass and interpolation shader study (owner, 2026-11-01, local time zone)
Both follow the GPU-load plan above and do not delay the alpha.

* **Fill pre-pass.** The opt-in one-sided fill (`VKIFRAME_ONESIDED_FILL`, #73, needs `VKIFRAME_EXPERIMENTAL=1`, read at game start) searches 2 x 169 field pixels per occluded output pixel. Measured on the RX 9070 in a test scene: interpolation 0.11 ms with it off, 0.23 ms on. On the Deck the Builder expects about 1 to 4 ms per generated frame in the worst scenes (extrapolated, not measured; occluded pixels were 3 to 37 % of a frame in the captures, the Builder's figure). A pre-pass at field resolution would cut that cost by about 16 times (the Builder's estimate). Worth doing only if the owner keeps the option: offline, the four variants were worse than the default two-sided interpolation in 93 of 100 comparisons on hard pixels, and the best of them (`fill_consist`) in 21 of 25 triplets (25 triplets from five Deck captures, CPU reference flow; the Auditor recomputed it).
* **Interpolation shader study, measurement first.** The Auditor's recompute and the Builder's checks found no defect: the two-sided interpolation beat the plain cross-fade on all pixels in 16 of 25 triplets (hard pixels 19 of 25) and lost by a small margin on near-static content. The questions for the study: (i) does the two-sided form pay for its cost (about 0.6 ms on the Deck, 0.9 % and 2.1 % lower error than the one-sided form on these captures; default on or off); (ii) do the near-static cases improve with the static-UI mask or a gate (the Builder's throwaway CPU experiment on 19 of the 25 triplets, not in the repo: a dead zone on short interpolation vectors that falls back to the cross-fade, at three widths, gave no gain: mean change against the cross-fade -8.0 % to -9.1 % on all pixels against -9.1 % for the default); (iii) is a forward-splatting variant with a consistent fill worth porting (about 2 to 3 % lower mean error in the CPU reference, needs scattering on the GPU, cost not known). The open quality item is thin lines and tree trunks (`STATUS.md` open item 0); the study starts with a diagnostic sweep of the shader's thresholds on the CPU reference (a report, no repo change), then error images, and only then one shader change for the cause found.
  * **Validation, so a retune is not overfitted:** new captures from at least 10 scenes in 4 games; hold out whole scenes, not triplets; fixed triplet spacing chosen without looking at images; a paired comparison per triplet against the current shader with the mean change per scene and a bootstrap interval over scenes. An improvement means a better mean on hard pixels and on all pixels, no scene worse by more than 2 %, and a win in at least two thirds of the triplets. Run the real shader on the GPU with the pan fallback and the UI mask on. It must not get worse in: flag-off identity, cost (the `b8-l2` layer cost of 5.1 +/- 0.2 ms), the UI mask, the pan fallback, thin lines, occlusions; the owner's look test on the Deck decides.
  * **Older captures** made while the dump stalled the game (gaps of 78 to 410 ms) are not used for tuning; they are only a stress test for very large motion.

---

## Deferred: optional D3D hint provider (parked by the owner, 2026-11-01, local time zone)
**Goal:** give the layer information it cannot see from the swapchain image alone. Not scheduled; it does not move ahead of the order of work above. It stays within the design decisions: the project stays a Vulkan layer, no DXVK fork, and the layer stays the only presenter.

* **Idea:** an optional proxy DLL in front of DXVK or vkd3d (a `dxgi.dll` or `d3d11.dll` override) that passes the layer, per frame and through a versioned shared-memory channel (the same pattern as `live.conf`): a scene image from before the UI pass, a depth image, a view and projection matrix, and the game's own velocity buffer where one exists. The layer must work as it does today when the proxy is absent.
* **What it could improve:** a UI-free scene to interpolate with the UI composited over the output (instead of the UI threshold), depth for occlusion strips (STATUS.md open item 1), and a camera-motion prior for flow.
* **Limits:** nothing is generic; buffers and matrices differ per engine, so most of the gain comes from per-engine or per-game work. Games without a velocity buffer give no motion vectors. D3D9, D3D11 and D3D12 each need their own hooks; D3D12 is the largest. Anti-cheat may reject DLL overrides. Per-draw hooks add game-thread cost.
* **Cheaper experiment first:** a ReShade-style depth and UI heuristic inside the layer (depth-format images that match the swapchain size and have the most draws, copied before they are cleared; draws to the back buffer after the main depth pass as the UI). It needs no proxy DLL.
* **Start, when it is taken up:** D3D11 only, one or two games, and a recorded artifact comparison against the flow-only baseline. Continue only if the pre-UI scene and the camera prior give a clear gain.

---

## Deferred: generic upscaler hook for games that have an upscaler (on hold, owner, 2026-11-01, local time zone)
**Goal:** for games that ship an upscaler (DLSS, FSR2/3, XeSS), take the game's own motion vectors, depth and jitter from the upscaler calls and give them to the layer, so the frames in the vkIFrames present queue are better and steadier than flow estimated from the final image. On hold: it is a large piece of work and does not move ahead of the order of work above. It is the first and most valuable use of the hint provider above, with the same rules: the layer stays the only presenter, it works as today without the proxy, and the channel is a versioned shared-memory format like `live.conf`.

* **What the game gives an upscaler:** motion vectors, depth, exposure, reactive or transparency masks and the jitter offset, per frame, at render resolution. Jitter and vector scale are in the call, so converting them is solvable.
* **Hook options, in the order to try:**
  1. *A stand-in for the DLSS library (NGX).* The proxy answers the game's NGX/DLSS calls and keeps a copy of the inputs; it does not upscale. The game must believe the GPU is NVIDIA (vendor ID and NVAPI answers), which is opt-in per game because some games take other code paths on NVIDIA. DLSS always ships as its own DLL, so there is always something to wrap, and DLSS-only games are covered. This is our own implementation, written from the public interfaces (the NGX/DLSS, FSR and XeSS APIs and their documentation).
  2. *Wrap a shipped FSR2/3 or XeSS DLL.* Works only where the game ships the upscaler as a DLL; many engines compile FSR in, which then needs per-game work.
* **Limits:** the player must switch the game's upscaler on, and the render-resolution setting then matters (a native-resolution mode if the game has one). A native Vulkan game with FSR gives the layer only anonymous compute dispatches. Streamline-based games add a layer. Anti-cheat may reject DLL replacement. D3D11 and D3D12 need separate work. Frame generation inside the game (FSR3 FG) is a separate case, because the game already interpolates.
* **Decky Loader:** it cannot read the game's buffers or present anything; it runs in the Steam UI, not in the game. It can install the proxy per game, set the launch option for the DLL override and show which hints are active, and it keeps writing `live.conf`. Whether a plugin can set launch options from the Steam UI is not verified.
* **Not verified:** none of this is tested. The Deck has no game installed that ships a DLSS, FSR or XeSS library (checked 2026-11-01).
* **First experiment, when it is taken up:** one game that ships `nvngx_dlss.dll`. Record its motion vectors and depth to disk and compare an interpolation that uses them against the flow-only baseline offline (as for the one-sided fill test). Build the proxy only if the gain is clear.

