# Stage 5: Static UI Masking & Dynamic Motion Interpolation

## 1. Overview & Objectives

Stage 5 turns the motion field into clean intermediate frames. The layer will:

1. Detect static 2D elements (HUD, crosshairs, health bars) by difference-thresholding $Frame_A$ against $Frame_B$.
2. Filter the motion vectors with a **bilateral filter** guided by the frame colors and the UI mask, so background motion never bleeds across a UI boundary.
3. Reduce the mean flow magnitude **on the GPU** (no CPU readback) to drive the high-velocity fallback.
4. Run the **backward-gather interpolation** with the per-tick weight $t$ from the output clock (DESIGN §2.C), replacing Stage 3's blend.
5. Handle occlusions and fast motion by falling back to cross-fade and, smoothly, to a 50/50 alpha blend.

---

## 2. Repository Changes

```text
include/
└── interpolation_engine.hpp                   # new: records the Stage 5 passes
shaders/
├── ui_mask.comp                         # new (replaces the empty placeholder of the same name)
├── vector_bilateral_filter.comp         # new
├── flow_stats.comp                      # new
└── motion_interp.comp                     # new (replaces blend.comp as the synthetic-frame shader)
```

---

## 3. Pass Order

For each native pair, once, on the compute queue after the flow (Stage 4):

1. `ui_mask`  ($A$, $B$ → UI mask)
2. `vector_bilateral_filter` (forward flow, $B$, UI mask → filtered flow)
3. `flow_stats` (filtered flow → mean magnitude buffer; the buffer is cleared with `vkCmdFillBuffer` first)

Then, once per synthetic output tick ($t$ from the output clock), on the graphics queue:

4. `motion_interp` ($A$, $B$, filtered flow, occlusion mask, UI mask, stats buffer → synthetic image), followed by the blit into the hidden swapchain image from Stage 3.

Steps 1–3 depend only on the pair, so they run once however many ticks the pair has.

---

## 4. Shader Implementations

All images are `rgba16f` (linear) unless stated. Flow vectors are in full-resolution pixels, pointing from $A$ to $B$ (Stage 4).

### A. Static UI Detection Mask (`shaders/ui_mask.comp`)

```glsl
#version 450
layout(local_size_x = 16, local_size_y = 16, local_size_z = 1) in;

layout(set = 0, binding = 0, rgba16f) uniform readonly  image2D u_frame_a;
layout(set = 0, binding = 1, rgba16f) uniform readonly  image2D u_frame_b;
layout(set = 0, binding = 2, r8)      uniform writeonly image2D u_ui_mask;

layout(push_constant) uniform Params { float threshold; } u_params;   // VKIFRAME_UI_MASK_THRESHOLD

void main() {
    ivec2 pos = ivec2(gl_GlobalInvocationID.xy);
    if (any(greaterThanEqual(pos, imageSize(u_frame_a)))) return;

    float diff = distance(imageLoad(u_frame_a, pos).rgb, imageLoad(u_frame_b, pos).rgb);

    // Pixels that stay the same while the scene moves are 2D overlays. Static background pixels are also
    // flagged; copying those is the correct result anyway.
    imageStore(u_ui_mask, pos, vec4(diff < u_params.threshold ? 1.0 : 0.0));
}
```

**Known weakness:** because "static" is judged from one pair, very slow motion over low-contrast areas (sky, dark scenes) can fall under the threshold and be copied from $A$, so it steps instead of gliding. Mitigations to try: require the pixel to stay static over several native pairs, restrict the mask to high-contrast or edge-adjacent pixels, or keep only a coarse 2D-overlay detector (screen-edge regions and high-contrast edges).

The default threshold (0.02) was chosen for 8-bit sRGB values. The comparison now happens in linear light, where dark tones are compressed, so it needs re-tuning on real content.

### B. Bilateral Vector Filter (`shaders/vector_bilateral_filter.comp`)

```glsl
#version 450
layout(local_size_x = 16, local_size_y = 16, local_size_z = 1) in;

layout(set = 0, binding = 0, rg16f)   uniform readonly  image2D u_flow_in;
layout(set = 0, binding = 1, rgba16f) uniform readonly  image2D u_guide;       // frame B
layout(set = 0, binding = 2, r8)      uniform readonly  image2D u_ui_mask;
layout(set = 0, binding = 3, rg16f)   uniform writeonly image2D u_flow_out;

const int   R           = 2;
const float SIGMA_SPACE = 2.0;
const float SIGMA_COLOR = 0.1;

void main() {
    ivec2 pos  = ivec2(gl_GlobalInvocationID.xy);
    ivec2 size = imageSize(u_flow_in);
    if (any(greaterThanEqual(pos, size))) return;

    vec3  c0  = imageLoad(u_guide, pos).rgb;
    bool  ui0 = imageLoad(u_ui_mask, pos).r > 0.5;

    vec2  acc  = vec2(0.0);
    float wsum = 0.0;
    for (int dy = -R; dy <= R; ++dy)
        for (int dx = -R; dx <= R; ++dx) {
            ivec2 q  = clamp(pos + ivec2(dx, dy), ivec2(0), size - 1);
            vec3  dc = imageLoad(u_guide, q).rgb - c0;
            float ws = exp(-float(dx * dx + dy * dy) / (2.0 * SIGMA_SPACE * SIGMA_SPACE));
            float wc = exp(-dot(dc, dc) / (2.0 * SIGMA_COLOR * SIGMA_COLOR));
            // Never average UI vectors with scene vectors.
            float wu = ((imageLoad(u_ui_mask, q).r > 0.5) == ui0) ? 1.0 : 0.0;
            float w  = ws * wc * wu;
            acc  += w * imageLoad(u_flow_in, q).xy;
            wsum += w;
        }

    imageStore(u_flow_out, pos, vec4(acc / max(wsum, 1e-6), 0.0, 0.0));
}
```

