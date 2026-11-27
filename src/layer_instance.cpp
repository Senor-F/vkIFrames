// Layer hooks: instance and device creation and destruction, queue lookup.
#include "layer_internal.hpp"

#include <algorithm>
#include <cstring>
#include <string>
#include <vector>

using namespace fg_layer;

namespace fg_layer {

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

VkLayerDeviceCreateInfo* FindDeviceCallback(const VkDeviceCreateInfo* ci) {
    auto* p = reinterpret_cast<VkLayerDeviceCreateInfo*>(const_cast<void*>(ci->pNext));
    while (p && !(p->sType == VK_STRUCTURE_TYPE_LOADER_DEVICE_CREATE_INFO && p->function == VK_LOADER_DATA_CALLBACK))
        p = reinterpret_cast<VkLayerDeviceCreateInfo*>(const_cast<void*>(p->pNext));
    return p;
}

bool HasExt(const char* const* exts, uint32_t n, const char* name) {
    for (uint32_t i = 0; i < n; ++i) if (std::strcmp(exts[i], name) == 0) return true;
    return false;
}

// Finds a structure of the given type anywhere in a pNext chain.
const VkBaseInStructure* FindInChain(const void* chain, VkStructureType type) {
    for (auto* p = static_cast<const VkBaseInStructure*>(chain); p; p = p->pNext)
        if (p->sType == type) return p;
    return nullptr;
}

bool DeviceExtSupported(const std::vector<VkExtensionProperties>& props, const char* name) {
    for (const auto& p : props) if (std::strcmp(p.extensionName, name) == 0) return true;
    return false;
}
} // namespace

// ---------------------------------------------------------------- instance

VkResult Impl_CreateInstance(const VkInstanceCreateInfo* pCreateInfo,
                                                   const VkAllocationCallbacks* pAllocator,
                                                   VkInstance* pInstance) {
    auto* link = FindInstanceLink(pCreateInfo);
    if (!link || !link->u.pLayerInfo) return VK_ERROR_INITIALIZATION_FAILED;

    PFN_vkGetInstanceProcAddr gipa = link->u.pLayerInfo->pfnNextGetInstanceProcAddr;
    link->u.pLayerInfo = link->u.pLayerInfo->pNext;               // advance the chain for the next layer
    VkLayerInstanceLink* const next_link = link->u.pLayerInfo;    // every layer below advances this shared struct again in its own vkCreateInstance: each attempt below starts from this node

    auto create = reinterpret_cast<PFN_vkCreateInstance>(gipa(VK_NULL_HANDLE, "vkCreateInstance"));
    if (!create) return VK_ERROR_INITIALIZATION_FAILED;

    const uint32_t api = (pCreateInfo->pApplicationInfo && pCreateInfo->pApplicationInfo->apiVersion)
                             ? pCreateInfo->pApplicationInfo->apiVersion : VK_API_VERSION_1_0;
    const auto** exts = const_cast<const char**>(pCreateInfo->ppEnabledExtensionNames);
    const uint32_t n_exts = pCreateInfo->enabledExtensionCount;

    bool surface_caps2 = HasExt(exts, n_exts, VK_KHR_GET_SURFACE_CAPABILITIES_2_EXTENSION_NAME);
    bool props2 = api >= VK_API_VERSION_1_1 || HasExt(exts, n_exts, VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME);
    const bool uses_surface = HasExt(exts, n_exts, VK_KHR_SURFACE_EXTENSION_NAME);

    // Present timing needs surface capabilities 2 on the instance. Add it when the app uses surfaces and the implementation offers it; for a
    // Vulkan 1.0 app (vkcube is one) also the KHR extension in place of the core 1.1 functions. We do not ask the layers below whether it is
    // offered (their vkGetInstanceProcAddr need not accept a null instance: Mesa's device_select crashes on one): we try the instance with the
    // extras and, if the implementation says an extension is not present, again with fewer of them.
    struct Extras { bool props2_ext, caps2_ext; };
    const bool add_props2 = uses_surface && !props2, add_caps2 = uses_surface && !surface_caps2;
    std::vector<Extras> tries{{add_props2, add_caps2}};
    if (add_caps2) tries.push_back({add_props2, false});
    if (add_props2) tries.push_back({false, false});
    tries.erase(std::unique(tries.begin(), tries.end(), [](const Extras& x, const Extras& y) { return x.props2_ext == y.props2_ext && x.caps2_ext == y.caps2_ext; }), tries.end());

    VkResult r = VK_ERROR_EXTENSION_NOT_PRESENT;
    bool got_props2 = props2, got_caps2 = surface_caps2;
    for (const Extras& t : tries) {
        std::vector<const char*> names(exts, exts + n_exts);
        // surface capabilities 2 depends on get_physical_device_properties2 (or Vulkan 1.1): never add it alone
        const bool caps2_ok = t.caps2_ext && (props2 || t.props2_ext);
        if (t.props2_ext) names.push_back(VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME);
        if (caps2_ok) names.push_back(VK_KHR_GET_SURFACE_CAPABILITIES_2_EXTENSION_NAME);
        VkInstanceCreateInfo ci = *pCreateInfo;
        ci.enabledExtensionCount = uint32_t(names.size());
        ci.ppEnabledExtensionNames = names.data();
        link->u.pLayerInfo = next_link;                          // a failed attempt left the chain advanced past the layers below: put it back
        r = create(&ci, pAllocator, pInstance);
        if (r == VK_SUCCESS) { got_props2 = props2 || t.props2_ext; got_caps2 = surface_caps2 || caps2_ok; break; }
        // Any failure of a request that carries our extras (a layer or driver below may answer an unwanted extension with another code) is tried again with
        // fewer; the last try is the app's own list, and its result is what the app gets.
    }
    if (r != VK_SUCCESS) return r;
    props2 = got_props2; surface_caps2 = got_caps2;
    if (uses_surface && !surface_caps2) FG_LOG("present timing: instance extension VK_KHR_get_surface_capabilities2 unavailable");

    InstanceDispatch* inst = RegisterInstance(GetKey(*pInstance), *pInstance, gipa);
    inst->api_version = api;
    inst->props2 = props2;
    inst->surface_caps2 = surface_caps2;
    if (!surface_caps2) inst->GetPhysicalDeviceSurfaceCapabilities2KHR = nullptr;
    FG_LOG("instance created (vkIFrames %s, commit %s, commit time %s unix)", FRAMEGEN_VERSION, FRAMEGEN_COMMIT, FRAMEGEN_COMMIT_TIME);
    return VK_SUCCESS;
}

