// VKIFRAME_DUMP buffer pool with a fake driver: allocation failures at every step free everything that was allocated, the pool falls back
// to fewer frames, and nothing is leaked or unmapped twice. No GPU, no real Vulkan.
#include "dump_pool.hpp"
#include "vk_iframes_layer.hpp"

#include <cstdio>
#include <cstdlib>
#include <map>
#include <set>

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); ++g_fail; } } while (0)

// ---- fake driver
static int  f_calls[5];                 // 0 CreateBuffer, 1 AllocateMemory, 2 BindBufferMemory, 3 MapMemory
static int  f_fail_call;                // which call kind fails (-1 none)
static int  f_fail_after;               // ... after this many successful calls of that kind
static long f_budget;                   // bytes AllocateMemory may hand out in total (-1 unlimited)
static uint64_t f_next;
static std::set<uint64_t> f_bufs, f_mems; static std::set<void*> f_mapped; static std::map<uint64_t, void*> f_store;
static bool f_cached;

static bool Fails(int kind) { return f_fail_call == kind && f_calls[kind]++ >= f_fail_after; }
static VkResult VKAPI_CALL F_CreateBuffer(VkDevice, const VkBufferCreateInfo* ci, const VkAllocationCallbacks*, VkBuffer* b) {
    if (Fails(0)) return VK_ERROR_OUT_OF_HOST_MEMORY;
    if (f_fail_call != 0) ++f_calls[0];
    *b = (VkBuffer)(uintptr_t)(++f_next); f_bufs.insert(f_next); (void)ci; return VK_SUCCESS;
}
static void VKAPI_CALL F_DestroyBuffer(VkDevice, VkBuffer b, const VkAllocationCallbacks*) { CHECK(f_bufs.erase((uint64_t)(uintptr_t)b) == 1); }
static void VKAPI_CALL F_GetReq(VkDevice, VkBuffer, VkMemoryRequirements* r) { r->size = 1 << 20; r->alignment = 1; r->memoryTypeBits = 0x3; }
static long f_used;
static VkResult VKAPI_CALL F_Alloc(VkDevice, const VkMemoryAllocateInfo* ai, const VkAllocationCallbacks*, VkDeviceMemory* m) {
    if (Fails(1)) return VK_ERROR_OUT_OF_DEVICE_MEMORY;
    if (f_budget >= 0 && f_used + long(ai->allocationSize) > f_budget) return VK_ERROR_OUT_OF_DEVICE_MEMORY;
    if (f_fail_call != 1) ++f_calls[1];
    f_used += long(ai->allocationSize);
    *m = (VkDeviceMemory)(uintptr_t)(++f_next); f_mems.insert(f_next); f_store[f_next] = std::malloc(ai->allocationSize); return VK_SUCCESS;
}
static void VKAPI_CALL F_Free(VkDevice, VkDeviceMemory m, const VkAllocationCallbacks*) {
    CHECK(f_mems.erase((uint64_t)(uintptr_t)m) == 1); f_used -= 1 << 20; std::free(f_store[(uint64_t)(uintptr_t)m]); f_store.erase((uint64_t)(uintptr_t)m);
}
static VkResult VKAPI_CALL F_Bind(VkDevice, VkBuffer, VkDeviceMemory, VkDeviceSize) { return Fails(2) ? VK_ERROR_UNKNOWN : VK_SUCCESS; }
static VkResult VKAPI_CALL F_Map(VkDevice, VkDeviceMemory m, VkDeviceSize, VkDeviceSize, VkMemoryMapFlags, void** p) {
    if (Fails(3)) return VK_ERROR_MEMORY_MAP_FAILED;
    *p = f_store[(uint64_t)(uintptr_t)m]; f_mapped.insert(*p); return VK_SUCCESS;
}
static void VKAPI_CALL F_Unmap(VkDevice, VkDeviceMemory m) { CHECK(f_mapped.erase(f_store[(uint64_t)(uintptr_t)m]) == 1); }
static void VKAPI_CALL F_MemProps(VkPhysicalDevice, VkPhysicalDeviceMemoryProperties* mp) {
    mp->memoryTypeCount = 2;
    mp->memoryTypes[0].propertyFlags = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    mp->memoryTypes[1].propertyFlags = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT | (f_cached ? VK_MEMORY_PROPERTY_HOST_CACHED_BIT : 0);
}
static void VKAPI_CALL F_Copy(VkCommandBuffer, VkImage, VkImageLayout, VkBuffer, uint32_t, const VkBufferImageCopy*) {}

