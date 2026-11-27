[index](../STAGE3.md) | [next](02-output-clock-per-tick-t-and-the-bypass-g.md)

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
├── live_config.cpp         # live.conf
└── live_config_io.cpp      # live.log, status.json, file reads and writes
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

`ENABLED=0` (live) makes the whole layer a plain bypass, including no base-rate cap (except during a benchmark's "off" seconds, where the cap stays in force so both states pace the game the same way). The earlier draft of this page used 1.12 as the default for `MIN_GAIN`; the code's default is now 1.01, and the 1.12 value is what the simulation tuned (see the table in 4.E). DESIGN.md §2.B and STAGE6.md §4 say the same.

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