> **Format requirements.** `r8` and `rg16f` storage images (here and in Stages 4–5) need the `shaderStorageImageExtendedFormats` device feature. Check it at device creation and enable it, or fall back to `rgba16f` for these images. `motion_interp` also binds `sampler2D` inputs, so `ComputePass` (Stage 3) must support combined image samplers as well as storage images.

### C. Mean-Velocity Reduction (`shaders/flow_stats.comp`)

The interpolation shader needs the mean flow magnitude to decide the fallback. Reading it back to the CPU would stall the pipeline, so the reduction writes a small storage buffer that the interpolation shader reads directly.

```glsl
#version 450
layout(local_size_x = 16, local_size_y = 16, local_size_z = 1) in;

layout(set = 0, binding = 0, rg16f) uniform readonly image2D u_flow;
layout(set = 0, binding = 1) buffer FlowStats { uint sum_fixed; } u_stats;   // cleared to 0 before dispatch

shared uint s_sum;

void main() {
    if (gl_LocalInvocationIndex == 0u) s_sum = 0u;
    barrier();

    ivec2 pos = ivec2(gl_GlobalInvocationID.xy);
    if (all(lessThan(pos, imageSize(u_flow)))) {
        float mag = min(length(imageLoad(u_flow, pos).xy), 64.0);   // clamp so the 32-bit sum cannot overflow
        atomicAdd(s_sum, uint(mag * 16.0));                         // fixed point, 1/16 pixel
    }
    barrier();

    if (gl_LocalInvocationIndex == 0u) atomicAdd(u_stats.sum_fixed, s_sum);
}
```

At 64 px × 16 × 2.07 M pixels (1080p) the worst-case sum is about 2.1 G, below 2³². That still fits at 1440p (3.7 M pixels × 1024 ≈ 3.8 G, below 4.29 G) but overflows at 4K: clamp lower or accumulate per row there.

### D. Dynamic Motion Interpolation (`shaders/motion_interp.comp`)

Backward gather (DESIGN §2.C). With $V$ pointing from $A$ to $B$, the content at output pixel $p$ at time $t$ came from $p - tV$ in $A$ and will be at $p + (1-t)V$ in $B$. (The earlier draft used the opposite signs, which moves content the wrong way.)

```glsl
#version 450
layout(local_size_x = 16, local_size_y = 16, local_size_z = 1) in;

layout(set = 0, binding = 0) uniform sampler2D u_tex_a;                       // ring image A (RGBA16F, linear sampling)
layout(set = 0, binding = 1) uniform sampler2D u_tex_b;
layout(set = 0, binding = 2, rg16f)   uniform readonly  image2D u_flow;       // filtered, full resolution
layout(set = 0, binding = 3, r8)      uniform readonly  image2D u_occlusion;
layout(set = 0, binding = 4, r8)      uniform readonly  image2D u_ui_mask;
layout(set = 0, binding = 5, rgba16f) uniform writeonly image2D u_out;
layout(set = 0, binding = 6) readonly buffer FlowStats { uint sum_fixed; } u_stats;

layout(push_constant) uniform Params {
    float t;                // t of this output tick (DESIGN §2.C)
    float max_velocity;     // VKIFRAME_MAX_VELOCITY_FALLBACK, default 32
    float fallback_range;   // pixels over which the blend ramps in, default 16
} u_params;

void main() {
    ivec2 pos  = ivec2(gl_GlobalInvocationID.xy);
    ivec2 size = imageSize(u_out);
    if (any(greaterThanEqual(pos, size))) return;

    vec2 uv = (vec2(pos) + 0.5) / vec2(size);

    // Static UI: take it from A unchanged; never move it.
    if (imageLoad(u_ui_mask, pos).r > 0.5) {
        imageStore(u_out, pos, textureLod(u_tex_a, uv, 0.0));
        return;
    }

    vec2  v   = imageLoad(u_flow, pos).xy;
    float occ = imageLoad(u_occlusion, pos).r;

    vec4 plain_a = textureLod(u_tex_a, uv, 0.0);
    vec4 plain_b = textureLod(u_tex_b, uv, 0.0);

    vec4 color;
    if (occ > 0.5) {
        color = mix(plain_a, plain_b, u_params.t);                      // no reliable motion: cross-fade
    } else {
        vec2 uv_a = uv - (v * u_params.t) / vec2(size);
        vec2 uv_b = uv + (v * (1.0 - u_params.t)) / vec2(size);
        color = mix(textureLod(u_tex_a, uv_a, 0.0), textureLod(u_tex_b, uv_b, 0.0), u_params.t);
    }

    // High-velocity fallback: ramp toward a 50/50 blend when mean motion exceeds the stability limit.
    float mean_v = float(u_stats.sum_fixed) / (16.0 * float(size.x) * float(size.y));
    float fallback = clamp((mean_v - u_params.max_velocity) / u_params.fallback_range, 0.0, 1.0);
    color = mix(color, mix(plain_a, plain_b, 0.5), fallback);

    imageStore(u_out, pos, color);
}
```

