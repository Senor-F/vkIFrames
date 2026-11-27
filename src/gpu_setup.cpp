// GPU ring slots, blend pass creation, teardown and the dump pool recorder
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
