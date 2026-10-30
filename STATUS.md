# vkIFrames: Implementation Status and Measurements

Last updated 2026-10-30. Complements [ROADMAP.md](ROADMAP.md) and [DESIGN.md](DESIGN.md). Source of truth is the git repository; this page records what was measured on hardware.

## Where the stages stand

| Stage | State |
| :--- | :--- |
| 1 Layer skeleton | Done. |
| 2 Present timing | Done. The Deck and the Wayland desktop expose `VK_EXT_present_timing`; the desktop reports feedback only (no absolute target times), so pacing stays vblank driven. Games that use present timing themselves are detected and the layer's own use is skipped. |
| 3 Hold one, ring buffer, blend | Done. 45 to 90 in Cuphead on the Deck is locked: shown 90.0 fps, 0 drops, base jitter 0.01 to 0.02 ms in steady state. |
| 4 Optical flow | Running on RADV (RX 9070) and on the Steam Deck. CPU reference in `include/flow_ref.hpp`, tested against known motion; GPU matches it bit for bit on the desktop. |
| 5 Interpolation | Done for now (flow guided interpolation, `VKIFRAME_INTERPOLATE=1`) with colour-guided vector weighting and a match-cost fade. Optional static-UI mask (`VKIFRAME_UI_MASK_THRESHOLD`, off by default, awaiting a Deck look test); the mean velocity fallback is still to do. |
| 5b Phase lock | `VKIFRAME_PHASE_LOCK` (default on) with `VKIFRAME_PHASE_OFFSET` (default 0.5): at a near-integer base/target ratio the content delay is slewed so the output ticks sit at fixed positions between native frames (t = 0.25 / 0.75 at 45 to 90). |
| 6 Integration | Started: settings renamed `VKIFRAME_*` (old `FRAMEGEN_*` still read), implicit-layer manifest with `VKIFRAME_ENABLE`/`VKIFRAME_DISABLE`, `scripts/install.sh` and `uninstall.sh`, `VKIFRAME_LOG_FILE`. Untested on the Deck: whether Steam's container sees `~/.local/share/vulkan/implicit_layer.d`. |

## Measurements

* **Flow cost, desktop RX 9070**: 1.09 ms per pair at 500x500.
* **Flow cost, Steam Deck at 1280x720**: half resolution field 7.5 to 9.2 ms per pair; quarter resolution field (default, `VKIFRAME_FLOW_LEVEL=2`) 7.2 ms; with the narrower middle level search and sub-pixel vectors **4.8 to 5.0 ms per pair** (about 22 percent of GPU time at 45 pairs per second). Whole Deck GPU reading in Cuphead 45 to 90: about 36 to 38 percent.
* **Interpolation quality**: on test frames (a rotating cube, predicting a real middle frame), the interpolation error is about 3 times lower than the cross-fade. A quarter resolution field was more accurate than the half resolution one, and the sub-pixel fit lowered the error further. On a synthetic sprite scene the colour guidance cut the error along the outline by only about 2 percent; the match-cost fade never triggered there (costs stay below 0.025).
* **Phase lock cost**: the content delay rises from about 22.3 ms to 29.5 ms (+7 ms) with the lock at offset 0. Offset 0.5 puts both ticks of a pair between native frames.
* **Cuphead on the Deck, 45 to 90, interpolation (user, same session, by feel)**:
  * lock off, guide and cost fade off: smoother, a lot more noticeable distortion;
  * lock off, guide and cost fade at defaults: smooth, noticeable distortion (less than above);
  * lock on at offset 0 (a native frame every pair): reduced edge distortion but choppy (sharp and soft frames alternate at 45 Hz);
  * **lock on at offset 0.5, defaults for guide and cost fade: smoothest; distortion noticeable only when there are lots of lines on screen; ghosting on character movement passable.** This is the best setting so far and the default now.
