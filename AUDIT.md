# Audit notes (Auditor)

Open items, in priority order, checked against `main` 6c0e949. Findings go on the board issue; this file holds the detail. Not compiled or run by the Auditor: everything is from reading the source and the GitHub objects. Nothing is a Deck measurement unless it says so.

**Status 2026-11-01, `main` 8226bc8 (the code is that of #76 on top of #75; behaviour changed since c5230b8 by #62 to #75: capture path, instance-extension retry, log caps, static runtime, version string, defaults, installer, Decky zip, one-sided fill, swapchain capacity, packaging script):** sections 1 to 11 are the original plan (items 1 and 2 are in main; the "checked against `main` 6c0e949" line above is the date of the plan, not of the current state). Section 13 is the current review log: which heads were reviewed and passed and which merged tree equals them, the sanitizer results, the measurements recomputed (b8-l2, the Deck captures, the fill check), the library hashes and how far they are tied to a commit, the release-readiness checklist and what is still open before the alpha, and the residuals. Merged since: #76 (release polish) as `0370bbf`, #77 (ROADMAP.md) as `e15b946`, #78 (this file) as `8226bc8`; the merged tree equals the passed heads (#76 tree identical to head `b2f46d05a8b46b90e79ccc71964424021162bcb4`, ROADMAP.md sha256 `5c86fbee05300073fb3205e097992b574c733a75c5de7ba137b82b67a8b321fe` as passed, this file identical to the one passed). Open at this update: PR #64 (CLAUDE.md, UI order; on hold with the UI); the alpha artifacts are being cut by the Builder and are not yet checked. Every PR from #56 on was merged by squash on the head passed here; from #61 on, after each merge the tree of `main` was compared with the tree that merging the passed heads in order gives, and they were equal.

Every item below uses the same layout: what it is, what it costs today, what happens if we do it, what happens if we do not, a recommendation, and how to decide. Numbers marked "estimate" are the Auditor's arithmetic from the Builder's Deck figures, not measurements; the measurement logic (section 1) replaces them with real ones.

## Priority order (owner, 22:24; reordered 06:00)

| # | Item | Effort | Expected gain | Risk | Status |
|---|---|---|---|---|---|
| 1 | Measurement logic and `bench.json` | medium | decides everything below | low | in main; closed except acceptance (b) |
| 2 | Refresh-rate clamp and cost warning | small | removes wasted frames and latency | low | in main; unit-tested only (the Deck reports no refresh rate) |
| 3 | Flow passes: tiling, then 16-bit, then 8-bit integer | medium to large | highest (flow is 48 to 69 % of layer GPU time, depending on the scene) | medium (look test) | after `bench.json` |
| 4 | Ring and output format | medium | small (estimate 1 to 2 % bandwidth) | medium | only if the blits measure above 0.3 ms |
| 5 | Locking, command pools, descriptors | medium | removes rare spikes | medium (races) | only if percentiles show spikes |
| 6 | Barriers | small | small | low (validation layer) | with the validation run |
| 7 | Timestamps only in stats mode | tiny | tiny | none | part of item 1 |
| 8 | Fewer submits | medium | negligible | medium | skip unless CPU share is high |
| 9 | Async compute queue | large | at most the layer's cost on the game | high (hangs) | last; undecided: the first `bench.json` was at a 30 fps cap and cannot show the cost |

Closed: session link in a pull request body; checked security items (section 11); item 1 except acceptance (b).

**First `bench.json` (Deck, cap 30, target 90).** Recomputed by the Auditor and the PM from `stats.jsonl`: all derived numbers match. Layer cost on the game -0.02 ms mean, -0.24 ms p95 (noise is about 0.07 ms per segment). Caveat: frame time was 33.33 ms in every segment, which is the 30 fps cap, so the game was not GPU-bound and the run cannot show what the layer costs a game that fills the GPU. Item 9 stays undecided until a GPU-bound run exists. Layer GPU share 0.377 (flow 0.182, interpolation 0.195); flow 6.09 ms per pair, interpolation 2.18 ms. `refresh_hz` was 0 in every record. Open: acceptance (b), the cost of `STATS=0` against the previous build. Needed from the owner: one GPU-bound `bench.json` (section 10).

## 1. Measurement logic for the Builder (first; so the Auditor can audit, compute and close)

- **If we do.** Every decision below comes from one file instead of guesses; stats are off by default and cost nothing when off.
- **If we do not.** Items 3 to 9 stay guesses; we could build the wrong thing (for example an async queue that hides nothing).
- **Recommendation.** First.

Goal: one run on the Deck produces one small file with every number items 3 to 9 need, including the on/off comparison and the derived layer cost. No hand arithmetic, no screenshots of logs.

**Switch.** `VKIFRAME_STATS=1` (also a live key) turns collection on; off by default, and the off path must cost nothing (no timestamps written, no extra atomics beyond what exists today).

**Per second, one record** (appended to a stats file in the private config folder, mode 0600, rotated like the log):
- mode (off / interpolate / extrapolate), target, refresh rate, native fps, shown fps, generated frames per native frame;
- game frame time: mean, 95th and 99th percentile, max (milliseconds), from present-to-present intervals;
- layer CPU: game-thread CPU ms per frame inside present, presenter thread share of one core;
- waits per frame on the game thread: shared mutex, ring slot, command fence, acquire;
- GPU: flow ms per pair and interpolation ms per generated frame (the existing timestamps, only while stats are on), and the derived layer GPU share = (pairs per s x flow ms + generated per s x interpolation ms) / 1000;
- pacing: native frame lateness against its slot (mean, max), cancelled predicted ticks, dropped frames, present-queue depth.

**A/B benchmark.** Use the existing `ENABLED` live key (disabled = pure pass-through). A benchmark run (button in the Decky plugin and a command-line switch) does: 30 s warm-up, then 3 x (60 s off, 60 s on) in the same scene, then writes `bench.json` with, for each state, the mean and percentiles above, plus the derived values:
- layer cost on the game = frame time on - frame time off (mean and 95th percentile);
- GPU share of the layer, and flow share of that;
- game-thread CPU per frame and presenter core share;
- the verdict field the Auditor needs for item 9: `async_gain_upper_bound_ms` = layer cost on the game (the most a second queue can hide).
The run must stop and restore the user's settings when the user changes a setting or leaves the game, and say so in the file.

**Privacy of the files.** No game name, process name, path, host name, user name or IP in any record or in `bench.json`; mode 0600; folder 0700; no symlink following (same rules as `live.log`).

**Acceptance.** (a) A 7-minute run produces `bench.json` that the owner can paste or attach; (b) stats off costs nothing measurable (frame time with `STATS=0` equals today's); (c) a unit test feeds synthetic intervals and checks the percentiles and the derived values; (d) the Auditor computes the same derived numbers independently from the per-second records and the numbers match to rounding.

**Closing rule.** When `bench.json` from a 60 -> 90 run exists, the Auditor checks it against this list and closes item 1; items 3 to 9 are then decided from it (for item 9: if the layer cost on the game is small, the async queue is not worth building).

## 2. Refresh-rate clamp and cost warning (small; not implemented)

- **What it is.** The status file reports the screen's refresh rate, but nothing limits the live target to it.
- **Cost today.** On a fixed-refresh screen a target above the refresh rate only fills the present queue: each extra generated frame costs about 1.4 ms of GPU time (Builder's figure) and is never shown, and the queue adds latency.
- **If we do.** The target stops at the refresh rate (no clamp on a variable-refresh display, where the layer already sees an unknown refresh duration), and one warning appears when the layer's work per second exceeds the base frame time budget. No wasted GPU work, bounded latency. Cost: a few lines and a test.
- **If we do not.** A user (or the plugin slider) can ask for a target the screen cannot show and pay for frames that are thrown away.
- **Recommendation.** Do it, right after the measurement logic.
- **Decide with.** Nothing to measure; check behaviour on a fixed-refresh and a variable-refresh display.

## 3. Flow passes: tiling, then precision (the biggest lever)

- **Context.** Builder's Deck numbers (60 fps game, 90 fps output): flow 4.84 ms per pair, interpolation 1.43 ms per generated frame, about 42 % of GPU time, about 69 % of that is flow; game-thread waits are about 0.1 ms per frame.
- **What it is.** The flow engine reads 32-bit float luma pyramids and flow fields; block matching is the expensive part (earlier Deck timings: coarse search 1.25 ms, refine at the 1/8 level 1.45 ms, refine at the 1/4 level 1.75 ms, pyramid 0.3 ms per pair).
- **If we do, in this order.** (1) Shared-memory tiling of the block-matching loops: a pure speed-up with identical output, no look test needed. (2) 16-bit storage for the pyramid, the masks and the flow fields: halves their bandwidth. (3) 16-bit arithmetic: about twice the ALU rate on this GPU class. (4) 8-bit integer dot products for the matching sums: up to four times fewer operations on 8-bit luma, but the tie-break constants are smaller than one 8-bit level, and the CPU reference must mirror every rounding. Warp coordinates and the sub-pixel fit stay 32-bit. Risks: precision regressions that show as smearing; measure with the offline error tool and a look test after each step.
- **If we do not.** Flow stays at 4.84 to 6.09 ms per pair (Deck, two runs), 48 to 69 % of the layer's GPU cost.
- **Recommendation.** Highest expected gain; start with tiling, stop at the first step that reaches the goal.
- **Decide with.** Per-pass timestamps in `bench.json`; offline error numbers; look test.

## 4. Ring and output format (16-bit float ring, 8-bit swapchain)

- **What it is.** Ring slots and the synthetic image are always 16-bit float (8 bytes per pixel); the swapchain is normally 8-bit (4 bytes). Every capture and every generated frame goes through a blit that converts the format.
- **Cost today (estimate).** About 8 MB per frame at the Deck's screen size against 4 MB; about 16 MB more traffic per generated frame, about 0.5 to 1.5 GB/s in total, around 1 to 2 % of the memory bandwidth, plus about 0.1 to 0.3 ms per blit. Small next to flow.
- **If we do** (ring in the swapchain's 8-bit format, plain copy instead of blit, 16-bit kept for 10-bit/HDR): half the ring memory (matters on shared memory), a little less traffic, copies can run on a transfer queue. Cost: blending happens in gamma space instead of linear light when the swapchain is sRGB (a very small visual difference, needs a look test), one rounding to 8 bits per frame (invisible, the output is 8-bit anyway), more format paths to build and test.
- **If we do not.** Linear-light blending stays, no new code paths; we keep paying the small bandwidth and blit cost.
- **Recommendation.** Do not change on a guess. Have the measurement logic time the capture blit and the final blit separately and log the swapchain format once. If the two blits plus the extra reads cost less than about 0.3 ms per frame, skip B. If much more, build the 8-bit ring behind a switch (`auto` / `16f`).
- **Decide with.** Per-blit timestamps from `bench.json`; look test of mid-tone blends.

## 5. Locking, command pools and descriptors

- **What it is.** The synthesis function holds the shared mutex for its whole body; the game thread and the presenter share one command pool and one fence ring; descriptor sets are rewritten for every generated frame. (The swapchain-image wait is already outside the mutex.)
- **Cost today.** Measured waits per game-thread frame: image acquire 0.07 ms, mutex 0.00, ring slot 0.00, fence 0.02. Descriptor writes are microseconds of CPU.
- **If we do** (per-thread pools and fence rings, descriptors written once per slot): removes rare long stalls when the swapchain runs out of images or many ticks queue up, and a little CPU. Cost: real concurrency work, risk of races that show up as hangs, more tests.
- **If we do not.** About 0.1 ms per frame of waiting stays; the risk is occasional spikes, not average cost.
- **Recommendation.** Medium priority. Do it only if `bench.json` shows frame-time spikes (95th/99th percentile far above the mean) or non-zero waits; the descriptor part is cheap and safe to do anyway.
- **Decide with.** Percentiles and the four wait counters.

## 6. Barriers that name every pipeline stage

- **What it is.** Capture, held-present and synthesis barriers use the "all commands" stage as source or destination.
- **Cost today (estimate).** Each one can stall unrelated work queued behind it (including the game's own work while we share its queue): small GPU bubbles, tens of microseconds each, several per frame.
- **If we do.** Name the real stages (transfer, compute) and drop the source stage where a semaphore already orders the work. Small edit, small gain, and a risk of removing a barrier that was needed, which only the synchronization validation layer catches.
- **If we do not.** Small bubbles continue; matters more while the layer shares the game's queue.
- **Recommendation.** Do it together with the validation run; low effort.
- **Decide with.** Warp and flow timestamps before and after; synchronization validation clean.

## 7. Timestamps and wakeups

- **What it is.** GPU timestamps are written at 5 sites on every pair, including bottom-of-pipe stamps that make the pipeline drain; the extrapolation tick wait is already a condition-variable wait.
- **Cost today (estimate).** Tens of microseconds per pair; small but constant.
- **If we do.** Stamps only while the stats switch is on (it is part of the measurement logic). Zero cost in normal use.
- **If we do not.** A tiny constant cost stays.
- **Recommendation.** Do it as part of section 1 (it comes for free there).

## 8. Fewer submits

- **What it is.** One submit per capture, per flow and per generated frame, each with a fence wait.
- **Cost today (estimate).** Around three submits per output frame at 90 fps; tens of microseconds of CPU each, well under 1 % of a core.
- **If we do.** Merge flow with the first warp; with timeline semaphores record several ticks at once. Saves little; adds complexity.
- **If we do not.** Nothing noticeable.
- **Recommendation.** Skip unless the CPU line shows the presenter thread using a large share of a core.

## 9. Async compute queue (last; only if the measurements justify it)

- **What it is.** Run flow and interpolation on a second, low-priority compute queue so the game's frames do not queue behind ours.
- **Cost today.** Flow and interpolation share the game's queue. How much that delays the game is exactly what the on/off comparison measures.
- **If we do.** The most it can give is the layer's measured cost on the game's frame time (the "upper bound" field in `bench.json`); on a small GPU there may be no idle units to overlap with, so the real gain can be well below that. Cost: device-creation changes, timeline semaphores, new failure modes (a hang if any cancelled or failed frame leaves a semaphore unsignalled), long validation work.
- **If we do not.** The game keeps paying the measured layer cost; GPU savings from sections 3 and 4 reduce it anyway.
- **Recommendation.** Last. Build only if `bench.json` shows a layer cost on the game that cheaper items cannot remove.
- **Review points for the design:**
  1. Bound the gain first (the on/off comparison).
  2. Request global priority LOW for the compute queue (needs no privilege) instead of relying on a queue priority of 0.0 in another family.
  3. Timeline-semaphore feature: if the app's feature chain already holds the Vulkan 1.2 features struct, set the bit there; never add both structs. Below API 1.2 also enable the extension.
  4. Every cancelled, failed or dropped tick must still signal its timeline value; add a test that cancels a tick between flow and synthesis; reset the values when the swapchain is recreated.
  5. Barriers order work inside one queue only: unpin a ring slot only after the GPU reached its timeline value, never at submit time; assert it.
  6. The compute queue needs its own mutex and a place in the "lock all queues" list used by the wait-idle hooks; if the family has no spare queue, fall back and log it once.
  7. Validate with synchronization validation switched on, including a long soak with extrapolation on.

## 10. Needed from the owner (Deck)

- One `bench.json` in a GPU-bound scene: the game's own frame time above 16.7 ms and not equal to a cap, base-fps cap raised above what the game reaches, a static view held for the whole run, target 90 (item 1 produces it). The first run (cap 30) does not count for item 9.
- A look test of anything that changes the output (ring format, direct write into the swapchain image).

## 11. Checked and closed (owner agrees, 22:24)

Private config folder and files (0700/0600, no symlink following, no temp-directory fallback); the layer's log no longer takes over the game's error output; plugin library check (owner, permissions, file name ends in `.so`); plugin log read from the end of the file; lockfile for the plugin; temporary files in the deploy script; path escaping in the install scripts; default 45 fps cap documented in the README.

## 12. Rules for these notes

No session links, no real name, email, IP, host name or home path; commits as `Claude (Auditor)`, no session line in the message; fast-forward pushes only; no merges or rewrites by the Auditor.

## 13. Review log, sanitizer results, measurements and residuals (2026-11-01, `main` c5230b8; updated for 8226bc8)

How to read it. A PASS covers only the exact head named. "Measured" means run in the Auditor's container (Arch, GCC 16, AddressSanitizer, UndefinedBehaviorSanitizer, ThreadSanitizer, mock-driver tests); "read" means from the source. Nothing here is a Deck measurement unless it says so, and the Deck runs were done by the Tester and recomputed by the Auditor from the raw files.

**Reviews with a PASS (all merged).**

| PR | Head reviewed | Subject |
|---|---|---|
| #35 | `af78dc7` | hidden acquire: reserve the command buffer entry while `gpu_mu` is dropped (#25) |
| #36 | `92225121bdd7e919cbfaca47b060fa272e5e612e` | failure paths of `EndAndSubmit`, `DumpImage` and the hidden acquire (#26, #28, part of #27) |
| #38 | `280a5e8081c363cd0d2aec92362b6b58dd099d65` | hook hardening (#23, #29, #30) |
| #39 | `41ed60c5731b3aefc31336447659507cb34b99cc` | Decky backend: no symlink following as root, tolerant settings (#21, #22, #24) |
| #40 | `be2cb918573bb6c06046aa65117ebb7d30c6fd90` | docs and stale text (#31, #32, #34) |
| #37 | `1faf1c4ab079b3c4bd2b4d5dd912f7b3f5abaf4a` | scheduler: a tick on the next native frame is that frame, not a pass at t = 1 |
| #45 | `4da63852dfd5f8e02e265b9cd4646644ca9dc8d9` | scheduler: re-anchor the output clock when `TARGET_FPS` changes while running |
| #46 | `2b52c367b0618672401cf178b4fe3019028a737b` | give back the swapchain image acquired on the hidden path when a later step fails (#27) |
| #47 | `8759be66d6e68c28bd7ce25748d51341a6941b09` | the global registries are never destroyed (#41) |
| #48 | `4b5ce90278886fde3f8b677266e3f10561e681e4` | `last_queue` atomic, `gpu` atomic pointer, idle-flush test (#33) |
| #56 | `18bef0a41b5e88f1603943e9a6b144fd76012375` | comment-only: the stale `MIN_GAIN` 1.12 comments in `output_clock.hpp` (squash-merged as `afdec48`) |
| #51 | `8f3f5f3e232c029eadfda5707b2160023aef1efd` | ROADMAP: the deferred D3D hint provider (docs; merged) |
| #52 | `79480452f10862b18687df588a61369a2d7e6093` | ROADMAP: the deferred upscaler hook (docs; merged) |
| #53 | `8a01f86cf61a50aa046c9c13058c10ff8a48449c` | README, DESIGN: the prior-art credit line removed (docs; merged; the owner's decision) |
| #54 | `920eeebda9d1ef9a9ddd9dba72657729880ce446` | STAGE3.md, STAGE4.md restored and brought up to date (docs; merged) |
| #55 | `2bea6bded5ddac6ff27cf97e4b9ed9816106dcc0` | all documents compared with the code (docs; merged) |
| #57 | `7a17fde69242cb9470e738f3050984c5001836ab` | STATUS.md "Build state", ROADMAP line 17 (docs; merged; STATUS.md sha256 `b5e2c954…d25a9e`) |
| #58 | `e0123ad004b84b6c58da18c51061bfab91193b5e` | STAGE3.md line 66: `ENABLED=0` keeps the cap in a benchmark's off seconds (docs; squash-merged as `9906c68`) |
| #59 | `1288bbb3225161f827e83973692427a6791ae8d4` | STATUS.md "Build state" at main 9906c68 (docs; squash-merged as `f550d13`; STATUS.md sha256 `0eab178e…09f9ed1`) |
| #61 | `97f2eb44e13598a11a6cbf37277c1be03f20683e` | STATUS.md "Hardware" bullet, b8-l2 result and its scope, two stale "not run" sentences (docs; squash-merged as `6aed42e`; STATUS.md sha256 `64c5f186…402f`) |
| #62 | `3794d137ba1b8bfd496fb6ce5064e6d11ee5ccba` | `VKIFRAME_DUMP` reads the images into host memory, files written after the window; `VKIFRAME_DUMP_FRAMES` (5 to 60) and `times.txt`. Heads `3e0b123e` (an uncaught `bad_alloc` in the flush, nothing logged when the game ends inside the window) and `e5ad0eb6` (`<stdexcept>` missing, build break with GCC 16) got CHANGES. Squash `3a8aa2a`. |
| #65 | `534c4ce636359c4c73b537f591dd6f79143a5310` | `VKIFRAME_DUMP`: GPU copies into preallocated host buffers recorded in the flow and synthesis command buffers, one wait after the window, files from the mapped memory (replaces #63; the extra TRANSFER_SRC commit that landed on #63 was split out). Heads `749a4363` (buffers freed under a copy that may still run after a fence timeout) got CHANGES. Squash `59e98e9`. |
| #66 | `b82cf825244a773338885b66042703f0d3a156c5` | TRANSFER_SRC only on `flow_fwd`, `flow_bwd` (final level) and the mask, only with `VKIFRAME_DUMP`, format-feature query, dropped cleanly when unsupported; mock test with the usage refused. Squash `da13da6`. |
| #67 | `c71617b327ec9f6070d08f623b46e730f9deaa77` | CreateInstance: the layer tries the extra instance extensions and retries with fewer instead of asking the layers below with a null instance (rework of #42; Mesa's device-select crash). Head `87ce33e7` got CHANGES (an error other than EXTENSION_NOT_PRESENT was returned at once). Squash `64267af`. |
| #69 | `9cef98b58c6a3b86885d426a13fb3ff9e64cf7f2` | static libstdc++/libgcc, exactly three exported symbols (version script, `scripts/check_so.sh`, a host-allocator load test). Head `0b34dd90` got CHANGES (the test control was optimised away by GCC 16). Squash `51a9bfa`. |
| #68 | `6696a7b35cee433a097b2aa879bf1254524997dd` | `live.log` and `stats.jsonl` capped at 8 MiB (one old file), no `stats.jsonl` rotation inside a benchmark run, throttled repeating lines when `live.log` cannot be opened, plugin log and temp files capped; the dump logs the window end and the flush time. Head `52a80c12` got CHANGES (a data race in the throttle counter, periodic lines not covered, rotation inside a benchmark). Squash `8956ef3`. |
| #70 | `fef5945014bc1447131582f7831360d7724a8a6c` | version string without a build time (version, commit, commit time), reproducible builds, schema defaults aligned with the layer and a test that keeps them aligned, `INTERPOLATE` defaults to 1 (owner). Head `73db050c` got CHANGES (a sentence claiming agreeing defaults while `INTERPOLATE` differed); `784ac599` passed, `fef59450` is the head rebased onto main with the new default. Squash `db46f6c`. |
| #71 | `331b9a00cfc24f54a82526020fef86ce7e400496` | `install.sh` / `uninstall.sh` from an extracted release folder (no root, idempotent, rename-based copy, `--purge`). Head `39873bf1` got CHANGES (a library path with `&` produced a broken manifest on bash 5.2 and later; shellcheck findings; relative XDG paths in `uninstall.sh`). Squash `49c1e90`. |
| #72 | `0d07ad815dcc7274bd032d745ff50228b53d67d3` | Decky zip builder (deterministic, self-checked, user README, archive NOTICE header, LGPL-2.1 source paragraph for the bundled `@decky/api`, no dangling source-map comment). Head `5b71a658` got CHANGES. Squash `170c21f`. |
| #73 | `c13b53bad489d2d02a06dabe15693ccf3b1c5825` | opt-in one-sided fill (`VKIFRAME_ONESIDED_FILL`, EXPERIMENTAL gate, game-start key). Flag-off identity rerun by the Auditor: old and new `flow_interp.comp` byte-identical for 10 of 10 outputs on random inputs (Lavapipe), the flag on differs. Squash `cdbc63d`. |
| #74 | `13160e8301482696164fee41f3d99163a0bc8d55` | swapchain capacity: one capacity C for the swapchain size and the live clamp, EXPERIMENTAL no longer adds images by itself, C limited by the granted image count, `synth_capacity` in `status.json`, pass-through when a surface cannot hold even one synthetic frame. Head `9559207d` got CHANGES (capacity 1 forced where no image is left). Squash `4443f66`. |
| #75 | `8a3887b9e542ea017ea23177ed13bd9989f6ddd9` | `scripts/package.sh`: pinned toolchain only, two builds from two checkouts must be byte-identical, strip then `check_so.sh`, shellcheck required, deterministic tar and zip, `SHA256SUMS` and `SHA256SUMS-contents`, user README and NOTICE header for the tar. Head `2e8509d0` got CHANGES; one of my three points (CMake not checked) was wrong and is retracted. Squash `969683e`. |

**PASS, merged as `0370bbf` (2026-11-01):** #76 head `b2f46d05a8b46b90e79ccc71964424021162bcb4` (release polish: the panel texts checked one by one against the code and the docs, the alpha line in the tar README generator; no defaults, ranges, keys or behaviour changed; the panel width on the Deck is not checked). ROADMAP.md (the post-alpha GPU-load plan, two new sections): PASS on file sha256 `5c86fbee05300073fb3205e097992b574c733a75c5de7ba137b82b67a8b321fe`, merged as `e15b946` (#77); the file on `main` 8226bc8 has that hash. Every merged PR above was checked after the merge: the tree of `main` equals the tree of the passed heads merged in order (git merge-tree).

What the last four showed. #45: the plans are bit-identical to the old header at a steady period (1.2 million random plans per mode, compared byte for byte), the grid stays continuous across 3133 random period switches, and five of the six new tests fail on the old header. #46: every step after a hidden acquire that can fail is covered (the index check and the three failure points of `EndAndSubmit`); the PR's test gives 131 acquired and 131 presented against 131 and 130 on the old source; the give-back's own failure paths were checked with throwaway test variants, not in the repo. #47: ThreadSanitizer library-side reports go from 9 to 1. #48: ThreadSanitizer reports are 9 before and 9 after.

**Docs against code (read, 2026-11-01; PM PRs #54 and #55, checked on `main` with code unchanged since c5230b8).** The tips of the branches were fetched at the time of the check; the heads that were open then were #54 `920eeebda9d1ef9a9ddd9dba72657729880ce446` and #55 `eb34fc31f2b775c3726fda2d8089bbc92d148045` (later `2bea6bded5ddac6ff27cf97e4b9ed9816106dcc0`), and the PASS on exact heads came afterwards (table above); this first check was not a PASS on those heads. Nothing was compiled or run. Checked and matching: the re-anchor (#45), the next-native-frame tick (#37, epsilon 0.02), the hidden-path give-back (#46), the registries (#47), the atomic `gpu` and `last_queue` (#48), the idle flush (200 ms) and stall (100 ms) limits, the queue mutex hooks, the defaults and live ranges in the README tables, the `FRAMEGEN_` fallback, the 4-level flow pyramid with the exhaustive plus or minus 4 search, the gate (dwell 8, exit 1 + (g - 1) / 4), the phase-lock window, and the "not built" names (`VKIFRAME_ASYNC`, `DEBUG_OVERLAY`, `DUMP_FLOW`, `MAX_VELOCITY_FALLBACK` are read nowhere). The F7 and F8 hotkeys and `vkiframes-ctl` are in no doc and not in the code. Not checked: the measured numbers in STATUS.md and STAGE5/6, and the ROADMAP text of #51 and #52. Findings, all sent to the Builder and passed on to the PM (items 1, 3 and 4 are applied in #55, which is merged; item 2 is #56, merged; the PASS on exact heads is in the table above): (1) DESIGN.md:45 says a tick within epsilon of B is always left to the next pair; the code defers it only when it is not the pair's first tick (`output_clock.hpp:129`). (2) The `MIN_GAIN` 1.12 code comments (fixed by #56). (3) STAGE3.md:195 "polled with timeout 0": each try is timeout 0, the hook retries until the game's own timeout. (4) README and LIVECONF give `~/.config/vkiframes`; the code uses `XDG_CONFIG_HOME` first and honours `VKIFRAME_LIVE_CONF` and `VKIFRAME_LIVE_LOG`. #55 is stacked on #54 and does not contain #53, so README and DESIGN overlap with #53: merge order matters.

**Sanitizers (measured, mock tests only).**
- ThreadSanitizer on `main` b056bb9: 4 of the 12 tests fail (`mock_synth`, `mock_cmdreuse`, `mock_detach`, `mock_extrap`). 9 reports point into the library: 8 are the exit-time teardown of #41 (`layer_dispatch.cpp:12` five times and `:15` once, plus the heap-use-after-free in `PresenterMain` at `layer_main.cpp:751` and `:755`), 1 is the `mock_detach` read below. The other warnings are the tests' own mock globals.
- On `main` c5230b8 (#46, #47, #48 merged): 1 library-side report, the `mock_detach` one. The same 4 tests still fail under ThreadSanitizer because of their own mock globals.
- AddressSanitizer and UndefinedBehaviorSanitizer: no memory error and no runtime error on b056bb9 and on the heads of #46, #47 and #48 (not re-run on c5230b8 itself). LeakSanitizer lists only the tests' own mock allocations (10532 bytes in 45 allocations once, 256 bytes in 32 allocations five times).
- Method note: run `ctest` with `--output-on-failure` or `-V`, or the sanitizer text is lost; UndefinedBehaviorSanitizer prints and continues.

**Deck measurements recomputed (the Tester ran them).**
- `STATS=1` over `STATS=0` (568d786, 6 pairs): +0.0044 ms, standard error 0.0152 ms, 95 % interval -0.035 to +0.043 ms: "no cost above 0.05 ms at this resolution", narrowly; a 10 s or 15 s settle, the median, or a clock offset of -5 s move it to "inconclusive". It does not cover the always-on timestamps.
- `SYNTH_MARGIN=0` with #37 (`1faf1c4`, library sha256 `bb88489f06f077612f7c878ab08d4ec72ec7011349facc5c427fcad859b0627e`), stationary scene, native 30, shown 60: 1.015 passes per game frame against 1.619 on 568d786; 330 of 341 seconds within 1.0 +/- 0.1 (328 if the two seconds at exactly 1.100 are counted outside); no late, dropped or cancelled frames. Layer GPU time per game frame, derived from means: 5.932 ms against 6.333 ms. One run, one scene; the cap hides the layer's cost.
- The same setting on `bff7aa7` (library sha256 `a1a5053f99ba275fc2a3d3a25ccd4d557760dcc6c0b754328c98302168d0c77a`) in a moving scene with fps dips: not valid under the native-30 rule. 1.258 passes per game frame (up to 2.82); the stationary segment gives 1.014; seconds with uneven frame times average 1.76 against 1.07. Nothing about cost or saving can be read from it.
- **The b8-l2 baseline on `c5230b8` (library sha256 `4ab92b6267d058b1e13d75b542a46ff80e08f3c3bc5fef7620984647a949b5d4`) was run (Tester, one run, 6 pairs) and is accepted (Auditor, recomputed from the raw files; the report's sha256 for the bench json `1b9e95af…c5` and the stats file `08178d4b…87b2` match).** Layer cost mean 5.111 ms, standard error 0.016 (pairs 5.111, 5.102, 5.181, 5.078, 5.117, 5.075; the first series on 568d786 gave 5.136); the on-minus-off frame time is 23.090 - 17.979 = 5.111 ms. The stats file held 791 older lines; this run is lines 792 to 1534 (743 records: 345 off, 344 on, 30 warm-up, 24 settle). Late, dropped and cancelled are 0 in every record (the older lines too). Shown fps 89.996 on average, 88.97 to 90.90 in single seconds; layer-off native 55.62 fps (17.98 ms). Read from the report, not checked by the Auditor: the Decky panel was reset 2 min 11 s and the three baseline lines rewritten 36 s before the bench; nothing changed during it. The six pair values were not rebuilt from the jsonl (its segments are not marked); a coarser pairing gave a mean of 5.115. **Scope:** the acceptance covers only the saved-game scene of the 568d786 series, as a regression check of this library against that series, one run. It says nothing about other games, scenes or resolutions, and nothing about the game used for the fill-test captures (frames 1280x720): its cost needs its own baseline and series (owner's call). The report did not give the swapchain size.
- The offline check of the one-sided fill was recomputed (see "Fill check" below). The raw frames were opened for that purpose only, on the owner's word as relayed by the PM; nothing was copied out of the folder and nothing in this file is a picture or a crop.


**Library hashes and how far they are tied to a commit.** A binary is tied to a commit only by the Builder's statement and by the strings embedded in it; the Auditor's own builds differ (compiler, glibc, flags) and a byte comparison with them means nothing. Hashes seen: `4ab92b6267d058b1e13d75b542a46ff80e08f3c3bc5fef7620984647a949b5d4` (c5230b8, the b8-l2 library; the Tester computed it on the Deck); `cf078a1d7a93e13336b099d2022b9315e74bc1fde3b26481b94c446fbf1b9e7c` (built by the Builder from `534c4ce6` in a clean worktree, staged as lib-534c4ce, used for the five Deck captures; the embedded build time string and the strings of the capture path were checked, not the commit); `07d21c29...aaea` (the capacity library, stripped, built with `package.sh` from main 969683e, staged for the Tester; the Builder's statement). **Withdrawn:** `859d8423...a33d` (said to be lib-534c4ce, overwritten by a later build) and `3d0563cb...8fcb`, `5a027e54...`, `ffee9c82...` and the other per-PR hashes the Builder gave while reviewing: they were builds of PR heads and say nothing about the release. The release library will be the one `scripts/package.sh` builds in the pinned Ubuntu 22.04 container from the cut commit; its hash and the hashes of the tar and the zip are named in the Auditor's final report.

**Deck captures and the capture path (read, 2026-11-01).** Five Deck captures with lib-534c4ce (30 pairs each, 45 files, 290 MB): the pool was 44 buffers in host-cached memory, "capture complete, 45 of 45 files written" every time, the steps between the 30 frames were 21.1 to 23.2 ms (medians 22.2 to 22.3 ms, window 643 to 644 ms) against 74 to 410 ms with the earlier path; the owner reported a hitch at the end of each capture, which is the write of the files (about 0.3 s under the GPU lock, one stall by design); a hitch "at the start" of one capture was not placed. One game, one run each. The older stalled-path sets are not used.

**Fill check (recomputed, 2026-11-01).** 25 triplets (A, TRUTH, B = frames k, k+1, k+2) from five captures (dash k = 8, 11, 14, 17, 20; forest 3, 8, 13, 18, 23; sprite 16, 19, 22, 25, 27; pan 3, 8, 13, 18, 23; slow 3, 8, 13, 18, 23), 1280x720, chosen by fixed spacing and not by looking at images. The capture hash lists matched all 45 files of each set. The Builder's tool (`fill_eval.cpp`, sha256 `a9b610b87ede90ca72bb87bcd7f9e48cb7335fabaf97a53918861ed40db4ae18`) was rebuilt by the Auditor with GCC 16 against `tools/interp_eval/interp_eval.cpp` and `include/` of main; all 325 rows (13 methods x 25) equal the Builder's. The plain cross-fade row was recomputed with separate code (0 differences). Result for the four one-sided variants against the layer's two-sided interpolation (100 comparisons per measure, no ties): worse on all pixels in 90 and on hard pixels in 93; mean penalty `fill_consist` +3.0 % / +6.0 %, `fill_mask_bg` +3.7 / +7.3, `fill_small` +4.3 / +8.5, `fill_mask_nobg` +6.9 / +12.6 (all / hard); `fill_consist` is better on hard pixels in 4 of 25 triplets (-1.0 to -2.7 %). The layer's two-sided interpolation against the plain cross-fade: better on all pixels in 16 of 25 triplets and on hard pixels in 19 of 25 (mean error 0.01702 against 0.02438 and 0.06838 against 0.08966); it loses in the near-static triplets (hard-pixel share below about 3 %: slow, one dash triplet) by at most 0.0008 mean error. It beats the single-sided interpolation only marginally (-0.9 % all, -2.1 % hard; better in 10 and 15 of 25 triplets). A forward-splatting variant in the same tool (`splat_fill_consist`) is 1.8 % and 2.8 % lower in mean error than the two-sided form and better in 15 and 17 of 25 triplets; it needs scattering on the GPU. Limits: the flows are the CPU reference's, without the real shader's pan fallback and static-UI mask; the triplets are not independent samples (two sprite triplets share a frame); no statistical test. Conclusion given to the PM: no defect shown, a shader rework is not needed before the alpha, a measurement study with the real shader belongs after it (ROADMAP.md draft, PR #77).

**GPU-load feasibility (reasoned, 2026-11-01; nothing run on a Deck).** On a GPU-bound game the layer cost equals the sum of its own passes (b8-l2: flow 3.12 ms per pair + 2.08 passes x 0.906 ms = 5.00 ms against 5.11 ms measured). The flow shaders have no shared-memory reuse: by the code constants the coarse, refine-L3 and refine-L2 passes read about 57 M, 82 M and 104 M texels per pair at nearly the same loads per millisecond, so their time follows the number of loads; the work does not depend on the picture, so the drift of the flow time between runs (3.1 to 8.4 ms) is not the scene (cause not shown: GPU clock, power or memory contention are the candidates). Order proposed: measure first (clock and power logging, a standalone pass benchmark on the Deck), the no-code decisions after the look tests (`FLOW_LEVEL=3` -0.99 ms, `INTERP_BIDIR=0` -0.44 ms, together -1.39 ms in one scene; base-rate guidance), load reduction in the flow shaders (reasoned saving 40 to 60 % of the three heavy passes, uncertain by a factor of two), interpolation bandwidth, a near-static gate, a temporal predictor, the async queue last (probably never). Deriving the backward flow from the forward flow is not recommended.

**Release readiness (checked 2026-11-01; open items marked).**
- Anonymity: the staged library has no home path, user name or host name in its strings; the archives are made with owner 0/0, empty names, fixed times, sorted entries (`tar --sort=name --owner=0 --group=0 --numeric-owner --mtime`, `gzip -n`, `zip` with fixed times); my scan of the unpacked test archives found only the known false positive (`XDG_SESSION_TYPE`). Owner decisions: game names stay in the project's texts (not a finding), the git history and the account name stay as they are (reported only). **Open:** the scan of the FINAL artifacts for real name, email, IP, host, Steam ID, save name and session links.
- Install and uninstall (#71, passed): no root, idempotent, rename-based copy, the manifest from the template with a JSON-escaped path, `--purge`; the Decky plugin and the scripts agree (test). **Open:** the Tester's clean install from the real artifacts on the Deck (script and Decky "install from ZIP").
- Safe defaults: nothing happens without `VKIFRAME_ENABLE=1`; with it the layer is on with default values (`ENABLED` 1, `INTERPOLATE` 1 by the owner's decision, so the flow engine runs); dump, bench, experimental, extrapolation and the one-sided fill are off; `tests/test_defaults.cpp` keeps the panel schema and the layer in step. **Open:** the effective-defaults run on the Deck from the artifacts (effective values in `status.json` against the README table).
- Bounded files and logs (#68, passed). `VKIFRAME_LOG_FILE` is uncapped and documented as for short runs.
- Portability: the library needs glibc 2.34, nothing else (static libstdc++/libgcc, three exported symbols, `check_so.sh` in `package.sh` and as a ctest); the floor comes from the build host, so the release is built in the pinned Ubuntu 22.04 container. Tested on one Deck (Game Mode); other GPUs and Desktop Mode untested. **Open:** `nm -D`, `readelf -d` and `objdump -T` on the final stripped library.
- Licences: `LICENSE` (Apache-2.0) and `NOTICE` in the tar; the Decky zip carries the plugin's BSD-3 with the original Steam Deck Homebrew copyright line, the layer's licence and notice, and `THIRD_PARTY_LICENSES.txt` (`@decky/api` LGPL-2.1 in full with the source and rebuild paragraph, `react-icons` MIT with the Font Awesome CC BY 4.0 attribution, `tslib` 0BSD). **Open:** check these in the final zip.
- README known issues. **Open:** the README draft has not been reviewed yet: cost about 5 ms at 720p in one scene on the Deck, thin-line distortion, forced FIFO, one saved-game baseline, Game Mode only tested, no hotkeys or overlay, the panel is the only live control and outside edits of `live.conf` are overwritten, an out-of-memory in the layer ends the game, up to three create calls on the error path, the cost of the default `INTERPOLATE=1` and of the one-sided fill (Deck cost unmeasured), the capacity rules, the unsigned checksum file.
- Still to do before the alpha (as of this update): the Tester's four Game Mode starts for #74; the cut of the artifacts from main in the pinned container with `scripts/package.sh --builder`; the Auditor's final-artifact checks and PASS on named hashes; the Tester's clean install; the STATUS.md and README reviews. Publishing is the owner's act.
**Residuals still open.**
1. `layer_main.cpp:954` (a plain read of `sc->presenter` in `Hook_QueuePresentKHR`) against `:804` (`StopPresenter` sets it to null on the detach path): triggered only by `mock_detach`, which presents while the swapchain is destroyed. Vulkan requires external synchronization of presenting on and destroying the same swapchain, so this is test-only (the Builder's reading; it matches the Auditor's). Not changed.
2. `layer_main.cpp:768` to `:770`: the presenter's watchdog log reads `sc->frames` and `sc->gpu->held.valid`, `.present_id`, `.fence` without a lock. Log only, after at least 2 s without a present and at most every 5 s, but the fields are plain and written under `flow_mu` or `mu`, so the atomic pointer of #48 does not make that read race-free. No issue opened. Small fix: take `flow_mu` with a try-lock for that log line, or leave the fields out.
3. The PR #48 text says the presenter's reads of `sc->gpu` and `sc->last_queue` are ordered by `last_present_ns`. That is true for `last_queue` (written at `:877`, just before the store at `:878`), where the atomic closes a real but tiny formal race (a later write against the unlocked read at `:752`). It is not the reason for `sc->gpu`: that write (`:896`) comes after the store, and it is ordered by thread creation (`StartPresenter` runs after it) and by stopping the presenter before the null write at destroy. The atomic on `gpu` is harmless and not needed. The calibration fields are guarded by `sc.mu`.
4. #46: the given-back image is presented after an UNDEFINED to PRESENT_SRC transition, so its content is undefined (the code comment says it shows what it held last); the present goes straight to the driver, not through `PresentWithTiming`; the paths inside the give-back are best effort.
5. #47: if the loader unloads the layer library while the process goes on, the five heap objects stay allocated (a few hundred bytes per load). No search was made for a static initialiser in another translation unit that calls into the registries.
6. PR #42 is closed as superseded by #67 (the null-instance lookup is gone; the layer may now call the next `vkCreateInstance` up to three times on the error path, which the mock cannot show how the real loader copes with).
7. `stats.cpp:186` says that with `SYNTH_MARGIN=0` the native frame "costs no pass". Measured: true in a stationary scene with the phase locked and offset 0 (1.015), not in a moving scene with uneven frame times (1.258 up to 2.82).
8. Image count: since #74 the swapchain is built for `app_min + C + 2` images with `C = min(limit, max(MAX_SYNTHETIC at game start, 4))` (`EXPERIMENTAL` only raises the ceiling); an application with a fixed array of 8 swapchain images still breaks when the sum exceeds 8; the decision (document, or a cap setting) is the owner's. The live clamp is process-wide (the latest swapchain wins) and lowers the live value for good when a smaller swapchain appears; a swapchain recreated mid-game re-reads `live.conf` and can get a larger capacity without a restart.
9. Not covered by the audit: the `GpuFlow` barriers and flow command recording in `optical_flow.cpp` apart from the dump copies (read, and run on the Builder's host GPU by the Builder), `flow_ref.hpp`, `timing_engine.hpp`, the plugin front end, the root-only parts of the Decky backend (read, not run), the panel width of the longest texts, and the image quality of flow level 3 and of bidirectional off. Nothing was measured on the Deck by the Auditor.
10. #68: a thread inside `fprintf` on the retired `live.log` stream across two rotations could touch a closed stream (hours of log apart); accepted, documented in the PR. The plugin rotates `live.log` only when `status.json` is stale.
11. #69: an out-of-memory in a standard container inside the layer throws `std::bad_alloc` that nobody catches, so the game aborts; documented in the README.
12. #74: the pass-through path (a surface that cannot hold one synthetic frame) returns before the present-timing setup, so that swapchain presents without timing; harmless.
13. The live-file work is designed (a second file for ctl and the overlay, per-key precedence, a stateless base rule, `synth_capacity` and the effective values in `status.json`) and reviewed as a design, not built; `vkiframes-ctl` and the in-game overlay are not started (the UI is on hold, PR #64). Until they land, outside edits of `live.conf` are overwritten by the plugin.
14. Decisions pending with the owner: whether to ship the straight version of the one-sided fill (no field-resolution pre-pass); the GPU-load plan and the look tests for `FLOW_LEVEL=3` and `INTERP_BIDIR=0`; the post-alpha shader study.
