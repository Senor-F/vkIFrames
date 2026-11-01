#include "dump_pool.hpp"

#include <algorithm>
#include <new>

static uint32_t PickType(const VkPhysicalDeviceMemoryProperties& mp, uint32_t bits, VkMemoryPropertyFlags want) {
    for (uint32_t i = 0; i < mp.memoryTypeCount; ++i)
        if ((bits & (1u << i)) && (mp.memoryTypes[i].propertyFlags & want) == want) return i;
    return UINT32_MAX;
}

bool DumpPool::AllocateOne(const DeviceDispatch& dt, Buf& b, size_t bytes, const VkPhysicalDeviceMemoryProperties& mp, size_t index) {
    VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO}; bi.size = bytes; bi.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT; bi.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    VkResult r = dt.CreateBuffer(dt.device, &bi, nullptr, &b.buf);
    if (r != VK_SUCCESS) { b.buf = VK_NULL_HANDLE; FG_LOG("dump: buffer %zu (%.1f MB): CreateBuffer failed (%d)", index, double(bytes) / 1048576.0, int(r)); return false; }
    VkMemoryRequirements req; dt.GetBufferMemoryRequirements(dt.device, b.buf, &req);
    uint32_t type = PickType(mp, req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT | VK_MEMORY_PROPERTY_HOST_CACHED_BIT);   // cached: the CPU reads it back after the window
    if (type == UINT32_MAX) type = PickType(mp, req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    if (type == UINT32_MAX) { FG_LOG("dump: buffer %zu: no host-visible coherent memory type", index); return false; }
    VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, nullptr, req.size, type};
    r = dt.AllocateMemory(dt.device, &ai, nullptr, &b.mem);
    if (r != VK_SUCCESS) { b.mem = VK_NULL_HANDLE; FG_LOG("dump: buffer %zu (%.1f MB): AllocateMemory failed (%d)", index, double(req.size) / 1048576.0, int(r)); return false; }
    r = dt.BindBufferMemory(dt.device, b.buf, b.mem, 0);
    if (r != VK_SUCCESS) { FG_LOG("dump: buffer %zu: BindBufferMemory failed (%d)", index, int(r)); return false; }
    void* p = nullptr;
    r = dt.MapMemory(dt.device, b.mem, 0, VK_WHOLE_SIZE, 0, &p);
    if (r != VK_SUCCESS || !p) { FG_LOG("dump: buffer %zu: MapMemory failed (%d)", index, int(r)); return false; }
    b.ptr = static_cast<uint8_t*>(p); b.bytes = bytes;
    return true;
}

bool DumpPool::AllocateAll(const DeviceDispatch& dt, const DumpLayout& l, int n) {
    VkPhysicalDeviceMemoryProperties mp{};
    dt.instance->GetPhysicalDeviceMemoryProperties(dt.physical_device, &mp);
    std::vector<size_t> sizes;
    try {
        sizes.assign(size_t(n), l.a_bytes);
        sizes.insert(sizes.end(), {l.b_bytes, l.fwd_bytes, l.bwd_bytes, l.mask_bytes});
        sizes.insert(sizes.end(), size_t(l.synth), l.a_bytes);
        bufs.assign(sizes.size(), Buf{});
    } catch (const std::bad_alloc&) { bufs.clear(); FG_LOG("dump: out of memory for the buffer list"); return false; }
    frames = n;
    for (size_t i = 0; i < sizes.size(); ++i) {
        if (!AllocateOne(dt, bufs[i], sizes[i], mp, i)) { Destroy(dt); return false; }
    }
    ready = true;
    return true;
}

bool DumpPool::Create(const DeviceDispatch& dt, const DumpLayout& l, int wanted) {
    Destroy(dt);
    if (!dt.CreateBuffer || !dt.DestroyBuffer || !dt.GetBufferMemoryRequirements || !dt.AllocateMemory || !dt.FreeMemory ||
        !dt.BindBufferMemory || !dt.MapMemory || !dt.UnmapMemory || !dt.CmdCopyImageToBuffer || !dt.instance || !dt.instance->GetPhysicalDeviceMemoryProperties) {
        FG_LOG("dump: the driver lacks the buffer functions, the capture is off");
        return false;
    }
    for (int n = std::max(wanted, 5);; n = std::max(5, n / 2)) {
        const double mb = (double(l.a_bytes) * double(n + l.synth) + double(l.b_bytes + l.fwd_bytes + l.bwd_bytes + l.mask_bytes)) / 1048576.0;
        FG_LOG("dump: allocating host-visible buffers for %d frames, %.0f MB, before the game runs", n, mb);
        if (AllocateAll(dt, l, n)) {
            if (n != wanted) FG_LOG("dump: only %d of the %d frames asked for fit; the window is %d frames", n, wanted, n);
            return true;
        }
        if (n <= 5) break;
        FG_LOG("dump: not enough memory for %d frames, trying fewer", n);
    }
    FG_LOG("dump: not enough host memory even for 5 frames, the capture is off");
    return false;
}

void DumpPool::Destroy(const DeviceDispatch& dt) {
    for (auto& b : bufs) {
        if (b.ptr) dt.UnmapMemory(dt.device, b.mem);
        if (b.buf) dt.DestroyBuffer(dt.device, b.buf, nullptr);
        if (b.mem) dt.FreeMemory(dt.device, b.mem, nullptr);
    }
    bufs.clear(); bufs.shrink_to_fit();
    ready = false; frames = 0;
}

void DumpPool::ClearRecorded() { for (auto& b : bufs) b.recorded = false; }

size_t DumpPool::TotalBytes() const { size_t t = 0; for (const auto& b : bufs) t += b.bytes; return t; }