* **GPU cost per stage, Steam Deck, Cult of the Lamb, 45 to 90, flow level 2 (log lines `flow stages` and `interpolation: GPU`, user's Deck, 2026-10-30)**:
  * flow, per pair: 4.8 ms = pyramid 0.3, coarse 1.25, refine L3 1.45, refine L2 1.75, occlusion 0.02 (5.2 to 5.5 ms when the game is busy, one window 6.4);
  * interpolation, per synthetic frame: 2.35 ms with the pan check read per pixel (48 samples), **0.77 ms** with it computed once per workgroup (commit 03a8e62); the output is identical;
  * layer cost per 22 ms base frame (flow + two interpolations): about 9.5 ms before, about 6.3 ms after;
  * `VKIFRAME_FLOW_BWD_SKIP=2` (skip the finest backward refine at any level, mask from a 160x90 field): expected about 0.9 ms per pair at level 2, measured on the Deck: refine L2 1.75 -> 0.91 ms, flow 4.85 -> 4.02 ms per pair (Cuphead, 45 to 90). **Look test, Cuphead: the smudging on the trees is much more noticeable than without the skip (user, 2026-10-30), so it stays off by default**; Lamb showed no visible difference. Mode 1 (default) only acts at flow level 1.
* **Two-sided interpolation (`VKIFRAME_INTERP_BIDIR=1`, branch architect/bidir-warp, not yet run on the Deck)**: A is moved forward along A -> B, B is moved back along B -> A, each trusted by its own match cost and occlusion mask, so uncovered background comes only from B and covered background only from A. Offline (`interp_eval`, method `bidirectional`): Cuphead dash, trackable triplet MAE 0.0258 against 0.0274 for the single interpolation (6% lower; 8% lower on hard pixels), with visibly cleaner tree-trunk edges; the single interpolation's block patches are still visible in both. It needs the full-resolution backward flow, so it turns `VKIFRAME_FLOW_BWD_SKIP` off. **Deck test (Cuphead, 45 to 90, 2026-10-30)**: compiles and runs; interpolation 1.15 ms per synthetic frame against 0.78 ms without (+0.37 ms), flow unchanged at 4.86 ms per pair (the full backward flow is already refined at level 2); the owner reports the frames are a lot smoother (the whole build) but the trees still interpolation, so it is **off by default**; kept as an optional switch.

## Findings that changed the design

* **Display delivery was verified at protocol level.** `WAYLAND_DEBUG` on KWin shows the layer committing about 1184 distinct frames in 10 s, 1180 reported presented, 0 discarded, flips 8.333 ms apart. A MangoHud overlay loaded above the layer shows only the game's own present rate and cannot be used to measure the layer's output.
* **vkcube is a poor judge of frame rate quality**; a real game with its own clock is the test.
* **The tick marker and bar** (`VKIFRAME_TICKMARK=1|2|3`) are diagnostics that draw into the output frames; keep them out of normal use.
* **Games must run below the target to be interpolated.** With Cuphead at 90 on a 90 Hz screen the layer stays in bypass, as designed; use `VKIFRAME_BASE_FPS_CAP=45`.
* **Fractional t on every tick causes the most visible distortion.** Without the lock, every shown frame is moved at an arbitrary t. Holding the ticks at a fixed offset removes the beat; offset 0 gave uneven sharpness (choppy), offset 0.5 is uniform.
* **The mock tests ignore `VKIFRAME_*` variables from the shell** (a build shell with `VKIFRAME_TARGET_FPS=120` broke `mock_pacer`).
* The layer logs a `settings:` line at start so a log shows which options were active.

* **UI mask, offline (`tools/interp_eval`, `UI_MODE=1`, `HUD=x0,y0,x1,y1`)**: the HUD error drops to the cross-fade level (Lamb 0.0049 to 0.0032), but pixels of slow, flat motion are also taken as static, so whole-frame MAE rises at 0.03 (Lamb 0.043 to 0.045, Cuphead 2 0.027 to 0.032). A plain 3x3 difference of means was worse. Hence opt-in. Film grain keeps the mean \|A-B\| of a static HUD near 0.03, so thresholds below that do nothing.

* **Extrapolation (`VKIFRAME_EXTRAPOLATE=1`, offline, one native interval ahead)**: steady motion improves a lot (Lamb MAE 0.092 to 0.062, Cuphead dash 0.066 to 0.037); pans fall back to repeating the newest frame; near-static scenes are slightly worse than repeating (Cuphead 0.0011 to 0.0037, flow noise; a dead zone for vectors under 0.4 to 1.2 px helps a little). Latency: content delay at 15 fps with interpolation was 68 ms (one native frame); extrapolation shows the new frame on the next output slot, so the added delay is under one output frame (about 11 ms at 90 Hz). Untested on the Deck. **Start / stop / reversal check (9 triplets of the Lamb and Cuphead captures, one interval ahead):** on the 4 starts and stops the whole-frame fallback makes the prediction equal to repeating the newest frame (change -2 % to +0 %, no overshoot, no gain); the 2 steady triplets with real motion gain 33 % and 44 %; the one weak case is a stop into an almost static scene (Cuphead, error 0.0011 to 0.0037, tiny in absolute terms). Deck result at 15, 30, 45 fps base, 90 Hz: locked 90.0 fps, no drops. The base fps cap used for these tests waited before the present (every frame shown late by interval minus render time); it now waits after it (`VKIFRAME_CAP_EARLY=1` restores the old way).

## Open items
0. **Tree trunks and thin line art still distort** (Cuphead forest, user look test, 2026-10-30). The offline error images show block-shaped patches of constant displacement at the field resolution (320x180, 4 px blocks, patches of about 16 to 24 px), and a bright error band at trunk edges; the two-sided interpolation reduces the band but not the blocks. Ideas not yet tried: refine the field per pixel at the finest level only near edges, edge-aware (guided) filtering of the flow before the interpolation, a smaller final patch size at level 1 with the cost reduced elsewhere, `VKIFRAME_FALLBACK=near` for pans. Revisit after the next stage.

1. Occlusion strips (uncovered background behind a moving sprite) are cross-faded, which leaves a ghost; a one-sided fill from the frame that actually shows the background would remove it.
2. Dense line art still distorts; try stronger cost-fade thresholds (`VKIFRAME_CONF_LO/HI`) on the Deck and compare.
3. UI mask, mean velocity fallback, refresh divisor snapping on the Deck.
4. Lock stays off for non-integer ratios; its behaviour around game hitches (re-lock takes a few seconds) can be tightened.
5. Mock tests do not exercise the flow engine yet (stub shaders).
6. Implicit layer manifest and hiding `VK_EXT_present_timing` from games (Stage 6).
