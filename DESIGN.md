# vkIFrames: Dynamic Rate-Matching Vulkan Frame Interpolation Layer — Architectural Blueprint

## 1. Overview & Objectives

This blueprint describes an independent Vulkan layer for Linux that performs **dynamic frame interpolation** that hits a user-chosen **Target FPS** (e.g. 60 Hz or 120 Hz) exactly.

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

* **Bypass:** when Ratio is at or below 1 (the game is at or above the target), nothing is held and native frames pass straight through with **no added latency**. To avoid flapping near the boundary, interpolation starts above Ratio $1.12$ and stops at or below $1.03$, after 8 consecutive frames on the other side (`VKIFRAME_MIN_GAIN` sets the start value). A game between about 54 and 60 FPS on a 60 FPS target therefore stays in bypass: the gain is small and the latency and artifact cost would not be worth it.
* **Interpolation active:** the layer holds one native frame (§4.1) and drives the output clock.
* **Clamp:** at most $k_{\max}$ synthetic frames between two natives (provisional default 3). Below $\text{Target FPS} / (k_{\max}+1)$ the clamp is hit; the extra ticks are dropped, so the output rate falls below the target rather than interpolating over very large gaps.
* **No integer hysteresis is needed.** Because $t$ changes smoothly with the frame time, there is no half-integer boundary to oscillate around.

### C. Dynamic Fractional Temporal Weighting ($t$-Weighting)
For output tick $j$ at content time $\tau_j$, falling between $Frame_A$ (arrival $T_A$) and $Frame_B$ (arrival $T_B$), the fractional position is:
$$t_j = \frac{\tau_j - T_A}{T_B - T_A}, \qquad \tau_j = T_{\text{tick},j} - L$$
where $L$ is the content delay: one smoothed base frame time (the cost of holding a frame) plus a jitter buffer of twice the measured jitter, capped at half a frame, and rate-limited so it can never make the content timeline run backwards (STAGE3 §4.E). $T_A$ and $T_B$ are the *smoothed* arrival times (§3.C), so game-side jitter does not move $t$. A tick with $t_j < \epsilon$ ($\epsilon \approx 0.02$) presents $Frame_A$ itself and costs no synthetic frame; a tick close to $B$ is still produced synthetically, because $Frame_B$ only becomes presentable as the first tick of the next pair.

Let $V$ be the dense optical flow field from $Frame_A$ to $Frame_B$ in full-resolution pixels. Forward mapping defines the target as $I_t(x + t \cdot V(x)) = I_A(x)$. Because forward splatting leaves holes, the shaders use the equivalent **backward gather**, sampling $V$ at the output pixel $p$:
$$I_t(p) = (1 - t)\, I_A\big(p - t\,V(p)\big) + t\, I_B\big(p + (1 - t)\,V(p)\big)$$

Reading $V$ at $p$ instead of at the true source pixel is an approximation that holds when motion is locally smooth. Occluded pixels (§3.A) fall back to a plain cross-fade.

### D. Precision Presentation Timing
Using **`VK_EXT_present_timing`**, every output tick is presented at its scheduled time:

$$T_{\text{tick},j} = T_{\text{anchor}} + j \cdot \Delta T_{\text{target}}$$

The anchor is set when interpolation starts, at the first held frame's arrival plus $L$, and is re-anchored (with a short smooth phase slew) if it drifts from the display by more than half a tick, or after a flush. When a native frame $Frame_B$ arrives, the layer emits every tick that now has both neighbours available, computing $t_j$ per §2.C. Each tick is either a native frame or a synthetic frame at its own $t_j$. Because the clock is fixed and $t$ absorbs the rounding, **no error accumulates and spacing is always even**.


---

## 3. Core Processing Modules

### A. Hierarchical Optical Flow Engine
* **Pyramidal Scale Depth:** a 4-to-5 level downsampling pyramid ($1/2$ down to $1/16$ or $1/32$ resolution) handles the large displacements typical of low base rates (e.g. camera pans at 30 FPS).
* **Forward-Backward Consistency:** vectors are evaluated in both directions to flag occlusions and edge artifacts.

### B. Static Element and UI Masking
* **Difference Thresholding:** compares consecutive frames to isolate invariant elements (crosshairs, HUD text, health bars). Static scene regions are also flagged, which is harmless because copying them is correct.
* **Bilateral Edge Filtering:** spatial weighting guided by source colors, which never mixes vectors across a UI / non-UI boundary, so background 3D motion does not bleed into static interfaces.

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
3. Run optical flow and interpolation (async compute, Stages 4–5; a simple blend in Stage 3).
4. For each output tick of this pair (§2.D): compute $t_j$. A game-acquired swapchain image can only be returned by presenting it, so the **first tick uses the held real image $A$ as its carrier** (left untouched if $t_j < \epsilon$, otherwise the synthetic result is blitted into it; $A$ itself is preserved in the ring). Every later tick acquires a *hidden* swapchain image (the layer acquires images on its own, which the game never sees), blits the synthetic result into it, and presents it with a `VkPresentTimingsInfoEXT` target time.
5. Record the held frame; return the previous real present's result to the game (`VK_ERROR_OUT_OF_DATE_KHR`, `VK_SUBOPTIMAL_KHR` are forwarded).

