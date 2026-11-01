# Asynchronous layer: design note (stage 1) and measurements so far

> **Status (2026-11-01, local time zone):** stage 3 is done and merged; stage 2 (per-thread command pools) and stage 1 (the compute queue, `VKIFRAME_ASYNC`) are not started. Nothing in the code reads `VKIFRAME_ASYNC`. The numbers below are from the Deck logs of the time (flow 4.84 ms per pair); later runs in STATUS.md show the flow cost drifting upward. AUDIT.md item 9 holds the async queue last in the order of work, only if the measurements show it is worth it.

Request: PM ("make the layer asynchronous"). Owner order: stage 3, then 2, then 1. One actor on the code (Builder); the Auditor reviews.
Nothing here is measured on the Deck yet unless a number says so.

## What the Deck log says today (60 fps game, 90 fps output, bidirectional interpolation)
- Game thread in the layer: 0.14-0.16 ms CPU per frame; presenter thread about 3 % of one core.
- Game thread waits per frame: acquire 0.07 ms, gpu lock 0.00, ring slot 0.00, cmd fence 0.02. These are the waits stage 2 and 3 remove; they are already near zero in this scenario, so the expected gain of stages 2 and 3 is small. They matter when the swapchain runs out of free images (stage 3 case) or when many ticks queue up.
- GPU cost: flow 4.84 ms per pair (60 pairs/s = 290 ms/s), interpolation 1.43 ms per generated frame (about 90/s = 130 ms/s): about 42 % of the GPU's time per second goes to the layer. On a single small GPU this competes with the game on the same graphics queue; this is what stage 1 targets.

## Stage 3 (done, in main)
The presenter waits for a free swapchain image in finite 1 ms tries, outside gpu_mu. VKIFRAME_ACQUIRE_WAIT_US=0 restores the old polling.

## Stage 2 (per-thread command pools/fences): not started
One VkCommandPool and one cb/fence ring (kCmdBuffers = 8) are shared by the game thread (capture) and the presenter (flow, synthesis), all under gpu_mu. Vulkan requires external synchronisation of a pool, so the lock cannot simply go. Plan: two pools (game thread: capture; presenter: flow + synthesis), each with its own cb/fence ring and descriptor-set rows, so BeginCmd on one side never waits for a fence the other side owns; gpu_mu keeps only ring-slot selection and pins. Expected gain per the log above: small.

## Stage 1 (compute queue): design
1. Device creation: if VKIFRAME_ASYNC=1 and the GPU has a compute-capable family other than the game's graphics family (RADV: compute-only family), add one queue to VkDeviceQueueCreateInfo with priority 0.0 (game queues keep theirs); otherwise fall back to the single-queue path and log why. Add timelineSemaphore (core 1.2) to the enabled features if the app did not.
2. Queue handle: vkGetDeviceQueue after creation, vkSetDeviceLoaderData on it, registered like the game's queues (QueueMutex).
3. What can move: flow (pure compute) and the interpolation/blend compute pass. The blit into the swapchain image (vkCmdBlitImage) needs graphics, and the capture blit too; both stay on the graphics queue.
4. Sharing: ring slots, synth image and flow images become VK_SHARING_MODE_CONCURRENT over both families (the ring already is when there are several families). All images stay in GENERAL, so no ownership transfers or layout changes.
5. Synchronisation (timeline semaphores, one per swapchain):
   - capture of frame N signals value c(N) on the graphics queue; flow of pair (A, B) on the compute queue waits for c(B) (c(A) < c(B)) and signals f(pair);
   - the synthesis of a tick: compute part on the compute queue waits f(pair) and the capture values, signals s(tick); the blit on the graphics queue waits s(tick) and the swapchain acquire semaphore, signals the present semaphore;
   - WAR: the next pair's flow waits for the previous pair's last synthesis s to finish reading the flow images; a ring slot is unpinned only when the CPU knows the compute work that reads it is done (timeline wait or fence), not when it was submitted.
6. Command buffers: a separate pool/ring/fences for the compute family (a pool is per family), descriptor-set and query-pool rows per entry as today.
7. Switch and baseline: VKIFRAME_ASYNC=0|1, default 0; the current single-queue path stays as the A/B baseline.
8. Gate: base frame time with the layer must not get worse than the previous stage, measured on the Deck (A/B logs); the Auditor reviews queue ownership, semaphore and barrier correctness and a validation-layer run.
9. Test plan: mock tests for the new submit order and semaphore values; real runs only on the Deck (the build sandbox has no GPU). Validation: VK_LOADER_LAYERS_ENABLE=*validation* in a distrobox run, or on the Deck with the validation layer installed.

## Risks to review
- RADV's compute queue priority is advisory; the Deck's single GPU may not overlap work, then the benefit is blocking removal, not compute (as the PM noted).
- Wrong WAR handling shows up as flicker or a hang, not a crash: needs the validation layer and a long soak on the Deck.
- Games that already create a compute queue family: we add one more queue to a family they use; the per-queue mutex covers submits, and queue count limits must be checked (queueCount).
