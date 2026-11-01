# Stage 5: Static UI Masking & Dynamic Motion Interpolation

> **Status: done for now, in one shader.** This page keeps the first Stage 5 design (the goals, the weakness notes, the format remarks, the verification) and brings it up to the code on `main`. The first design had four shaders and an `InterpolationEngine` class (UI mask, bilateral vector filter, mean-velocity reduction, interpolation). The code does the same jobs inside a single shader, `shaders/flow_interp.comp`, in a different way, and the differences are listed in section 5. Quality is still being improved: thin lines, tree trunks and occlusion strips are open items in STATUS.md. Settings use the `VKIFRAME_*` names; the old `FRAMEGEN_*` and `VKIFRAME_WARP*` names are still read as fallbacks.

## 1. Overview & Objectives

Stage 5 turns the motion field into clean intermediate frames. The layer:

1. Detects static 2D elements (HUD, crosshairs, health bars) by difference-thresholding $Frame_A$ against $Frame_B$. **Done**, per pixel, inside the interpolation shader (section 4.A).
2. Weights the motion vectors by the frame colour so background motion does not bleed across an edge, such as a UI boundary. **Done differently**: instead of a separate bilateral filter pass, the four nearest field vectors are blended with weights guided by luma (section 4.B).
3. Falls back to a cross-fade where the motion cannot be trusted, **without any CPU readback**. **Done differently**: the whole-frame statistic is the share of bad matches on a fixed grid, computed once per workgroup inside the shader, not a mean flow magnitude from a separate reduction pass (section 4.C).
4. Runs the **backward-gather interpolation** with the per-tick weight $t$ from the output clock (DESIGN §2.C), replacing Stage 3's blend. **Done**, in a one-sided and a two-sided form (section 4.D).
5. Handles occlusions and fast motion by falling back to cross-fade, smoothly. **Done**; the first design's final fallback was a 50/50 alpha blend, the code's is the time-weighted cross-fade $(1-t)A + tB$.

---

## 2. Repository Changes

```text
shaders/
└── flow_interp.comp    # the interpolation: UI mask, guided vectors, two-sided gather, fallbacks, extrapolation (replaces blend.comp as the synthetic-frame shader)
src/
├── optical_flow.cpp    # builds the pipeline and the descriptor set (set 18, six storage images)
└── gpu_frames.cpp      # GpuSynthesize binds the images and pushes the parameters per tick
tools/interp_eval/      # offline scoring of interpolation methods; the C++ reference of the same math is include/flow_ref.hpp (InterpPixel)
```

Not built, and no longer planned: `include/interpolation_engine.hpp`, `shaders/ui_mask.comp`, `shaders/vector_bilateral_filter.comp`, `shaders/flow_stats.comp`, `shaders/motion_interp.comp`. `shaders/blend.comp` stays as the fallback when interpolation is off or flow fails.

---

## 3. Pass Order

For each native pair, once, on the present queue (Stage 4, `GpuFlow`): luma pyramids, forward and backward flow, the occlusion mask. The first design put three more per-pair passes here (UI mask, bilateral filter, mean-velocity reduction); they no longer exist, so the pair's work is only the flow.

Then, once per synthetic output tick ($t$ from the output clock), in `GpuSynthesize`:

1. `flow_interp` ($A$, $B$, forward flow with its match cost, mask, backward flow → synthetic image), followed by the blit into the real image of $A$ or a hidden swapchain image from Stage 3.

The flow images are read by every tick of the pair, so the next pair's flow waits for the previous pair's synthesis to finish reading (ordered by the barriers inside the command buffers on the one queue). With the extrapolation mode (`VKIFRAME_EXTRAPOLATE=1`) the same shader runs with `extrap` set (section 4.E).

---

## 4. Shader Implementation (`shaders/flow_interp.comp`)

One dispatch per synthetic frame, `local_size 16x16`. Inputs: $A$ and $B$ (`rgba16f` ring images), the forward field `u_flow` (A→B, `rgba16f`: x, y, match cost, 0), the backward field `u_bwd` (B→A), the occlusion mask `u_mask` (`r32f`), and the output. Vectors are in pixels of the flow level; the shader scales them by the (power of two) ratio of output size to field size. Push constants: `t`, `sigma` (`INTERP_GUIDE`), `cost_lo` / `cost_hi` (`CONF_LO` / `CONF_HI`), `cand_thr` (`INTERP_CAND`), `pan_lo` / `pan_hi` (`PAN_LO` / `PAN_HI`), `bidir`, `ui_thr` (`UI_MASK_THRESHOLD`), `extrap`.

### A. Static UI detection (inside the shader, `VKIFRAME_UI_MASK_THRESHOLD`)

The first design had a separate pass writing an `r8` mask from the colour distance of one pixel. The code decides per pixel at the end of the shader: where the **3×3 mean of $|{\rm luma}(A) - {\rm luma}(B)|$** stays below the threshold, the pixel takes the plain cross-fade (for extrapolation, the pixel of $B$) instead of the moved one. The mean over nine pixels ignores film grain but not a moving edge. The weight is soft: `1 - smoothstep(thr, 2 thr, mean)`.

