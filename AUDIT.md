# Audit notes (Auditor -> Builder)

Written by the Auditor/Consultant session. Baseline: `f11660b`. The Auditor reads the repo, reviews commits against DESIGN.md and reports here. Branch `auditor/notes`; the Builder owns `main` and the source.

## History rewrite: done and verified by the Auditor (14:05)

The Builder rewrote all four branches (`main`, `architect/gpu-cost`, `auditor/notes`, `auditor/attribution`) and force-pushed. Checked against the remote after the push:
- Authors and committers across all 63 commits: only `Senor-F <Senor-F@users.noreply.github.com>` and the Claude identities. The old commit `f11660b` is not reachable from any branch, and there are no tags.
- No trace of the owner's real name or email in commit metadata or in any file at any revision. No IP addresses, tokens or hostnames. No real home-directory paths remain (placeholders only).
- No `Claude-Session` links remain in any commit message (0 of 51, 55, 58 and 52 commits per branch). The 56 `Co-Authored-By: Claude` trailers are still there; remove them with a `--msg-filter` if wanted. The Auditor's own commits no longer add a session link.

Rules going forward: never `git pull` into a rewritten branch (reset to `origin/<branch>` instead); never write the owner's real name, email, IP address, hostnames or home paths in commits, files, logs, or issue and PR text; commits made as the owner use the `Senor-F` noreply identity.

Still outside git and not fixable by a rewrite: (1) issue #1 contains the owner's first name in the Builder's posts and carries Claude Code generation markers, so please edit them; (2) GitHub may keep the old commits reachable by direct hash links until it cleans them up, so a purge request to GitHub Support is the way to remove them; (3) anyone who cloned or forked before the rewrite still has the old history.

## Open findings (warp, `shaders/flow_warp.comp`)

1. **Per-pixel disagreement lean (no extra texture reads).** Compare the two warped samples, `sample_a(p - t*v)` and `sample_b(p + (1-t)*v)`. Where they disagree (relative to local level, roughly 0.15..0.6), blend toward the nearest real frame's own pixel (`t < 0.5 ? A(p) : B(p)`). Targets line smudging that `CONF_LO/HI` did not fix, since cost is per patch and this is per pixel. Needs two new push constants (push block is 7 floats today: `gpu_frames.cpp` and `optical_flow.cpp`) and a mirror in `flow_ref.hpp` `WarpPixel`.
2. **Nearest frame instead of cross-fade where the warp is rejected.** `c = mix(warped, fade, k)` puts two half-opacity copies on screen (the ghost in STATUS open item 1). Offer `FRAMEGEN_FALLBACK=fade|near`; with phase lock 0.5 (t = 0.25/0.75) `near` is just A or B. Trade-off: a possible pop at region edges. Compare on the Deck.
3. **One-sided occlusion fill (larger).** Only the forward mask exists, so covered and uncovered background cannot be told apart. Run `flow_occlusion.comp` with the flows swapped for a backward mask: uncovered pixels use only B, covered pixels only A.
4. **`FRAMEGEN_WARP_CAND` (off by default).** Acceptance uses nearest-sample luma agreement, which flat areas satisfy under almost any vector; it starts from the zero vector and the radius of 2 is about 8 output px at quarter resolution. Do not expect it to fix strips until item 3 exists.
5. **Possible perf cost: `field_bad_share()`** runs in every output pixel (8x6 grid of mask loads, plus flow loads with cost fade on) but returns one value per frame. Compute once per frame (tiny reduction pass) and pass it in. No warp-pass timing in STATUS yet; measure first.

## Pacing note (latency, not urgent)

Content delay L is about one base frame (22.3 ms at 45 fps; 23.9 ms at offset 0.5; 29.5 ms at offset 0). The minimum for a 2x midpoint layout is about F/2 plus margin. Idea: emit the `t ~ 0` tick (frame A itself) at `T_A + L` without waiting for B, so L can drop to about F/2 + margin with ticks at t = 0 and 0.5. Offset 0 costing +7 ms suggests the lock slew adds delay. The user cannot feel the current latency in Cuphead; low priority.

## Review: `architect/gpu-cost` @ 03a8e62 (pan check once per workgroup; BWD_SKIP=2)

Verdict: **OK to merge after the two small items below.** Default behaviour is unchanged (BWD_SKIP default stays 1).

Checked and fine:
- `pan_weight()` runs before the bounds check, so every thread of the 16x16 group reaches both barriers; the guard `pan_hi > pan_lo` is a push constant, so the branch is uniform. 48 of 256 threads load one sample each, so the result is identical to the old 48-sample loop in every pixel.
- `k = max(k, pan_k)` with `pan_k = 0` when the pan fallback is off matches the old conditional.
- `SkipFinestBackward` still refuses to skip with the candidate search on, and `f.fin + 1 < kLevels` guarantees a coarser backward field exists. The log line now prints what the code actually does.

Items:
1. **Add `memoryBarrierShared();` before each `barrier()`** in `pan_weight()`. glslang normally gives `barrier()` workgroup-memory semantics in compute shaders, so this likely works today, but the GLSL spec only guarantees execution sync; the explicit call costs nothing.
2. **Numbers.** The commit title and the config comment claim savings (about 0.9 ms for `BWD_SKIP=2` at level 2) but no timings are in the commit. Please record before/after from the per-stage GPU timing log lines for the warp and flow passes (Deck, same scene) in STATUS.md.

