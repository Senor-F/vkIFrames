# vkIFrames: Dynamic Rate-Matching Vulkan Frame Interpolation Layer — Architectural Blueprint

## 1. Overview & Objectives

This blueprint describes an independent Vulkan layer for Linux that performs **dynamic frame interpolation** that hits a user-chosen **Target FPS** (e.g. 60 Hz or 120 Hz) exactly. Its fallback to a single real frame where an interpolation cannot be trusted is inspired by [afmf-linux](https://github.com/serialexperimentslainnnn/afmf-linux); see the credits in the README.

Instead of a fixed multiplier, the layer continuously rescales its interpolation ratio and temporal weights to follow the game's fluctuating base frame rate. It uses **`VK_EXT_present_timing`** for precise frame pacing and temporal smoothing to avoid cadence judder. When the game already reaches the target, the layer steps aside and frames pass through untouched.

**Terms used throughout the docs**

| Term | Meaning |
| :--- | :--- |
| Native frame | A frame the game rendered and presented. |
| Synthetic frame | A frame the layer generated between two native frames. |
| $Frame_A$, $Frame_B$ | The older and newer of two consecutive native frames. |
| Held frame | A native frame whose real presentation the layer has postponed (see §4.1). |
| $T_B$ | Arrival time (CLOCK_MONOTONIC) of $Frame_B$'s `vkQueuePresentKHR` call. |

---

## 2. Core Mathematical Formulations

### A. Dynamic Target Interval and Ratio
Let $\text{Target FPS}$ be the user-chosen output rate, giving a constant target frame interval:
$$\Delta T_{\text{target}} = \frac{1}{\text{Target FPS}}$$

Let $\Delta T_{\text{base}}$ be the **smoothed** frame time of the game (§3.C). The dynamic interpolation ratio is:
$$\text{Ratio} = \frac{\Delta T_{\text{base}}}{\Delta T_{\text{target}}}$$

### B. Output Clock and Frame Count (user-targeted)
The user's **Target FPS** is the exact output rate, not just a cap. The layer runs a fixed **output clock** with a tick every $\Delta T_{\text{target}}$, and every tick presents exactly one frame: a native frame, or a synthetic frame at some fractional position between two native frames. The number of synthetic frames per native pair is therefore not an integer chosen in advance. It is however many ticks fall inside that pair's interval:

$$n_{\text{pair}} = \#\{\text{ticks } \tau_j \in [T_A, T_B)\} \in \{\lfloor \text{Ratio} \rfloor,\ \lceil \text{Ratio} \rceil\}$$

where $\text{Ratio} = \Delta T_{\text{base}} / \Delta T_{\text{target}}$ and $\Delta T_{\text{base}}$ is the smoothed frame time (§3.C). A 40 FPS game on a 60 Hz target (Ratio 1.5) alternates between 1 and 2 outputs per pair, and the output cadence stays an even 16.7 ms.

* **Bypass:** when Ratio is at or below 1 (the game is at or above the target), nothing is held and native frames pass straight through with **no added latency**. To avoid flapping near the boundary, the gate has hysteresis and a dwell: interpolation starts above Ratio $g$ (`VKIFRAME_MIN_GAIN`) and stops at or below $1 + (g - 1)/4$, after 8 consecutive frames on the other side. The gate was tuned in simulation with $g = 1.12$ (exit $1.03$), which keeps a game between about 54 and 60 FPS on a 60 FPS target in bypass, because the gain there is small and the latency and artifact cost would not be worth it. The shipped default is $g = 1.01$ (exit $1.0025$), so the layer interpolates at almost any ratio above 1; raise `VKIFRAME_MIN_GAIN` to get the wider bypass band.
* **Interpolation active:** the layer holds one native frame (§4.1) and drives the output clock.
* **Clamp:** at most $k_{\max}$ synthetic frames between two natives (provisional default 3). Below $\text{Target FPS} / (k_{\max}+1)$ the clamp is hit; the extra ticks are dropped, so the output rate falls below the target rather than interpolating over very large gaps.
* **No integer hysteresis is needed.** Because $t$ changes smoothly with the frame time, there is no half-integer boundary to oscillate around.

### C. Dynamic Fractional Temporal Weighting ($t$-Weighting)
For output tick $j$ at content time $\tau_j$, falling between $Frame_A$ (arrival $T_A$) and $Frame_B$ (arrival $T_B$), the fractional position is:
$$t_j = \frac{\tau_j - T_A}{T_B - T_A}, \qquad \tau_j = T_{\text{tick},j} - L$$
where $L$ is the content delay: one smoothed base frame time (the cost of holding a frame) plus a jitter buffer of twice the measured jitter, capped at half a frame, and rate-limited so it can never make the content timeline run backwards (STAGE3 §4.E). $T_A$ and $T_B$ are the *smoothed* arrival times (§3.C), so game-side jitter does not move $t$. A tick with $t_j < \epsilon$ ($\epsilon = 0.02$) presents $Frame_A$ itself and costs no synthetic frame. A tick within $\epsilon$ of $B$ is not produced in this pair unless it would be the pair's only tick: it is left to the next pair, where it is $t = 0$ and shows the native frame (#37; before that fix such a tick was a full interpolation pass at $t = 1$ for a frame that is $B$). The first tick of a pair is never deferred (`emitted > 0` in `PairScheduler`), so a pair keeps at least one tick. With the phase lock (§4.7) and `VKIFRAME_SYNTH_MARGIN` the caller can also keep $t$ away from 0 and 1 so that no native frame is shown as it is.

Let $V$ be the dense optical flow field from $Frame_A$ to $Frame_B$ in full-resolution pixels. Forward mapping defines the target as $I_t(x + t \cdot V(x)) = I_A(x)$. Because forward splatting leaves holes, the shaders use the equivalent **backward gather**, sampling $V$ at the output pixel $p$:
$$I_t(p) = (1 - t)\, I_A\big(p - t\,V(p)\big) + t\, I_B\big(p + (1 - t)\,V(p)\big)$$

Reading $V$ at $p$ instead of at the true source pixel is an approximation that holds when motion is locally smooth. Occluded pixels (§3.A) fall back to a plain cross-fade.

The default **two-sided** form (`VKIFRAME_INTERP_BIDIR=1`) uses the backward field as well: $A$ is moved forward by $t$ along $V_{A\to B}$ and $B$ is moved back by $(1-t)$ along $V_{B\to A}$, each trusted by its own match cost and occlusion mask, so uncovered background comes only from $B$ and covered background only from $A$; where both are unreliable the cross-fade carries the pixel. The field is read at the flow level (quarter resolution by default) with bilinear weights guided by the luma of the frame, and the vectors are scaled to full-resolution pixels. The shader is `shaders/flow_interp.comp` (STAGE5.md).

### D. Precision Presentation Timing
Using **`VK_EXT_present_timing`**, every output tick is presented at its scheduled time:

$$T_{\text{tick},j} = T_{\text{anchor}} + j \cdot \Delta T_{\text{target}}$$

The anchor is set when interpolation starts, at the first held frame's arrival plus $L$, and again after a flush or a stall (the next run starts a new clock). While a run goes on, the grid is moved to the next tick under the old period and counted from 0 again when the target period changes (a live `TARGET_FPS` change, #45); the phase lock (§4.7) slews $L$ so the ticks keep their position between native frames. The layer does not re-anchor to the display's vblank phase; the presenter calls present a fixed lead (`VKIFRAME_PACE_LEAD_US`) before the tick is due. When a native frame $Frame_B$ arrives, the layer emits every tick that now has both neighbours available, computing $t_j$ per §2.C. Each tick is either a native frame or a synthetic frame at its own $t_j$. Because the clock is fixed and $t$ absorbs the rounding, **no error accumulates and spacing is always even**.


---

## 3. Core Processing Modules

### A. Hierarchical Optical Flow Engine
* **Pyramidal Scale Depth:** a 4-level luma pyramid ($1/2$ down to $1/16$ resolution; a fifth level is a small change) handles the large displacements typical of low base rates (e.g. camera pans at 30 FPS). The search is exhaustive (±4) at the coarsest level and refined at each finer level; the finished field is at quarter resolution by default (`VKIFRAME_FLOW_LEVEL`) with sub-pixel vectors and a per-vector match cost (STAGE4.md).
* **Forward-Backward Consistency:** vectors are evaluated in both directions to flag occlusions and edge artifacts; the mask is a smooth step of the disagreement, not a hard 0/1.

### B. Static Element and UI Masking
* **Difference Thresholding** (`VKIFRAME_UI_MASK_THRESHOLD`, default 0.08, 0 = off): inside the interpolation shader, where the 3×3 mean of $|A - B|$ in luma stays below the threshold the pixel takes the plain cross-fade instead of the moved one, so a HUD is not dragged along by motion next to it. The mean ignores film grain but not a moving edge. Slow flat motion can also fall under the threshold; the setting needs a look test on the Deck.
* **Colour-guided vector weighting** (`VKIFRAME_INTERP_GUIDE`, default 0.08): the bilinear weights of the four nearest field vectors are scaled by how close their luma is to the output pixel's, so vectors are not averaged across an edge. This replaced the first design's separate bilateral-filter pass.

### C. Cadence Judder Mitigation (Temporal Smoothing)
Raw frame deltas are filtered with an **Exponential Moving Average (EMA)** so that micro-stutter does not cause erratic ratio changes:
$$\Delta T_{\text{smoothed}} = \alpha \cdot \Delta T_{\text{current}} + (1 - \alpha) \cdot \Delta T_{\text{previous}}$$
with $\alpha \in (0, 1)$ (default 0.15). The smoothed values feed $\Delta T_{\text{base}}$, the smoothed arrival times $T_A, T_B$ and the content delay $L$, so $t$ varies smoothly (§2.B, §2.C).

---

## 4. Presentation Pipeline

A layer cannot show a frame *between* two frames the game has already presented, so interpolation has to **delay** native presentation by one frame. This section defines how.

### 4.1 Hold-one scheduling
* The layer returns from the game's `vkQueuePresentKHR` immediately but does **not** present the game's image. It keeps that real swapchain image *held* (acquired, unpresented).
* When the next native frame arrives, the layer emits the output ticks that now have both neighbours (§2.D). The held real image **carries the first tick** of the pair (§4.2), and the new frame becomes the held one.
* In bypass (§2.B) nothing is held: the previous held frame (if any) is flushed and the new frame is presented right away.
* **Latency cost:** about one base frame time whenever interpolating. This is inherent to interpolation.
* **Flush rules:** a held frame is presented if the gap since the last native present exceeds the clamp (100 ms), when the swapchain is destroyed, or when interpolation switches to bypass.

### 4.2 Per-frame flow (inside the `vkQueuePresentKHR` hook)
1. Measure the arrival time $T_B$, update the EMA, and work out which output ticks now have both neighbours (§2.B).
2. Submit a **capture**: wait on the game's present semaphores, blit the game's image into the internal ring buffer (RGBA16F), and signal a layer-owned semaphore that later gates the real present of that image.
3. Run optical flow and interpolation (Stages 4–5; a simple blend in Stage 3). Today this happens on the presenter thread, on the present queue, as one flow command buffer per pair and one interpolation pass per tick; an asynchronous compute queue is not built (§4.4).
4. For each output tick of this pair (§2.D): compute $t_j$. A game-acquired swapchain image can only be returned by presenting it, so the **first tick uses the held real image $A$ as its carrier** (left untouched if $t_j < \epsilon$, otherwise the synthetic result is blitted into it; $A$ itself is preserved in the ring). Every later tick acquires a *hidden* swapchain image (the layer acquires images on its own, which the game never sees), blits the synthetic result into it, and presents it with a `VkPresentTimingsInfoEXT` target time.
5. Record the held frame. The result of the game's call comes from the capture, or from an error the presenter thread noted for an earlier present (`VK_ERROR_OUT_OF_DATE_KHR`, `VK_SUBOPTIMAL_KHR` are forwarded).

The game's thread does the measuring, the plan and the capture (steps 1 and 2) inside the call. Steps 3 and 4 run on a per-swapchain presenter thread, which presents each tick at its time (`VKIFRAME_PRESENTER=0` runs them on the game's thread instead). Both use the queue the game passed in; every queue has a mutex that the hooks and the layer's own submits and presents take, so the layer never submits to a queue while the game does. Lock order and the acquire rules are in STAGE3.md §4.F.

