# Stage 3: Hold-One Presentation, Ring Buffer & Passthrough Engine

> **Status: done and running in the layer.** This page keeps the original Stage 3 design (overview, tuning table, success criteria, the Proton addenda) and brings every detail up to the code on `main`. Where the code differs from the first sketch, the text says so under "Differences from the first design". The build-order log of 3a to 3c-ii and its verification counts are kept in section 6. Settings use the `VKIFRAME_*` names; the old `FRAMEGEN_*` names are still read as a fallback.

## 1. Overview & Objectives

Stage 3 builds the frame plumbing before any optical flow exists. The layer will:

1. **Hold one frame.** Instead of presenting the game's image immediately, keep it (acquired, unpresented) and present it when the next native frame arrives, so that synthetic frames can be inserted *between* the two (DESIGN §4.1).
2. **Capture** each native frame into an internal RGBA16F ring buffer (DESIGN §4.3).
3. Run the **output clock** (one tick every 1/Target FPS), compute a fractional $t$ per tick, and **schedule** each present per DESIGN §2.D.
4. Produce the synthetic frames with a **placeholder blend** (`mix(A, B, t)`) in a compute pass. Stage 5 replaced it with the flow-guided interpolation; the blend stays as the fallback when flow or interpolation is off (`VKIFRAME_INTERPOLATE=0`) or fails.
5. Bypass completely (no hold, no added latency) while the game is at or above the target.

Why this differs from the original draft: calling `vkQueuePresentKHR` several times with the same present info just re-presents the *same* image, and presenting the game's image immediately leaves nowhere to put an in-between frame. A hidden-image path (the layer acquires its own swapchain images for synthetic frames) is required.

---

## 2. Repository Changes

The first design listed `frame_storage.hpp` and `compute_pass.hpp` as new files. The code ended up with fewer, larger units:

```text
include/
├── vk_iframes_layer.hpp     # function-pointer tables, SwapchainState, Config, TimingEngine glue
├── output_clock.hpp         # InterpolationGate, PairScheduler (pure logic, no Vulkan)
├── timing_engine.hpp        # EMA of the base frame time and the jitter estimate
├── gpu_frames.hpp           # GpuState: ring buffer, command buffers, fences, HeldFrame, reference counting of ring slots
├── gpu_internal.hpp         # helpers shared with the flow engine: Barrier, MemBarrier, BeginCmd, EndAndSubmit, CreateStorageImage
└── live_config.hpp          # settings that can change while a game runs (live.conf)
shaders/
└── blend.comp               # placeholder synthetic frame (plain temporal mix)
src/
├── layer_dispatch.cpp       # dispatch tables, registries (never destroyed), the queue-family map
├── layer_main.cpp          # device/swapchain/present hooks, PlanAndCount, ExecutePair, the presenter thread
├── gpu_frames.cpp          # ring buffer, capture, GpuSynthesize, flush of the held frame
└── live_config.cpp         # live.conf
```

The Vulkan entry points are listed once in the dispatch tables (`DeviceDispatch`, `InstanceDispatch`); a new call is added there and used as `dt.CmdBlitImage(...)` and so on. Calls go through the dispatch tables, never as bare `vk*` symbols.

---

## 3. Configuration

Only the settings Stage 3 uses are listed here; the full set is in `include/vk_iframes_layer.hpp` (`struct Config`) and README.md.