```glsl
float st = 1.0 - smoothstep(u_params.ui_thr, 2.0 * u_params.ui_thr, acc / 9.0);
c = mix(c, (u_params.extrap > 0.5) ? imageLoad(u_b, p) : (1.0 - t) * imageLoad(u_a, p) + t * imageLoad(u_b, p), st);
```

**Known weakness** (from the first design, still true): because "static" is judged from one pair, very slow motion over low-contrast areas (sky, dark scenes) can fall under the threshold and be copied, so it steps instead of gliding. Offline the mask brings the HUD error down to the cross-fade level, but whole-frame error also rises at the 0.03 setting (STATUS.md). Mitigations to try: require the pixel to stay static over several native pairs, restrict the mask to high-contrast or edge-adjacent pixels, or keep only a coarse 2D-overlay detector (screen-edge regions). The first design's default threshold was 0.02, chosen for 8-bit sRGB values; the comparison is in linear light, where dark tones are compressed. The shipped default is **0.08**, and 0 switches the mask off; it still needs a look test on the Deck (STATUS.md open item 3).

### B. Colour-guided vector weighting (replaces the bilateral filter pass)

The first design filtered the whole vector field in its own pass: a 5×5 window, spatial and colour Gaussians, and a rule that never averages UI vectors with scene vectors. The code does the smoothing only where the vector is used: the vector at output pixel $p$ is the bilinear blend of the four nearest field vectors, with each bilinear weight multiplied by `0.02 + exp(-d² / (2 σ²))`, where $d$ is the luma difference between the output pixel and the pixel at that field vector's centre (`GUIDED_FIELD` macro, σ = `VKIFRAME_INTERP_GUIDE`, default 0.08; 0 = plain bilinear). A vector from the other side of an edge gets almost no weight, so edge pixels keep the vector of their own side. The match cost (third channel) is blended with the same weights.

The field is read at its own level (quarter resolution by default) and the vector is scaled to full-resolution pixels. The sampler for $A$ and $B$ is bilinear, written by hand because GLSL cannot pass a formatted image as a parameter (`BILINEAR` macro: one function per frame).

**Format requirements** from the first design, still relevant: `r8` and `rg16f` storage images need `shaderStorageImageExtendedFormats`. The code avoids the question by using `rgba16f` for the fields and `r32f` for the mask and the luma pyramids, and it reads $A$ and $B$ with `imageLoad` and its own bilinear function instead of combined image samplers, so `ComputePass` needs nothing beyond storage images.

### C. Fallbacks to the cross-fade (replaces the mean-velocity reduction)

The first design reduced the mean flow magnitude into a storage buffer (`flow_stats.comp`, with fixed-point atomics) and ramped to a 50/50 blend above 32 pixels. The code has three smooth fallbacks, all in the one shader, all to the time-weighted cross-fade $(1-t)A + tB$:

* **Per pixel, match cost:** `smoothstep(cost_lo, cost_hi, cost)`; defaults `CONF_LO` 0.06, `CONF_HI` 0.14 (`HI <= LO` turns it off).
* **Per pixel, occlusion and speed:** the mask value, and `smoothstep(FAST_LOW, FAST_HIGH, |v|)` with 48 and 96 full-resolution pixels.
* **Whole frame, the pan fallback:** the share of the flow field that is occluded or badly matched, read from a fixed 8×6 grid of field pixels (48 samples). One workgroup works it out once, through shared memory (48 threads read one sample each) instead of every pixel reading all 48. The share goes through `smoothstep(pan_lo, pan_hi, share)` (`PAN_LO` 0.15, `PAN_HI` 0.35; `HI <= LO` = off) and fades the whole frame to the cross-fade, because during a fast camera pan interpolation only smears. Calm scenes measured 0.04 to 0.1 on real captures.

The pan statistic is a whole-frame value; a per-tile statistic could catch local fast motion better (open question, DESIGN §6).

### D. Interpolation: backward gather, one-sided and two-sided

Backward gather (DESIGN §2.C). With $V$ pointing from $A$ to $B$, the content at output pixel $p$ at time $t$ came from $p - tV$ in $A$ and will be at $p + (1-t)V$ in $B$. (The earlier draft used the opposite signs, which moves content the wrong way.)

* **One-sided** (`VKIFRAME_INTERP_BIDIR=0`): `moved = (1-t) A(p - tV) + t B(p + (1-t)V)`, then `mix(moved, cross-fade, k)` where $k$ is the largest of the mask, the match-cost fade, the pan fallback and the speed fade. The optional candidate search (`VKIFRAME_INTERP_CAND`, off by default) picks, at badly matched pixels, the neighbouring vector that makes $A$ and $B$ agree; on real game footage it made ghosting worse.
* **Two-sided** (`VKIFRAME_INTERP_BIDIR=1`, default): $A$ is moved forward by $t$ along the forward vector and $B$ is moved back by $1-t$ along the backward vector, each **trusted by its own** mask, match cost and speed, with weights $(1-t)c_f$ and $t\,c_b$. Uncovered background then comes only from $B$ and covered background only from $A$; where neither is trusted, the cross-fade carries the pixel. It needs the full-resolution backward flow, so it turns `VKIFRAME_FLOW_BWD_SKIP` off, and costs about 0.37 ms more per synthetic frame on the Deck. Offline (Cuphead dash) the error was 6 % lower than the one-sided form, with cleaner tree-trunk edges (STATUS.md).