### 4.3 Images, formats and sharing
* Internal images are **`VK_FORMAT_R16G16B16A16_SFLOAT`**. Swapchain formats are usually 8-bit (often sRGB), which cannot be used as storage images. Capture and final output use `vkCmdBlitImage`, which also converts to and from the swapchain format. Interpolating then happens in linear light.
* Blit requires `BLIT_SRC`/`BLIT_DST` format features on the swapchain format; the layer checks this at swapchain creation and disables itself (passthrough) if they are missing.
* Ring-buffer images are device-local `RGBA16F`, stay in `GENERAL`, and are `EXCLUSIVE` today because everything runs on the present queue. With a separate compute family they would become `VK_SHARING_MODE_CONCURRENT` across the two families, which avoids ownership-transfer barriers (§4.4). The ring has 6 slots that are reference counted, so a frame a queued job still needs is never overwritten.

### 4.4 Queues
**Not built; flow and interpolation run on the game's present queue.** The plan, kept for the asynchronous queue (AUDIT.md item 9, ASYNC.md): the compute family queue must be requested in `vkCreateDevice`, so the layer would add a `VkDeviceQueueCreateInfo` there (raising `queueCount` of an existing entry if the family is already in use, subject to the family's limit). If the GPU has no separate compute-capable family or no spare queue, flow work stays on the game's queue (still correct, no async overlap). The layer's cost is serial GPU time, so a second queue helps only if the GPU can overlap the work; that is why it comes last and only if the measurements show it is worth it.