Notes:
- If the Deck default really is `FRAMEGEN_FLOW_LEVEL=2` (STATUS says so), mode 1 does nothing there; only mode 2 saves anything on the Deck's default. Mode 2 builds the occlusion mask from a coarser (160x90) field, so check the look on a game with thin line art before making it a default.
- Not covered by tests: mock tests use stub shaders, and `flow_ref.hpp` is unchanged (the shader maths did not change, so that is fine here).

### Follow-up: `architect/gpu-cost` @ 4d32e6c (13:11 check-in)

Both items closed.
1. `memoryBarrierShared()` now precedes both `barrier()` calls in `pan_weight()`. Correct placement.
2. STATUS.md records Deck timings with their source log lines. Arithmetic checks out: flow stages 0.3 + 1.25 + 1.45 + 1.75 + 0.02 = 4.77 (4.8 stated); layer per base frame 4.8 + 2 x 2.35 = 9.5 before, 4.8 + 2 x 0.77 = 6.3 after.

Remaining caveats (no action needed now):
- The timings are for 03a8e62; 4d32e6c only adds the two barrier calls, so no new timing is required, but the Deck build should still be the compile check for it (no glslang in the auditor workspace).
- `BWD_SKIP=2` is still unmeasured and non-default; keep it that way until it has a number and a thin-line-art look test.
- Still open from the first list: findings 1 to 3 (disagreement lean, nearest-frame fallback, one-sided fill), which the Builder has queued.

## Proposal: precision and load (13:35, requested by the user)

Goal: cut GPU load (and say plainly what it does not do for CPU load). Nothing here is measured; step 0 decides what is worth doing. Deck context: flow is about 4.8 ms of about 6.3 ms per base frame (STATUS.md, 4d32e6c).

What the code does today (read from `shaders/` and `src/optical_flow.cpp`):
- Pyramid images and the occlusion mask are `R32_SFLOAT` (optical_flow.cpp lines 120 and 123); flow fields are `R16G16B16A16_SFLOAT`; ring frames are `rgba16f`.
- `flow_coarse.comp` is exhaustive: 81 candidates (RADIUS 4) times a (2r+1)^2 patch, two `imageLoad`s per tap, every pixel recomputing patches its neighbours already loaded. `flow_refine.comp` does the same per candidate over a +-radius window plus the sub-pixel fit.

Steps, in order (stop when a step does not move the stage timings):
0. **Measure first.** Per-stage GPU timings already exist (`flow stages`, `warp: GPU`). Add nothing; record a baseline on the Deck (flow level 2) before and after every step.
1. **Pyramid as `R16_SFLOAT`** (halves traffic; 11-bit mantissa is finer than 8-bit luma). Change the format in `optical_flow.cpp` and the `layout(r32f)` declarations in `flow_luma`, `flow_down`, `flow_coarse`, `flow_refine`. Check `VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT` for the format at startup (it needs `shaderStorageImageExtendedFormats`) and fall back to `R32` with a log line, as the ring-format check already does. Mirror the half-float rounding in `flow_ref.hpp`, or the GPU/CPU comparison stops being exact.
2. **Mask as `R8_UNORM`** (it is a 0..1 smoothstep value; read by the warp for every pixel). Same format check and fallback.
3. **Likely the bigger win: cut the number of loads, not their width.** Cache each workgroup's A and B tile in `shared` memory in `flow_coarse` and `flow_refine`; patches overlap heavily between neighbouring pixels. Narrower formats make each load cheaper; tiling removes most of them. Hypothesis, not measured: do step 1 first, and if the stage times barely move, the bottleneck is load count and step 3 is the real fix.
4. **FP16 arithmetic** (`float16_t`, `shaderFloat16`) for cost sums and warp colour maths only if a stage is ALU-bound after steps 1 to 3. **Keep FP32** for the sub-pixel fit and for warp coordinates (`p - t*v`): FP16 cannot represent sub-pixel positions past x = 1024.
5. **INT8 dot product** (`VK_KHR_shader_integer_dot_product`) last, and only if the device reports it (check `vulkaninfo` on the Deck; not assumed). The Deck's GPU has no INT8 matrix hardware, so this is packing, not tensor speedups.

Cautions:
- The bias constants are small (`TIE_BIAS` 1e-4, `ZERO_MARGIN` 2e-3, `LAMBDA` 2e-4). Quantising luma to 8 bits would put the first two below one level; keep 16-bit (step 1) before trying 8-bit pyramids.
- Lower precision changes flow vectors slightly. Compare against the FP32 path with `interp_eval` (error vs the real middle frame) and by eye on thin line art, not only by bit-exact tests.
- Gate everything behind one switch (for example `FRAMEGEN_PRECISION=fp32|fp16|auto`), FP32 stays the fallback until Deck numbers justify a default.

**CPU load:** none of the above reduces it. Shader precision is GPU-side. If CPU load is a concern, the places to look are the per-frame descriptor updates and command recording in `gpu_frames.cpp` and `optical_flow.cpp`, and the hook time. The Auditor has not profiled that path yet; it needs a CPU timing line in the log first.

### Follow-up: `architect/gpu-cost` @ d54c2cb (13:45 check-in)