| Setting | Default | Meaning |
| :--- | :--- | :--- |
| `VKIFRAME_TARGET_FPS` | 60 (90 on the Steam Deck OLED) | Output rate. Also limited to the display's refresh rate unless the display is variable-refresh or the rate is unknown. Live. |
| `VKIFRAME_MIN_GAIN` | 1.01 in the code | Interpolate only above this base-time / target-time ratio; the gate exits at `1 + (enter - 1) / 4`. Live. |
| `VKIFRAME_MAX_SYNTHETIC` | 3 | $k_{max}$: most synthetic frames between two natives. Live up to the swapchain's headroom. |
| `VKIFRAME_BASE_FPS_CAP` | 45 | The layer limits the game's own present rate (0 = off). Live. |
| `VKIFRAME_PHASE_LOCK`, `VKIFRAME_PHASE_OFFSET` | on, 0.5 | See section 4.E. Live. |
| `VKIFRAME_SYNTH_MARGIN` | 0.15 | Keeps $t$ at least this far from 0 and 1 in an interpolated pair, so no native frame is shown as is (0 = off). Live. |
| `VKIFRAME_HOLD` | 1 | 0 keeps only the dry-run plan; nothing is held. |
| `VKIFRAME_SYNTH` | 1 | 0 holds frames but never makes synthetic ones (the 3b behaviour). |
| `VKIFRAME_PRESENTER` | 1 | 0 presents the ticks from the game thread (the 3c-i behaviour, no pacing thread). |
| `VKIFRAME_PACE_LEAD_US` | 2000 | The presenter calls present this long before a tick is due. |
| `VKIFRAME_ACQUIRE_WAIT_US` | 1000 | How long the presenter waits per try for a free swapchain image, outside the capture mutex (0 = the old polling under the mutex). |
| `VKIFRAME_CMD_BUFFERS` | 8 (4 to 32) | Command buffer entries per swapchain; a GPU-bound game makes the game thread wait on the fence of the oldest one. Live. |
| `VKIFRAME_ABSOLUTE` | 0 | Give each tick an absolute target time where the surface supports it (untested: the Wayland compositor on the desktop test machine offers none). |
| `VKIFRAME_DEBUG_RING` | 0 | Present the ring copy of each held frame (shows that capture works). |
| `VKIFRAME_TICKMARK` | 0 | Diagnostic: draws a marker and a stepping bar into the output frames. |

`ENABLED=0` (live) makes the whole layer a plain bypass, including no base-rate cap. The earlier draft of this page used 1.12 as the default for `MIN_GAIN`; the code's default is now 1.01, and the 1.12 value is what the simulation tuned (see the table in 4.E). DESIGN.md §2.B and STAGE6.md §4 say the same.

---

## 4. Implementation Details

### A. Device creation: required features

Stage 3 needs `synchronization2`. The application may not have enabled it, so `Hook_CreateDevice` walks the `pNext` chain: if the chain already contains `VkPhysicalDeviceVulkan13Features` or `VkPhysicalDeviceSynchronization2Features`, the layer reads the bit there; otherwise it asks the physical device (through `vkGetPhysicalDeviceFeatures2`, when the instance has the properties2 path) and adds the `VkPhysicalDeviceSynchronization2Features` structure and `VK_KHR_synchronization2` to the extension list. The KHR names are used throughout so Vulkan 1.0 apps (vkcube) work. If the device cannot do it, the layer logs `hold: synchronization2 not available on this device, frame holding is off` and the swapchain stays in passthrough.

Differences from the first design: `timelineSemaphore` is **not** requested. Everything runs on the present queue, so queue order plus barriers is enough; fences and binary semaphores carry the ordering. A timeline semaphore comes back only if flow ever moves to a separate compute queue (item 9 of AUDIT.md, last in the order of work; see ASYNC.md).

### B. Swapchain creation: image count, usage, checks

```cpp
// Hook_CreateSwapchainKHR (src/layer_main.cpp), condensed
bool hold = cfg.hold && dt->sync2 && !(ci.flags & VK_SWAPCHAIN_CREATE_PROTECTED_BIT_KHR);
// the surface must allow TRANSFER_SRC and TRANSFER_DST usage; the swapchain format must support BLIT_SRC and BLIT_DST;
// the ring format must support BLIT, SAMPLED_IMAGE and STORAGE_IMAGE. Anything missing logs why and leaves the swapchain in passthrough.

// Images that exist at once beyond what the game expects: 1 held + up to k_max queued synthetic + 1 hidden acquire.
uint32_t wanted = std::max(ci.minImageCount, kMinImageCount);               // kMinImageCount = 4
if (hold) wanted = std::max(wanted, app_min_images + std::max(cfg.max_synthetic, LiveMaxSyntheticLimit()) + 2);
if (caps.maxImageCount != 0) wanted = std::min(wanted, caps.maxImageCount);
if (hold && ci.minImageCount < app_min_images + 1) hold = false;            // no spare image: passthrough
if (hold) ci.imageUsage |= VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
```