All of this runs on the game's thread, inside the call, on the queue the game passed in. Using that queue is legal because the game has externally synchronized it for the duration of the call. Only the optical flow work goes to a separate queue.

### 4.3 Images, formats and sharing
* Internal images are **`VK_FORMAT_R16G16B16A16_SFLOAT`**. Swapchain formats are usually 8-bit (often sRGB), which cannot be used as storage images. Capture and final output use `vkCmdBlitImage`, which also converts to and from the swapchain format. Interpolating then happens in linear light.
* Blit requires `BLIT_SRC`/`BLIT_DST` format features on the swapchain format; the layer checks this at swapchain creation and disables itself (passthrough) if they are missing.
* Ring-buffer images use `VK_SHARING_MODE_CONCURRENT` across the graphics and compute queue families, which avoids ownership-transfer barriers. The game's swapchain images are never touched by the compute queue.

### 4.4 Queues
The compute family queue used for optical flow must be requested in `vkCreateDevice`, so the layer adds a `VkDeviceQueueCreateInfo` there (raising `queueCount` of an existing entry if the family is already in use, subject to the family's limit). If the GPU has no separate compute-capable family or no spare queue, flow work is submitted on the game's queue instead (still correct, no async overlap).

### 4.5 Device and swapchain modifications
| Where | Change |
| :--- | :--- |
| `vkCreateDevice` | Enable `VK_EXT_present_timing` and `VK_KHR_present_id2` (if the driver exposes them), their feature structs, `timelineSemaphore`, `synchronization2`, and the extra compute queue. |
| `vkCreateSwapchainKHR` | Add `VK_SWAPCHAIN_CREATE_PRESENT_TIMING_BIT_EXT` and `..._PRESENT_ID_2_BIT_KHR`; OR in `TRANSFER_SRC \| TRANSFER_DST` usage; force `FIFO` (target times apply to FIFO only); raise `minImageCount` to the game's value $+\,k_{\max} + 2$ (one held image, up to $k_{\max}$ queued synthetic images, one hidden acquire), clamped to the surface maximum, in which case $k_{\max}$ is lowered to fit. |
| After swapchain creation | `vkSetSwapchainPresentTimingQueueSizeEXT` with a multiple of the image count; pick a time domain matching CLOCK_MONOTONIC if reported. |

### 4.6 Fallback without present timing
If the driver lacks `VK_EXT_present_timing`, the layer presents the frames for each pair back to back on FIFO and lets vertical blank pace them, using the same $t$ values. This is exact when the display refresh equals the Target FPS and degrades gracefully otherwise (not on VRR). The same code path is used for testing before Stage 2 lands.

---

## 5. Architectural Best Practices

* **Asynchronous Compute Isolation:** optical flow and interpolation shaders run on a dedicated `VK_QUEUE_COMPUTE_BIT` family queue, synchronized with the graphics queue by timeline semaphores, to avoid contention with the game's rendering.
* **Swapchain Image Expansion:** enforce the larger image count from §4.5 (never fewer than 4) so that capture, compute and pacing never stall the game.
* **Velocity-Based Blend Fallback:** when mean flow magnitude exceeds the stability limit, blend smoothly toward a 50/50 temporal alpha blend, trading geometric interpolation for mild motion blur. The mean is computed on the GPU and consumed by the interpolation shader without CPU readback.
* **Never call `vk*` entry points directly inside the layer.** Layers must call down the chain through the dispatch tables (Stage 1); direct calls re-enter the loader.

---

## 6. Known Limitations and Open Questions

* **Input latency** rises by about one base frame time while interpolating (§4.1).
* **Games should be frame-capped** at their intended base rate. An uncapped game is back-pressured by the swapchain, which couples its rate to the layer's output and can make the measured frame time oscillate.
* **`VK_KHR_present_id` / `present_wait` games:** the game's present IDs can only be attached when its image is finally presented, one frame late. Games that wait on them tightly may stall; behavior needs testing.
* **One swapchain per present call** is interpolated; multi-swapchain presents pass through.
* **Games using `VK_EXT_present_timing` themselves** conflict with the layer; the layer should hide the extension from the application (filter `vkEnumerateDeviceExtensionProperties`).
* **Mean-velocity fallback** is a whole-frame statistic and can miss local fast motion. Per-tile statistics are a possible refinement.
* **Tuned in simulation only:** the bypass thresholds, the jitter buffer, the delay rate limit and the EMA seeding were tuned with `tools/clock_sim` on synthetic frame-time patterns. **$k_{\max}$, the UI threshold, the flow budget** and the real-world behaviour of all of the above are provisional and need tuning on real games.
* **Forced `FIFO`** (needed for target times) overrides a game's mailbox / immediate mode and does not suit every VRR setup; behavior needs testing per compositor.
* **Game Mode (gamescope):** games reach the display through gamescope's own Vulkan WSI layer, and its `VK_EXT_present_timing` support was still a draft at the last check, so the extension may be missing there and the vblank-paced fallback (§4.6) carries the Steam Deck's main use case. Steam's frame limiter also acts below this layer and would cap the layer's output rather than the game's base rate; see TARGETS.md §3.
* **Driver support:** `VK_EXT_present_timing` is new; verify that the target driver exposes it (`vulkaninfo | grep present_timing`).
