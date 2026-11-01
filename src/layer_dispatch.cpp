#include "vk_iframes_layer.hpp"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <memory>
#include <mutex>
#include <unordered_map>

namespace {
// The registries are allocated on the heap and never freed. A presenter thread that is still running at exit (or was detached because it was stuck in a
// driver call) reaches them through the hooks; destroying them in the static destruction at exit was a use after free (#41). The process is ending,
// the system takes the memory back.
std::mutex& g_mutex = *new std::mutex;
auto& g_instances  = *new std::unordered_map<void*, std::unique_ptr<InstanceDispatch>>;
auto& g_devices    = *new std::unordered_map<void*, std::unique_ptr<DeviceDispatch>>;
auto& g_swapchains = *new std::unordered_map<uint64_t, std::unique_ptr<SwapchainState>>;   // keyed by the swapchain handle
struct QueueInfo { uint32_t family; void* device; std::unique_ptr<std::mutex> mu; };
auto& g_queues     = *new std::unordered_map<void*, std::unique_ptr<QueueInfo>>;               // VkQueue -> family, owning device, mutex

struct HookEntry { const char* name; PFN_vkVoidFunction fn; bool device_level; };

#define HOOK(n, dev) { "vk" #n, reinterpret_cast<PFN_vkVoidFunction>(Hook_##n), dev }
const HookEntry kHooks[] = {
    HOOK(CreateInstance,       false),
    HOOK(DestroyInstance,      false),
    HOOK(CreateDevice,         false),
    HOOK(DestroyDevice,        true),
    HOOK(CreateSwapchainKHR,   true),
    HOOK(DestroySwapchainKHR,  true),
    HOOK(QueuePresentKHR,      true),
    HOOK(QueueSubmit,          true),
    HOOK(QueueSubmit2,         true),
    HOOK(QueueSubmit2KHR,      true),
    HOOK(QueueBindSparse,      true),
    HOOK(QueueWaitIdle,        true),
    HOOK(DeviceWaitIdle,       true),
    HOOK(AcquireNextImageKHR,  true),
    HOOK(AcquireNextImage2KHR, true),
    HOOK(WaitForPresentKHR,    true),
    HOOK(WaitForFences,        true),
    HOOK(GetFenceStatus,       true),
    HOOK(GetDeviceQueue,       true),
    HOOK(GetDeviceQueue2,      true),
    { "vkGetInstanceProcAddr", reinterpret_cast<PFN_vkVoidFunction>(FrameGen_GetInstanceProcAddr), false },
    { "vkGetDeviceProcAddr",   reinterpret_cast<PFN_vkVoidFunction>(FrameGen_GetDeviceProcAddr),   true  },
};
#undef HOOK

PFN_vkVoidFunction FindHook(const char* name, bool device_only) {
    for (const auto& h : kHooks)
        if ((!device_only || h.device_level) && std::strcmp(h.name, name) == 0) return h.fn;
    return nullptr;
}
} // namespace

InstanceDispatch* RegisterInstance(void* key, VkInstance instance, PFN_vkGetInstanceProcAddr gipa) {
    auto t = std::make_unique<InstanceDispatch>();
    t->GetInstanceProcAddr = gipa;
#define X(name) t->name = reinterpret_cast<PFN_vk##name>(gipa(instance, "vk" #name));
    FRAMEGEN_INSTANCE_FUNCS(X)
#undef X
    std::lock_guard lock(g_mutex);
    return (g_instances[key] = std::move(t)).get();
}

DeviceDispatch* RegisterDevice(void* key, VkDevice device, VkPhysicalDevice pd,
                               PFN_vkGetDeviceProcAddr gdpa, InstanceDispatch* instance) {
    auto t = std::make_unique<DeviceDispatch>();
    t->GetDeviceProcAddr = gdpa;
    t->device = device;
    t->physical_device = pd;
    t->instance = instance;
#define X(name) t->name = reinterpret_cast<PFN_vk##name>(gdpa(device, "vk" #name));
    FRAMEGEN_DEVICE_FUNCS(X)
#undef X
    std::lock_guard lock(g_mutex);
    return (g_devices[key] = std::move(t)).get();
}

InstanceDispatch* LookupInstance(void* key) { std::lock_guard l(g_mutex); auto it = g_instances.find(key); return it == g_instances.end() ? nullptr : it->second.get(); }
DeviceDispatch*   LookupDevice(void* key)   { std::lock_guard l(g_mutex); auto it = g_devices.find(key);   return it == g_devices.end()   ? nullptr : it->second.get(); }
void UnregisterInstance(void* key) { std::lock_guard l(g_mutex); g_instances.erase(key); }
void UnregisterDevice(void* key)   { std::lock_guard l(g_mutex); g_devices.erase(key); }