The game sees a swapchain that simply has more images than it asked for; it keeps working with the indices it is handed. The count is sized for the largest $k_{max}$ the live settings allow, so a live increase of `MAX_SYNTHETIC` never needs a new swapchain. Present mode forcing (FIFO), present timing and present id 2 flags are Stage 2.

### C. Ring buffer (`GpuState` in `include/gpu_frames.hpp`, `src/gpu_frames.cpp`)

All ring images stay permanently in `VK_IMAGE_LAYOUT_GENERAL`. That layout is valid for blit, sampling and storage use, so only execution/memory barriers are needed and no layout transitions. The ring is created at the first present, after the present queue's family is known (the layer records it by hooking `vkGetDeviceQueue` and `vkGetDeviceQueue2`).

* **Format and size:** `VK_FORMAT_R16G16B16A16_SFLOAT`, the size of the swapchain, `kRingCapacity = 6` slots. The first design asked for at least 4 (held, newest, being captured, one in flight); 6 leaves room for the jobs that are queued for the presenter thread.
* **Sharing:** `EXCLUSIVE`. The first design said `CONCURRENT` once a separate compute family exists; no such family is used today (flow runs on the present queue), so the ring stays exclusive.
* **Reference counting:** ring slots are counted (`GpuPin` / `GpuUnpin`, `pins[]` under `gpu_mu`). A slot referenced by the held frame or by a queued job is never overwritten; when all six are pinned, the capture waits (`wait_ring_ns` in the stats) instead of corrupting a frame still needed.
* **Synthetic output:** one image (`synth`) is enough. Jobs run in queue order and each result is blitted into a swapchain image at once, so the second small ring of the first design was not needed.
* **Memory:** device-local; there is no fallback to memory type 0 when none matches (creation fails and holding stays off).
* **First use:** one barrier `UNDEFINED -> GENERAL` per image, recorded with the first capture.
* **Command buffers:** `n_cmd` entries per swapchain (the `CMD_BUFFERS` setting), each with its own fence, descriptor sets and query slots; they get their loader dispatch word through the loader's `SetDeviceLoaderData` callback.

### D. Barrier helper (Synchronization2)

`gpuint::Barrier` and `gpuint::MemBarrier` (`include/gpu_internal.hpp`) wrap `VkImageMemoryBarrier2` / `VkMemoryBarrier2` with `vkCmdPipelineBarrier2`. `MemBarrier` is the compute-to-compute dependency used between passes, since all images stay in `GENERAL`.

```cpp
void Barrier(const DeviceDispatch& dt, VkCommandBuffer cb, VkImage image, VkImageLayout from, VkImageLayout to,
             VkPipelineStageFlags2 src_stage, VkAccessFlags2 src_access, VkPipelineStageFlags2 dst_stage, VkAccessFlags2 dst_access);
```

### E. Output clock, per-tick $t$ and the bypass gate (`include/output_clock.hpp`)

This is pure logic with no Vulkan in it. `InterpolationGate` and `PairScheduler` replace the first sketch's `InterpolationGate`, `OutputClock` and the loop in the present hook. Times are doubles in CLOCK_MONOTONIC nanoseconds (exact to well under a nanosecond for any realistic uptime). `tests/test_scheduler.cpp` replays the traces of `tools/clock_sim` (regenerate with `tools/clock_sim/export_ref.py`) and compares.

```cpp
// Interpolation on/off with hysteresis and dwell, so a game hovering near the target does not flap.
struct InterpolationGate {
    double enter{1.12}, exit{1.03};         // enter = VKIFRAME_MIN_GAIN; exit = 1 + (enter - 1) / 4
    int    dwell{8};
    bool   active{false};
    int    streak{0};
    bool Update(double ratio) {             // ratio = smoothed base frame time / target frame time
        const bool want = active ? ratio > exit : ratio > enter;
        streak = (want != active) ? streak + 1 : 0;
        if (streak >= dwell) { active = want; streak = 0; }
        return active;
    }
    void Reset() { active = false; streak = 0; }
};
```

