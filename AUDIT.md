# Audit notes (Auditor)

Open items only, checked against `main` 6c0e949. Findings go on the board issue; this file holds the detail. Not compiled or run by the Auditor: everything below is from reading the source and the GitHub objects. Nothing here is a Deck measurement unless it says so.

## 1. Open hit: a session link in a pull request body

The body of pull request 1 (docs-only) ends with a link to one specific Claude Code session (a real id, not the generic footer). Files, commit messages, issue and comment text are clean. Fix (Builder or PM): edit the description to the plain `https://claude.ai/code` footer or delete the line. The cause is the footer a tool adds to pull request descriptions; check every new pull request body before it is merged.

## 2. Layer: refresh-rate clamp and cost warning (not implemented)

The status file reports `refresh_hz`, but nothing limits the live `TARGET_FPS` to it. On a fixed-refresh display a target above the refresh rate only fills the present queue (more latency, no more frames shown). Wanted: clamp the live target to the reported refresh rate unless the display is variable-refresh; log one warning when (generated frames per second x warp ms) plus (pairs per second x flow ms) exceeds the base frame time budget.

## 3. GPU load and CPU<->GPU load (priority list)

State on `main`, by reading the code:

| Item | Finding | Status |
|---|---|---|
| B. Ring and output format | Ring slots and the synth image are always RGBA16F; the swapchain is 8-bit. Each capture and each generated frame costs a full-frame blit with format conversion (about 16.6 MB per 1080p frame in 16F). On the Deck the CPU shares this memory. Wanted: ring format follows the swapchain (16F only for 10-bit/HDR), copy instead of blit where formats match, write the warp straight into the swapchain image when storage usage is allowed (blit as fallback). | open |
| C. Locking and descriptors | The synthesis function holds the shared mutex for its whole body; one command pool and one fence ring are shared by the game thread and the presenter; descriptor sets are rewritten for every generated frame. Wanted: per-thread pools and fence rings, descriptors written once per ring slot (or an indexed array plus slot numbers in push constants). Acquire wait is already outside the mutex. | open (acquire part done) |
| D. Barriers | Capture, held-present and synthesis barriers still name `ALL_COMMANDS` as source or destination stage, which stalls other work in the queue. Name the real stages; where a semaphore already orders the work the swapchain barrier needs no source stage. | open |
| E. Timestamps and wakeups | GPU timestamps are written at 5 sites on every pair (bottom-of-pipe stamps add pipeline drains); keep them for a stats switch or sample one pair in N. The extrapolation tick wait is already a condition-variable wait. | open (wait part done) |
| F. Fewer submits | One submit per capture, per flow, per generated frame, each with a fence wait. Merge flow with the first warp; with timeline semaphores several ticks can be recorded at once. Lowest value. | open |
| Precision | FP16 storage for pyramid and masks, FP16/INT8 arithmetic in block matching, shared-memory tiling; warp coordinates and the sub-pixel fit stay FP32. | backlog |

Builder's own Deck numbers (60 fps game, 90 fps output): flow 4.84 ms per pair, interpolation 1.43 ms per generated frame, about 42 % of GPU time, about 69 % of that is flow. The game-thread waits are already about 0.1 ms. So the layer's GPU cost, mostly flow, is the main lever; a second queue can only stop game frames queueing behind ours and overlap with idle units.

## 4. Async compute queue (stage 1 design): review points

1. Bound the gain first: game frame time with the layer on against off, same GPU-bound scene. That gap is the most a second queue can hide.
2. Ask for global priority LOW on the compute queue (needs no privilege) instead of relying on a queue priority of 0.0 in another family.
3. Timeline-semaphore feature: if the app's feature chain already holds the Vulkan 1.2 features struct, set the bit there; never add both structs. Below API 1.2 also enable the timeline-semaphore extension.
4. Most important: every cancelled, failed or dropped tick must still signal its timeline value, or the next wait hangs the GPU. Add a test that cancels a tick between flow and synthesis. Reset the values when the swapchain is recreated.
5. Cross-queue write-after-read: the existing barriers order work inside one queue only. Unpin a ring slot only after the GPU reached its timeline value, never at submit time; assert it.
6. The compute queue needs its own mutex and must be in the "lock all queues" list used by the queue/device wait-idle hooks. If the family has no spare queue, fall back and log it once.
7. Validate with synchronization validation switched on (plain validation misses cross-queue hazards), including a long soak with extrapolation on.

## 5. Checked and closed

Private config folder and files (0700/0600, no symlink following, no temp-directory fallback); the layer's log no longer takes over the game's error output; plugin library check (owner, permissions, file name ends in `.so`); plugin log read from the end of the file; lockfile for the plugin; temporary files in the deploy script; path escaping in the install scripts; default 45 fps cap documented in the README.

## 6. Needed from the owner (Deck)

- The layer's CPU log line and the three wait counters from a 60 -> 90 run.
- Game frame time with the layer on and off in the same scene.
- A look test of anything that changes the output (ring format, direct write into the swapchain image).

## 7. Rules for these notes

No session links, no real name, email, IP, host name or home path; commits as `Claude (Auditor)`, no session line in the message; fast-forward pushes only; no merges or rewrites by the Auditor.
