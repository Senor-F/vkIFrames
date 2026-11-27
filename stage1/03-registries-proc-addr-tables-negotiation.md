[index](../STAGE1.md) | [previous](02-dispatch-tables-shared-declarations.md) | [next](04-hooks.md)

### B. Registries, Proc-Addr Tables, Negotiation (`src/layer_dispatch.cpp`)

```cpp
#include "vk_iframes_layer.hpp"

#include <cstring>
#include <memory>
#include <mutex>
#include <unordered_map>

namespace {
std::mutex g_mutex;
std::unordered_map<void*, std::unique_ptr<InstanceDispatch>> g_instances;
std::unordered_map<void*, std::unique_ptr<DeviceDispatch>>   g_devices;

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
```