`PairScheduler::OnNativeFrame(now, smoothed, jitter, raw, params)` is called once per native frame, after `TimingEngine::Update`, and returns a **Plan** of one of three kinds:

* `Bypass`: the gate is closed, or the raw interval to the previous frame exceeded 100 ms (a pause or loading screen, which also resets the gate). `flush_held` says a frame is being held and must be presented now.
* `Hold`: the first frame of a run. It is held and the clock starts: the anchor is `v_cur + L`, so tick 0 is not already in the past.
* `Pair`: the ticks to emit between the held frame A and the new frame B, each a `PlannedTick {time_ns, t, native_a}`, or `carrier_only` when no tick falls in the pair (the held frame must still go out). Ticks beyond $k_{max}$ + 1 are counted in `dropped`.

How the clock works, from the first design and still true:

* The smoothed arrival times of A and B (`m_v_prev`, `m_v_cur`) advance on the smoothed cadence and slew 1/8 toward the real arrival, so there is no jitter and no drift.
* The content delay $L$ is one smoothed frame time (the hold) plus a jitter buffer: `WantedDelay = smoothed + min(2 * jitter, smoothed / 2)`. Tick $j$ is presented at `anchor + j * period` and shows content time $\tau_j = T_{tick,j} - L$, which lies in $[v_{prev}, v_{cur})$ for the ticks of the current pair. The buffer makes sure a tick is almost never due before the frame that makes it possible has arrived.
* $L$ moves toward its wanted value by at most 2 % of a target frame per pair, so a hitch cannot push the content timeline backwards.
* $t = (\tau - v_{prev}) / (v_{cur} - v_{prev})$, clamped to $[0, 1]$. A tick within `kNativeFrac` (0.02) of A is the native frame A itself (no interpolation pass). A tick that close to B is left to the next pair, where it is $t = 0$, unless it would be the pair's only tick.

What was added after the first design:

* **Phase lock** (`VKIFRAME_PHASE_LOCK`, default on, with `VKIFRAME_PHASE_OFFSET`, default 0.5). At a near-integer base/target ratio (within 0.03 of a whole number of at least 2, such as 45 to 90) $L$ is slewed so the ticks sit at fixed positions between native frames (0.25 / 0.75 at 45 to 90 with offset 0.5). Without it every shown frame is moved at an arbitrary $t$, which gave the most visible distortion. Offset 0 puts a tick on the native frame and was choppy (sharp and soft frames alternate); offset 0.5 is uniform. The lock costs about 7 ms of content delay (22.3 to 29.5 ms in the Deck measurement; STATUS.md) and the delay is not steady with the lock on at some ratios (a known open point there).
* **Re-anchor** (`RebaseOnPeriodChange`, #45): the clock places tick $i$ at `anchor + i * period`. When `TARGET_FPS` changes live the old index would be multiplied by the new period, so the grid moves to the next tick under the old period and counts from 0 again.
* **Synth margin** (`VKIFRAME_SYNTH_MARGIN`): applied by the caller to $t$ in an interpolated pair.
* **Extrapolation** (`OnNativeFrameExtrap`, `VKIFRAME_EXTRAPOLATE=1`, experimental): no frame is held back; the new native frame goes out on the next slot of the fixed clock, and the slots until the next native frame show frames predicted from it. Its design and numbers are in STATUS.md.

**Tuning from simulation** (`tools/clock_sim`: EMA, gate, clock and $t$ replayed on synthetic frame-time patterns: steady 30/40/45/55 FPS, ±20% jitter, ramps, wobble, 30 and 60 ms hitches):

| Choice | Result |
| :--- | :--- |
| Anchor at `v_cur` (first draft) | Tick 0 was already in the past, and the first pairs got 1–4 ticks. Fixed: anchor = `v_cur + L`. |
| EMA seeded from the first interval | Removes the slow start from the assumed 60 FPS; steady rates are exact from the second frame. |
| Content delay = smoothed + 2× jitter (capped at ½ frame) | Late ticks (due before their frame arrived) drop from about 7% to about 1% at ±20% jitter, for +5 to +7 ms of latency. 3× jitter gains little and adds latency; the jitter estimate is a slow EMA (0.05). |
| Delay rate-limited to 2% of a target frame per pair | Without it a 60 ms hitch made content time run backwards (advance per tick as low as −0.47 with 3× jitter, 0.09 even at 2×). With it the advance per tick stays within 0.97–1.03 of ideal. |
| Gate: enter 1.12, exit 1.03, dwell 8 | No flapping at ≤ 10% jitter over 48–58 FPS bases; one extra flip at ±20% jitter. Enter 1.08 / exit 1.02 / dwell 5 flapped (9 extra flips at ±20% jitter). Enter 1.2 / exit 1.1 flapped (11 flips) on a game wobbling 44–56 FPS. |

These numbers come from simulation, not from real games; the remaining unknowns are the display's real timing behaviour and how the images look. Re-run `python3 tools/clock_sim/clock_sim.py` after changing any of them. The default `MIN_GAIN` has since moved to 1.01 (the layer interpolates at almost any ratio above 1); the gate logic and the hysteresis are unchanged.

### F. The present hook (hold-one, output clock)

`Hook_QueuePresentKHR` (`src/layer_main.cpp`) is the only place the pieces meet. In order:

1. **Passthrough case.** More than one swapchain in a present (or one the layer does not know): the game's present goes out untouched, under the queue mutex. `ENABLED=0` is not a separate path: it raises the gate's threshold so far that every frame is a plain bypass, and it also switches off the base-rate cap.
2. **Base-rate cap** (`BASE_FPS_CAP`): the layer spaces the game's presents. By default the wait happens after the present, so the game starts its next frame as late as it can and its input is not stale.
3. **Plan.** `PlanAndCount` calls `TimingEngine::Update` and `PairScheduler::OnNativeFrame` with the live settings and counts the plan for the once-a-second log.
4. **Holding checks.** The application's present chain is classified. Present id, fence and per-present mode structs are *carried* with the held frame; present id 2 or present timing in the application's chain leaves the swapchain in passthrough (logged once as `hold: off`). The GPU state is created on the first present.
5. **Bypass.** A held frame (if any) is presented first (`PresentHeld`), the presenter's queue is drained (`DrainPresenter`), then the game's own present goes out (`plain_present`), untouched.
6. **Hold or Pair.** `GpuCapture` blits the game's image B into a ring slot (`present_id`, fence and mode move to the `HeldFrame`). For a `Pair`, a `PresentJob{A, B, plan, queue}` is queued for the presenter thread and B becomes the held frame; for `Hold` only B is held. The game thread returns at once; the game's `VkResult` comes from the capture or from an error the presenter noted (`pending_error`, e.g. `VK_ERROR_OUT_OF_DATE_KHR`).

The presenter thread runs `ExecutePair` for each job, in order:

* It computes the flow of the pair once (`GpuFlow`, Stage 4) when flow is on.
* For each planned tick it sleeps until `tick - PACE_LEAD_US` (a tick far in the future is a scheduler hiccup and is not waited for), then:
  * **Tick 0** is carried by the held image of A. If it is the native frame (`native_a`) the real image is presented as is (`PresentFrame`); otherwise `PresentSynthetic(..., hidden=false)` computes the synthetic frame, blits it into the real image of A, and presents that.
  * **Later ticks** use hidden images: `PresentSynthetic(..., hidden=true)` acquires a swapchain image of its own with `vkAcquireNextImageKHR` (finite tries of `ACQUIRE_WAIT_US`, outside the capture mutex), computes the frame, blits it, and presents it.
* If a synthetic frame fails, `synth_failed` is set and the layer falls back to the 3b behaviour for the swapchain (held frames, no synthetic ones). The game is never failed for it.
* `carrier_only` pairs present the held frame alone at `carrier_time_ns`.
* Every present goes through `PresentWithTiming` (Stage 2): a timing chain when the surface supports it, otherwise the vblank-paced path.

`GpuSynthesize` (`src/gpu_frames.cpp`) builds one synthetic frame under `gpu_mu`: it takes the next command buffer entry, gets the target image (the carrier's own image of A, after A's capture semaphore; or a hidden image from `vkAcquireNextImageKHR`, whose semaphore the submit waits on), runs the compute pass (`blend.comp`, or the flow-guided interpolation of Stage 5) from ring[A] and ring[B] into `synth`, blits `synth` into the target image, and submits with a semaphore that the present waits on. If a later step fails after a hidden image was acquired, the image is presented as it is so the swapchain does not lose it (#46). The first design had one `PresentCarrier` and one `PresentSynthetic` in the hook and a layer timeline; the structure is the same, with fences and binary semaphores instead of the timeline.

**Flushing.** A held frame is presented when: the gap since the last native present exceeds the clamp (100 ms, a stall), the gate closes (`Bypass` with `flush_held`), the swapchain is destroyed or recreated, the game waits on its fence or present id (below), or no present has come for 200 ms (the presenter's idle flush; its test came with #48).

**Thread safety.** Every queue has a mutex (`QueueMutex`), taken by the hooks for `vkQueueSubmit`, `vkQueueSubmit2(KHR)`, `vkQueueBindSparse`, `vkQueueWaitIdle`, `vkDeviceWaitIdle` (all queues) and by the layer's own submits and presents. The game's `vkAcquireNextImageKHR` / `vkAcquireNextImage2KHR` are polled in tries of timeout 0 (`PolledAcquire` keeps retrying with short naps until the game's own timeout runs out) so no lock is held while waiting, and share one mutex (`acquire_mu`) with the layer's hidden acquire. Lock order: `gpu_mu`, then the queue mutex; the swapchain mutex `sc.mu` is never held across a queue operation. `gpu` and `last_queue` of the swapchain state are atomic (#48), and the global registries are never destroyed (#47).

### G. Placeholder synthetic-frame shader (`shaders/blend.comp`)

```glsl
#version 450
layout(local_size_x = 16, local_size_y = 16, local_size_z = 1) in;
layout(set = 0, binding = 0, rgba16f) uniform readonly  image2D u_frame_a;
layout(set = 0, binding = 1, rgba16f) uniform readonly  image2D u_frame_b;
layout(set = 0, binding = 2, rgba16f) uniform writeonly image2D u_out;
layout(push_constant) uniform Params { float t; } u_params;

void main() {
    ivec2 p = ivec2(gl_GlobalInvocationID.xy);
    if (any(greaterThanEqual(p, imageSize(u_out)))) return;
    imageStore(u_out, p, mix(imageLoad(u_frame_a, p), imageLoad(u_frame_b, p), u_params.t));
}
```

The shipped shader is the same plus a diagnostic: with `VKIFRAME_TICKMARK` the push constant carries extra information (`t >= 2` encodes a marker and a bar position) and the shader draws a 64x64 square (red for the first half of a pair, green for the second) and a bar along the bottom that steps every displayed frame. It is for looking at the cadence and must stay off in normal use.

The first design had a `ComputePass` helper class. The code creates the shader module, descriptor layout, pipeline layout and pipeline per pass inside `GpuInit` and `FlowCreate`, with one descriptor set per command buffer entry (its fence guards the update); there is no separate class. Shaders are compiled by `glslangValidator` at build time and embedded as `generated/<name>_spv.h` (see STAGE4.md §6).

---

## 5. Verification & Testing Procedure

### Step 1: Build, including shaders
```bash
cmake -B build && cmake --build build -j"$(nproc)"   # needs glslang-tools; -DFRAMEGEN_SKIP_SHADERS=ON embeds stubs for mock builds
ctest --test-dir build                               # mock tests: no GPU needed
```
The tests that cover Stage 3 are `scheduler_vs_sim` (`tests/test_scheduler.cpp`, replays `tests/data/ref_*.txt` from `tools/clock_sim`), `mock_hold` (submit and present order, semaphores, flush on stall, every image shown exactly once), `mock_synth` (synthetic frame sequencing, hidden acquire), `mock_pacer` (presenter timing, queue operations never overlap, acquire polling semantics, drain order), `mock_cmdreuse`, `mock_detach`, `mock_extrap` and `mock_timing` (Stage 2). The older `scripts/test_stage3*.sh` hardware scripts were not carried into this repository; the checks they made are in the section 6 log.

### Step 2: Enable the layer and cap the game
```bash
export VK_LAYER_PATH=$PWD/build
export VK_LOADER_LAYERS_ENABLE=VK_LAYER_CUSTOM_frame_gen,VK_LAYER_KHRONOS_validation
export VKIFRAME_TARGET_FPS=60
# 30 FPS source: VKIFRAME_BASE_FPS_CAP=30 (the layer's own cap), DXVK_FRAME_RATE=30, or the game's own limiter
```

### Step 3: Read the layer's own counters
The layer logs native and output rates once per second (it sees both; an overlay layer sits on only one side):
```text
native 45.0 fps -> output 90.0 fps (planned) | shown 90.0 fps | interpolating | pairs 45 (synthetic 90) bypass 0 carrier-only 0 dropped 0 | content delay 25.4 ms
```
`VKIFRAME_STATS=1` also writes `stats.jsonl` (one JSON line a second) and enables the benchmark; see README.md.

### Step 4: Success criteria
1. A 30 FPS base with a 60 FPS target shows `output ≈ 60` with even spacing (check `actual − target` from Stage 2).
2. A 40 FPS base (non-integer ratio) also shows `output ≈ 60` with an even 16.7 ms cadence; `t` alternates between values such as 0, 0.67 and 0.33. Output is never uneven in pairs.
3. A game at or above the target is a **pure passthrough**: no hold, no added latency, and the log reports bypass. (STATUS.md records the measured cost of bypass seconds; it is small but not zero, because the hook still runs.)
4. Moving a game between 30 and 45 FPS changes $t$ smoothly with no pops, and the gate does not flap near the target.
5. Pausing the game for more than 100 ms flushes the held frame (the screen does not freeze on an old image).
6. No `VK_ERROR_DEVICE_LOST`, no leaks across 10 minutes, and validation reports no synchronization hazards.

---

## 6. Build order and verification log

Stage 3 was built in three steps so each could be verified on its own (counts are from the RX 9070 machine, Wayland, Mesa 26.2.0-devel; the hardware scripts named here belonged to the first repository).

* **3a (done, 7/7):** the scheduling logic of §4.E without any GPU work, in `include/output_clock.hpp`. `tests/test_scheduler.cpp` replays the traces of `tools/clock_sim` and matches Python to 0.1 ns. In the layer it ran first as a **dry run**: it planned every pair and logged `native X fps -> output Y fps`, but changed no present (`VKIFRAME_HOLD=0` keeps that mode).
* **3b (done, 18/18):** `include/gpu_frames.hpp`, `src/gpu_frames.cpp`. Hold one frame, capture it into the ring, present the held image one native frame later (still no blending, so frames shown = frames rendered). Differences from the first design: fences and binary semaphores instead of a timeline semaphore (everything runs on the present queue); `VK_KHR_synchronization2` is enabled when the app has not (the KHR names are used so Vulkan 1.0 apps like vkcube work); the layer records each queue's family by hooking `vkGetDeviceQueue(2)`; its command buffers get their loader dispatch word through the loader's `SetDeviceLoaderData` callback; the ring is created at the first present. `VKIFRAME_DEBUG_RING=1` presents the ring copy instead of the real image. Tests: `tests/test_hold.cpp`.
* **3c-i (done, 20/20; 40/30/45 FPS sources all shown at 60.0 to 60.5 FPS with synthetic frames, bypass and hold-only unchanged):** `shaders/blend.comp` (plain temporal mix of the two captured frames, RGBA16F storage images), compiled at build time. `GpuSynthesize` runs the compute pass into one internal output image and blits it into the target swapchain image: the real image of A for the first tick of a pair, a hidden image (polled `vkAcquireNextImageKHR` with timeout 0) for the later ticks. Any failure sets `synth_failed` and the layer falls back to 3b behaviour. `VKIFRAME_SYNTH=0` is hold only. Tests: `tests/test_synth.cpp`. Known limit at the time: the hidden acquire ran on the game's present thread, so a game that acquires from another thread at the same time could race on the swapchain; 3c-ii removed this.
* **3c-ii (done, 19/19 in two clean runs; present-interval sd 0.0 to 1.5 ms with the thread against 6.8 to 9.0 ms without, mean 16.67 ms):** a presenter thread per swapchain presents each planned tick at its time (it sleeps until `tick - PACE_LEAD_US`), so a 60 FPS target on a 120 Hz display is paced in software instead of presenting back-to-back. The game thread captures frame B, queues a job (A, B, ticks) and returns; a bypass present or a stall first waits until the queue is empty (`DrainPresenter`), so order is kept. Thread safety as in §4.F; ring slots are reference counted (ring of 6 today). `VKIFRAME_PRESENTER=0` restores the 3c-i behaviour. Tests: `tests/test_pacer.cpp` (59/59 presents within 3.5 ms of 16.67 ms, also clean under ThreadSanitizer). Not covered: aligning the present call to the vblank phase (the lead is a fixed guess; a later step can use the present-timing feedback). Two small differences from the sketches in the first design: times are doubles, and the tick position is `anchor + n * target` in floating point (an integer target interval drifts by about 1 ms per 25 s at 60 FPS).

After that, Stage 4 added the flow engine on the same queue, Stage 5 replaced the blend with the interpolation, and the audit fixes of #45 to #48 touched this stage's code (clock re-anchor, never destroying the registries, the presenter's idle flush, atomic `gpu` and `last_queue`). Hardware results since then are in STATUS.md.

---

## Addendum: applications that use VK_KHR_present_id (Proton / DXVK / vkd3d-proton)

Proton games chain a `VkPresentIdKHR` to every present. The layer carries that id with the held frame and re-attaches it when the frame is finally shown (the carrier of a pair keeps A's id, hidden synthetic images carry none), so ids stay increasing and each is presented once. Present id 2 and present timing structures in the application's chain still leave the swapchain in passthrough (logged once as `hold: off ...`). `vkWaitForPresentKHR` is hooked: waiting for the id of the frame being held releases that frame first, otherwise the wait would only end when the game's next present arrives. First measured on the Deck (Game Mode, a Proton game): the layer loads inside Steam's Linux Runtime container (no glibc problem with the Ubuntu 22.04 build), but with ids in the chain holding stayed off until this change.

## Addendum: swapchain present fences (Proton/DXVK freeze)

Cuphead (DXVK, GE-Proton) froze after its first frame with holding on. Gamescope's WSI layer forces VK_EXT/KHR_swapchain_maintenance1, so the game chains a VkSwapchainPresentFenceInfoKHR to each present and may wait for it. A held frame's fence only signals when the layer finally shows it, so a game that waits on it before submitting the next frame deadlocks. Now:
- the fence is carried with the held frame and re-attached when it is presented (carrier keeps A's fence; hidden synthetic images carry none);
- vkWaitForFences / vkGetFenceStatus on a fence of a held frame, and vkWaitForPresentKHR on its id, present the held frame first (`flow_mu` serializes this with the present hook);
- the first present logs the game's pNext sTypes ("present chain of the first present");
- a watchdog on the presenter thread logs "watchdog: no present for N s | held | in acquire | in present-wait" when the game stops presenting.

Result of the first run of the fence build on the Deck: Cuphead's present chain is fence (1000275001) + present mode (1000275003) + present id (1000294000). The present-mode struct was classed as unsupported, so holding switched itself off. It is now carried like the fence (re-attached to the carrier; hidden synthetic images carry none).

## Addendum: DXVK waits for every present (vkWaitForPresentKHR)

Run with the fence+mode build on the Deck: no freeze, but `synthetic 0`. DXVK calls vkWaitForPresentKHR for the id it just presented. The first fix answered that by showing the held frame at once, so nothing was ever held long enough to pair. Now a wait for the held frame's own id is answered immediately (the frame is only shown one frame late; waits for older ids are real). A game that then stops presenting gets the held frame from an idle flush in the presenter thread (no present for 200 ms). Waits on fences of held frames still flush (the fence must really signal).