### 4.5 Device and swapchain modifications
| Where | Change |
| :--- | :--- |
| `vkCreateDevice` | Enable `VK_EXT_present_timing` and `VK_KHR_present_id2` (if the driver exposes them and the application does not use them itself), their feature structs, and `synchronization2` (the KHR names, so Vulkan 1.0 applications work). `timelineSemaphore` and the extra compute queue are not requested (§4.4). |
| `vkCreateSwapchainKHR` | Add `VK_SWAPCHAIN_CREATE_PRESENT_TIMING_BIT_EXT` and `..._PRESENT_ID_2_BIT_KHR`; OR in `TRANSFER_SRC \| TRANSFER_DST` usage; force `FIFO` (target times apply to FIFO only); raise `minImageCount` to the game's value $+\,k_{\max} + 2$ (one held image, up to $k_{\max}$ queued synthetic images, one hidden acquire), clamped to the surface maximum, in which case $k_{\max}$ is lowered to fit. |
| After swapchain creation | `vkSetSwapchainPresentTimingQueueSizeEXT` with a multiple of the image count; pick a time domain matching CLOCK_MONOTONIC if reported. |

### 4.6 Fallback without present timing
If the driver lacks `VK_EXT_present_timing`, or offers it for feedback only (no absolute target times, as on the desktop test machine's Wayland session and in the Deck's Game Mode), the layer presents on FIFO and the vertical blank paces the frames, using the same $t$ values. The presenter thread sleeps until a fixed lead (`VKIFRAME_PACE_LEAD_US`, default 2 ms) before each tick, so a 60 FPS target on a 120 Hz display is paced in software instead of presenting back to back. This is exact when the display refresh divides evenly by the Target FPS (45 or 90 on a 90 Hz screen; 60 does not divide 90) and degrades otherwise (not on VRR). This is the path the Steam Deck's Game Mode uses today.

