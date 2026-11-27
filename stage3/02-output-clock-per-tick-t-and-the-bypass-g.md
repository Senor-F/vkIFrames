[index](../STAGE3.md) | [previous](01-overview-objectives.md) | [next](03-build-order-and-verification-log.md)

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