void Impl_DestroyInstance(VkInstance instance, const VkAllocationCallbacks* pAllocator) {
    void* key = GetKey(instance);                                  // read before the handle dies
    InstanceDispatch* id = LookupInstance(key);
    if (!id) return;                                               // not created through this layer: there is no next function to call
    t_guard_forwarded = true;                                       // the next layer's destroy is called: the guard must not call it again
    id->DestroyInstance(instance, pAllocator);
    UnregisterInstance(key);
}

// ---------------------------------------------------------------- device

VkResult Impl_CreateDevice(VkPhysicalDevice physicalDevice,
                                                 const VkDeviceCreateInfo* pCreateInfo,
                                                 const VkAllocationCallbacks* pAllocator,
                                                 VkDevice* pDevice) {
    InstanceDispatch* inst = LookupInstance(GetKey(physicalDevice));
    auto* link = FindDeviceLink(pCreateInfo);
    if (!inst || !link || !link->u.pLayerInfo) return VK_ERROR_INITIALIZATION_FAILED;
    const Config& cfg = GetConfig();

    PFN_vkGetDeviceProcAddr gdpa = link->u.pLayerInfo->pfnNextGetDeviceProcAddr;
    link->u.pLayerInfo = link->u.pLayerInfo->pNext;
    PFN_vkSetDeviceLoaderData set_loader_data = nullptr;
    if (auto* cb = FindDeviceCallback(pCreateInfo)) set_loader_data = cb->u.pfnSetDeviceLoaderData;

    std::vector<const char*> names(pCreateInfo->ppEnabledExtensionNames,
                                   pCreateInfo->ppEnabledExtensionNames + pCreateInfo->enabledExtensionCount);
    const uint32_t n_names = uint32_t(names.size());
    PFN_vkGetPhysicalDeviceFeatures2 get_features2 = inst->api_version >= VK_API_VERSION_1_1 && inst->GetPhysicalDeviceFeatures2
                                                         ? inst->GetPhysicalDeviceFeatures2 : inst->GetPhysicalDeviceFeatures2KHR;
    const bool app_has_swapchain = HasExt(names.data(), n_names, VK_KHR_SWAPCHAIN_EXTENSION_NAME);

    // What the driver offers (read once).
    std::vector<VkExtensionProperties> props;
    if (inst->EnumerateDeviceExtensionProperties) {
        uint32_t n = 0;
        inst->EnumerateDeviceExtensionProperties(physicalDevice, nullptr, &n, nullptr);
        props.resize(n);
        if (n) inst->EnumerateDeviceExtensionProperties(physicalDevice, nullptr, &n, props.data());
        props.resize(n);
    }

    // ---- present timing (Stage 2): every condition is checked; if one fails the device is created untouched.
    const char* kTimingExts[] = {VK_EXT_PRESENT_TIMING_EXTENSION_NAME, VK_KHR_PRESENT_ID_2_EXTENSION_NAME,
                                 VK_KHR_CALIBRATED_TIMESTAMPS_EXTENSION_NAME};
    VkPhysicalDevicePresentTimingFeaturesEXT f_timing{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRESENT_TIMING_FEATURES_EXT};
    VkPhysicalDevicePresentId2FeaturesKHR    f_id2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRESENT_ID_2_FEATURES_KHR};
    bool want_timing = false;
    const bool app_uses_timing = HasExt(names.data(), n_names, VK_EXT_PRESENT_TIMING_EXTENSION_NAME) ||
                                 HasExt(names.data(), n_names, VK_KHR_PRESENT_ID_2_EXTENSION_NAME) ||
                                 HasExt(names.data(), n_names, VK_KHR_PRESENT_ID_EXTENSION_NAME) ||
                                 FindInChain(pCreateInfo->pNext, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRESENT_TIMING_FEATURES_EXT) ||
                                 FindInChain(pCreateInfo->pNext, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRESENT_ID_2_FEATURES_KHR) ||
                                 FindInChain(pCreateInfo->pNext, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRESENT_ID_FEATURES_KHR);
    if (!app_has_swapchain)        FG_LOG("present timing: skipped (no VK_KHR_swapchain on this device)");
    else if (app_uses_timing)      FG_LOG("present timing: skipped (the application uses present timing / present id itself)");
    else if (!inst->surface_caps2) FG_LOG("present timing: skipped (surface capabilities 2 not enabled on the instance)");
    else if (!inst->props2 || !get_features2) FG_LOG("present timing: skipped (no physical-device-properties2 / features2 entry point)");
    if (app_has_swapchain && !app_uses_timing && inst->surface_caps2 && inst->props2 && get_features2 && !props.empty()) {
        bool all = true;
        for (const char* e : kTimingExts) all = all && DeviceExtSupported(props, e);
        if (all) {
            VkPhysicalDeviceFeatures2 q{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
            q.pNext = &f_timing; f_timing.pNext = &f_id2;
            get_features2(physicalDevice, &q);
            if (f_timing.presentTiming && f_timing.presentAtAbsoluteTime && f_id2.presentId2) want_timing = true;
            else FG_LOG("present timing: device features not all supported");
        } else {
            FG_LOG("present timing: device extensions not offered by this driver");
        }
    }

    // ---- synchronization2 (Stage 3): needed to capture and hold frames.
    bool sync2_ok = false, add_sync2 = false;
    if (cfg.hold && app_has_swapchain) {
        const auto* v13 = FindInChain(pCreateInfo->pNext, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES);
        const auto* s2  = FindInChain(pCreateInfo->pNext, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SYNCHRONIZATION_2_FEATURES);
        if (v13)      sync2_ok = reinterpret_cast<const VkPhysicalDeviceVulkan13Features*>(v13)->synchronization2;
        else if (s2)  sync2_ok = reinterpret_cast<const VkPhysicalDeviceSynchronization2Features*>(s2)->synchronization2;
        else if (inst->props2 && get_features2 && DeviceExtSupported(props, VK_KHR_SYNCHRONIZATION_2_EXTENSION_NAME)) {
            VkPhysicalDeviceSynchronization2Features f{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SYNCHRONIZATION_2_FEATURES};
            VkPhysicalDeviceFeatures2 q{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, &f};
            get_features2(physicalDevice, &q);
            add_sync2 = sync2_ok = f.synchronization2;
        }
        if (!sync2_ok) FG_LOG("hold: synchronization2 not available on this device, frame holding is off");
    }

    VkDeviceCreateInfo ci = *pCreateInfo;
    VkPhysicalDevicePresentTimingFeaturesEXT en_timing{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRESENT_TIMING_FEATURES_EXT};
    VkPhysicalDevicePresentId2FeaturesKHR    en_id2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRESENT_ID_2_FEATURES_KHR};
    VkPhysicalDeviceSynchronization2Features en_sync2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SYNCHRONIZATION_2_FEATURES};
    if (want_timing || add_sync2) {
        const void* chain = pCreateInfo->pNext;                       // our structures go in front of the app's chain
        if (add_sync2) {
            if (!HasExt(names.data(), n_names, VK_KHR_SYNCHRONIZATION_2_EXTENSION_NAME)) names.push_back(VK_KHR_SYNCHRONIZATION_2_EXTENSION_NAME);
            en_sync2.synchronization2 = VK_TRUE; en_sync2.pNext = const_cast<void*>(chain); chain = &en_sync2;
        }
        if (want_timing) {
            for (const char* e : kTimingExts) if (!HasExt(names.data(), uint32_t(names.size()), e)) names.push_back(e);
            en_timing.presentTiming = VK_TRUE; en_timing.presentAtAbsoluteTime = VK_TRUE;
            en_timing.presentAtRelativeTime = f_timing.presentAtRelativeTime;
            en_id2.presentId2 = VK_TRUE;
            en_id2.pNext = const_cast<void*>(chain); en_timing.pNext = &en_id2; chain = &en_timing;
        }
        ci.pNext = chain;
        ci.enabledExtensionCount = uint32_t(names.size());
        ci.ppEnabledExtensionNames = names.data();
    }

    VkResult r = inst->CreateDevice(physicalDevice, &ci, pAllocator, pDevice);
    if (r != VK_SUCCESS) return r;

    DeviceDispatch* dt = RegisterDevice(GetKey(*pDevice), *pDevice, physicalDevice, gdpa, inst);
    dt->timing_supported = want_timing && dt->SetSwapchainPresentTimingQueueSizeEXT && dt->GetSwapchainTimeDomainPropertiesEXT &&
                           dt->GetPastPresentationTimingEXT && dt->GetCalibratedTimestampsKHR;
    dt->set_loader_data = set_loader_data;
    dt->sync2 = sync2_ok && set_loader_data && (dt->QueueSubmit2 || dt->QueueSubmit2KHR) && (dt->CmdPipelineBarrier2 || dt->CmdPipelineBarrier2KHR) &&
                dt->GetSwapchainImagesKHR && dt->CmdBlitImage && dt->CreateImage && dt->CreateCommandPool && dt->CreateFence && dt->CreateSemaphore;
    for (uint32_t i = 0; i < pCreateInfo->queueCreateInfoCount; ++i) {
        const uint32_t f = pCreateInfo->pQueueCreateInfos[i].queueFamilyIndex;
        if (std::find(dt->queue_families.begin(), dt->queue_families.end(), f) == dt->queue_families.end()) dt->queue_families.push_back(f);
    }
    FG_LOG("device created (present timing %s, frame holding %s)", dt->timing_supported ? "enabled" : "off", dt->sync2 ? "available" : "off");
    return VK_SUCCESS;
}