### 4.7 Phase lock
At a near-integer ratio of base to target (within 0.03 of a whole number of at least 2, such as 45 to 90), `VKIFRAME_PHASE_LOCK` (default on) slews $L$ so the ticks sit at fixed positions between native frames: $t = 0.25 / 0.75$ at 45 to 90 with `VKIFRAME_PHASE_OFFSET` 0.5. Without it every shown frame is moved at an arbitrary $t$, which gave the most visible distortion on the Deck; offset 0 puts a tick on the native frame and was choppy (sharp and soft frames alternate). The price is a content delay that is not steady and about 7 ms more of it (STATUS.md). Details and the code are in STAGE3.md §4.E.

---

## 5. Architectural Best Practices

* **Asynchronous Compute Isolation (planned, not built):** optical flow and interpolation shaders on a dedicated `VK_QUEUE_COMPUTE_BIT` family queue, synchronized with the graphics queue by timeline semaphores, to avoid contention with the game's rendering. Today they share the present queue (§4.4); ASYNC.md has the design and the measurements so far.
* **Swapchain Image Expansion:** enforce the larger image count from §4.5 (never fewer than 4) so that capture, compute and pacing never stall the game.
* **Cross-fade fallbacks:** three smooth fallbacks to the plain cross-fade (a time-weighted blend of A and B) trade geometric interpolation for mild blur, all inside the interpolation shader with no CPU readback: per pixel, where the match cost is high (`VKIFRAME_CONF_LO/HI`), the pixel is occluded, or the vector is very fast (48 to 96 full-resolution pixels); and for the whole frame when a large share of the flow field is occluded or badly matched, measured on a fixed 8×6 grid of field pixels (`VKIFRAME_PAN_LO/HI`, the "pan fallback" for fast camera pans). The first design used one whole-frame mean flow magnitude with a 50/50 blend; that statistic and its `flow_stats` pass were not built.
* **Never call `vk*` entry points directly inside the layer.** Layers must call down the chain through the dispatch tables (Stage 1); direct calls re-enter the loader.

