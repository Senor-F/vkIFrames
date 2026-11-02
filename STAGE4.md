# Stage 4: Hierarchical Optical Flow Compute Engine

> **Status: built and running, on the present queue.** This page keeps the original Stage 4 design (the goals, the conventions, the queue-selection plan, the shader ideas, the engine interface, the build and the test procedure) and brings it up to the code on `main`. The motion estimation itself is done and verified; what was **not** built is the asynchronous compute queue with timeline semaphores (section 3). That is item 9 of AUDIT.md, last in the order of work, and only if the measurements show it is worth it (ASYNC.md: the layer's cost is serial GPU time, so a second queue does not add capacity). Where the code differs from the first design, the text says so. Settings use the `VKIFRAME_*` names; the old `FRAMEGEN_*` names are still read as a fallback.

## 1. Overview & Objectives

Stage 4 builds the motion estimation core: dense per-pixel displacement vectors between $Frame_A$ and $Frame_B$. The layer:

1. ~~Chooses a compute-capable queue family (preferably one without graphics) and requests a queue from it when the device is created.~~ **Not built.** Flow is recorded in one command buffer per pair on the present queue (section 3 keeps the plan for the day this is built).
2. Builds compute pipelines for a 4-level downsample pyramid. Level 1 is the luma of the frame at half resolution, levels 2 to 4 are 1/4, 1/8 and 1/16. A fifth level is a small change (`FlowState::kLevels`, the tables in `include/flow_ref.hpp`).
3. Runs coarse-to-fine block matching to track large motion (critical for 30 FPS base rates): an exhaustive search at the coarsest level, then a refinement at each finer level.
4. Runs the search in both directions ($A \rightarrow B$ and $B \rightarrow A$) and compares them to produce an **occlusion mask**.
5. ~~Synchronizes the compute queue with the graphics queue using timeline semaphores.~~ **Not built**, for the same reason. The passes are ordered by memory barriers inside the command buffer, and the command buffer is ordered after the captures of both frames.

**Conventions.** Frames come from the Stage 3 ring (RGBA16F, linear light). Vectors point from $A$ to $B$: $A(p) \approx B(p + v)$. Each pyramid level works in its own pixels and doubles the vector on the way up; the finished field is at the level of `VKIFRAME_FLOW_LEVEL`. The first design stored the field at **full frame resolution** in `RG16F`; the code stores `RGBA16F` (x, y, **match cost**, 0) at the finished level, which is quarter resolution by default (level 2). Multiply a vector by $2^{level}$ for full-resolution pixels. The interpolation (Stage 5) samples the field bilinearly and uses the match cost to fade to a cross-fade where the match is poor.

**The reference implementation.** `include/flow_ref.hpp` is a CPU version of the same algorithm. `tests/test_flow_ref.cpp` (`flow_reference`) checks it against known motion, and the GPU shaders mirror it (each shader's header names its `flowref::` function). On the RX 9070 machine the GPU result matches it bit for bit.

---

## 2. Repository Changes

```text
include/
├── flow_ref.hpp             # CPU reference of the whole algorithm and its constants (tested)
├── gpu_frames.hpp           # FlowState (pipelines, images, descriptor sets, timestamps), FlowImage
└── gpu_internal.hpp         # helpers shared with Stage 3: barriers, command buffer entries, CreateStorageImage
src/
└── optical_flow.cpp         # FlowCreate / FlowDestroy / GpuFlow: pipelines, descriptors, command recording
shaders/
├── flow_luma.comp           # level 1: luma of the ring image, 2x2 average
├── flow_down.comp           # levels 2..4: 2x2 average
├── flow_coarse.comp         # coarsest level: exhaustive search
├── flow_refine.comp         # one finer level: start from the coarser vector, small search, optional sub-pixel fit
├── flow_occlusion.comp      # forward/backward consistency -> mask
├── flow_vis.comp            # debug view of the motion field (VKIFRAME_DEBUG_FLOW=1)
└── flow_interp.comp         # Stage 5: the interpolation that uses the field (see STAGE5.md)
tools/interp_eval/           # offline scoring of interpolation methods on dumped captures
```

The first design listed `optical_flow.hpp`, `downsample.comp`, `optical_flow_coarse.comp`, `optical_flow_refine.comp` and `occlusion_check.comp`. The engine interface lives in `gpu_frames.hpp` and `src/optical_flow.cpp`, and the shaders were renamed `flow_*`.

---

## 3. Queue Selection and Device Creation: the plan for the async queue (not built)

Today `GpuFlow` records on the queue of the game's present, inside `ExecutePair` on the presenter thread. The plan below is the original design and stays valid for when the asynchronous queue is taken up (AUDIT.md item 9, ASYNC.md). It would also need the ring to become `CONCURRENT` across the two families, and flow to wait on a timeline semaphore signalled by the capture.

### A. Pick a compute family

```cpp
static uint32_t PickComputeFamily(const std::vector<VkQueueFamilyProperties>& fam) {
    for (uint32_t i = 0; i < fam.size(); ++i)                       // dedicated async compute family
        if ((fam[i].queueFlags & VK_QUEUE_COMPUTE_BIT) && !(fam[i].queueFlags & VK_QUEUE_GRAPHICS_BIT)) return i;
    return UINT32_MAX;                                              // none: run flow on the game's queue instead
}
```

### B. Add the queue in `Hook_CreateDevice`

`VkDeviceQueueCreateInfo::pQueuePriorities` points at an application-owned array sized for the application's `queueCount`, so raising the count requires a new array.

```cpp
std::vector<VkDeviceQueueCreateInfo> qci(ci.pQueueCreateInfos, ci.pQueueCreateInfos + ci.queueCreateInfoCount);
std::vector<std::vector<float>> prio_storage;                       // keeps the new arrays alive until vkCreateDevice returns
prio_storage.reserve(qci.size() + 1);                               // no reallocation: pointers stay valid

uint32_t compute_family = PickComputeFamily(families);
uint32_t compute_index  = 0;
if (compute_family != UINT32_MAX) {
    auto it = std::find_if(qci.begin(), qci.end(), [&](auto& q) { return q.queueFamilyIndex == compute_family; });
    if (it == qci.end()) {
        prio_storage.push_back({1.0f});
        qci.push_back({VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, nullptr, 0, compute_family, 1, prio_storage.back().data()});
    } else if (it->flags == 0 && it->queueCount < families[compute_family].queueCount) {
        compute_index = it->queueCount;                             // the new queue is the one after the app's
        prio_storage.emplace_back(it->pQueuePriorities, it->pQueuePriorities + it->queueCount);
        prio_storage.back().push_back(1.0f);
        it->queueCount += 1;
        it->pQueuePriorities = prio_storage.back().data();
    } else {
        compute_family = UINT32_MAX;                                // no spare queue
    }
}
ci.queueCreateInfoCount = uint32_t(qci.size());
ci.pQueueCreateInfos    = qci.data();
```

After `vkCreateDevice` succeeds, `vkGetDeviceQueue(device, compute_family, compute_index, &compute_queue)` stores the queue in the device state. If `compute_family == UINT32_MAX`, flow work stays on the game's queue, as it does today; it is still correct, just not asynchronous.

### C. Learn which family the game's queue belongs to

This part **is built** (Stage 3 needs it): the layer hooks `vkGetDeviceQueue` and `vkGetDeviceQueue2` and records `queue handle -> family index` (`QueueFamilyOf`, `src/layer_dispatch.cpp`). With a separate compute family, the ring would use `VK_SHARING_MODE_CONCURRENT` across `{graphics family, compute family}`; today it is `EXCLUSIVE`.

---

## 4. Shader Implementations

All shaders use `local_size 16x16`. Bounds are always clamped: out-of-range `imageLoad` is undefined behavior, so every neighbor read is clamped to the edge. Frames enter as `rgba16f` (linear RGB); everything after the first pass works on **luma** in `r32f`, because the search compares brightness patches and one channel is a third of the memory traffic of three.

### A. Pyramid (`shaders/flow_luma.comp`, `shaders/flow_down.comp`)

Pyramid level 1 is not the ring image itself, as the first design had it: `flow_luma.comp` reads the RGBA16F ring image and writes the **luma** (Rec. 709 weights 0.2126 / 0.7152 / 0.0722; `flow_interp.comp` computes its own luma with the Rec. 601 weights 0.299 / 0.587 / 0.114) of a 2x2 box average, so level 1 is half resolution. `flow_down.comp` makes levels 2 to 4 by the 2x2 average of the level above. Sizes round up and reads clamp to the edge. Both mirror `flowref::Down`. The pyramid exists for A and for B (`FlowState::pyr[2][4]`, `R32F`).

```glsl
float luma(ivec2 q, ivec2 m) { return dot(imageLoad(u_src, min(q, m)).rgb, vec3(0.2126, 0.7152, 0.0722)); }
...
float l = luma(s, m) + luma(s + ivec2(1, 0), m) + luma(s + ivec2(0, 1), m) + luma(s + ivec2(1, 1), m);
imageStore(u_dst, p, vec4(l * 0.25, 0.0, 0.0, 0.0));
```

### B. Coarse optical flow (`shaders/flow_coarse.comp`)

Runs at the coarsest level (1/16). The ±4 search radius there covers about ±64 full-resolution pixels. For each pixel it tries every offset in the window and keeps the one with the lowest **mean absolute luma difference** over a patch (the first design summed the colour distance of a 3x3 patch; the code uses one channel and a patch radius that grows toward the coarse levels, `kPatch = {1, 2, 3, 3}`, so the coarsest level compares 7x7 patches, which is cheap there). Two small penalties keep flat areas from producing random vectors, the same idea as the first design's tie-break toward zero motion:

```glsl
const int   RADIUS    = 4;
const float TIE_BIAS  = 1e-4;     // prefers small vectors
const float ZERO_MARGIN = 2e-3;   // a non-zero vector must beat the zero vector by this much (mean abs luma)
...
float c = patch_cost(pos, cand, u_params.patch_radius, size) + TIE_BIAS * float(dx * dx + dy * dy) + ((dx != 0 || dy != 0) ? ZERO_MARGIN : 0.0);
if (c < best) { best = c; bv = ivec2(dx, dy); }
...
imageStore(u_flow, pos, vec4(vec2(bv), best, 0.0));      // (vx, vy, match cost, 0)
```

The zero-vector margin was added after the first version, so that flat areas no longer inherit coarse noise (the old repository's commit `Flow: zero-vector candidate with a margin, flat areas no longer inherit coarse noise`).

### C. Per-level refinement (`shaders/flow_refine.comp`)

Dispatched once for each level from coarse to fine, down to the finished level (`VKIFRAME_FLOW_LEVEL`). Where the first design upsampled the coarser vector ×2 and searched ±1 pixel around it, the code is a little richer, all of it mirrored in `flowref::Refine`:

* **Candidates from four coarse neighbours.** The start vector is the best of the pixel's own coarse vector and its three nearest other coarse vectors (each doubled and rounded). This lets a pixel at a motion edge pick the neighbour that belongs to its side.
* **A small search** around the best start, with radius `kRadius = {1, 1, 2, 2}` (the finer levels only correct the half-pixel error left by the coarser).
* **A pull toward the coarse vector** (`kLambda = 2e-4` times the squared distance), plus the same zero-vector margin as above. The zero vector is always a candidate and is compared last, without the coarse-vector penalty, so flat areas drop a wrong coarse vector but the search centre never jumps to zero.
* **Sub-pixel vectors at the finished level** (`VKIFRAME_FLOW_SUBPIXEL`, default on): a parabola through the plain patch costs at the best vector and its four neighbours gives a fractional vector. At the coarser levels the vectors stay whole pixels.
* The output keeps the **match cost** in the third channel for the interpolation's confidence fade.

For the backward field, the whole chain runs again with $A$ and $B$ swapped. With the interpolation's default two-sided mode (`VKIFRAME_INTERP_BIDIR=1`) both fields are used at full quality. With the one-sided interpolation, `VKIFRAME_FLOW_BWD_SKIP` can stop the backward refinement one level short (the mask then reads the coarser backward field, and the threshold gets a pixel of slack). Mode 1 (the default) does this only when the finished level is 1; mode 2 does it at any level and stays off by default, because the smudging on tree trunks got worse in a look test (STATUS.md).

### D. Forward-backward occlusion check (`shaders/flow_occlusion.comp`)

The mask is no longer a hard 0/1 as in the first design. For each pixel it follows the forward vector to the target, looks up the backward vector there, and takes how far the two are from cancelling; a smooth step turns that into a value between 0 (the vectors agree) and 1 (they do not, or the target is outside the frame):

```glsl
vec2  f = imageLoad(u_fwd, pos).xy;
ivec2 t = ivec2(floor(vec2(pos) + f + 0.5));
float m;
if (any(lessThan(t, ivec2(0))) || any(greaterThanEqual(t, size))) m = 1.0;                // leaves the frame -> occluded
else {
    float s = u_params.bwd_scale;                                                         // 1 = same level, 2 = the backward field is one level coarser
    ivec2 tb = clamp(ivec2(floor((vec2(t) + 0.5) / s)), ivec2(0), imageSize(u_bwd) - 1);
    vec2 b = imageLoad(u_bwd, tb).xy * s;
    m = smoothstep(u_params.threshold, 2.0 * u_params.threshold, length(f + b));
}
```

The threshold is `kOcclThreshold = 1.5` pixels of the finished level (the first design had a hard 2.5). The mask is allocated at level-1 size (`R32F`); the part that covers the finished field is used.

---

## 5. Optical Flow Engine (`src/optical_flow.cpp`)

The engine's state is `FlowState` inside `GpuState` (`include/gpu_frames.hpp`); `FlowCreate` builds the pipelines and images once per swapchain, `GpuFlow` computes the flow of a pair, `FlowDestroy` frees them. Only `GpuState` needs to know its sizes.

* **Images** (all stay in `GENERAL`, created device-local): the luma pyramids `pyr[2][4]` (`R32F`), the forward and backward fields `fwd[4]`, `bwd[4]` (`RGBA16F`; the finished field is `fwd[fin]`, `fin = FLOW_LEVEL - 1`), and the mask (`R32F`).
* **Descriptor sets:** `kPasses = 19` per command buffer entry, in the order documented at the top of `optical_flow.cpp`: luma A and B, down A and B (levels 2 to 4), coarse forward and backward, refine forward and backward (levels 3, 2, 1), occlusion, the debug view, the interpolation. Sets 17 and 18 are bound by `GpuSynthesize`.
* **One command buffer per pair**, recorded on the present queue from the presenter thread under `gpu_mu`: luma of A and B, a barrier, the down passes, the coarse search, the refinements, the same chain with A and B swapped, then the occlusion pass. Between passes the code records a compute-to-compute memory barrier (`COMPUTE_SHADER` write to read and write, `gpuint::MemBarrier`). Everything queued before (the captures, the previous pair's reads of these images) is ordered before the first pass by one `ALL_COMMANDS` memory barrier.
* **Called from** `ExecutePair` once per pair, before the tick loop, when `VKIFRAME_FLOW=1`, `VKIFRAME_INTERPOLATE=1` (the interpolation needs the field and implies flow) or `VKIFRAME_DEBUG_FLOW=1`. A failure at any step sets `failed` and switches only the engine off for that swapchain; synthetic frames fall back to the plain blend and the game is not touched.
* **Timing:** timestamp queries between the stages feed the log line `flow: GPU x.xx ms per pair (max ...) over N pairs, field WxH` and `flow stages (ms, mean): pyramid | coarse | refine L3 | refine L2 | ... | occlusion`, every 120 pairs, and the stats and benchmark (`VKIFRAME_STATS`, `bench.json`). The interpolation pass has its own timestamps (`interpolation: GPU ... ms per generated frame`).
* **Async queue:** not built. The interface of the first design (`Initialize`, `Submit` returning a timeline value, `FlowTimeline()`) would come back with it.

The game's own work is delayed only by the layer's serial GPU time on the same queue: the work is submitted from the presenter thread, and the game thread waits only for the lock and the command-buffer fence (`wait_lock_ns`, `wait_fence_ns` in the stats).

**Measured cost** (STATUS.md has the full records and their conditions): on the Steam Deck at 1280x720, the quarter-resolution field with narrower mid-level search and sub-pixel vectors took 4.8 to 5.0 ms per pair (about 22 % of GPU time at 45 pairs per second); the cost later drifted to 6.1 ms and, in the minutes after a benchmark, 7.5 to 8.4 ms, for a cause that is not known. On the RX 9070 desktop it is about 1.1 ms per pair at 500x500. The provisional budget of this stage (1.5 ms at 1080p, 2.5 ms for the whole chain in Stage 6) is therefore **not met on the Deck**; reducing the cost is open work.

---

## 6. Build: Compile and Embed SPIR-V

A layer is loaded from anywhere, so it must not look for `.spv` files by relative path. Compile at build time and embed the bytes in the library. `CMakeLists.txt` lists each shader with the name of its array and compiles it with `glslangValidator`:

```cmake
set(FRAMEGEN_SHADERS blend:kBlendSpv flow_luma:kFlowLumaSpv flow_down:kFlowDownSpv flow_coarse:kFlowCoarseSpv
                     flow_refine:kFlowRefineSpv flow_occlusion:kFlowOcclusionSpv flow_vis:kFlowVisSpv flow_interp:kFlowInterpSpv)
...
COMMAND ${FRAMEGEN_GLSLANG} -V -S comp --vn ${symbol} -o ${hdr} ${CMAKE_SOURCE_DIR}/shaders/${sname}.comp
```

Each shader is then available as `static const uint32_t k<Name>Spv[]` in `generated/<name>_spv.h`, and is passed to `vkCreateShaderModule`. Install `glslang-tools` (or the Vulkan SDK). `-DFRAMEGEN_SKIP_SHADERS=ON` embeds stubs instead, for the mock tests without a GPU: the layer then reports that the shader is not built and never makes synthetic frames. To inspect a shader by hand, `glslangValidator -V -S comp shaders/x.comp -o x.spv` produces a plain `.spv`. A new shader file is added to this list and to `FlowState` / `GpuInit`.

---

## 7. Verification & Testing Procedure

### Step 1: Build and run the reference test
```bash
cmake -B build && cmake --build build -j"$(nproc)"
ctest --test-dir build -R flow_reference     # the CPU reference against known motion
```
The mock tests skip the flow engine (the stub shaders are not valid SPIR-V), so only a run on a real GPU exercises the shaders; the GPU result was compared with the reference on the RX 9070 machine.

### Step 2: Debug view and dump
`VKIFRAME_DEBUG_FLOW=1` replaces every synthetic frame with the motion-field picture (`flow_vis.comp`): hue = direction, saturation = length, a white wash where the pixel is occluded.

`VKIFRAME_DUMP=N` writes the capture of pair N under `VKIFRAME_DUMP_DIR` (default `/tmp/vkIFrames_dump`; under Steam or Proton `/tmp` is private to the game's container, so use a folder under the home directory): `B.rgba16f`, `flow_fwd.rgba16f`, `flow_bwd.rgba16f`, `mask.r32f`, and for five consecutive pairs the native frame `pair<k>_A.rgba16f` and the synthetic frames `synth<nn>_t<t>.rgba16f`. The folder is created with mode 0700 and the layer refuses a folder that is not the user's own, because these files are screenshots. `tools/interp_eval` scores interpolation methods on such dumps offline.

```bash
export VK_LAYER_PATH=$PWD/build
export VK_LOADER_LAYERS_ENABLE=VK_LAYER_CUSTOM_frame_gen,VK_LAYER_KHRONOS_validation
export VKIFRAME_DEBUG_FLOW=1        # or VKIFRAME_FLOW=1 for the log lines only
vkcube                              # or any camera-pan scene capped at 30 FPS (VKIFRAME_BASE_FPS_CAP=30)
```

### Step 3: Success criteria
1. **Motion vectors:** the color-coded map matches the visible camera or object motion in direction and magnitude; static areas are near zero.
2. **Occlusion mask:** newly revealed and leaving regions are highlighted at the borders of moving objects.
3. **Budget (provisional):** the whole chain, both directions, measured with the timestamp queries, is **≤ 1.5 ms at 1080p** (headroom within the 2.5 ms total of Stage 6). Not met on the Deck today (section 5).
4. No device loss, no hangs, no validation errors, and the game's own frame time does not increase measurably (the measured increase is in STATUS.md).

---

## Implementation notes (engine written)

These notes came with the first version of the engine and are kept, brought up to date.

- `src/optical_flow.cpp`: `FlowCreate` / `FlowDestroy` / `GpuFlow`. One command buffer per pair on the present queue, 19 descriptor sets per command-buffer entry (layout documented at the top of the file). Called from `ExecutePair` once per pair, before the tick loop, when `VKIFRAME_FLOW=1`, `VKIFRAME_INTERPOLATE=1` or `VKIFRAME_DEBUG_FLOW=1`.
- Algorithm = `include/flow_ref.hpp` (tested by `flow_reference`): levels 1..4, exhaustive ±4 at level 4, refine with patch/radius {1,2,3,3} / {1,1,2,2}, forward and backward, consistency mask.
- Since Stage 5 the flow **does** change the synthetic frames: with `VKIFRAME_INTERPOLATE=1` the interpolation (`flow_interp.comp`) uses the field, the match cost and the mask; without it (or if flow fails) the plain blend of Stage 3 is used. The first notes said the opposite, which was true before Stage 5.
- GPU time: timestamp queries, logged every 120 pairs as `flow: GPU x.xx ms per pair` and `flow stages`.
- The shaders were not compiled in the sandbox where the engine was first written, so the first GPU build could report glslang errors. The mock tests still skip the engine (stub shaders), so only a run on a real GPU exercises it.
