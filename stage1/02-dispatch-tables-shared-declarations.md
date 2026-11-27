[index](../STAGE1.md) | [previous](01-overview-objectives.md) | [next](03-registries-proc-addr-tables-negotiation.md)

### A. Dispatch Tables & Shared Declarations (`include/vk_iframes_layer.hpp`)

Function lists are X-macros, so adding a hooked or used function in a later stage is a one-line change.

```cpp
#pragma once

#include <vulkan/vulkan.h>
#include <vulkan/vk_layer.h>

#include <cstdio>

#define FRAMEGEN_EXPORT extern "C" __attribute__((visibility("default")))
#define FG_LOG(...) do { std::fprintf(stderr, "[FrameGen] " __VA_ARGS__); std::fputc('\n', stderr); } while (0)

// The loader's dispatch pointer is the first word of every dispatchable handle.
// Handles created from one instance/device share it, so it is the registry key
// (a VkQueue has the same key as its VkDevice, a VkPhysicalDevice the same as its VkInstance).
template <typename T>
inline void* GetKey(T handle) { return *reinterpret_cast<void**>(handle); }

// ---- Functions the layer calls down the chain. Later stages append names. ----
#define FRAMEGEN_INSTANCE_FUNCS(X)                  \
    X(DestroyInstance)                              \
    X(CreateDevice)                                 \
    X(EnumerateDeviceExtensionProperties)           \
    X(GetPhysicalDeviceSurfaceCapabilitiesKHR)      \
    X(GetPhysicalDeviceMemoryProperties)            \
    X(GetPhysicalDeviceQueueFamilyProperties)

#define FRAMEGEN_DEVICE_FUNCS(X)                    \
    X(DestroyDevice)                                \
    X(CreateSwapchainKHR)                           \
    X(DestroySwapchainKHR)                          \
    X(QueuePresentKHR)

struct InstanceDispatch {
    PFN_vkGetInstanceProcAddr GetInstanceProcAddr{};
#define X(name) PFN_vk##name name{};
    FRAMEGEN_INSTANCE_FUNCS(X)
#undef X
};

struct DeviceDispatch {
    PFN_vkGetDeviceProcAddr GetDeviceProcAddr{};
    VkDevice         device{};
    VkPhysicalDevice physical_device{};
    InstanceDispatch* instance{};
#define X(name) PFN_vk##name name{};
    FRAMEGEN_DEVICE_FUNCS(X)
#undef X
};

// ---- Registries (layer_dispatch.cpp) ----
InstanceDispatch* RegisterInstance(void* key, VkInstance instance, PFN_vkGetInstanceProcAddr gipa);
DeviceDispatch*   RegisterDevice(void* key, VkDevice device, VkPhysicalDevice physical_device,
                                 PFN_vkGetDeviceProcAddr gdpa, InstanceDispatch* instance);
InstanceDispatch* LookupInstance(void* key);
DeviceDispatch*   LookupDevice(void* key);
void UnregisterInstance(void* key);
void UnregisterDevice(void* key);

// ---- Hooks (layer_main.cpp) ----
VKAPI_ATTR VkResult VKAPI_CALL Hook_CreateInstance(const VkInstanceCreateInfo*, const VkAllocationCallbacks*, VkInstance*);
VKAPI_ATTR void     VKAPI_CALL Hook_DestroyInstance(VkInstance, const VkAllocationCallbacks*);
VKAPI_ATTR VkResult VKAPI_CALL Hook_CreateDevice(VkPhysicalDevice, const VkDeviceCreateInfo*, const VkAllocationCallbacks*, VkDevice*);
VKAPI_ATTR void     VKAPI_CALL Hook_DestroyDevice(VkDevice, const VkAllocationCallbacks*);
VKAPI_ATTR VkResult VKAPI_CALL Hook_CreateSwapchainKHR(VkDevice, const VkSwapchainCreateInfoKHR*, const VkAllocationCallbacks*, VkSwapchainKHR*);
VKAPI_ATTR void     VKAPI_CALL Hook_DestroySwapchainKHR(VkDevice, VkSwapchainKHR, const VkAllocationCallbacks*);
VKAPI_ATTR VkResult VKAPI_CALL Hook_QueuePresentKHR(VkQueue, const VkPresentInfoKHR*);

// ---- Exported entry points ----
FRAMEGEN_EXPORT VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL FrameGen_GetInstanceProcAddr(VkInstance, const char*);
FRAMEGEN_EXPORT VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL FrameGen_GetDeviceProcAddr(VkDevice, const char*);
FRAMEGEN_EXPORT VKAPI_ATTR VkResult VKAPI_CALL FrameGen_NegotiateLoaderLayerInterfaceVersion(VkNegotiateLayerInterface*);
```

