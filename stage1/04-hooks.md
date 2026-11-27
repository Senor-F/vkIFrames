[index](../STAGE1.md) | [previous](03-registries-proc-addr-tables-negotiation.md) | [next](05-verification-testing-procedure.md)

### C. Hooks (`src/layer_main.cpp`)

```cpp
#include "vk_iframes_layer.hpp"

#include <algorithm>
#include <atomic>

namespace {
constexpr uint32_t kMinImageCount = 4;

VkLayerInstanceCreateInfo* FindInstanceLink(const VkInstanceCreateInfo* ci) {
    auto* p = reinterpret_cast<VkLayerInstanceCreateInfo*>(const_cast<void*>(ci->pNext));
    while (p && !(p->sType == VK_STRUCTURE_TYPE_LOADER_INSTANCE_CREATE_INFO && p->function == VK_LAYER_LINK_INFO))
        p = reinterpret_cast<VkLayerInstanceCreateInfo*>(const_cast<void*>(p->pNext));
    return p;
}

VkLayerDeviceCreateInfo* FindDeviceLink(const VkDeviceCreateInfo* ci) {
    auto* p = reinterpret_cast<VkLayerDeviceCreateInfo*>(const_cast<void*>(ci->pNext));
    while (p && !(p->sType == VK_STRUCTURE_TYPE_LOADER_DEVICE_CREATE_INFO && p->function == VK_LAYER_LINK_INFO))
        p = reinterpret_cast<VkLayerDeviceCreateInfo*>(const_cast<void*>(p->pNext));
    return p;
}
} // namespace

VKAPI_ATTR VkResult VKAPI_CALL Hook_CreateInstance(const VkInstanceCreateInfo* pCreateInfo,
                                                   const VkAllocationCallbacks* pAllocator,
                                                   VkInstance* pInstance) {
    auto* link = FindInstanceLink(pCreateInfo);
    if (!link || !link->u.pLayerInfo) return VK_ERROR_INITIALIZATION_FAILED;

    PFN_vkGetInstanceProcAddr gipa = link->u.pLayerInfo->pfnNextGetInstanceProcAddr;
    link->u.pLayerInfo = link->u.pLayerInfo->pNext;               // advance the chain for the next layer

    auto create = reinterpret_cast<PFN_vkCreateInstance>(gipa(VK_NULL_HANDLE, "vkCreateInstance"));
    if (!create) return VK_ERROR_INITIALIZATION_FAILED;

    VkResult r = create(pCreateInfo, pAllocator, pInstance);
    if (r != VK_SUCCESS) return r;

    RegisterInstance(GetKey(*pInstance), *pInstance, gipa);
    FG_LOG("instance created");
    return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL Hook_DestroyInstance(VkInstance instance, const VkAllocationCallbacks* pAllocator) {
    void* key = GetKey(instance);                                  // read before the handle dies
    auto destroy = LookupInstance(key)->DestroyInstance;
    destroy(instance, pAllocator);
    UnregisterInstance(key);
}

VKAPI_ATTR VkResult VKAPI_CALL Hook_CreateDevice(VkPhysicalDevice physicalDevice,
                                                 const VkDeviceCreateInfo* pCreateInfo,
                                                 const VkAllocationCallbacks* pAllocator,
                                                 VkDevice* pDevice) {
    InstanceDispatch* inst = LookupInstance(GetKey(physicalDevice));
    auto* link = FindDeviceLink(pCreateInfo);
    if (!inst || !link || !link->u.pLayerInfo) return VK_ERROR_INITIALIZATION_FAILED;

    PFN_vkGetDeviceProcAddr gdpa = link->u.pLayerInfo->pfnNextGetDeviceProcAddr;
    link->u.pLayerInfo = link->u.pLayerInfo->pNext;

    // Stages 2-4 will build a modified VkDeviceCreateInfo here (extensions, features, extra queue).
    VkResult r = inst->CreateDevice(physicalDevice, pCreateInfo, pAllocator, pDevice);
    if (r != VK_SUCCESS) return r;

    RegisterDevice(GetKey(*pDevice), *pDevice, physicalDevice, gdpa, inst);
    FG_LOG("device created");
    return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL Hook_DestroyDevice(VkDevice device, const VkAllocationCallbacks* pAllocator) {
    void* key = GetKey(device);
    auto destroy = LookupDevice(key)->DestroyDevice;
    destroy(device, pAllocator);
    UnregisterDevice(key);
}

VKAPI_ATTR VkResult VKAPI_CALL Hook_CreateSwapchainKHR(VkDevice device,
                                                       const VkSwapchainCreateInfoKHR* pCreateInfo,
                                                       const VkAllocationCallbacks* pAllocator,
                                                       VkSwapchainKHR* pSwapchain) {
    DeviceDispatch* dt = LookupDevice(GetKey(device));

    VkSwapchainCreateInfoKHR ci = *pCreateInfo;

    // Raise the image count toward kMinImageCount, but never beyond what the surface allows
    // (maxImageCount == 0 means "no limit").
    VkSurfaceCapabilitiesKHR caps{};
    if (dt->instance->GetPhysicalDeviceSurfaceCapabilitiesKHR(dt->physical_device, ci.surface, &caps) == VK_SUCCESS) {
        uint32_t wanted = std::max(ci.minImageCount, kMinImageCount);
        if (caps.maxImageCount != 0) wanted = std::min(wanted, caps.maxImageCount);
        if (wanted != ci.minImageCount) {
            FG_LOG("vkCreateSwapchainKHR: minImageCount %u -> %u", ci.minImageCount, wanted);
            ci.minImageCount = wanted;
        }
    }

    return dt->CreateSwapchainKHR(device, &ci, pAllocator, pSwapchain);
}

VKAPI_ATTR void VKAPI_CALL Hook_DestroySwapchainKHR(VkDevice device, VkSwapchainKHR swapchain,
                                                    const VkAllocationCallbacks* pAllocator) {
    DeviceDispatch* dt = LookupDevice(GetKey(device));
    FG_LOG("vkDestroySwapchainKHR");
    dt->DestroySwapchainKHR(device, swapchain, pAllocator);
}

VKAPI_ATTR VkResult VKAPI_CALL Hook_QueuePresentKHR(VkQueue queue, const VkPresentInfoKHR* pPresentInfo) {
    DeviceDispatch* dt = LookupDevice(GetKey(queue));          // a queue shares its device's key

    static std::atomic<uint64_t> frame_count{0};
    uint64_t n = ++frame_count;
    if (n % 300 == 0) FG_LOG("vkQueuePresentKHR (frame %llu)", static_cast<unsigned long long>(n));

    return dt->QueuePresentKHR(queue, pPresentInfo);
}
```

---

