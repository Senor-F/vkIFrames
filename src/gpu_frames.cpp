#include "gpu_internal.hpp"
#include "live_config.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <initializer_list>
#include <cstdint>
#include <time.h>
#include "blend_spv.h"      // generated from shaders/blend.comp by CMake (kBlendSpv)

namespace gpuint {

void Barrier(const DeviceDispatch& dt, VkCommandBuffer cb, VkImage image, VkImageLayout from, VkImageLayout to,
             VkPipelineStageFlags2 src_stage, VkAccessFlags2 src_access, VkPipelineStageFlags2 dst_stage, VkAccessFlags2 dst_access) {
    VkImageMemoryBarrier2 b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
    b.srcStageMask = src_stage; b.srcAccessMask = src_access; b.dstStageMask = dst_stage; b.dstAccessMask = dst_access;
    b.oldLayout = from; b.newLayout = to;
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = image; b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    VkDependencyInfo dep{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    dep.imageMemoryBarrierCount = 1; dep.pImageMemoryBarriers = &b;
    (dt.CmdPipelineBarrier2 ? dt.CmdPipelineBarrier2 : dt.CmdPipelineBarrier2KHR)(cb, &dep);
}

void MemBarrier(const DeviceDispatch& dt, VkCommandBuffer cb, VkPipelineStageFlags2 src_stage, VkAccessFlags2 src_access,
                VkPipelineStageFlags2 dst_stage, VkAccessFlags2 dst_access) {
    VkMemoryBarrier2 b{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
    b.srcStageMask = src_stage; b.srcAccessMask = src_access; b.dstStageMask = dst_stage; b.dstAccessMask = dst_access;
    VkDependencyInfo dep{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    dep.memoryBarrierCount = 1; dep.pMemoryBarriers = &b;
    (dt.CmdPipelineBarrier2 ? dt.CmdPipelineBarrier2 : dt.CmdPipelineBarrier2KHR)(cb, &dep);
}

VkResult Submit(const DeviceDispatch& dt, VkQueue q, const VkSubmitInfo2& si, VkFence fence) {
    std::mutex* qm = QueueMutex(q);
    std::unique_lock<std::mutex> lk;
    if (qm) lk = std::unique_lock<std::mutex>(*qm);                  // the game thread submits to the same queue
    return (dt.QueueSubmit2 ? dt.QueueSubmit2 : dt.QueueSubmit2KHR)(q, 1, &si, fence);
}

void Blit(const DeviceDispatch& dt, VkCommandBuffer cb, VkImage src, VkImageLayout src_layout, VkImage dst, VkImageLayout dst_layout, VkExtent2D e) {
    VkImageBlit r{};
    r.srcSubresource = r.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    r.srcOffsets[1] = r.dstOffsets[1] = {int32_t(e.width), int32_t(e.height), 1};
    dt.CmdBlitImage(cb, src, src_layout, dst, dst_layout, 1, &r, VK_FILTER_NEAREST);
}

// Takes the next command buffer, waiting for its previous submission to finish, and starts recording.
bool BeginCmd(const DeviceDispatch& dt, GpuState& g, uint32_t& entry) {
    const uint32_t n = std::clamp<uint32_t>(uint32_t(Live().cmd_buffers.load()), 4u, g.n_cmd);   // live setting; every entry has its own fence, so changing n while frames are in flight is safe
    entry = g.cb_next++ % n;
    for (uint32_t tries = 1; (g.cb_reserved >> entry & 1u) && tries < n; ++tries) entry = g.cb_next++ % n;      // an entry that is being recorded while gpu_mu is dropped is not ours
    if (g.cb_reserved >> entry & 1u) { FG_LOG("hold: every command buffer entry is reserved"); return false; }
    if (g.fences[entry] == VK_NULL_HANDLE) return false;                    // its fence could not be recreated after a failed submit (EndAndSubmit)
    if (dt.WaitForFences(dt.device, 1, &g.fences[entry], VK_TRUE, 2'000'000'000ull) != VK_SUCCESS) { FG_LOG("hold: waiting for a command buffer timed out"); return false; }
    if (dt.ResetCommandBuffer(g.cbs[entry], 0) != VK_SUCCESS) return false;
    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    return dt.BeginCommandBuffer(g.cbs[entry], &bi) == VK_SUCCESS;
}

bool EndAndSubmit(const DeviceDispatch& dt, VkQueue queue, GpuState& g, uint32_t entry,
                  const std::vector<VkSemaphoreSubmitInfo>& waits, VkSemaphore signal) {
    if (dt.EndCommandBuffer(g.cbs[entry]) != VK_SUCCESS) return false;
    VkCommandBufferSubmitInfo cbi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO};
    cbi.commandBuffer = g.cbs[entry];
    VkSemaphoreSubmitInfo sig{VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO};
    sig.semaphore = signal; sig.stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    VkSubmitInfo2 si{VK_STRUCTURE_TYPE_SUBMIT_INFO_2};
    si.waitSemaphoreInfoCount = uint32_t(waits.size()); si.pWaitSemaphoreInfos = waits.data();
    si.commandBufferInfoCount = 1; si.pCommandBufferInfos = &cbi;
    si.signalSemaphoreInfoCount = signal ? 1 : 0; si.pSignalSemaphoreInfos = &sig;
    if (dt.ResetFences(dt.device, 1, &g.fences[entry]) != VK_SUCCESS) return false;
    const VkResult r = Submit(dt, queue, si, g.fences[entry]);
    if (r != VK_SUCCESS) {
        FG_LOG("hold: queue submit failed (%d)", int(r));
        // the fence stays unsignaled and would block the next wait: make it signaled again by submitting nothing is not
        // possible, so recreate it
        dt.DestroyFence(dt.device, g.fences[entry], nullptr);
        VkFenceCreateInfo fi{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO}; fi.flags = VK_FENCE_CREATE_SIGNALED_BIT;
        if (dt.CreateFence(dt.device, &fi, nullptr, &g.fences[entry]) != VK_SUCCESS) {
            g.fences[entry] = VK_NULL_HANDLE;                              // never keep a destroyed handle: BeginCmd refuses this entry from now on
            FG_LOG("hold: could not recreate the fence of command buffer entry %u, the entry is out of use", entry);
        }
        return false;
    }
    return true;
}

void DumpInit(const DeviceDispatch& dt, GpuState& g) {
    if (const char* why = DumpNeedsFlowNote(GetConfig().dump_pair, GetConfig().flow)) {
        static std::atomic<bool> told{false};
        if (!told.exchange(true)) FG_LOG("%s", why);                      // once per process; the capture itself is unchanged
        return;
    }
    if (GetConfig().dump_pair <= 0 || !g.flow.ready) return;
    const FlowImage& fin = g.flow.fwd[g.flow.fin];
    DumpLayout l;
    l.a_bytes = size_t(g.extent.width) * g.extent.height * 8;
    l.b_bytes = l.a_bytes;
    l.fwd_bytes = l.bwd_bytes = size_t(fin.extent.width) * fin.extent.height * 8;
    l.mask_bytes = size_t(fin.extent.width) * fin.extent.height * 4;
    l.synth = 10;                                            // two for each of the first five pairs
    if (g.dump_pool.Create(dt, l, GetConfig().dump_frames)) g.dump.frames = g.dump_pool.frames;
    else FG_LOG("dump: VKIFRAME_DUMP is off for this swapchain");
}

DumpPool::Buf* DumpRecord(const DeviceDispatch& dt, VkCommandBuffer cb, VkImage image, VkExtent2D extent, uint32_t bpt, DumpPool::Buf* buf, const char* name) {
    const VkDeviceSize bytes = VkDeviceSize(extent.width) * extent.height * bpt;
    if (!buf || buf->recorded || !buf->ptr || buf->bytes < bytes) { FG_LOG("dump: %s: no buffer for this image, skipped", name); return nullptr; }
    MemBarrier(dt, cb, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_MEMORY_WRITE_BIT, VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_READ_BIT);
    VkBufferImageCopy rg{}; rg.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}; rg.imageExtent = {extent.width, extent.height, 1};
    dt.CmdCopyImageToBuffer(cb, image, VK_IMAGE_LAYOUT_GENERAL, buf->buf, 1, &rg);
    MemBarrier(dt, cb, VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_HOST_BIT, VK_ACCESS_2_HOST_READ_BIT);
    buf->bytes = size_t(bytes);                                                                // the file is exactly the image
    try { buf->name = name; } catch (const std::bad_alloc&) { return nullptr; }
    buf->recorded = true;
    return buf;
}

// One wait after the window: every copy was recorded into a command buffer of this swapchain, so all fences covering them signal when they finished.
void DumpFlush(const DeviceDispatch& dt, GpuState& g) {
    if (!g.dump_pool.ready) { FG_LOG("dump: capture FAILED, the buffer pool is not available"); g.dump.Drop(""); return; }
    for (uint32_t i = 0; i < g.n_cmd; ++i) {
        if (g.fences[i] == VK_NULL_HANDLE) continue;
        if (dt.WaitForFences(dt.device, 1, &g.fences[i], VK_TRUE, 2'000'000'000ull) != VK_SUCCESS) {
            FG_LOG("dump: capture FAILED, a GPU copy did not finish in 2 s; nothing is written");
            g.dump.Drop("");
            g.dump_pool.ready = false;                                                    // no more copies; the buffers stay allocated (a copy may still run) until GpuDestroy, which waits for the device first
            return;
        }
    }
    const char* dir = GetConfig().dump_dir;
    for (auto& b : g.dump_pool.bufs) {
        if (!b.recorded) continue;
        char path[512]; std::snprintf(path, sizeof path, "%s/%s", dir, b.name.c_str());
        if (!g.dump.AddView(path, b.ptr, b.bytes)) FG_LOG("dump: %s: not queued (out of memory)", path);
    }
    g.dump.Flush(dir);
    g.dump_pool.Destroy(dt);                                                                  // one capture per game start: give the memory back
}

void DumpMaybeFlush(const DeviceDispatch& dt, GpuState& g, bool force) {
    if (!g.dump_pool.ready) return;
    if (force || g.dump.Ready()) DumpFlush(dt, g);
}

void DumpTeardown(const DeviceDispatch& dt, GpuState& g) {
    size_t n = 0; for (const auto& b : g.dump_pool.bufs) n += b.recorded ? 1 : 0;
    if (n) FG_LOG("dump: %zu queued images not written (the game ended, the engine went off or the swapchain was recreated inside the window)", n);
    g.dump.Drop("");
    g.dump_pool.Destroy(dt);
}

bool CreateSlot(const DeviceDispatch& dt, GpuState& g, GpuState::Slot& s) {
    VkImageCreateInfo ci{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ci.imageType = VK_IMAGE_TYPE_2D; ci.format = GpuState::kRingFormat;
    ci.extent = {g.extent.width, g.extent.height, 1}; ci.mipLevels = 1; ci.arrayLayers = 1;
    ci.samples = VK_SAMPLE_COUNT_1_BIT; ci.tiling = VK_IMAGE_TILING_OPTIMAL;
    ci.usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT;
    ci.sharingMode = dt.queue_families.size() > 1 ? VK_SHARING_MODE_CONCURRENT : VK_SHARING_MODE_EXCLUSIVE;
    ci.queueFamilyIndexCount = ci.sharingMode == VK_SHARING_MODE_CONCURRENT ? uint32_t(dt.queue_families.size()) : 0;
    ci.pQueueFamilyIndices = ci.queueFamilyIndexCount ? dt.queue_families.data() : nullptr;
    ci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (dt.CreateImage(dt.device, &ci, nullptr, &s.image) != VK_SUCCESS) return false;
    VkMemoryRequirements req; dt.GetImageMemoryRequirements(dt.device, s.image, &req);
    const uint32_t type = FindMemoryType(dt, req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, nullptr, req.size, type};
    if (type == UINT32_MAX || dt.AllocateMemory(dt.device, &ai, nullptr, &s.memory) != VK_SUCCESS ||
        dt.BindImageMemory(dt.device, s.image, s.memory, 0) != VK_SUCCESS) return false;
    VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vi.image = s.image; vi.viewType = VK_IMAGE_VIEW_TYPE_2D; vi.format = GpuState::kRingFormat;
    vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    if (!dt.CreateImageView) return true;                       // no view: the compute pass stays off (CreateBlendPass checks)
    return dt.CreateImageView(dt.device, &vi, nullptr, &s.view) == VK_SUCCESS;
}


// The compute pass of Stage 3 (blend.comp). Failure here only switches synthetic frames off.
bool CreateBlendPass(const DeviceDispatch& dt, GpuState& g) {
    const bool have = dt.CreateShaderModule && dt.DestroyShaderModule && dt.CreateDescriptorSetLayout && dt.DestroyDescriptorSetLayout &&
                      dt.CreatePipelineLayout && dt.DestroyPipelineLayout && dt.CreateComputePipelines && dt.DestroyPipeline &&
                      dt.CreateDescriptorPool && dt.DestroyDescriptorPool && dt.AllocateDescriptorSets && dt.UpdateDescriptorSets &&
                      dt.CmdBindPipeline && dt.CmdBindDescriptorSets && dt.CmdPushConstants && dt.CmdDispatch && dt.AcquireNextImageKHR &&
                      dt.DestroyImageView && g.synth.view;
    if (!have) { FG_LOG("synth: the driver does not expose the compute functions, synthetic frames are off"); return false; }
    if (sizeof(kBlendSpv) < 256) { FG_LOG("synth: shader not built into this library, synthetic frames are off"); return false; }
    VkShaderModuleCreateInfo mi{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    mi.codeSize = sizeof(kBlendSpv); mi.pCode = kBlendSpv;
    if (dt.CreateShaderModule(dt.device, &mi, nullptr, &g.shader) != VK_SUCCESS) { FG_LOG("synth: the driver rejected blend.comp"); return false; }
    VkDescriptorSetLayoutBinding b[3]{};
    for (uint32_t i = 0; i < 3; ++i) { b[i].binding = i; b[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE; b[i].descriptorCount = 1; b[i].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT; }
    VkDescriptorSetLayoutCreateInfo li{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO}; li.bindingCount = 3; li.pBindings = b;
    if (dt.CreateDescriptorSetLayout(dt.device, &li, nullptr, &g.set_layout) != VK_SUCCESS) return false;
    VkPushConstantRange pc{VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(float)};
    VkPipelineLayoutCreateInfo pl{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    pl.setLayoutCount = 1; pl.pSetLayouts = &g.set_layout; pl.pushConstantRangeCount = 1; pl.pPushConstantRanges = &pc;
    if (dt.CreatePipelineLayout(dt.device, &pl, nullptr, &g.pipe_layout) != VK_SUCCESS) return false;
    VkComputePipelineCreateInfo pi{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
    pi.stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_COMPUTE_BIT, g.shader, "main", nullptr};
    pi.layout = g.pipe_layout;
    if (dt.CreateComputePipelines(dt.device, VK_NULL_HANDLE, 1, &pi, nullptr, &g.pipeline) != VK_SUCCESS) { FG_LOG("synth: compute pipeline creation failed"); return false; }
    VkDescriptorPoolSize ps{VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 3 * g.n_cmd};
    VkDescriptorPoolCreateInfo dp{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    dp.maxSets = g.n_cmd; dp.poolSizeCount = 1; dp.pPoolSizes = &ps;
    if (dt.CreateDescriptorPool(dt.device, &dp, nullptr, &g.desc_pool) != VK_SUCCESS) return false;
    VkDescriptorSetLayout layouts[GpuState::kCmdBuffers]; for (auto& l : layouts) l = g.set_layout;
    VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    ai.descriptorPool = g.desc_pool; ai.descriptorSetCount = g.n_cmd; ai.pSetLayouts = layouts;
    if (dt.AllocateDescriptorSets(dt.device, &ai, g.sets) != VK_SUCCESS) return false;
    g.acquire.assign(g.n_cmd, VK_NULL_HANDLE);
    VkSemaphoreCreateInfo si{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    for (auto& s : g.acquire) if (dt.CreateSemaphore(dt.device, &si, nullptr, &s) != VK_SUCCESS) return false;
    return true;
}

} // namespace gpuint
using namespace gpuint;

GpuState* GpuInit(const DeviceDispatch& dt, SwapchainState& sc, uint32_t family) {
    auto* g = new GpuState;
    g->n_cmd = kMaxCmdBuffers;                 // always allocate the most; CMD_BUFFERS (live) picks how many are used
    g->extent = sc.extent; g->family = family; g->swapchain = sc.handle;
    bool ok = true;

    uint32_t n = 0;
    ok = ok && dt.GetSwapchainImagesKHR(dt.device, sc.handle, &n, nullptr) == VK_SUCCESS && n > 0;
    if (ok) { g->swap_images.resize(n); ok = dt.GetSwapchainImagesKHR(dt.device, sc.handle, &n, g->swap_images.data()) >= 0; }

    for (auto& slot : g->ring) { if (!ok) break; ok = CreateSlot(dt, *g, slot); }
    ok = ok && CreateSlot(dt, *g, g->synth);
    if (ok) {
        VkCommandPoolCreateInfo pi{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
        pi.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT; pi.queueFamilyIndex = family;
        ok = dt.CreateCommandPool(dt.device, &pi, nullptr, &g->pool) == VK_SUCCESS;
    }
    if (ok) {
        VkCommandBufferAllocateInfo ai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        ai.commandPool = g->pool; ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY; ai.commandBufferCount = g->n_cmd;
        ok = dt.AllocateCommandBuffers(dt.device, &ai, g->cbs) == VK_SUCCESS;
        // Command buffers are dispatchable objects: the loader must initialise their dispatch word, or layers below us break.
        for (uint32_t i = 0; ok && i < g->n_cmd; ++i) ok = dt.set_loader_data(dt.device, g->cbs[i]) == VK_SUCCESS;
    }
    for (uint32_t i = 0; i < g->n_cmd; ++i) {
        if (!ok) break;
        VkFenceCreateInfo fi{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO}; fi.flags = VK_FENCE_CREATE_SIGNALED_BIT;
        ok = dt.CreateFence(dt.device, &fi, nullptr, &g->fences[i]) == VK_SUCCESS;
    }
    if (ok) {
        g->captured.assign(g->swap_images.size(), VK_NULL_HANDLE);
        g->staged.assign(g->swap_images.size(), VK_NULL_HANDLE);
        VkSemaphoreCreateInfo si{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
        for (auto* v : {&g->captured, &g->staged})
            for (auto& s : *v) ok = ok && dt.CreateSemaphore(dt.device, &si, nullptr, &s) == VK_SUCCESS;
    }
    if (!ok) { FG_LOG("hold: GPU resource setup failed"); GpuDestroy(dt, g); return nullptr; }
    g->synth_ready = GetConfig().synth && CreateBlendPass(dt, *g);
    FG_LOG("synth: synthetic frames %s", g->synth_ready ? "ready (blend.comp)" : "off");
    if (g->synth_ready && GetConfig().flow && !FlowCreate(dt, *g)) { FG_LOG("flow: the optical-flow engine could not be built, it stays off"); FlowDestroy(dt, *g); }
    DumpInit(dt, *g);
    FG_LOG("hold: ring %u x RGBA16F %ux%u, %zu swapchain images, queue family %u", GpuState::kRingCapacity, g->extent.width, g->extent.height, g->swap_images.size(), family);
    return g;
}

void GpuPin(GpuState& g, uint32_t slot) { std::lock_guard l(g.gpu_mu); ++g.pins[slot]; }
void GpuUnpin(GpuState& g, uint32_t slot) { { std::lock_guard l(g.gpu_mu); if (g.pins[slot] > 0) --g.pins[slot]; } g.pin_cv.notify_all(); }

bool GpuCapture(const DeviceDispatch& dt, VkQueue queue, GpuState& g, const VkPresentInfoKHR& pi, HeldFrame& out) {
    const uint32_t idx = pi.pImageIndices[0];
    if (idx >= g.swap_images.size()) return false;
    auto mono = [] { timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts); return uint64_t(ts.tv_sec) * 1000000000ull + ts.tv_nsec; };
    const uint64_t t_enter = mono();
    std::unique_lock<std::mutex> lk(g.gpu_mu);
    g.wait_lock_ns += mono() - t_enter;
    // a slot nobody references (the presenter may still be using the older ones); wait a little if all are taken
    uint32_t slot = UINT32_MAX;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(200);
    for (;;) {
        for (uint32_t i = 1; i <= GpuState::kRingCapacity && slot == UINT32_MAX; ++i) {
            const uint32_t c = (g.ring_write + i) % GpuState::kRingCapacity;
            if (g.pins[c] == 0) slot = c;
        }
        if (slot != UINT32_MAX) break;
        const uint64_t t_wait = mono();
        const bool timed_out = g.pin_cv.wait_until(lk, deadline) == std::cv_status::timeout;
        g.wait_ring_ns += mono() - t_wait;
        if (timed_out) { FG_LOG_ROUTINE("hold: no free ring slot for 200 ms"); return false; }
    }
    auto& rs = g.ring[slot];
    uint32_t e;
    const uint64_t t_cmd = mono();
    const bool began = BeginCmd(dt, g, e);
    g.wait_fence_ns += mono() - t_cmd;
    if (!began) return false;
    VkCommandBuffer cb = g.cbs[e];

    constexpr auto ALL = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, XFER = VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT;
    // ring slot: whatever used it before (reads or writes) finishes first; first use also moves it to GENERAL
    Barrier(dt, cb, rs.image, rs.initialized ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL,
            ALL, VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT, XFER, VK_ACCESS_2_TRANSFER_WRITE_BIT);
    // the game's image: ready for presentation -> readable as a transfer source
    Barrier(dt, cb, g.swap_images[idx], VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
            ALL, 0, XFER, VK_ACCESS_2_TRANSFER_READ_BIT);
    Blit(dt, cb, g.swap_images[idx], VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, rs.image, VK_IMAGE_LAYOUT_GENERAL, g.extent);
    Barrier(dt, cb, g.swap_images[idx], VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
            XFER, VK_ACCESS_2_TRANSFER_READ_BIT, ALL, 0);

    std::vector<VkSemaphoreSubmitInfo> waits(pi.waitSemaphoreCount);
    for (uint32_t i = 0; i < pi.waitSemaphoreCount; ++i) {
        waits[i] = {VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO};
        waits[i].semaphore = pi.pWaitSemaphores[i]; waits[i].stageMask = ALL;
    }
    if (!EndAndSubmit(dt, queue, g, e, waits, g.captured[idx])) return false;
    rs.initialized = true; g.ring_write = slot; g.pins[slot] = 1;                       // the held-frame reference
    out = {true, idx, slot};
    return true;
}

bool GpuPrepareHeldPresent(const DeviceDispatch& dt, VkQueue queue, GpuState& g, const HeldFrame& h, bool via_ring, VkSemaphore& wait_sem) {
    std::lock_guard<std::mutex> lk(g.gpu_mu);
    wait_sem = g.captured[h.image_index];
    if (!via_ring) return true;

    uint32_t e;
    if (!BeginCmd(dt, g, e)) return false;
    VkCommandBuffer cb = g.cbs[e];
    constexpr auto ALL = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, XFER = VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT;
    Barrier(dt, cb, g.ring[h.slot].image, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL, XFER, VK_ACCESS_2_TRANSFER_WRITE_BIT, XFER, VK_ACCESS_2_TRANSFER_READ_BIT);
    Barrier(dt, cb, g.swap_images[h.image_index], VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, ALL, 0, XFER, VK_ACCESS_2_TRANSFER_WRITE_BIT);
    Blit(dt, cb, g.ring[h.slot].image, VK_IMAGE_LAYOUT_GENERAL, g.swap_images[h.image_index], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, g.extent);
    Barrier(dt, cb, g.swap_images[h.image_index], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, XFER, VK_ACCESS_2_TRANSFER_WRITE_BIT, ALL, 0);
    VkSemaphoreSubmitInfo w{VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO};
    w.semaphore = g.captured[h.image_index]; w.stageMask = ALL;
    if (!EndAndSubmit(dt, queue, g, e, {w}, g.staged[h.image_index])) return false;
    wait_sem = g.staged[h.image_index];
    return true;
}

void GpuDestroy(const DeviceDispatch& dt, GpuState* g) {
    if (!g) return;
    {   // vkDeviceWaitIdle needs every queue of the device to be externally synchronized: take the same mutexes as Hook_DeviceWaitIdle (sorted order), so the game's
        // other threads cannot be inside vkQueueSubmit/Present while it runs. No caller holds a queue mutex here.
        std::vector<std::unique_lock<std::mutex>> locks;
        for (std::mutex* m : DeviceQueueMutexes(GetKey(dt.device))) locks.emplace_back(*m);
        dt.DeviceWaitIdle(dt.device);
    }
    DumpTeardown(dt, *g);
    FlowDestroy(dt, *g);
    for (auto* v : {&g->captured, &g->staged, &g->acquire}) for (auto s : *v) if (s) dt.DestroySemaphore(dt.device, s, nullptr);
    if (g->pipeline)    dt.DestroyPipeline(dt.device, g->pipeline, nullptr);
    if (g->pipe_layout) dt.DestroyPipelineLayout(dt.device, g->pipe_layout, nullptr);
    if (g->desc_pool)   dt.DestroyDescriptorPool(dt.device, g->desc_pool, nullptr);       // frees its sets
    if (g->set_layout)  dt.DestroyDescriptorSetLayout(dt.device, g->set_layout, nullptr);
    if (g->shader)      dt.DestroyShaderModule(dt.device, g->shader, nullptr);
    for (auto f : g->fences) if (f) dt.DestroyFence(dt.device, f, nullptr);
    if (g->pool) dt.DestroyCommandPool(dt.device, g->pool, nullptr);           // frees its command buffers
    auto free_slot = [&](GpuState::Slot& s) {
        if (s.view && dt.DestroyImageView) dt.DestroyImageView(dt.device, s.view, nullptr);
        if (s.image)  dt.DestroyImage(dt.device, s.image, nullptr);
        if (s.memory) dt.FreeMemory(dt.device, s.memory, nullptr);
    };
    for (auto& s : g->ring) free_slot(s);
    free_slot(g->synth);
    delete g;
}

// A hidden swapchain image was acquired but the frame could not be made. The layer cannot hand an acquired image back (no vkReleaseSwapchainImages),
// and an image that is neither presented nor released is lost until the swapchain is recreated, so it is presented as it is: it shows what the image
// held last, once, and the caller switches synthetic frames off. `transition`: the image goes UNDEFINED -> PRESENT_SRC first (a present needs that
// layout), in a small submit that waits on the acquire semaphore and signals the image's staged semaphore; without it (or when that submit fails) the
// present waits on the acquire semaphore itself. Called with gpu_mu held. Best effort: a failure here is logged and nothing more can be done.
static void GiveBackAcquired(const DeviceDispatch& dt, VkQueue queue, GpuState& g, uint32_t idx, VkSemaphore acquire_sem, bool transition) {
    constexpr auto ALL = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    VkSemaphore wait = acquire_sem;
    if (transition && idx < g.swap_images.size() && idx < g.staged.size()) {
        uint32_t e2;
        if (BeginCmd(dt, g, e2)) {
            Barrier(dt, g.cbs[e2], g.swap_images[idx], VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, ALL, 0, ALL, 0);
            VkSemaphoreSubmitInfo w{VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO}; w.semaphore = acquire_sem; w.stageMask = ALL;
            if (EndAndSubmit(dt, queue, g, e2, {w}, g.staged[idx])) wait = g.staged[idx];
        }
    }
    VkPresentInfoKHR pi{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
    pi.waitSemaphoreCount = 1; pi.pWaitSemaphores = &wait;
    pi.swapchainCount = 1; pi.pSwapchains = &g.swapchain; pi.pImageIndices = &idx;
    VkResult r;
    {
        std::mutex* qm = QueueMutex(queue);
        std::unique_lock<std::mutex> lk;
        if (qm) lk = std::unique_lock<std::mutex>(*qm);                        // the game thread presents and submits to this queue too
        r = dt.QueuePresentKHR(queue, &pi);
    }
    if (r >= 0) FG_LOG("synth: the failed frame's swapchain image %u was presented as it is, so it is not lost", idx);
    else FG_LOG("synth: could not give swapchain image %u back (present %d); it stays with the layer until the swapchain is recreated", idx, int(r));
}

VkResult GpuSynthesize(const DeviceDispatch& dt, VkQueue queue, GpuState& g, const HeldFrame& a, const HeldFrame& b, float t,
                       bool hidden, uint32_t& out_image_index, VkSemaphore& wait_sem, bool extrap) {
    if (!g.synth_ready) return VK_ERROR_FEATURE_NOT_PRESENT;
    if (extrap && !(GetConfig().interpolate && g.flow.ready && g.flow.pair_valid)) return VK_ERROR_FEATURE_NOT_PRESENT;   // a prediction needs the flow of this pair
    std::unique_lock<std::mutex> lk(g.gpu_mu);
    uint32_t e;
    if (!BeginCmd(dt, g, e)) return VK_ERROR_UNKNOWN;
    VkCommandBuffer cb = g.cbs[e];
    constexpr auto ALL = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, XFER = VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT, CS = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;

    // The target swapchain image: the carrier's own image, or a hidden image acquired here. The acquire is polled (never a
    // blocking wait on the game's thread while images may be released only by our own earlier presents).
    uint32_t idx = a.image_index;
    std::vector<VkSemaphoreSubmitInfo> waits;
    VkSemaphoreSubmitInfo w{VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO}; w.stageMask = ALL;
    bool acquired = false;                                                   // a hidden image is ours and not yet handed on
    if (hidden) {
        const uint64_t wait_ns = uint64_t(GetConfig().acquire_wait_us) * 1000ull;
        const uint64_t deadline = [] { timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts); return uint64_t(ts.tv_sec) * 1000000000ull + ts.tv_nsec + 2'000'000'000ull; }();
        // With a wait time the presenter does not hold gpu_mu while it waits for a free image (the game thread captures under that mutex):
        // entry e is marked in g.cb_reserved while the lock is dropped, so BeginCmd (the game thread's captures) skips it and nobody resets the
        // command buffer that is being recorded; the lock is taken again before recording goes on.
        // wait_ns == 0 is the old behaviour: poll with 200 us naps under the lock.
        if (wait_ns) { g.cb_reserved |= 1u << e; lk.unlock(); }
        auto relock = [&] { if (wait_ns) { lk.lock(); g.cb_reserved &= ~(1u << e); } };
        for (;;) {
            VkResult ar;
            { std::lock_guard<std::mutex> al(g.acquire_mu); ar = dt.AcquireNextImageKHR(dt.device, g.swapchain, wait_ns, g.acquire[e], VK_NULL_HANDLE, &idx); }
            if (ar == VK_SUCCESS || ar == VK_SUBOPTIMAL_KHR) break;
            if (ar != VK_NOT_READY && ar != VK_TIMEOUT) { relock(); FG_LOG("synth: acquire failed (%d)", int(ar)); dt.EndCommandBuffer(cb); return ar; }
            timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
            if (uint64_t(ts.tv_sec) * 1000000000ull + ts.tv_nsec > deadline) { relock(); FG_LOG("synth: no free swapchain image for 2 s"); dt.EndCommandBuffer(cb); return VK_TIMEOUT; }
            if (!wait_ns) { timespec nap{0, 200'000}; nanosleep(&nap, nullptr); }
        }
        relock();
        if (idx >= g.swap_images.size()) { FG_LOG("synth: the driver returned image index %u of %zu", idx, g.swap_images.size()); dt.EndCommandBuffer(cb); GiveBackAcquired(dt, queue, g, idx, g.acquire[e], false); return VK_ERROR_UNKNOWN; }
        w.semaphore = g.acquire[e]; waits.push_back(w); acquired = true;
    } else {
        w.semaphore = g.captured[a.image_index]; waits.push_back(w);               // A's own capture must be done before its image is overwritten
    }

    FlowState& fs = g.flow;
    // GPU time of the previous interpolation pass that used this entry (its fence has been waited for)
    if (fs.qpool && fs.interp_pending[e]) {
        uint64_t t[FlowState::kInterpStamps];
        if (dt.GetQueryPoolResults(dt.device, fs.qpool, FlowState::kQSlots * e + FlowState::kInterpFirst, FlowState::kInterpStamps, sizeof(t), t, sizeof(uint64_t), VK_QUERY_RESULT_64_BIT) == VK_SUCCESS && t[1] >= t[0]) {
            const double ms = double(t[1] - t[0]) * double(fs.ts_period_ns) * 1e-6;
            fs.interp_sum += ms; fs.interp_max = std::max(fs.interp_max, ms);
            { fs.stat_interp_ns += uint64_t(ms * 1e6); ++fs.stat_interp_n; }
            if (++fs.interp_count >= 240) {
                FG_LOG_ROUTINE("interpolation: GPU %.2f ms per generated frame (max %.2f) over %u frames", fs.interp_sum / fs.interp_count, fs.interp_max, fs.interp_count);
                fs.interp_sum = 0.0; fs.interp_max = 0.0; fs.interp_count = 0;
            }
        }
        fs.interp_pending[e] = false;
    }

    // Stage 4 debug view: the motion field instead of the blend
    const bool vis = GetConfig().debug_flow && g.flow.ready && g.flow.pair_valid;
    const bool interp = !vis && GetConfig().interpolate && g.flow.ready && g.flow.pair_valid;

    // descriptors: A, B (read), synth (write)
    VkDescriptorImageInfo di[6];
    VkWriteDescriptorSet wr[6]{};
    uint32_t nw = 0;
    auto bindv = [&](VkDescriptorSet set, std::initializer_list<VkImageView> views) {
        uint32_t binding = 0;
        for (VkImageView v : views) {
            di[nw] = {VK_NULL_HANDLE, v, VK_IMAGE_LAYOUT_GENERAL};
            wr[nw].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; wr[nw].dstSet = set; wr[nw].dstBinding = binding++;
            wr[nw].descriptorCount = 1; wr[nw].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE; wr[nw].pImageInfo = &di[nw];
            ++nw;
        }
    };
    if (vis) bindv(g.flow.sets[e][17], {g.ring[a.slot].view, g.flow.fwd[g.flow.fin].view, g.flow.mask.view, g.synth.view});
    else if (interp) bindv(g.flow.sets[e][18], {g.ring[a.slot].view, g.ring[b.slot].view, g.flow.fwd[g.flow.fin].view, g.flow.mask.view, g.synth.view, g.flow.bwd[g.flow.fin].view});
    else     bindv(g.sets[e], {g.ring[a.slot].view, g.ring[b.slot].view, g.synth.view});
    dt.UpdateDescriptorSets(dt.device, nw, wr, 0, nullptr);

    // captures (transfer writes) -> shader reads
    Barrier(dt, cb, g.ring[a.slot].image, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL, XFER, VK_ACCESS_2_TRANSFER_WRITE_BIT, CS, VK_ACCESS_2_SHADER_STORAGE_READ_BIT);
    Barrier(dt, cb, g.ring[b.slot].image, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL, XFER, VK_ACCESS_2_TRANSFER_WRITE_BIT, CS, VK_ACCESS_2_SHADER_STORAGE_READ_BIT);
    if (vis || interp) MemBarrier(dt, cb, CS, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT, CS, VK_ACCESS_2_SHADER_STORAGE_READ_BIT);   // the flow passes of this pair
    // the output image: the previous job's blit has read it; first use moves it to GENERAL
    Barrier(dt, cb, g.synth.image, g.synth.initialized ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL,
            ALL, VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT, CS, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);
    // VKIFRAME_TICKMARK: the shaders get t + 4*(1+bar_x); bar_x is the bar's left edge in pixels (0 for mode 1)
    float t_push = t;
    if (const int mode = GetConfig().tickmark; mode > 0) {
        if (t < g.mark_last_t) ++g.mark_pair;
        g.mark_last_t = t;
        const uint64_t span = g.extent.width > 24 ? g.extent.width - 24 : 1;
        uint64_t x = 0;
        if (mode == 2) x = (g.mark_tick * 6) % span;
        if (mode >= 3) x = (g.mark_pair * 12) % span;
        ++g.mark_tick;
        t_push = t + 4.0f * float(1 + x);
    }
    if (vis) {
        dt.CmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, g.flow.vis.pipeline);
        dt.CmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, g.flow.vis.layout, 0, 1, &g.flow.sets[e][17], 0, nullptr);
    } else if (interp) {
        dt.CmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, g.flow.interp.pipeline);
        dt.CmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, g.flow.interp.layout, 0, 1, &g.flow.sets[e][18], 0, nullptr);
        const float interp_pc[11] = {t_push, GetConfig().interp_guide, GetConfig().conf_lo, GetConfig().conf_hi, GetConfig().cand_thr, GetConfig().pan_lo, GetConfig().pan_hi, g.flow.pair_bidir ? 1.0f : 0.0f, GetConfig().ui_mask, extrap ? 1.0f : 0.0f, GetConfig().onesided_fill ? 1.0f : 0.0f};
        dt.CmdPushConstants(cb, g.flow.interp.layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(interp_pc), interp_pc);
    } else {
        dt.CmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, g.pipeline);
        dt.CmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, g.pipe_layout, 0, 1, &g.sets[e], 0, nullptr);
        dt.CmdPushConstants(cb, g.pipe_layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(float), &t_push);
    }
    const bool time_interp = interp && fs.qpool;
    if (time_interp) {
        dt.CmdResetQueryPool(cb, fs.qpool, FlowState::kQSlots * e + FlowState::kInterpFirst, FlowState::kInterpStamps);
        dt.CmdWriteTimestamp(cb, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, fs.qpool, FlowState::kQSlots * e + FlowState::kInterpFirst);   // after everything queued before, so the interval is the warp alone
    }
    dt.CmdDispatch(cb, (g.extent.width + 15) / 16, (g.extent.height + 15) / 16, 1);
    Barrier(dt, cb, g.synth.image, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL, CS, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT, XFER, VK_ACCESS_2_TRANSFER_READ_BIT);
    DumpPool::Buf* dump_buf = nullptr;                                             // VKIFRAME_DUMP: the synthetic frame is copied out in this same command buffer
    if (g.dump.synth_left > 0 && g.dump_pool.ready && g.dump_tick < 10) {
        char name[64]; std::snprintf(name, sizeof name, "synth%02d_t%.3f.rgba16f", g.dump_tick, double(t));
        dump_buf = DumpRecord(dt, cb, g.synth.image, g.extent, 8, &g.dump_pool.Synth(g.dump_tick), name);
        if (dump_buf) { ++g.dump_tick; --g.dump.synth_left; }
    }
    if (time_interp) {
        dt.CmdWriteTimestamp(cb, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, fs.qpool, FlowState::kQSlots * e + FlowState::kInterpFirst + 1);
        fs.interp_pending[e] = true;
    }

    // into the swapchain image (the carrier's image holds a finished frame in PRESENT_SRC; a hidden image has no content)
    Barrier(dt, cb, g.swap_images[idx], hidden ? VK_IMAGE_LAYOUT_UNDEFINED : VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            ALL, 0, XFER, VK_ACCESS_2_TRANSFER_WRITE_BIT);
    Blit(dt, cb, g.synth.image, VK_IMAGE_LAYOUT_GENERAL, g.swap_images[idx], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, g.extent);
    Barrier(dt, cb, g.swap_images[idx], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, XFER, VK_ACCESS_2_TRANSFER_WRITE_BIT, ALL, 0);

    if (!EndAndSubmit(dt, queue, g, e, waits, g.staged[idx])) {
        // Synthetic frames are switched off for this swapchain after a failure (the caller); the acquired image is presented, not kept.
        if (dump_buf) { dump_buf->recorded = false; --g.dump_tick; ++g.dump.synth_left; }     // the copy never ran
        if (acquired) { FG_LOG("synth: submit failed after acquiring swapchain image %u", idx); GiveBackAcquired(dt, queue, g, idx, g.acquire[e], true); }
        return VK_ERROR_UNKNOWN;
    }
    g.synth.initialized = true;
    DumpMaybeFlush(dt, g, false);
    out_image_index = idx; wait_sem = g.staged[idx];
    return VK_SUCCESS;
}
