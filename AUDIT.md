# Audit notes (Auditor)

Open items, in priority order, checked against `main` 6c0e949. Findings go on the board issue; this file holds the detail. Not compiled or run by the Auditor: everything is from reading the source and the GitHub objects. Nothing is a Deck measurement unless it says so.

**Status 2026-11-01, `main` f550d13 (behaviour last changed at c5230b8; since then only documentation and, in #56 `afdec48`, two comments in `output_clock.hpp`):** sections 1 to 11 are the original plan (items 1 and 2 are in main; the "checked against `main` 6c0e949" line above is the date of the plan, not of the current state). Section 13 is the current review log: which heads were reviewed and passed, the sanitizer results, the measurements recomputed so far, the docs-against-code check, and the residuals that are still open. Open PR at this update: #42 (parked). #59 was squash-merged as `f550d13` on the head passed here. #56 and #58 were merged by squash on the heads passed here (`afdec48`, `9906c68`); the tree of main equals those heads for the files they touch.

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

## 13. Review log, sanitizer results, measurements and residuals (2026-11-01, `main` c5230b8; updated for f550d13)

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
- **The b8-l2 baseline on the final library has not been run.** Acceptance (Auditor): layer cost 5.1 +/- 0.2 ms from `layer_cost_pairs_ms`, no late, dropped or cancelled frames in the on-seconds, shown fps 90; every file and number labelled with the commit and the library sha256; the report gives the off-arm native fps and frame time (the first series had a layer-off frame of 17.99 ms), and says whether anything touched `live.conf` or the Decky panel before the bench started; read only against its own pair costs and standard error, never against a layer-off baseline from another game start. First series on 568d786: 5.136 ms (standard error 0.010).
- The offline check of the one-sided fill (`tools/interp_eval`): the Auditor recomputes the error numbers from the raw captures. The folder of private captures is not opened without the owner's word.

**Residuals still open.**
1. `layer_main.cpp:954` (a plain read of `sc->presenter` in `Hook_QueuePresentKHR`) against `:804` (`StopPresenter` sets it to null on the detach path): triggered only by `mock_detach`, which presents while the swapchain is destroyed. Vulkan requires external synchronization of presenting on and destroying the same swapchain, so this is test-only (the Builder's reading; it matches the Auditor's). Not changed.
2. `layer_main.cpp:768` to `:770`: the presenter's watchdog log reads `sc->frames` and `sc->gpu->held.valid`, `.present_id`, `.fence` without a lock. Log only, after at least 2 s without a present and at most every 5 s, but the fields are plain and written under `flow_mu` or `mu`, so the atomic pointer of #48 does not make that read race-free. No issue opened. Small fix: take `flow_mu` with a try-lock for that log line, or leave the fields out.
3. The PR #48 text says the presenter's reads of `sc->gpu` and `sc->last_queue` are ordered by `last_present_ns`. That is true for `last_queue` (written at `:877`, just before the store at `:878`), where the atomic closes a real but tiny formal race (a later write against the unlocked read at `:752`). It is not the reason for `sc->gpu`: that write (`:896`) comes after the store, and it is ordered by thread creation (`StartPresenter` runs after it) and by stopping the presenter before the null write at destroy. The atomic on `gpu` is harmless and not needed. The calibration fields are guarded by `sc.mu`.
4. #46: the given-back image is presented after an UNDEFINED to PRESENT_SRC transition, so its content is undefined (the code comment says it shows what it held last); the present goes straight to the driver, not through `PresentWithTiming`; the paths inside the give-back are best effort.
5. #47: if the loader unloads the layer library while the process goes on, the five heap objects stay allocated (a few hundred bytes per load). No search was made for a static initialiser in another translation unit that calls into the registries.
6. PR #42 (the layer asks the next layer for instance extension support with a null instance, and Mesa's device-select layer dereferences it; any Vulkan 1.0 application with a surface is hit; reproduced in the Auditor's container with a software driver, not on the Deck): parked, not reviewed.
7. `stats.cpp:186` says that with `SYNTH_MARGIN=0` the native frame "costs no pass". Measured: true in a stationary scene with the phase locked and offset 0 (1.015), not in a moving scene with uneven frame times (1.258 up to 2.82).
8. Image count (read from the source and shown with a scratch build in the Auditor's container; not re-checked in this pass): with hold on, the layer asks for the application's minimum plus `max(MAX_SYNTHETIC, 4)` plus 2 images (more with `EXPERIMENTAL`). An application with a fixed array of 8 swapchain images breaks. The decision (document, size from `MAX_SYNTHETIC`, or a cap setting) is the owner's.
9. Not covered by the audit: the `GpuFlow` barriers and command recording in `optical_flow.cpp`, `flow_ref.hpp`, `timing_engine.hpp`, the plugin front end, the root-only parts of the Decky backend (read, not run), and the image quality of flow level 3 and of bidirectional off. Nothing was measured on the Deck by the Auditor.