static void Reset(int fail_call, int fail_after, long budget_mb) {
    for (int& c : f_calls) c = 0;
    f_fail_call = fail_call; f_fail_after = fail_after; f_budget = budget_mb < 0 ? -1 : budget_mb << 20; f_used = 0;
}
static bool Clean() { return f_bufs.empty() && f_mems.empty() && f_mapped.empty() && f_store.empty(); }

int main() {
    InstanceDispatch inst; inst.GetPhysicalDeviceMemoryProperties = F_MemProps;
    DeviceDispatch dt;
    dt.instance = &inst;
    dt.CreateBuffer = F_CreateBuffer; dt.DestroyBuffer = F_DestroyBuffer; dt.GetBufferMemoryRequirements = F_GetReq; dt.AllocateMemory = F_Alloc;
    dt.FreeMemory = F_Free; dt.BindBufferMemory = F_Bind; dt.MapMemory = F_Map; dt.UnmapMemory = F_Unmap; dt.CmdCopyImageToBuffer = F_Copy;
    std::freopen("/dev/null", "w", stderr);
    DumpLayout l; l.a_bytes = 1000; l.b_bytes = 1000; l.fwd_bytes = l.bwd_bytes = 100; l.mask_bytes = 50; l.synth = 10;

    {   // success: 30 + 4 + 10 buffers, all mapped, layout accessors, then freed
        Reset(-1, 0, -1); f_cached = true;
        DumpPool p;
        CHECK(p.Create(dt, l, 30) && p.ready && p.frames == 30 && p.bufs.size() == 44);
        CHECK(&p.B() == &p.bufs[30] && &p.Mask() == &p.bufs[33] && &p.Synth(9) == &p.bufs[43]);
        CHECK(p.A(0).ptr && p.Mask().bytes == 50 && p.A(29).bytes == 1000);
        p.Destroy(dt); CHECK(Clean() && !p.ready);
        p.Destroy(dt);                                       // twice is safe
    }
    // each kind of call failing at several points: 30 frames fail, the pool falls back to 15 frames and the failed attempt is fully freed
    for (int kind = 0; kind < 4; ++kind) for (int after : {0, 1, 17, 43}) {
        Reset(kind, after, -1); DumpPool p;
        const bool ok = p.Create(dt, l, 30);
        // the failing kind keeps failing after `after` successes, in every attempt: so no size can succeed unless it needs fewer than `after` calls
        if (ok) { CHECK(p.frames < 30); p.Destroy(dt); }
        CHECK(Clean());
    }
    {   // memory runs out: 44 buffers need 44 MiB, 29 MiB fit 15 frames (15+4+10 = 29)
        Reset(-1, 0, 29); f_cached = false; DumpPool p;
        CHECK(p.Create(dt, l, 30) && p.frames == 15 && p.bufs.size() == 29);
        p.Destroy(dt); CHECK(Clean());
    }
    {   // not even 5 frames: 19 MiB needed, 18 available
        Reset(-1, 0, 18); DumpPool p;
        CHECK(!p.Create(dt, l, 30) && !p.ready && p.bufs.empty() && Clean());
    }
    {   // a missing function: off, no crash
        DeviceDispatch d2; d2.instance = &inst; DumpPool p; CHECK(!p.Create(d2, l, 30));
    }
    {   // the window never goes below 5 even if asked for less
        Reset(-1, 0, -1); DumpPool p; CHECK(p.Create(dt, l, 2) && p.frames == 5); p.Destroy(dt); CHECK(Clean());
    }
    std::printf(g_fail ? "FAILED\n" : "dump pool: ok\n");
    return g_fail ? 1 : 0;
}