---

## 6. Known Limitations and Open Questions

* **Input latency** rises by about one base frame time while interpolating (§4.1).
* **Games should be frame-capped** at their intended base rate. An uncapped game is back-pressured by the swapchain, which couples its rate to the layer's output and can make the measured frame time oscillate.
* **`VK_KHR_present_id` / `present_wait` games:** the game's present IDs (and swapchain-maintenance fences and per-present modes) are carried with the held frame and attached when it is finally presented, one frame late. DXVK waits for every present it makes, so the layer answers a `vkWaitForPresentKHR` for the held frame's own id at once, a deliberate deviation from the specification; older ids and fences are real waits that flush the held frame first (STAGE3.md addenda). Present id 2 or present timing in the application's chain leaves that swapchain in passthrough.
* **One swapchain per present call** is interpolated; multi-swapchain presents pass through.
* **Games using `VK_EXT_present_timing` themselves** conflict with the layer. Today the layer detects it and skips its own use of present timing for that device (logged as `present timing: skipped`); hiding the extension from the application (filtering `vkEnumerateDeviceExtensionProperties`) is still open (Stage 6).
* **The pan fallback** is a whole-frame statistic (a share of bad matches on a fixed grid) and can miss local fast motion. Per-tile statistics are a possible refinement.
* **Tuned in simulation only:** the bypass thresholds, the jitter buffer, the delay rate limit and the EMA seeding were tuned with `tools/clock_sim` on synthetic frame-time patterns. **$k_{\max}$, the UI threshold, the flow budget** and the real-world behaviour of all of the above are provisional and need tuning on real games.
* **Forced `FIFO`** (needed for target times) overrides a game's mailbox / immediate mode and does not suit every VRR setup; behavior needs testing per compositor.
* **Game Mode (gamescope):** games reach the display through gamescope's own Vulkan WSI layer, and its `VK_EXT_present_timing` support was still a draft at the last check, so the extension may be missing there and the vblank-paced fallback (§4.6) carries the Steam Deck's main use case. Steam's frame limiter also acts below this layer and would cap the layer's output rather than the game's base rate; see TARGETS.md §3.
* **Driver support:** `VK_EXT_present_timing` is new; verify that the target driver exposes it (`vulkaninfo | grep present_timing`).