static uint64_t SwapchainKey(VkSwapchainKHR h) { return uint64_t(reinterpret_cast<uintptr_t>(h)); }
SwapchainState* RegisterSwapchain(VkSwapchainKHR handle) {
    auto s = std::make_unique<SwapchainState>();
    s->handle = handle;
    std::lock_guard l(g_mutex);
    return (g_swapchains[SwapchainKey(handle)] = std::move(s)).get();
}
SwapchainState* FindSwapchain(VkSwapchainKHR handle) {
    std::lock_guard l(g_mutex);
    auto it = g_swapchains.find(SwapchainKey(handle));
    return it == g_swapchains.end() ? nullptr : it->second.get();
}
void RegisterQueue(VkQueue queue, uint32_t family, void* device_key) {
    std::lock_guard l(g_mutex);
    auto& slot = g_queues[reinterpret_cast<void*>(queue)];
    if (!slot) slot.reset(new QueueInfo{family, device_key, std::make_unique<std::mutex>()});   // the same queue may be fetched twice: keep its mutex
    else { slot->family = family; slot->device = device_key; }
}
uint32_t QueueFamilyOf(VkQueue queue) { std::lock_guard l(g_mutex); auto it = g_queues.find(reinterpret_cast<void*>(queue)); return it == g_queues.end() ? UINT32_MAX : it->second->family; }
std::mutex* QueueMutex(VkQueue queue) { std::lock_guard l(g_mutex); auto it = g_queues.find(reinterpret_cast<void*>(queue)); return it == g_queues.end() ? nullptr : it->second->mu.get(); }
std::vector<std::mutex*> DeviceQueueMutexes(void* device_key) {
    std::lock_guard l(g_mutex); std::vector<std::mutex*> v;
    for (auto& [q, info] : g_queues) if (info->device == device_key) v.push_back(info->mu.get());
    std::sort(v.begin(), v.end());                                  // a fixed order for taking several at once
    return v;
}
// Same, for a device whose presenter thread could not be stopped: it may hold a queue's mutex inside a driver call.
void LeakQueues(void* device_key) { std::lock_guard l(g_mutex); for (auto it = g_queues.begin(); it != g_queues.end();) { if (it->second->device == device_key) { (void)it->second.release(); it = g_queues.erase(it); } else ++it; } }
void UnregisterQueues(void* device_key) { std::lock_guard l(g_mutex); for (auto it = g_queues.begin(); it != g_queues.end();) it = it->second->device == device_key ? g_queues.erase(it) : std::next(it); }
void DestroySwapchainState(VkSwapchainKHR handle) { std::lock_guard l(g_mutex); g_swapchains.erase(SwapchainKey(handle)); }
// A presenter thread that could not be stopped still holds pointers to these objects: forget them without freeing (a deliberate leak at shutdown).
void LeakSwapchainState(VkSwapchainKHR handle) { std::lock_guard l(g_mutex); auto it = g_swapchains.find(SwapchainKey(handle)); if (it == g_swapchains.end()) return; (void)it->second.release(); g_swapchains.erase(it); }
void LeakDevice(void* key) { std::lock_guard l(g_mutex); auto it = g_devices.find(key); if (it == g_devices.end()) return; (void)it->second.release(); g_devices.erase(it); }

FRAMEGEN_EXPORT VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL
FrameGen_GetInstanceProcAddr(VkInstance instance, const char* name) {
    if (auto fn = FindHook(name, /*device_only=*/false)) return fn;   // apps may fetch device functions here too
    if (!instance) return nullptr;
    InstanceDispatch* d = LookupInstance(GetKey(instance));
    return d ? d->GetInstanceProcAddr(instance, name) : nullptr;
}

FRAMEGEN_EXPORT VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL
FrameGen_GetDeviceProcAddr(VkDevice device, const char* name) {
    if (auto fn = FindHook(name, /*device_only=*/true)) return fn;
    DeviceDispatch* d = LookupDevice(GetKey(device));
    return d ? d->GetDeviceProcAddr(device, name) : nullptr;
}

FRAMEGEN_EXPORT VKAPI_ATTR VkResult VKAPI_CALL
FrameGen_NegotiateLoaderLayerInterfaceVersion(VkNegotiateLayerInterface* pVersionStruct) {
    if (!pVersionStruct || pVersionStruct->sType != LAYER_NEGOTIATE_INTERFACE_STRUCT)
        return VK_ERROR_INITIALIZATION_FAILED;
    if (pVersionStruct->loaderLayerInterfaceVersion >= 2) {
        pVersionStruct->pfnGetInstanceProcAddr       = FrameGen_GetInstanceProcAddr;
        pVersionStruct->pfnGetDeviceProcAddr         = FrameGen_GetDeviceProcAddr;
        pVersionStruct->pfnGetPhysicalDeviceProcAddr = nullptr;
    }
    if (pVersionStruct->loaderLayerInterfaceVersion > CURRENT_LOADER_LAYER_INTERFACE_VERSION)
        pVersionStruct->loaderLayerInterfaceVersion = CURRENT_LOADER_LAYER_INTERFACE_VERSION;
    return VK_SUCCESS;
}