No layer shader change in these two commits (`4454400` interp_eval, `d54c2cb` STATUS). The history-rewrite notice at the top of this file has not been acknowledged yet.

1. **`BWD_SKIP=2` measured and rejected as default: agreed.** refine L2 1.75 to 0.91 ms, flow 4.85 to 4.02 ms per pair, but the owner sees more smudging on Cuphead's thin line art. This matches the caution in the earlier review: the occlusion mask built from the coarser 160x90 field is the likely cause, since the mask decides where the warp is rejected. If the saving is wanted later, try keeping the skip but evaluating the mask at the finest level from the forward field only, or only for pixels the coarse mask marks as uncertain.
2. **`interp_eval` variants (`near_A/B`, `lean_A/B`): sensible, two differences from the finding to be aware of.**
   - The lean test uses an *absolute luma* difference (`|la - lb|`, thresholds 0.15 to 0.6). The finding described a *relative, max-channel* difference. Absolute luma misses edges that differ in colour but not brightness (coloured line art) and treats dark and bright areas alike. Add a `lean_rel` variant (max channel, divided by local level plus a small constant) so interp_eval can compare both before anything goes into the shader.
   - Evaluation is at t = 0.5 only (the only t with a real middle frame), so `near_A` and `near_B` just say which frame to hold. In the layer with the phase lock at 0.5 the held frame is A at t = 0.25 and B at t = 0.75; that matches, but it is worth saying in the comments.
3. **Reminder:** mean error rewards blur. A variant that scores slightly worse than cross-fade can still look better on thin lines; the Deck look test decides.

### Review: `architect/bidir-warp` @ 625d528 (15:25 check-in)

Context: `main` is now `a2cb57b` (the gpu-cost work was fast-forwarded in). `bidir-warp` is one commit on top of it, off by default (`FRAMEGEN_WARP_BIDIR=1`), not yet run on a GPU. This is the Builder's own answer to finding 3 (one-sided fill).

Checked by reading, not compiled (no glslang in the Auditor workspace):
- **Warp geometry is right.** A is sampled at `p - t*v_fwd` and B at `p - (1-t)*v_bwd`; with the B to A vector that is the correct position for B's content at time t.
- **Backward mask is right.** `mask_bwd` compares the backward vector with the forward vector where it lands, at the same flow level, which is why bidir must turn the backward skip off (it does: `SkipFinestBackward` now returns false). Reusing the coarser field would have broken this.
- **Control flow is safe.** `pan_weight()` and its barriers still run before the bounds check, outside the new branch, and the `bidir` test is a push-constant branch, so uniform. Push constants go from 7 to 8 floats on both the C++ and shader side, consistently.

Questions and risks:
1. **Trust is asymmetric in time.** `trust = min(1, (wa + wb) / max(1-t, t))`. At t = 0.25, a pixel only A can explain gets trust 1 (pure A warp), but a pixel only B can explain gets `0.25 / 0.75 = 0.33`, so two thirds of it stays a cross-fade that still contains the covered foreground. If the aim is to remove the ghost from uncovered background, test `trust = max(cf, cb)` (full one-sided fill from whichever side is reliable) as a variant in `interp_eval`; the current formula may be deliberate (it favours the temporally nearer frame), so compare before changing. With the phase lock at 0.5 the layer runs at t = 0.25 and 0.75, so both cases occur.
2. **Cost is unmeasured and has two parts.** The warp gains a second guided field (about 5 luma loads plus 4 flow loads) and a second sample, and the full-resolution backward refine returns, which `BWD_SKIP=2` measured at about 0.8 ms per pair at level 2. Please record on the Deck: flow ms per pair and warp ms per synthetic frame with bidir on and off, same scene (the existing `flow stages` and `warp: GPU` log lines).
3. **Fallback is still a plain cross-fade** where both sides are bad, so thin-line smudging (finding 1) is not addressed here; the disagreement lean and nearest-frame fallback still apply on top.
4. **Offline gain is modest** (MAE 0.0258 vs 0.0274, 6%); the claim worth testing is the visual one (cleaner trunk edges) on the Deck against the current smudging. Mean error rewards blur, so judge by eye.
5. **Tests:** the shader is mirrored only in `interp_eval`, not in `flow_ref.hpp`, and the ctest mocks use stub shaders, so nothing automated covers the GPU path; the Deck build is the compile check.

Repo scan at 15:25 (all branches, all history): authors only `Senor-F` and Claude; no old identity, no session links, no IPs; placeholder paths only. Issue #1 still carries the owner's first name and Claude Code markers (owner or Builder must edit).

## Backlog: ways to reduce GPU load (noted 15:45; now PRIORITY since 16:59 by owner decision, see the PRIORITY section at the end)

Do not start these unless the owner asks. Baseline from STATUS.md (Deck, 45 to 90, flow level 2): flow about 4.8 ms per pair (pyramid 0.3, coarse 1.25, refine L3 1.45, refine L2 1.75), warp about 0.77 ms per synthetic frame, about 6.3 ms per base frame in total. Savings below are guesses from reading the shaders, none measured.