The sampler uses linear filtering and clamp-to-edge. The mean is a whole-frame value; a per-tile statistic could catch local fast motion better (open question, DESIGN §6).

---

## 5. Interpolation Engine Integration (`include/interpolation_engine.hpp`)

```cpp
#pragma once

#include "vk_iframes_layer.hpp"
#include "compute_pass.hpp"

struct InterpParams {
    float t;
    float max_velocity;
    float fallback_range;
};

class InterpolationEngine {
public:
    bool Initialize(const DeviceDispatch& dt, VkExtent2D extent, uint32_t compute_family);
    void Cleanup();                                       // caller guarantees the GPU is idle
    bool RecreateForExtent(VkExtent2D extent);            // Stage 6

    // Once per native pair, recorded into the compute command buffer after the optical flow.
    void RecordPairPasses(VkCommandBuffer cmd, VkImageView a, VkImageView b,
                          VkImageView flow, VkImageView occlusion);

    // Once per synthetic frame, recorded into the graphics command buffer.
    // The velocity fallback is computed inside the shader from the GPU-side statistics.
    void RecordInterp(VkCommandBuffer cmd, VkImageView a, VkImageView b, VkImageView out, const InterpParams& p) {
        m_interp_pass.Dispatch(cmd, {a, b, m_filtered_flow_view, m_occlusion_view, m_ui_mask_view, out, m_stats_buffer},
                             &p, sizeof(p), (m_extent.width + 15) / 16, (m_extent.height + 15) / 16);
    }

private:
    const DeviceDispatch* m_dt{};
    VkExtent2D  m_extent{};
    ComputePass m_ui_mask_pass, m_bilateral_pass, m_stats_pass, m_interp_pass;
    VkImageView m_ui_mask_view{}, m_filtered_flow_view{}, m_occlusion_view{};
    VkBuffer    m_stats_buffer{};
    // images, buffer, sampler, memory ...
};
```

`RecordPairPasses` clears the stats buffer with `vkCmdFillBuffer`, dispatches the three pair passes with compute-to-compute barriers in between, and a buffer barrier before the stats are read by the interpolation pass. `Hook_QueuePresentKHR`'s `PresentSynthetic` (Stage 3) calls `RecordInterp` where it used to dispatch `blend.comp`, and waits on the flow timeline before doing so.

---

## 6. Verification & Testing Procedure

### Step 1: Build
```bash
cmake --build build -j"$(nproc)"          # the shader list in CMake now also contains ui_mask, vector_bilateral_filter, flow_stats, motion_interp
```

### Step 2: Run a validation target
Use a 3D game with persistent 2D UI (crosshair, health bar, chat), capped to 30 FPS:

```bash
export VK_LAYER_PATH=$PWD/build
export VK_LOADER_LAYERS_ENABLE=VK_LAYER_CUSTOM_frame_gen,VK_LAYER_KHRONOS_validation
export VKIFRAME_TARGET_FPS=60
DXVK_FRAME_RATE=30 <game>
```

### Step 3: Success criteria
1. **HUD integrity:** 2D interface elements stay sharp and still; they do not shift or smear into moving geometry.
2. **Dynamic $t$:** with a 30 FPS game on a 90 FPS target the two synthetic frames sit visibly at 1/3 and 2/3 of the motion between the natives; with a 40 FPS game on 60 the synthetic frames alternate between roughly 1/3 and 2/3. Motion is in the correct direction and evenly paced.
3. **Fallback:** rapid mouse swings fade into soft 50/50 blending rather than tearing geometry, and the transition has no visible pop.
4. **Occlusions:** edges of moving objects show a plain cross-fade in newly revealed areas rather than stretched pixels.
5. No GPU hangs, no validation errors, and the combined cost stays within the Stage 6 budget.
