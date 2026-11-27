[index](../STAGE4.md) | [next](02-shader-implementations.md)

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