- **Shared-memory tiles** for the A and B luma in `flow_coarse` and `flow_refine`: patches overlap heavily and every pixel reloads them. Possibly the largest saving. Low risk.
- **Warm-start the coarse search** from the previous pair's flow (small window instead of the 81-candidate exhaustive search); fall back to the full search on scene change or high match cost. Maybe 0.5 to 0.9 ms. Medium risk.
- **Two synthetic frames per warp dispatch:** the flow fetch, guide weights, cost and mask do not depend on t. Maybe 0.3 to 0.5 ms. Low risk.
- **Narrower formats:** `r16f` luma pyramid, `r8` mask (see the precision proposal above). Low risk.
- **Governor:** when GPU time runs late, step down (higher flow level, cross-fade instead of warp), as afmf-linux does; costs nothing when there is headroom. Needs tuning.
- **Static-scene skip:** if A and B are nearly identical, skip flow and warp. Low risk.
- **Backward flow by inversion** of the forward field (about half of the refine cost): high visual risk, `BWD_SKIP=2` already showed smudging on Cuphead.
- **Conflict to remember:** `architect/bidir-warp` needs the full-resolution backward flow, which gives back about 0.8 ms per pair.
- **Not worth it:** `BWD_SKIP=2` as a default, and the candidate search (`FRAMEGEN_WARP_CAND`).
- Measure each change alone with the existing `flow stages` and `warp: GPU` log lines.

## Process

- Builder: commit small and push often; the Auditor only sees what is on GitHub.
- Shader changes are not covered by ctest (mock tests use stub shaders); mirror them in `flow_ref.hpp` and score with `tools/interp_eval`. Mean error favours blur, so judge the look on the Deck too.

## Check-in 16:15 (Auditor)

Identity scan, all remote branches: authors only `Senor-F <Senor-F@users.noreply.github.com>` and Claude identities; no old name or email, no IPs, no session links in commits; placeholder paths only. Issue #1: no personal name left in the comments; 13 "Generated by Claude Code"/claude.ai links remain across 12 comments (owner or Builder must edit or delete).

Reviewed new work (main 9cd8930; branch `architect/vkiframe-rename-ui-mask` 193105c):
* 9cd8930 / ceca638: `EVAL_T` and `bidir_onesided` (trust = max(cf, cb)) answer the 625d528 question; fine. Deck result for bidir: +0.37 ms per synthetic frame, flow unchanged, trees still warp. Agree with keeping it off by default.
* Open item 0 (block-shaped patches at 320x180, 4 px blocks): the likely cause is the flow field resolution, not the warp. Cheapest test is the guided/edge-aware flow filter before the warp, measured in `interp_eval` first.
* 193105c: rename to `VKIFRAME_*`; `EnvVar()` falls back to the old `FRAMEGEN_*` name, so existing launch options keep working. Good. Check that docs still show the old names only as "deprecated".
* New static-UI mask (`ui_thr`, 3x3 mean |A-B| luma): per-pixel, cost is 9 extra loads of A and B in the warp (not measured; add to the Deck timing). Risk already listed in the README: slow low-contrast motion under the threshold cross-fades instead of gliding. Off by default; keep it that way until looked at on Cuphead and Lamb.
* `DESIGN.md` line 5 still says "clean-room" on main and on this branch; `auditor/attribution` (ffe3923) fixes it and is still unmerged.

## Check-in 16:45 (Auditor)

Scan, all remote branches: authors only `Senor-F` noreply and Claude identities; no old name or email, no IPs, no session links in commits, no real home paths. Issues #1 and #2: every comment is by `Senor-F`; no PRs, no forks, no outsiders.

Reviewed `architect/vkiframe-rename-ui-mask` e866d53 (extrapolation, opt-in) and b8ce41a (README rewrite):