void Impl_DestroyDevice(VkDevice device, const VkAllocationCallbacks* pAllocator) {
    void* key = GetKey(device);
    DeviceDispatch* dd = LookupDevice(key);
    if (!dd) return;                                               // not created through this layer: there is no next function to call
    t_guard_forwarded = true;
    dd->DestroyDevice(device, pAllocator);
    if (g_presenter_detached) { LeakQueues(key); LeakDevice(key); }           // a detached presenter thread still holds the dispatch table and may hold a queue mutex
    else { UnregisterQueues(key); UnregisterDevice(key); }
}

VKAPI_ATTR void VKAPI_CALL Hook_GetDeviceQueue(VkDevice device, uint32_t family, uint32_t index, VkQueue* pQueue) {
    DeviceDispatch* dt = LookupDevice(GetKey(device));
    if (!dt) { if (pQueue) *pQueue = VK_NULL_HANDLE; return; }
    dt->GetDeviceQueue(device, family, index, pQueue);
    if (pQueue && *pQueue) RegisterQueue(*pQueue, family, GetKey(device));
}

VKAPI_ATTR void VKAPI_CALL Hook_GetDeviceQueue2(VkDevice device, const VkDeviceQueueInfo2* pInfo, VkQueue* pQueue) {
    DeviceDispatch* dt = LookupDevice(GetKey(device));
    if (!dt || !dt->GetDeviceQueue2) { if (pQueue) *pQueue = VK_NULL_HANDLE; return; }
    dt->GetDeviceQueue2(device, pInfo, pQueue);
    if (pInfo && pQueue && *pQueue) RegisterQueue(*pQueue, pInfo->queueFamilyIndex, GetKey(device));
}
