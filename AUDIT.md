# Audit notes (Auditor)

Open items, in priority order, checked against `main` 6c0e949. Findings go on the board issue; this file holds the detail. Not compiled or run by the Auditor: everything is from reading the source and the GitHub objects. Nothing is a Deck measurement unless it says so.

Every item below uses the same layout: what it is, what it costs today, what happens if we do it, what happens if we do not, a recommendation, and how to decide. Numbers marked "estimate" are the Auditor's arithmetic from the Builder's Deck figures, not measurements; the measurement logic (section 1) replaces them with real ones.

## Priority order (owner, 22:24; reordered 06:00)

| # | Item | Effort | Expected gain | Risk | Status |
|---|---|---|---|---|---|
| 1 | Measurement logic and `bench.json` | medium | decides everything below | low | Builder, first |
| 2 | Refresh-rate clamp and cost warning | small | removes wasted frames and latency | low | next |
| 3 | Flow passes: tiling, then 16-bit, then 8-bit integer | medium to large | highest (flow is about 69 % of layer GPU time) | medium (look test) | after `bench.json` |
| 4 | Ring and output format | medium | small (estimate 1 to 2 % bandwidth) | medium | only if the blits measure above 0.3 ms |
| 5 | Locking, command pools, descriptors | medium | removes rare spikes | medium (races) | only if percentiles show spikes |
| 6 | Barriers | small | small | low (validation layer) | with the validation run |
| 7 | Timestamps only in stats mode | tiny | tiny | none | part of item 1 |
| 8 | Fewer submits | medium | negligible | medium | skip unless CPU share is high |
| 9 | Async compute queue | large | at most the layer's cost on the game | high (hangs) | last, gated by `bench.json` |

Closed: session link in a pull request body; checked security items (section 11). Needed from the owner: one `bench.json` from the Deck (section 10).

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
- **If we do not.** Flow stays at 4.84 ms per pair, 69 % of the layer's GPU cost.
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

- One `bench.json` from a 60 -> 90 run in a GPU-heavy scene (item 1 produces it).
- A look test of anything that changes the output (ring format, direct write into the swapchain image).

## 11. Checked and closed (owner agrees, 22:24)

Private config folder and files (0700/0600, no symlink following, no temp-directory fallback); the layer's log no longer takes over the game's error output; plugin library check (owner, permissions, file name ends in `.so`); plugin log read from the end of the file; lockfile for the plugin; temporary files in the deploy script; path escaping in the install scripts; default 45 fps cap documented in the README.

## 12. Rules for these notes

No session links, no real name, email, IP, host name or home path; commits as `Claude (Auditor)`, no session line in the message; fast-forward pushes only; no merges or rewrites by the Auditor.