The sampler uses clamp-to-edge. Interpolation happens in linear light.

### E. Extrapolation (`VKIFRAME_EXTRAPOLATE=1`, experimental)

The same shader with `extrap` set predicts a frame beyond $B$: $B$ is moved on along its own backward flow as if the motion went on for $s$ more native intervals, `out(x) = B(x + s v_b(x))`. It falls back to plain $B$ where the backward match is poor, the pixel is uncovered, the motion is fast, the vector is too small to be more than flow noise, or the whole frame cannot be matched. It is the low-latency mode of the README; the numbers are in STATUS.md.

### F. Diagnostics in the shader

With `VKIFRAME_TICKMARK` the push constant `t` carries extra information (`t >= 2`): the shader draws a 64×64 square (red for the first half of a pair, green for the second) and a bar along the bottom that steps every displayed frame, for looking at the cadence. It must stay off in normal use.

---

## 5. Differences from the first design, in one place

| First design | Code |
| :--- | :--- |
| `ui_mask.comp` writes an `r8` mask from the colour distance of one pixel; threshold 0.02 | In `flow_interp.comp`: 3×3 mean of the luma difference, soft threshold; default 0.08 |
| `vector_bilateral_filter.comp`, 5×5, spatial and colour Gaussians, UI-aware | Luma-guided weights on the four nearest vectors, in the interpolation shader (`INTERP_GUIDE`) |
| `flow_stats.comp`: mean flow magnitude with fixed-point atomics, ramp to 50/50 above 32 px (`VKIFRAME_MAX_VELOCITY_FALLBACK`) | Share of bad matches on an 8×6 grid inside the shader (`PAN_LO` / `PAN_HI`); no buffer, no atomics; fallback is the cross-fade |
| `motion_interp.comp` with `sampler2D` inputs, `rg16f`/`r8` images, a stats buffer | `flow_interp.comp` with `rgba16f`/`r32f` storage images and its own bilinear sampler |
| One-sided gather | One-sided and two-sided (default), match-cost fade, optional candidate search, extrapolation |
| `InterpolationEngine` class with `RecordPairPasses` / `RecordInterp` | `GpuSynthesize` binds the images and pushes ten floats per tick; the per-pair work is the flow only |
| Compute queue plus a flow timeline semaphore | Present queue; barriers inside the command buffers (STAGE4.md §3) |

---

## 6. Verification & Testing Procedure

### Step 1: Build
```bash
cmake --build build -j"$(nproc)"          # the shader list in CMakeLists.txt includes flow_interp
```

### Step 2: Run a validation target
Use a 3D game with persistent 2D UI (crosshair, health bar, chat), capped to 30 FPS:

```bash
export VK_LAYER_PATH=$PWD/build
export VK_LOADER_LAYERS_ENABLE=VK_LAYER_CUSTOM_frame_gen,VK_LAYER_KHRONOS_validation
export VKIFRAME_TARGET_FPS=60 VKIFRAME_INTERPOLATE=1 VKIFRAME_BASE_FPS_CAP=30
<game>                                    # or DXVK_FRAME_RATE=30 <game>
```

For image quality without a GPU, dump a capture (`VKIFRAME_DUMP=N`, `VKIFRAME_DUMP_DIR`) and score the methods offline with `tools/interp_eval` (README.md, "Evaluating changes offline").

### Step 3: Success criteria
1. **HUD integrity:** 2D interface elements stay sharp and still; they do not shift or smear into moving geometry (`VKIFRAME_UI_MASK_THRESHOLD`).
2. **Dynamic $t$:** with a 30 FPS game on a 90 FPS target the two synthetic frames sit visibly at 1/3 and 2/3 of the motion between the natives; with a 40 FPS game on 60 the synthetic frames alternate between roughly 1/3 and 2/3. Motion is in the correct direction and evenly paced. (With the phase lock and `PHASE_OFFSET` 0.5 at 45 to 90 the ticks sit at $t = 0.25$ and $0.75$.)
3. **Fallback:** rapid mouse swings fade into the cross-fade rather than tearing geometry, and the transition has no visible pop.
4. **Occlusions:** edges of moving objects show a cross-fade in newly revealed areas rather than stretched pixels. A ghost behind a moving sprite is the known remaining defect (STATUS.md open item 1).
5. No GPU hangs, no validation errors, and the combined cost stays within the Stage 6 budget (not met on the Deck today: flow about 5 ms per pair and interpolation 0.8 to 1.4 ms per synthetic frame, STATUS.md).