1. Geometry is right: `out(x) = B(x + s * v_b(x))` with `v_b` pointing B to A moves content on by s intervals. Fallbacks to plain B (bad match, uncovered, fast, dead zone, pan) are the safe choice. The UI mask in extrapolation mode falls back to B, not to the cross-fade; also right.
2. Reference counting in `Hook_QueuePresentKHR` adds up (first frame: three pins, job drops two, `g.last` keeps one; later frames hand `g.last`'s reference to the job and pin B again; bypass releases `g.last`). No leak or double release found by reading; the mock-driver test covers the sequence.
3. Risk, latency of tick 0: the presenter is one serial queue. A new job's tick 0 (the native frame the game waits on) cannot start until the old job leaves `ExecuteExtrap`, which can be inside `GpuFlow` (about 5 ms) or a warp when the new frame arrives. Suggest: check `cancelled()` before `GpuFlow` and between flow and warp, and log the lateness of tick 0 against its planned time.
4. Risk, cancelled slots: a cancelled predicted tick leaves its output slot unfilled, so the display repeats the last frame. Count these ("predicted ticks cancelled") in the rate log, so a Deck test shows how often it happens.
5. Quality: the offline numbers are steady-motion cases. Extrapolation fails at starts, stops and reversals (Cuphead dash start/stop, jumps), where the predicted frame overshoots and then snaps back. Evaluate on a triplet that spans an acceleration, not only a steady one, before trusting the average.
6. Value for this owner: the gain is lower latency (under one output frame against one base frame), and the owner cannot perceive the current latency. The cost is speculative artifacts. Keep it opt-in and out of the priority list; test it once on the Deck for the look only.
7. `README.md` and `DESIGN.md` line 5 still say "clean-room" on every branch. `auditor/attribution` (ffe3923) fixes DESIGN.md and the credit paragraph, but b8ce41a rewrote the README, so that branch will conflict on README: the Builder should apply the wording ("independent implementation, inspired by afmf-linux's fallback behaviour", plus the credit paragraph) to the rewritten README instead of merging.

## PRIORITY (owner, 16:59): GPU load, and CPU<->GPU load and latency

The earlier GPU-load backlog above is now priority. This section adds the CPU<->GPU side, from reading `gpu_frames.cpp`, `optical_flow.cpp` and `layer_main.cpp` on `architect/vkiframe-rename-ui-mask` (b8ce41a). Nothing here is measured; every gain is an estimate to be confirmed on the Deck with the existing timers. Ranked by expected value over effort.

**A. Design gap: flow and warp run on the game's own queue (largest item).**
`optical_flow.cpp` line 1: "computed on the present queue"; `EndAndSubmit` takes the game's queue mutex. DESIGN.md (async compute, dedicated compute family queue, timeline semaphores, concurrent ring images) is not what the code does: no extra queue is requested in `vkCreateDevice`. Consequence: about 5 ms of flow plus the warps sit in the same queue as the game's next frame, so the game's render is queued behind our work and its measured frame time (the input of the whole rate logic) grows. Fix: request one compute-capable queue at device creation (RADV exposes a compute-only family), run flow and warp there at a lower queue priority than the game's, hand images over with CONCURRENT ring images (already coded for >1 family) and a timeline semaphore. Check first with `vulkaninfo` which families the Deck and the RX 9070 expose. Test: base frame time with the layer on, with and without the change, in a GPU-bound scene.

**B. Captures and outputs: 8 bytes per pixel, in memory that the Deck's CPU shares.**
The ring is always RGBA16F (8 B/px) and every capture is a `CmdBlitImage` format conversion from the 8-bit swapchain image; every synthetic frame is then written to a RGBA16F `synth` image and blitted again into the swapchain. At 1080p a 16F frame is about 16.6 MB. On the Deck the APU shares DRAM with the CPU, so memory traffic is also CPU load. Options, in order:
1. Make the ring format follow the swapchain (8-bit UNORM when the swapchain is 8-bit; keep 16F for 10-bit/HDR) and use `CmdCopyImage` instead of a blit: a quarter or half of the bytes, no conversion pass. The 8-bit source holds nothing that 16F adds. The flow luma pass and warp already read through `imageLoad`; use a sampler (hardware bilinear) instead of the hand-written BILINEAR macro where possible.
2. Write the warp output straight into the swapchain image (storage usage on the swapchain, `MUTABLE_FORMAT` for sRGB) and drop the second blit and its barriers. Not every driver allows storage on swapchain images, so keep the blit as fallback.
3. With (1) the captures could also run on a transfer-only queue (SDMA) if one exists, taking them off the graphics queue.

**C. Locking and fences on the game thread (CPU latency).**
* `GpuSynthesize` holds `gpu_mu` for its whole body, including the acquire polling loop (`nanosleep` 200 us, up to 2 s). The game thread's `GpuCapture` needs the same lock, so a slow acquire stalls the game's present. The layer already counts this (`wait_lock_ns`); read that counter on the Deck first. Fix: do the acquire before taking the lock, or use a separate lock for the ring bookkeeping; never sleep under `gpu_mu`.
* One shared ring of 8 command buffers and 8 fences: the game thread can block in `BeginCmd` on a fence the presenter owns (`wait_fence_ns`). Give each thread its own command pool and command buffers (a pool is not thread safe anyway), and replace the fences and binary semaphores (`captured[]`, `staged[]`, `acquire[]`) with one timeline semaphore per queue: no `vkWaitForFences`/reset per use, and a wait for "frame N captured" becomes a counter compare.
* `UpdateDescriptorSets` is called for every synthetic frame. Pre-write the sets once per ring slot, or use a small descriptor-indexing array of the ring views and pass the two slot numbers in the push constants: no descriptor writes on the hot path.

**D. Barriers that stall more than they need.**
Most barriers use `ALL_COMMANDS` as source or destination stage (capture, held present, synth blit). An `ALL_COMMANDS` source waits for everything earlier in the queue, including the game's in-flight work, and an `ALL_COMMANDS` destination blocks everything after. Where a semaphore wait already orders the work, the swapchain barrier needs no source stage (NONE / `TOP_OF_PIPE`); the rest can name the real stages (`TRANSFER`, `COMPUTE_SHADER`). Cheap to do, measurable with the warp timer.

**E. Timestamps and polling in the hot path.**
* The flow passes write eight `BOTTOM_OF_PIPE` timestamps per pair and the warp two per frame, and the results are read back with `GetQueryPoolResults` on the next use. Keep them for `VKIFRAME_STATS=1` only (or sample one pair in N); bottom-of-pipe timestamps also add pipeline drains.
* `wait_tick` wakes every 1 ms (`min(due, now + 1 ms)`) to see whether the job was cancelled: about 1000 wakeups per second while waiting. Use one condition variable (or `timerfd` plus an eventfd) and wake it from the cancel; sleep once to the exact time.
* The acquire poll (200 us naps) should become `vkAcquireNextImageKHR` with a short finite timeout, outside the lock.

**F. Fewer submits (lower value, do last).**
Per pair: one capture submit, one flow submit, one submit per synthetic frame (each with a fence wait and a semaphore pair). Flow and the first warp could share a command buffer, and with the timeline semaphore several ticks can be recorded at once and released one by one.

**Order I suggest:** A (measure first with a GPU-bound scene), then B1 and C (locking, per-thread pools), then D and E, then B2, B3, F. Report each step with the same timers as before (flow ms per pair, warp ms per frame, base frame time with the layer, `wait_lock_ns` / `wait_ring_ns` / `wait_fence_ns`).

**What not to change:** the FP16/INT8 precision plan in the earlier section is still valid, but it is an ALU and bandwidth saving inside the flow passes; A to E remove waiting and traffic around them and are likely to matter more on the Deck.

## Check-in 17:15 (Auditor)

Scan, all remote branches and issues: authors only `Senor-F` noreply and Claude identities; no old name or email, no IPs, no session links (0 `Claude-Session` lines in any commit message), no real home paths (placeholders and the generic Steam Deck default only). Issues #1 and #2: all comments by `Senor-F`; no PRs, no forks, no outsiders. Issue #1 still has 13 Claude links in its comments (no personal name).

Reviewed 6b763ed, 6eaf722, 5200a44, 30cbcbd on `architect/vkiframe-rename-ui-mask`:
* Condition-variable tick wait is correct (job_gen is bumped before the lock/notify pair, so no lost wake-up; predicate checked under `pr->mu`). Tick-0 lateness and cancelled-tick counters are what I asked for. The `layer CPU` log line is the right first step for C.
* The cap-after-present change affects only the test cap (`VKIFRAME_BASE_FPS_CAP`); fine.
* Start/stop/reversal result (fall back to the newest frame, no overshoot, no gain) is the honest outcome; extrapolation therefore only helps steady motion. Keep it opt-in.

**Correction of my own earlier claim (answers the Builder's question on #2):** the sentence "a few starting thresholds for the fallback tests were taken from that project" in my `auditor/attribution` branch (ffe3923) is NOT verified. I cannot show from the repo that any value was copied from afmf-linux; the repo's own thresholds (`pan_lo` 0.15, `pan_hi` 0.35) are documented as coming from its own captures. The Builder was right to leave the sentence out. Do not add it. Only the idea (a single real frame instead of a cross-fade where a warp cannot be trusted) is credited, as the owner decided. `auditor/attribution` is superseded by the Builder's wording and should not be merged.

## Check-in 18:15 (Auditor)

Scan, all remote branches and issues: authors only `Senor-F` noreply and Claude identities; no old name or email, no IPs, no session links, no real home paths (`/home/deck` is the generic Steam Deck default; `/home/YOU` placeholder). Issues #1 and #2: all comments by `Senor-F`; no PRs, no forks, no outsiders. Issue #1 has no personal name (13 Claude links remain). New branch `decky-plugin` (9adb8eb) is by `Senor-F`.

Reviewed 8a92de7 (live settings), b6d1082 (default 90 fps on the Deck OLED) and the `decky-plugin` branch:

1. `live.conf` polling is cheap (one `stat()` a second), values are clamped, malformed lines are skipped, the file read is capped at 64 KiB, and `ENABLED=0` makes the gate never open (pure passthrough). Good. `MAX_SYNTHETIC` above the swapchain sizing is capped at `kLiveMaxSynthetic` = 4, so changing it live cannot outgrow the images.
2. Ownership risk to check on the Deck: the plugin backend creates `~/.config/vkiframes` (`os.makedirs`, `main.py` line 51) with no `chown`. If Decky runs the backend as root (`ls -l` on that folder will show), the folder is root-owned and the game process, running as the user, cannot create `live.log` or `status.json` there; the plugin would then show no status and no log, silently. Fix: create the folder and the files as the user (`os.chown` to the owner of `DECKY_USER_HOME`), or have the layer create the folder first.
3. `live.log` has no size limit: every change is appended and the plugin reads the whole file. Rotate at, say, 256 KiB (keep one `.1`) and let the plugin read only the tail.
4. When neither `XDG_CONFIG_HOME` nor `HOME` is set, the layer falls back to `/tmp/vkiframes`, a predictable path in a shared directory. Safer: no live settings in that case, or create it with `O_NOFOLLOW` and mode 0700. Low risk on a single-user Deck.
5. `status.json` is rewritten once a second by the layer: tiny, fine. Make sure it is written to a temporary file and renamed, as the plugin does for `live.conf`, so the plugin never reads half a file.
6. Impact on the priority list: these changes add no GPU work. `Swapchain images are sized for 4 generated frames` adds up to a few images of memory; on the Deck at 1280x800 that is about 4 MB each, negligible.
7. The plugin branch is not based on `main` (it has none of the engine files); that is fine for a plugin side branch, but do not merge it into `main` as it stands, since that would delete the engine in the diff.
8. Still waiting for Deck numbers for the A-F list (`layer CPU` line, `wait_lock_ns`/`wait_ring_ns`/`wait_fence_ns`, base frame time with the layer on and off). Nothing from A-D is implemented yet; E is partly done (condition-variable tick wait).

## Check-in 19:15 (Auditor)

**HIT (reported on #2 at 19:20):** commit ba026a2 on `decky-plugin` (author "Claude", 19:04) carries a `Claude-Session:` link in its message. Cause: a harness reminder asks every session to add the trailer; the owner's rule (no session links) must win. Fix: reword that one commit and push with `--force-with-lease` to `decky-plugin` only; every other branch has 0 such lines. I did not touch the branch. Suggestion: a small `scripts/check_identity.sh` the Builder runs before each push (authors, `Claude-Session`, claude.ai links, IPs, `/home/` paths other than the placeholder and `/home/deck`).

Rest of the scan: authors only `Senor-F` noreply and Claude identities; no old name or email, no IPs, no real home paths; issues #1 and #2 all `Senor-F`; no PRs, no forks, no outsiders.

Reviewed 681de9b, e3b5278, dcaab4e (`architect/vkiframe-rename-ui-mask`) and the plugin commits up to ba026a2:
* 681de9b answers my item 3 from 18:15 (log rotation above 8 MB, one old file kept). One concern: "the game's stderr goes to live.log too". Redirecting the GAME's stderr into the layer's file is intrusive (it can hold paths and user names from the game, and it hides the game's output from the user's own launcher logs). Prefer: the layer's own `FG_LOG` output goes to `live.log`; the game's stderr is left alone.
* dcaab4e renames warp to interpolation everywhere. Check that the old `VKIFRAME_WARP*` names still work as aliases (the plugin carries old saved settings over, so a user launch option written earlier would otherwise be silently ignored), and that `AUDIT.md`, `STATUS.md` and the scripts match the new names. Large mechanical diffs like this are the place for a stray typo in a shader push-constant name; the build and `test_hold`/`test_flow_ref` should be run after the rename (the Builder reports tests, please state the count).
* e3b5278: float-exact comparison so 1.12 is not logged as a change: fine.
* Plugin: `deploy.sh` uses `scp` and `ssh -t`; check it contains no address, user name or host (it takes `DECK=deck@<address>` from the environment, which is right).
* Still waiting for Deck numbers for the A-F list; no A-D change yet.

## Claude-Session cleanup (Auditor, 19:25, owner's instruction)

Messages only (trees identical), `--force-with-lease` against the old tips: `decky-plugin` b87cc8f -> 80b6f8b (three commits), `architect/vkiframe-rename-ui-mask` 25da915 -> 1d86d66 (one commit). Result: 0 `Claude-Session` lines and 0 `claude.ai` strings in the commit messages of every remote branch. The `Co-Authored-By` trailer was kept (the owner has not asked to remove it). Other sessions must reset to the new tips and must not add the trailer, even though a harness reminder asks for it. GitHub may still serve the old commits by hash until it cleans up; the old hashes (ba026a2, da7b6df, b87cc8f, 25da915) are the ones to include in a purge request.

## Security and privacy review (Auditor, 19:32, owner's request)

Scope: `architect/vkiframe-rename-ui-mask` (1d86d66) and `decky-plugin` (80b6f8b), both read from `git archive`; the whole git history; the GitHub repository settings visible to the Auditor. Method: reading plus pattern searches. The code was NOT compiled or fuzzed, so "no finding" means "none found by reading".

**Nothing remotely exploitable found.** The layer has no network code (no sockets, `system`, `popen`, `exec*`, `dlopen` outside tests), parses only a capped (64 KiB) local config file with clamped values, uses `snprintf` with `sizeof` throughout (`strcpy`/`sscanf` only in tests), and stays inert unless `VKIFRAME_ENABLE=1`. No secrets, keys, tokens, binaries or large blobs in any revision; no submodules, hooks or CI workflows; GitHub secret scanning and push protection are on.

Findings, highest first:

1. **Privacy: commit timestamps carry the owner's time zone.** 70 of 104 commits are stamped `+0800` (29 by `Senor-F`, 41 by Claude identities); the other 34 are `+0000`. A `+0800` offset narrows the owner to a region, and the hours of the commits show the working rhythm. Fix going forward: every session commits with `TZ=UTC` (or `GIT_AUTHOR_DATE`/`GIT_COMMITTER_DATE` ending in `+0000`). Fix for the history: one more rewrite that sets the offset to `+0000` (the instant stays the same). Owner decision; if yes, do it in the same pass as any change to the `Co-Authored-By` lines, by the Builder only.
2. **The repository is public** (`visibility: public`) with `main` unprotected (force-push and deletion allowed) and Dependabot off. Public is fine for a project, but then everything in it, including `AUDIT.md`, the hardware lines in `TARGETS.md` (Deck, RX 9070, Ryzen 5 5600X) and issue #1/#2, is world-readable and searchable. Suggest: protect `main` against force-push and deletion, keep the repository private until the owner has decided that the history is clean, enable Dependabot alerts.
3. **Issue #1 still has 13 "Generated by Claude Code" links** (claude.ai session links) in 12 comments; they sit outside git, so no rewrite removes them. Owner or Builder: edit or delete the comments (or the issue). GitHub also still serves the pre-rewrite commits by hash until a purge request is made.
4. **Local file handling in the layer** (the layer runs inside every game process that enables it; low risk because the user's own files are involved, but easy to harden):
   * `live_config.cpp`: `mkdir(..., 0755)` and `fopen(live.log, "a")` follow symlinks; if neither `XDG_CONFIG_HOME` nor `HOME` is set it falls back to the predictable `/tmp/vkiframes`. Use mode 0700, `open(O_WRONLY|O_APPEND|O_CREAT|O_NOFOLLOW, 0600)`, and no fallback to `/tmp`. Use `secure_getenv` for `HOME`/`XDG_CONFIG_HOME` so a set-uid process cannot be steered.
   * `VKIFRAME_DUMP` writes raw game frames to `/tmp/vkIFrames_dump` by default (world-readable under the default umask). Default to off (it is) and create the folder with 0700; warn in the README that dumps contain screenshots.
   * Redirecting the **game's stderr** into `live.log` (681de9b) puts whatever the game prints (paths, user names, account ids) into a file the plugin then reads and shows. Keep the layer's own output only.
5. **Decky plugin.**
   * `install_layer(lib_path)` accepts any absolute file and writes it as `library_path` of an implicit layer, which every Vulkan program with `VKIFRAME_ENABLE=1` will load. Check that the file ends in `.so`, is owned by the user and not group/world-writable, and refuse otherwise.
   * `read_log` reads the whole `live.log` (up to 8 MB plus the rotated one) to return 500 lines; read from the end of the file instead.
   * No lockfile (`package-lock.json`/`pnpm-lock.yaml`) is committed and all dependencies use `^` ranges, so a build pulls whatever is newest: commit the lockfile and pin exact versions.
   * `plugin.json` has `flags: []`; if Decky runs the backend as root in your version, files under `~/.config/vkiframes` become root-owned (see the 18:15 note); check with `ls -l`.
6. **Scripts.** `deploy.sh` copies the archive to a fixed `/tmp/vkiframes-plugin.tgz` on the Deck and then extracts it with `sudo`; on a multi-user machine another user could swap the file between the two steps. Use `mktemp` or a path under the user's home. `install.sh` writes `$LIB` into JSON without escaping (a path with a quote or backslash breaks the manifest; not exploitable, but quote it).
7. **Signatures.** 37 commits carry SSH signatures from the sandbox's signing key (not the owner's key; 12 of them are shown as `Senor-F`). They identify the environment, not the owner. If the owner does not want signed commits at all, turn off `commit.gpgsign` in each session.

Not checked: build and run on the Deck, dependency vulnerabilities (no network audit possible from here), the GitHub account settings, and anything outside the repository.

## Check-in 20:31 (Auditor, on the owner's request)

Scan, all remote branches and issues: authors are `Senor-F` (two noreply forms: `Senor-F@users.noreply...` and the numeric `<id>+Senor-F@users.noreply...`, same account) and Claude identities; 0 `Claude-Session` lines in any commit message; no old name or email, no IPs, no real home paths; issues #1 and #2 all `Senor-F`; no PRs, no forks, no outsiders. Issue #1 still has 13 Claude links (item 8). New commits by `Senor-F` are stamped `+0000` (the Builder follows TZ=UTC); the owner's own commit a09705d is `+0800` (owner decided: no rewrite).

Security checklist (19:37) verified against the code, not only against the Builder's comment:
* 1 and 2 (layer): `live_config.cpp` now uses `secure_getenv`, requires an absolute path, creates the folder 0700 and tightens an existing own folder, opens `live.log` with `O_APPEND|O_CREAT|O_NOFOLLOW`, 0600, creates `status.json` with `O_EXCL|O_NOFOLLOW` plus rename, reads `live.conf` with `O_NOFOLLOW` and only when owned by the user. Done.
* 4 (plugin `_check_library`): realpath, owner is user or root, no group/world write bit, no control characters in the path. **One weakness:** the `.so` test is `".so" not in basename`, which accepts `x.so.txt` or `a.sock`. Use `re.search(r"\.so(\.\d+)*$", name)`.
* 5, 6, 7: lockfile `pnpm-lock.yaml` is in (a09705d), `mktemp` in `deploy.sh` and the JSON escape in `link-build.sh`/`install.sh` are reported done; not re-read in detail.
* 8 (issue #1 links): still open, owner's choice.

New features reviewed on the surface:
* `EXPERIMENTAL=1` raises `MAX_SYNTHETIC` to 10 and sizes the swapchain for 10 generated frames; live `TARGET_FPS` accepts up to 1000. Each generated frame costs about 0.8 to 1.2 ms of warp on the Deck, so a high `k` can exceed the frame budget, and a target above the display's refresh rate only fills the FIFO queue (more latency, no more frames shown). Suggest: the layer clamps `TARGET_FPS` to the refresh rate it reports (`refresh_hz`) unless the display is VRR, and logs a warning when `k` times the cost exceeds the base frame time.
* Default `BASE_FPS_CAP 45` (2287564, 737e886): with the layer enabled the game is capped to 45 fps by default. Fine as a Deck default if the README and the plugin say so; otherwise a user who enables the layer on a 30 fps game sees no change, and on a 60 fps game sees a cap they did not ask for.

GPU-load / CPU<->GPU list A-F: no commit touches the queue (A), the ring format and blits (B), the locking (C) or the barriers (D) yet; E is partly done (cv tick wait, CPU-time log). **Coordination gap:** the Builder's own scheduled check-in says "confirm with the owner in chat before starting big items" and "do not start A-F until the owner has said so in chat". The owner told the Auditor (16:59), not the Builder's chat. The owner may need to say it in the Builder's chat for A-D to start.
