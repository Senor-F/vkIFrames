#include "vk_iframes_layer.hpp"
#include "live_config.hpp"
#include "gpu_frames.hpp"
#include "present_timing.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>
#include <cstring>
#include <string>
#include <vector>

namespace { bool StopPresenter(SwapchainState& sc); void HeldFencesDrop(DeviceDispatch& dt, SwapchainState* sc); std::atomic<bool> g_presenter_detached{false}; }

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

VKAPI_ATTR VkResult VKAPI_CALL Hook_CreateInstance(const VkInstanceCreateInfo* pCreateInfo,
                                                   const VkAllocationCallbacks* pAllocator,
                                                   VkInstance* pInstance) {
    auto* link = FindInstanceLink(pCreateInfo);
    if (!link || !link->u.pLayerInfo) return VK_ERROR_INITIALIZATION_FAILED;

    PFN_vkGetInstanceProcAddr gipa = link->u.pLayerInfo->pfnNextGetInstanceProcAddr;
    link->u.pLayerInfo = link->u.pLayerInfo->pNext;               // advance the chain for the next layer

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

VKAPI_ATTR void VKAPI_CALL Hook_DestroyInstance(VkInstance instance, const VkAllocationCallbacks* pAllocator) {
    void* key = GetKey(instance);                                  // read before the handle dies
    InstanceDispatch* id = LookupInstance(key);
    if (!id) return;                                               // not created through this layer: there is no next function to call
    id->DestroyInstance(instance, pAllocator);
    UnregisterInstance(key);
}

// ---------------------------------------------------------------- device

VKAPI_ATTR VkResult VKAPI_CALL Hook_CreateDevice(VkPhysicalDevice physicalDevice,
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

VKAPI_ATTR void VKAPI_CALL Hook_DestroyDevice(VkDevice device, const VkAllocationCallbacks* pAllocator) {
    void* key = GetKey(device);
    DeviceDispatch* dd = LookupDevice(key);
    if (!dd) return;                                               // not created through this layer: there is no next function to call
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

// ---------------------------------------------------------------- swapchain

VKAPI_ATTR VkResult VKAPI_CALL Hook_CreateSwapchainKHR(VkDevice device,
                                                       const VkSwapchainCreateInfoKHR* pCreateInfo,
                                                       const VkAllocationCallbacks* pAllocator,
                                                       VkSwapchainKHR* pSwapchain) {
    DeviceDispatch* dt = LookupDevice(GetKey(device));
    if (!dt) return VK_ERROR_INITIALIZATION_FAILED;
    const Config& cfg = GetConfig();

    VkSwapchainCreateInfoKHR ci = *pCreateInfo;
    const uint32_t app_min_images = ci.minImageCount;

    // Frame holding needs spare images (1 held + hidden acquires later), transfer usage on the swapchain images, and blit
    // support for the swapchain format and the ring format. Anything missing leaves this swapchain in passthrough.
    bool hold = cfg.hold && dt->sync2 && !(ci.flags & VK_SWAPCHAIN_CREATE_PROTECTED_BIT_KHR);
    VkSurfaceCapabilitiesKHR caps{};
    const bool have_caps = dt->instance->GetPhysicalDeviceSurfaceCapabilitiesKHR(dt->physical_device, ci.surface, &caps) == VK_SUCCESS;
    if (hold && have_caps) {
        const VkImageUsageFlags need_usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        VkFormatProperties fp{}, rp{};
        dt->instance->GetPhysicalDeviceFormatProperties(dt->physical_device, ci.imageFormat, &fp);
        dt->instance->GetPhysicalDeviceFormatProperties(dt->physical_device, GpuState::kRingFormat, &rp);
        const VkFormatFeatureFlags blit = VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT;
        const VkFormatFeatureFlags ring = blit | VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT;
        if ((caps.supportedUsageFlags & need_usage) != need_usage) { FG_LOG("hold: surface does not allow transfer usage"); hold = false; }
        else if ((fp.optimalTilingFeatures & blit) != blit)         { FG_LOG("hold: swapchain format %d cannot be blitted", int(ci.imageFormat)); hold = false; }
        else if ((rp.optimalTilingFeatures & ring) != ring)         { FG_LOG("hold: ring format cannot be used here"); hold = false; }
    } else if (hold) hold = false;

    int synth_capacity = kLiveMaxSyntheticNormal;                  // C for this swapchain (set below when the surface is known)
    if (have_caps) {
        // Images that exist at once beyond what the game expects: 1 held + up to k_max queued synthetic + 1 hidden acquire.
        uint32_t wanted = std::max(ci.minImageCount, kMinImageCount);
        synth_capacity = LiveSynthCapacityFor(cfg.max_synthetic);          // C: MAX_SYNTHETIC at game start (live.conf over the environment), at least 4, at most the limit EXPERIMENTAL allows
        if (hold) wanted = std::max(wanted, app_min_images + uint32_t(synth_capacity) + 2);
        if (caps.maxImageCount != 0) wanted = std::min(wanted, caps.maxImageCount);
        if (wanted != ci.minImageCount) {
            FG_LOG("vkCreateSwapchainKHR: minImageCount %u -> %u", ci.minImageCount, wanted);
            ci.minImageCount = wanted;
        }
        if (hold && ci.minImageCount < app_min_images + 1) { FG_LOG("hold: no spare image available (surface max %u)", caps.maxImageCount); hold = false; }
    }
    if (hold) ci.imageUsage |= VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;

    // Per-surface check: this surface must support present timing and present id 2, otherwise the flags would fail creation.
    VkPresentStageFlagsEXT stage_mask = 0;
    bool try_timing = false, surface_absolute = false;
    if (dt->timing_supported && dt->instance->GetPhysicalDeviceSurfaceCapabilities2KHR &&
        !(ci.flags & (VK_SWAPCHAIN_CREATE_PRESENT_TIMING_BIT_EXT | VK_SWAPCHAIN_CREATE_PRESENT_ID_2_BIT_KHR))) {
        VkPresentTimingSurfaceCapabilitiesEXT tcaps{VK_STRUCTURE_TYPE_PRESENT_TIMING_SURFACE_CAPABILITIES_EXT};
        VkSurfaceCapabilitiesPresentId2KHR    icaps{VK_STRUCTURE_TYPE_SURFACE_CAPABILITIES_PRESENT_ID_2_KHR};
        tcaps.pNext = &icaps;
        VkSurfaceCapabilities2KHR c2{VK_STRUCTURE_TYPE_SURFACE_CAPABILITIES_2_KHR};
        c2.pNext = &tcaps;
        VkPhysicalDeviceSurfaceInfo2KHR si{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SURFACE_INFO_2_KHR};
        si.surface = ci.surface;
        const VkResult cr = dt->instance->GetPhysicalDeviceSurfaceCapabilities2KHR(dt->physical_device, &si, &c2);
        FG_LOG("present timing: surface query result=%d timing=%u absolute=%u relative=%u id2=%u stages=0x%x", int(cr),
               tcaps.presentTimingSupported, tcaps.presentAtAbsoluteTimeSupported, tcaps.presentAtRelativeTimeSupported,
               icaps.presentId2Supported, unsigned(tcaps.presentStageQueries));
        if (cr == VK_SUCCESS &&
            tcaps.presentTimingSupported && icaps.presentId2Supported) {
            // Without absolute targets (e.g. a Wayland compositor lacking commit timing) timing still reports when frames
            // reached the screen; pacing then falls back to vblank (FIFO), exactly as DESIGN section 4.6 plans.
            if (!tcaps.presentAtAbsoluteTimeSupported) FG_LOG("present timing: surface has no absolute target times; feedback only, pacing stays vblank-driven");
            surface_absolute = tcaps.presentAtAbsoluteTimeSupported;
            stage_mask = tcaps.presentStageQueries;
            try_timing = stage_mask != 0;
        } else {
            FG_LOG("present timing: not supported on this surface");
        }
    }

    const VkPresentModeKHR app_mode = ci.presentMode;
    if (try_timing) {
        ci.flags |= VK_SWAPCHAIN_CREATE_PRESENT_TIMING_BIT_EXT | VK_SWAPCHAIN_CREATE_PRESENT_ID_2_BIT_KHR;
        if (ci.presentMode != VK_PRESENT_MODE_FIFO_KHR) {
            FG_LOG("present timing: forcing FIFO (app asked for mode %d)", int(ci.presentMode));
            ci.presentMode = VK_PRESENT_MODE_FIFO_KHR;
        }
    }

    VkResult r = dt->CreateSwapchainKHR(device, &ci, pAllocator, pSwapchain);
    if (r != VK_SUCCESS && try_timing) {
        FG_LOG("present timing: swapchain creation failed with the flags (%d), retrying without", int(r));
        ci.flags = pCreateInfo->flags; ci.presentMode = app_mode;
        try_timing = false;
        r = dt->CreateSwapchainKHR(device, &ci, pAllocator, pSwapchain);
    }
    if (r != VK_SUCCESS) return r;

    SwapchainState* sc = RegisterSwapchain(*pSwapchain);
    sc->hold_candidate = hold; sc->extent = ci.imageExtent; sc->format = ci.imageFormat;
    if (hold) {
        // What this swapchain can really hold: C limited by the images the driver granted (1 held + C queued synthetic + 1 hidden acquire beyond the app's own).
        uint32_t granted = 0;
        if (dt->GetSwapchainImagesKHR && dt->GetSwapchainImagesKHR(device, *pSwapchain, &granted, nullptr) >= 0 && granted > 0) {
            const int fit = int(granted) - int(app_min_images) - 2;
            if (fit < 1) {
                // Not even one synthetic frame has an image (held frame + one queued + one spare for the acquire): holding would only add a frame of latency and the first
                // synthetic tick would wait for an image that cannot be released. Pass-through for this swapchain: hold_candidate is read only by the present path
                // (before any GpuInit, capture or presenter thread exist), so turning it off here is safe.
                FG_LOG("synthetic frames: swapchain built for %d, but only %u images granted (the app asked for %u): not even one synthetic frame fits, frame holding is off for this swapchain", synth_capacity, granted, app_min_images);
                sc->hold_candidate = false; sc->synth_capacity = 0;
                LiveSetSynthCapacity(0);
                return VK_SUCCESS;
            }
            sc->synth_capacity = std::clamp(std::min(synth_capacity, fit), 1, kLiveMaxSynthetic);
            FG_LOG("synthetic frames: swapchain built for %d, %u images granted (the app asked for %u): capacity %d%s", synth_capacity, granted, app_min_images, sc->synth_capacity,
                   sc->synth_capacity < synth_capacity ? " (fewer than wanted: a higher MAX_SYNTHETIC is clamped to it)" : "");
        } else sc->synth_capacity = synth_capacity;
        LiveSetSynthCapacity(sc->synth_capacity);
    }
    if (try_timing) {
        if (!SetupSwapchainTiming(*dt, *sc, stage_mask, ci.minImageCount * 4))
            FG_LOG("present timing: setup failed, presenting without timing");
        sc->absolute_ok = sc->timing_enabled && surface_absolute;
    }
    return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL Hook_DestroySwapchainKHR(VkDevice device, VkSwapchainKHR swapchain,
                                                    const VkAllocationCallbacks* pAllocator) {
    DeviceDispatch* dt = LookupDevice(GetKey(device));
    if (!dt) return;
    StatsBenchAbort("the game closed its swapchain");           // a benchmark run stops and restores ENABLED
    FG_LOG("shutdown: vkDestroySwapchainKHR");
    const uint64_t t0 = NowNs();
    if (swapchain) {
        if (SwapchainState* sc = FindSwapchain(swapchain)) {
            if (sc->timing_enabled) DrainPastTimings(*dt, *sc);
            const bool detached = StopPresenter(*sc);                             // no job may touch the swapchain while it is destroyed
            FG_LOG("shutdown: presenter stopped (%.0f ms)", (NowNs() - t0) / 1e6);
            HeldFencesDrop(*dt, sc);                                              // a later wait on the held frame's fence must not reach this state
            if (detached) {
                // The presenter thread is stuck in a driver call and still holds the swapchain state, its GPU state and the device dispatch table:
                // none of them is freed (this is shutdown, a small leak); vkDestroyDevice leaves the dispatch table alone too.
                FG_LOG("shutdown: the presenter thread is still running; its swapchain state, GPU state and dispatch table are left allocated");
                g_presenter_detached = true;
                LeakSwapchainState(swapchain);
            } else {
                if (sc->gpu) { GpuDestroy(*dt, sc->gpu); sc->gpu = nullptr; }  // the held frame is simply dropped with its swapchain
                FG_LOG("shutdown: GPU resources destroyed (%.0f ms)", (NowNs() - t0) / 1e6);
                DestroySwapchainState(swapchain);
            }
        } else DestroySwapchainState(swapchain);
    }
    dt->DestroySwapchainKHR(device, swapchain, pAllocator);
    FG_LOG("shutdown: swapchain destroyed (%.0f ms)", (NowNs() - t0) / 1e6);
}

// ---------------------------------------------------------------- present

namespace {
// Stage 3a: runs the scheduler on the real arrival times and counts what it plans; 3b acts on the plan.
PairScheduler::Plan PlanAndCount(SwapchainState& sc, uint64_t now) {
    const Config& cfg = GetConfig();
    PairScheduler::Params p;
    const LiveSettings& live = Live();                       // the settings the plugin can change while a game runs
    const double want_fps = double(live.target_fps.load()), use_fps = EffectiveTargetFps(want_fps, sc.refresh_ns, sc.refresh_vrr);
    if (use_fps != want_fps && sc.clamp_logged_fps != float(want_fps)) { sc.clamp_logged_fps = float(want_fps); FG_LOG("TARGET_FPS %.1f is above the display's refresh rate: using %.2f fps (frames above it would only wait in the present queue)", want_fps, use_fps); }
    else if (use_fps == want_fps) sc.clamp_logged_fps = 0.0f;
    p.target_ns = 1e9 / use_fps; p.k_max = std::min(live.max_synthetic.load(), sc.synth_capacity); p.phase_lock = live.phase_lock.load() != 0; p.phase_offset = live.phase_offset.load();
    p.min_gain = live.enabled.load() ? live.min_gain.load() : 1e6f;   // disabled: the gain gate never opens, so every frame is a plain bypass (a held frame is flushed first)
    auto plan = cfg.extrapolate ? sc.sched.OnNativeFrameExtrap(double(now), sc.timing.SmoothedExactNs(), double(sc.timing.LastRawNs()), p)
                                : sc.sched.OnNativeFrame(double(now), sc.timing.SmoothedExactNs(), sc.timing.JitterExactNs(), double(sc.timing.LastRawNs()), p);
    if (plan.kind == PairScheduler::Kind::Pair && live.synth_margin.load() > 0.0f && !cfg.extrapolate) {   // only generated frames: no tick may sit on A (or B)
        const double m = double(live.synth_margin.load());
        for (int i = 0; i < plan.n; ++i) { plan.ticks[i].t = std::clamp(plan.ticks[i].t, m, 1.0 - m); plan.ticks[i].native_a = false; }
    }
    auto& r = sc.rates;
    ++r.native;
    switch (plan.kind) {
        case PairScheduler::Kind::Bypass: ++r.bypass; r.output += 1 + (plan.flush_held ? 1 : 0); break;
        case PairScheduler::Kind::Hold:   ++r.hold; break;
        case PairScheduler::Kind::Pair:   ++r.pair; r.output += plan.carrier_only ? 1 : plan.n; r.carrier += plan.carrier_only; r.dropped += plan.dropped; break;
    }
    return plan;
}

void LogRates(SwapchainState& sc, uint64_t now) {
    auto& r = sc.rates;
    if (r.last_ns == 0) { r = {.last_ns = now}; return; }
    if (now - r.last_ns < 1'000'000'000ull) return;
    const double secs = double(now - r.last_ns) / 1e9;
    LiveLogCap();                                                  // live.log stays under its cap while the game runs
    const bool want_stats = StatsWantsSecond();
    stats::Record rec;
    {
        const double nat = double(r.native) / secs, shown = double(r.shown) / secs;
        LiveWriteStatus(nat, shown, nat > 0.5 ? std::max(0, int(std::lround(shown / nat)) - 1) : 0, !Live().enabled.load() ? "off" : GetConfig().extrapolate ? "extrap" : sc.sched.GateActive() ? "interp" : "bypass", sc.refresh_ns ? 1e9 / double(sc.refresh_ns) : 0.0, StatsBenchLabel());
    }
    FG_LOG_ROUTINE("native %.1f fps -> output %.1f fps (planned) | shown %.1f fps%s | %s | pairs %llu (synthetic %llu) bypass %llu carrier-only %llu dropped %llu | content delay %.1f ms",
           double(r.native) / secs, double(r.output) / secs, double(r.shown) / secs, sc.gpu ? (GetConfig().extrapolate ? " (extrapolating)" : " (frame holding on)") : "",
           sc.sched.GateActive() ? "interpolating" : "bypass",
           (unsigned long long)r.pair, (unsigned long long)r.synthetic, (unsigned long long)r.bypass, (unsigned long long)r.carrier, (unsigned long long)r.dropped,
           sc.sched.ContentDelayNs() / 1e6);
    if (GetConfig().extrapolate) {
        const uint64_t planned = sc.ex_planned.exchange(0), cancelled = sc.ex_cancelled.exchange(0), ln = sc.ex_late_n.exchange(0), lsum = sc.ex_late_sum_ns.exchange(0), lmax = sc.ex_late_max_ns.exchange(0);
        rec.late_mean_ms = ln ? double(lsum) / double(ln) / 1e6 : 0.0; rec.late_max_ms = double(lmax) / 1e6; rec.cancelled = cancelled;
        FG_LOG_ROUTINE("extrapolation: predicted frames not shown %llu of %llu | native frame late against its slot: mean %.2f ms, max %.2f ms",
               (unsigned long long)cancelled, (unsigned long long)planned, ln ? double(lsum) / double(ln) / 1e6 : 0.0, double(lmax) / 1e6);
    }
    {
        const double n = std::max<double>(1.0, double(r.native));
        const double cpu_hook = double(sc.cpu_hook_ns.exchange(0)) / 1e6 / n, cpu_pres = double(sc.cpu_presenter_ns.exchange(0)) / 1e9 / secs;
        const double w_acq = double(sc.wait_acquire_ns.exchange(0)) / 1e6 / n, w_pres = double(sc.wait_present_ns.exchange(0)) / 1e6 / n;
        double w_lock = 0, w_ring = 0, w_fence = 0;
        if (sc.gpu) {
            w_lock = double(sc.gpu->wait_lock_ns.exchange(0)) / 1e6 / n; w_ring = double(sc.gpu->wait_ring_ns.exchange(0)) / 1e6 / n; w_fence = double(sc.gpu->wait_fence_ns.exchange(0)) / 1e6 / n;
            FG_LOG_ROUTINE("layer CPU: game thread %.3f ms per frame inside present (CPU time, waits excluded) | presenter thread %.1f %% of one core", cpu_hook, cpu_pres * 100.0);
            FG_LOG_ROUTINE("game thread waits per frame: acquire %.2f ms | in present %.2f ms (gpu lock %.2f, ring slot %.2f, cmd fence %.2f)", w_acq, w_pres, w_lock, w_ring, w_fence);
        }
        double flow_ms = 0, pairs_s = 0, interp_ms = 0, gen_s = 0;       // GPU time of the flow and of the interpolation in this second (copied while the readback runs)
        if (sc.gpu) {
            FlowState& fs = sc.gpu->flow;
            const uint64_t fn = fs.stat_flow_n.exchange(0), fns = fs.stat_flow_ns.exchange(0), in = fs.stat_interp_n.exchange(0), ins = fs.stat_interp_ns.exchange(0);
            flow_ms = fn ? double(fns) / 1e6 / double(fn) : 0.0; pairs_s = double(fn) / secs;
            interp_ms = in ? double(ins) / 1e6 / double(in) : 0.0; gen_s = double(in) / secs;
            // One warning per swapchain: the layer's own GPU work is longer than the game's frame time (3 seconds in a row), so the game cannot keep its frame rate.
            static const double kCostWarn = [] { const char* v = EnvVar("COST_WARN"); const double x = v ? std::atof(v) : 1.0; return x > 0.0 ? x : 1.0; }();
            const double ratio = LayerCostRatio(double(fns) / 1e6, double(ins) / 1e6, secs);
            sc.cost_over_windows = ratio > kCostWarn ? sc.cost_over_windows + 1 : 0;
            if (sc.cost_over_windows >= 3 && !sc.cost_warned) {
                sc.cost_warned = true;
                FG_LOG("WARNING: the layer's GPU work is %.0f %% of the game's frame time (flow %.2f ms x %.1f pairs/s + interpolation %.2f ms x %.1f frames/s = %.0f ms of GPU per second). The game cannot hold its frame rate with this: lower MAX_SYNTHETIC or FLOW_LEVEL, or the target.",
                       ratio * 100.0, flow_ms, pairs_s, interp_ms, gen_s, ratio * 1000.0);
            }
        }
        if (want_stats) {                                      // VKIFRAME_STATS / benchmark: this second as one record (stats.hpp)
            rec.secs = secs; rec.target_fps = EffectiveTargetFps(double(Live().target_fps.load()), sc.refresh_ns, sc.refresh_vrr); rec.refresh_hz = sc.refresh_ns ? 1e9 / double(sc.refresh_ns) : 0.0;
            rec.mode = !Live().enabled.load() ? "off" : GetConfig().extrapolate ? "extrap" : sc.sched.GateActive() ? "interp" : "bypass";
            rec.native_fps = double(r.native) / secs; rec.shown_fps = double(r.shown) / secs;
            rec.gen_per_native = rec.native_fps > 0.5 ? std::max(0.0, rec.shown_fps / rec.native_fps - 1.0) : 0.0;
            rec.cpu_game_ms = cpu_hook; rec.presenter_core = cpu_pres;
            rec.wait_lock_ms = w_lock; rec.wait_ring_ms = w_ring; rec.wait_fence_ms = w_fence; rec.wait_acquire_ms = w_acq; rec.wait_present_ms = w_pres;
            rec.dropped = r.dropped;
            rec.flow_ms = flow_ms; rec.pairs_per_s = pairs_s; rec.interp_ms = interp_ms; rec.generated_per_s = gen_s;
            stats::FillDerived(rec, sc.stat_ft);
        }
        sc.stat_ft.Clear();
    }
    if (want_stats) StatsOnSecond(rec, now);
    r = {.last_ns = now};
}

// One present of the queue's mutex-protected queue, with the Stage 2 timing chain when the swapchain has it.
// tick_ns (CLOCK_MONOTONIC) is the time the frame is due; it becomes an absolute target only with VKIFRAME_ABSOLUTE=1.
VkResult PresentWithTiming(DeviceDispatch& dt, VkQueue queue, SwapchainState& sc, const VkPresentInfoKHR& in, uint64_t now, double tick_ns = 0.0) {
    const Config& cfg = GetConfig();
    // Never mix with a present id from the application; our chain would clash with it.
    const bool app_has_ids = FindInChain(in.pNext, VK_STRUCTURE_TYPE_PRESENT_ID_KHR) ||
                             FindInChain(in.pNext, VK_STRUCTURE_TYPE_PRESENT_ID_2_KHR) ||
                             FindInChain(in.pNext, VK_STRUCTURE_TYPE_PRESENT_TIMINGS_INFO_EXT);
    { std::lock_guard<std::mutex> l(sc.mu); ++sc.rates.shown; }
    auto present = [&](const VkPresentInfoKHR* p) {
        std::mutex* qm = QueueMutex(queue);
        std::unique_lock<std::mutex> lk;
        if (qm) lk = std::unique_lock<std::mutex>(*qm);        // the game thread submits to this queue too
        return dt.QueuePresentKHR(queue, p);
    };
    if (!sc.timing_enabled || app_has_ids) return present(&in);
    for (int attempt = 0; attempt < 2; ++attempt) {
        TimingChain chain;
        VkPresentInfoKHR pi = in;
        {
            std::lock_guard<std::mutex> l(sc.mu);
            uint64_t target = 0;                                // 0: as soon as possible
            if (sc.absolute_ok && cfg.timing_test) target = ToDomainTime(dt, sc, now + 2 * sc.timing.SmoothedNs());
            else if (sc.absolute_ok && cfg.absolute && tick_ns > 0.0) target = ToDomainTime(dt, sc, uint64_t(tick_ns));
            pi.pNext = BuildTimingChain(chain, sc, target, in.pNext);
        }
        const VkResult r = present(&pi);
        if (r != VK_ERROR_PRESENT_TIMING_QUEUE_FULL_EXT) return r;
        std::lock_guard<std::mutex> l(sc.mu);
        ++sc.st.queue_full_total;
        DrainPastTimings(dt, sc);                               // free queue slots, then try once more
    }
    return present(&in);                                        // still full: present without timing rather than fail the game
}

// What the application chained to its present. VK_KHR_present_id (one id) and the swapchain-maintenance1 present fence are
// carried along with the held frame and re-attached when it is shown; present id 2, present timing and per-present mode
// structures cannot be re-created for a frame shown later, so those leave the swapchain in passthrough.
enum class AppChain { Plain, Carried, Unsupported };
struct AppChainInfo { AppChain kind{AppChain::Plain}; uint64_t id{0}; VkFence fence{}; VkPresentModeKHR mode{}; bool has_mode{false}; };
AppChainInfo ClassifyPresentChain(const VkPresentInfoKHR* pi) {
    AppChainInfo r;
    for (auto* p = static_cast<const VkBaseInStructure*>(pi->pNext); p; p = p->pNext) {
        switch (int(p->sType)) {
            case VK_STRUCTURE_TYPE_PRESENT_ID_KHR: {
                auto* x = reinterpret_cast<const VkPresentIdKHR*>(p);
                if (x->swapchainCount != 1 || !x->pPresentIds) { r.kind = AppChain::Unsupported; return r; }
                r.id = x->pPresentIds[0]; r.kind = AppChain::Carried; break; }
            case VK_STRUCTURE_TYPE_SWAPCHAIN_PRESENT_FENCE_INFO_KHR: {
                auto* x = reinterpret_cast<const VkSwapchainPresentFenceInfoKHR*>(p);
                if (x->swapchainCount != 1 || !x->pFences) { r.kind = AppChain::Unsupported; return r; }
                r.fence = x->pFences[0]; r.kind = AppChain::Carried; break; }
            case VK_STRUCTURE_TYPE_SWAPCHAIN_PRESENT_MODE_INFO_KHR: {
                auto* x = reinterpret_cast<const VkSwapchainPresentModeInfoKHR*>(p);
                if (x->swapchainCount != 1 || !x->pPresentModes) { r.kind = AppChain::Unsupported; return r; }
                r.mode = x->pPresentModes[0]; r.has_mode = true; r.kind = AppChain::Carried; break; }
            case VK_STRUCTURE_TYPE_PRESENT_ID_2_KHR:
            case VK_STRUCTURE_TYPE_PRESENT_TIMINGS_INFO_EXT:
                r.kind = AppChain::Unsupported; return r;
            default: break;                                                      // other structures are dropped for a held frame
        }
    }
    return r;
}

// The carried part of an application's present chain, as a pNext chain for the layer's own present of that frame.
struct CarriedExt {
    VkPresentIdKHR pid{VK_STRUCTURE_TYPE_PRESENT_ID_KHR};
    VkSwapchainPresentFenceInfoKHR fi{VK_STRUCTURE_TYPE_SWAPCHAIN_PRESENT_FENCE_INFO_KHR};
    VkSwapchainPresentModeInfoKHR mi{VK_STRUCTURE_TYPE_SWAPCHAIN_PRESENT_MODE_INFO_KHR};
    uint64_t id{0}; VkFence fence{}; VkPresentModeKHR mode{}; bool has_mode{false};
    const void* Link() {
        const void* head = nullptr;
        if (has_mode) { mi.swapchainCount = 1; mi.pPresentModes = &mode; mi.pNext = head; head = &mi; }
        if (fence) { fi.swapchainCount = 1; fi.pFences = &fence; fi.pNext = head; head = &fi; }
        if (id)    { pid.swapchainCount = 1; pid.pPresentIds = &id; pid.pNext = head; head = &pid; }
        return head;
    }
};

// Frames the layer holds back keep their application fence registered, so a wait for it releases the frame (see the hooks below).
void HeldFenceAdd(DeviceDispatch& dt, SwapchainState& sc, const HeldFrame& h) {
    if (!h.fence) return;
    std::lock_guard<std::mutex> l(dt.fence_mu);
    dt.held_fences.emplace_back(uint64_t(reinterpret_cast<uintptr_t>(h.fence)), &sc);
}
void HeldFencesDrop(DeviceDispatch& dt, SwapchainState* sc) {      // every held fence that belongs to a swapchain that is being destroyed
    std::lock_guard<std::mutex> l(dt.fence_mu);
    dt.held_fences.erase(std::remove_if(dt.held_fences.begin(), dt.held_fences.end(), [&](auto& e) { return e.second == sc; }), dt.held_fences.end());
}
void HeldFenceRemove(DeviceDispatch& dt, const HeldFrame& h) {
    if (!h.fence) return;
    std::lock_guard<std::mutex> l(dt.fence_mu);
    const uint64_t k = uint64_t(reinterpret_cast<uintptr_t>(h.fence));
    dt.held_fences.erase(std::remove_if(dt.held_fences.begin(), dt.held_fences.end(), [&](auto& e) { return e.first == k; }), dt.held_fences.end());
}

// Presents the real image of a held frame (through the ring when VKIFRAME_DEBUG_RING=1). Does not touch the ring pins.
VkResult PresentFrame(DeviceDispatch& dt, VkQueue queue, SwapchainState& sc, const HeldFrame& h, uint64_t now, double tick_ns = 0.0) {
    GpuState& g = *sc.gpu;
    VkSemaphore wait = VK_NULL_HANDLE;
    if (!GpuPrepareHeldPresent(dt, queue, g, h, GetConfig().debug_ring, wait)) {      // the debug round trip failed: show the real image
        GpuPrepareHeldPresent(dt, queue, g, h, /*via_ring=*/false, wait);
    }
    VkPresentInfoKHR pi{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
    pi.waitSemaphoreCount = 1; pi.pWaitSemaphores = &wait;
    pi.swapchainCount = 1; pi.pSwapchains = &sc.handle; pi.pImageIndices = &h.image_index;
    CarriedExt ext; ext.id = h.present_id; ext.fence = h.fence; ext.mode = h.mode; ext.has_mode = h.has_mode;                   // the frame's own application id and fence
    pi.pNext = ext.Link();
    return PresentWithTiming(dt, queue, sc, pi, now, tick_ns);
}

// Presents the frame being held (if any) and drops its ring reference. Returns VK_SUCCESS when nothing was held.
VkResult PresentHeld(DeviceDispatch& dt, VkQueue queue, SwapchainState& sc, uint64_t now) {
    GpuState& g = *sc.gpu;
    if (!g.held.valid) return VK_SUCCESS;
    const HeldFrame h = g.held;
    g.held.valid = false;
    HeldFenceRemove(dt, h);
    const VkResult r = PresentFrame(dt, queue, sc, h, now);
    GpuUnpin(g, h.slot);
    return r;
}

// Presents one synthetic frame: mix(A, B, t) written into the carrier's image (hidden == false) or a hidden image.
VkResult PresentSynthetic(DeviceDispatch& dt, VkQueue queue, SwapchainState& sc, const HeldFrame& a, const HeldFrame& b, float t, bool hidden, uint64_t now, double tick_ns, bool extrap = false) {
    uint32_t idx = 0; VkSemaphore wait = VK_NULL_HANDLE;
    const VkResult sr = GpuSynthesize(dt, queue, *sc.gpu, a, b, t, hidden, idx, wait, extrap);
    if (sr != VK_SUCCESS) return sr;
    VkPresentInfoKHR pi{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
    pi.waitSemaphoreCount = 1; pi.pWaitSemaphores = &wait;
    pi.swapchainCount = 1; pi.pSwapchains = &sc.handle; pi.pImageIndices = &idx;
    CarriedExt ext; if (!hidden) { ext.id = a.present_id; ext.fence = a.fence; ext.mode = a.mode; ext.has_mode = a.has_mode; }       // the carrier stands in for A: it keeps A's id and fence
    pi.pNext = ext.Link();
    { std::lock_guard<std::mutex> l(sc.mu); ++sc.rates.synthetic; }
    return PresentWithTiming(dt, queue, sc, pi, now, tick_ns);
}
} // namespace

// ---------------------------------------------------------------- presenter thread (Stage 3c-ii)

// A pair: A (held until now) and B (just captured), with the ticks the scheduler planned for it. A's ring slot reference was
// handed over from the held frame; B got one more for the job. Both are released when the job is done.
// extrap (VKIFRAME_EXTRAPOLATE): A is the previous frame (ring copy only) and B the new one, which this job presents as its tick 0; the later ticks are
// predictions from B and are dropped once a newer job exists (gen != SwapchainState::job_gen).
struct PresentJob { HeldFrame a, b; PairScheduler::Plan plan; VkQueue queue{}; bool extrap{false}; uint64_t gen{0}; };

struct Presenter {
    std::thread th;
    std::mutex mu;
    std::condition_variable cv, idle_cv;
    std::deque<PresentJob> q;
    bool busy{false};
    std::atomic<bool> stop{false};
};

namespace {
void NotePresentError(SwapchainState& sc, VkResult r) {
    if (r < 0) { int zero = 0; sc.pending_error.compare_exchange_strong(zero, int(r)); }
}

// Emits the planned ticks of a pair; A's real image is the carrier of the first one. Synthetic failures switch synthetic frames
// off, never the game. Runs on the presenter thread, or on the game thread with VKIFRAME_PRESENTER=0.
void ExecutePair(DeviceDispatch& dt, SwapchainState& sc, const PresentJob& j, Presenter* pr) {
    const Config& cfg = GetConfig();
    GpuState& g = *sc.gpu;
    const PairScheduler::Plan& plan = j.plan;
    const bool timed = pr != nullptr;                              // sleep until each tick only with the presenter thread
    auto wait_tick = [&](int i, const PlannedTick& tk) {
        if (!timed && !(cfg.pace_sleep && i > 0)) return;
        const double due = tk.time_ns - double(cfg.pace_lead_us) * 1e3, now = double(NowNs());
        if (due > now && due - now < 100e6) SleepUntilNs(uint64_t(due));    // a tick far in the future is a scheduler hiccup: do not stall for it
    };
    const bool synth = g.synth_ready && !sc.synth_failed && cfg.synth;
    bool carrier_done = false;
    if (synth && cfg.flow && !plan.carrier_only && plan.n > 0 && g.flow.ready && !g.flow.failed) GpuFlow(dt, j.queue, g, j.a, j.b);
    auto stopped = [&] { return pr && pr->stop.load(); };
    if (!plan.carrier_only) {
        for (int i = 0; i < plan.n && !stopped(); ++i) {
            const PlannedTick& tk = plan.ticks[i];
            wait_tick(i, tk);
            if (stopped()) break;
            const uint64_t now = NowNs();
            if (i == 0) {
                // the flow debug view replaces every frame, native ones included
                if ((tk.native_a && !(cfg.debug_flow && g.flow.pair_valid)) || !synth) { NotePresentError(sc, PresentFrame(dt, j.queue, sc, j.a, now, tk.time_ns)); carrier_done = true; continue; }
                const VkResult r = PresentSynthetic(dt, j.queue, sc, j.a, j.b, float(tk.t), /*hidden=*/false, now, tk.time_ns);
                if (r == VK_SUCCESS || r == VK_SUBOPTIMAL_KHR) { carrier_done = true; continue; }
                FG_LOG("synth: first synthetic frame failed (%d), synthetic frames are off for this swapchain", int(r));
                sc.synth_failed = true; NotePresentError(sc, PresentFrame(dt, j.queue, sc, j.a, now, tk.time_ns)); carrier_done = true; continue;
            }
            if (!synth || sc.synth_failed) break;
            const VkResult r = PresentSynthetic(dt, j.queue, sc, j.a, j.b, float(tk.t), /*hidden=*/true, now, tk.time_ns);
            if (r == VK_SUCCESS || r == VK_SUBOPTIMAL_KHR) continue;
            if (r == VK_ERROR_OUT_OF_DATE_KHR || r == VK_ERROR_SURFACE_LOST_KHR) { NotePresentError(sc, r); break; }
            FG_LOG("synth: synthetic frame failed (%d), synthetic frames are off for this swapchain", int(r));
            sc.synth_failed = true; break;
        }
    }
    if (!carrier_done && !stopped()) {                                           // carrier-only, or no tick at all
        PlannedTick tk{plan.carrier_time_ns, 0.0, true};
        if (plan.carrier_only) wait_tick(0, tk);
        NotePresentError(sc, PresentFrame(dt, j.queue, sc, j.a, NowNs(), plan.carrier_only ? plan.carrier_time_ns : 0.0));
    }
    GpuUnpin(g, j.a.slot); GpuUnpin(g, j.b.slot);
}

// Extrapolation job. Tick 0 is the native frame B (never skipped: the application waits on it); the other ticks are frames predicted from B,
// presented at their slots unless a newer frame has arrived (then they are stale and the new job takes over the clock).
void ExecuteExtrap(DeviceDispatch& dt, SwapchainState& sc, const PresentJob& j, Presenter* pr) {
    const Config& cfg = GetConfig();
    GpuState& g = *sc.gpu;
    const PairScheduler::Plan& plan = j.plan;
    auto stopped   = [&] { return pr && pr->stop.load(); };
    auto cancelled = [&] { return sc.job_gen.load() != j.gen; };
    // sleep until the tick is due; one wait on the presenter's condition variable, woken early by a cancel or a stop (no polling)
    auto wait_tick = [&](const PlannedTick& tk, bool cancellable) {
        if (!pr) return;
        const double due = tk.time_ns - double(cfg.pace_lead_us) * 1e3, now = double(NowNs());
        if (due <= now || due - now > 100e6) return;
        std::unique_lock<std::mutex> l(pr->mu);
        pr->cv.wait_until(l, std::chrono::steady_clock::time_point(std::chrono::nanoseconds((long long)due)), [&] { return stopped() || (cancellable && cancelled()); });
    };
    wait_tick(plan.ticks[0], false);
    NotePresentError(sc, PresentFrame(dt, j.queue, sc, j.b, NowNs(), plan.ticks[0].time_ns));
    {   // how late the native frame went out against its planned slot (the presenter is one serial queue: it may have been busy with the job before)
        const double late = std::max(0.0, double(NowNs()) - plan.ticks[0].time_ns);
        sc.ex_late_sum_ns += uint64_t(late); ++sc.ex_late_n;
        uint64_t m = sc.ex_late_max_ns.load(); while (uint64_t(late) > m && !sc.ex_late_max_ns.compare_exchange_weak(m, uint64_t(late))) {}
    }
    const bool synth = plan.n > 1 && g.synth_ready && !sc.synth_failed && cfg.synth && g.flow.ready && !g.flow.failed && !stopped() && !cancelled();
    if (synth) GpuFlow(dt, j.queue, g, j.a, j.b);
    int shown = 0;
    for (int i = 1; synth && i < plan.n; ++i) {
        wait_tick(plan.ticks[i], true);
        if (stopped() || cancelled()) break;
        const VkResult r = PresentSynthetic(dt, j.queue, sc, j.a, j.b, float(plan.ticks[i].t), /*hidden=*/true, NowNs(), plan.ticks[i].time_ns, /*extrap=*/true);
        if (r == VK_SUCCESS || r == VK_SUBOPTIMAL_KHR) { ++shown; continue; }
        if (r == VK_ERROR_FEATURE_NOT_PRESENT) break;                           // no flow for this pair: the native frame stays
        if (r == VK_ERROR_OUT_OF_DATE_KHR || r == VK_ERROR_SURFACE_LOST_KHR) { NotePresentError(sc, r); break; }
        FG_LOG("extrapolate: predicted frame failed (%d), synthetic frames are off for this swapchain", int(r));
        sc.synth_failed = true; break;
    }
    sc.ex_planned += uint64_t(plan.n - 1); sc.ex_cancelled += uint64_t(plan.n - 1 - shown);   // each one not shown leaves its output slot to repeat the last frame
    GpuUnpin(g, j.a.slot); GpuUnpin(g, j.b.slot);
}

void PresenterMain(DeviceDispatch* dt, SwapchainState* sc, Presenter* p) {
    uint64_t wd_logged = 0;
    for (;;) {
        PresentJob j;
        {
            std::unique_lock<std::mutex> l(p->mu);
            while (!p->cv.wait_for(l, std::chrono::milliseconds(50), [&] { return p->stop.load() || !p->q.empty(); })) {
                const uint64_t now = NowNs(), last = sc->last_present_ns.load();
                if (last && now - last > 200'000'000ull && sc->gpu && sc->last_queue) {   // idle flush: the game stopped presenting, show its last frame
                    l.unlock();
                    {
                        std::unique_lock<std::mutex> fl(sc->flow_mu, std::try_to_lock);
                        if (fl.owns_lock() && sc->gpu->held.valid && NowNs() - sc->last_present_ns.load() > 200'000'000ull) {
                            FG_LOG_ROUTINE("hold: no present for %.0f ms, showing the held frame", (NowNs() - last) / 1e6);
                            NotePresentError(*sc, PresentHeld(*dt, sc->last_queue, *sc, NowNs()));
                        }
                    }
                    l.lock();
                    if (p->stop.load() || !p->q.empty()) break;
                }
                if (!(last && now - last > 2'000'000'000ull)) continue;                      // watchdog: the game stopped presenting
                if (last && now - last > 2'000'000'000ull && now - wd_logged > 5'000'000'000ull) {
                    wd_logged = now;
                    const uint64_t aw = sc->acquire_wait_since_ns.load(), pw = sc->wait_present_since_ns.load();
                    FG_LOG_ROUTINE("watchdog: no present for %.1f s | frames %llu | held %d (id %llu fence %d) | in acquire %.1f s | in present-wait %.1f s (id %llu)",
                           (now - last) / 1e9, (unsigned long long)sc->frames, sc->gpu ? int(sc->gpu->held.valid) : -1,
                           (unsigned long long)(sc->gpu ? sc->gpu->held.present_id : 0), sc->gpu ? int(sc->gpu->held.fence != VK_NULL_HANDLE) : 0,
                           aw ? (now - aw) / 1e9 : 0.0, pw ? (now - pw) / 1e9 : 0.0, (unsigned long long)sc->wait_present_id.load());
                }
            }
            if (p->stop.load()) break;
            j = p->q.front(); p->q.pop_front(); p->busy = true;
        }
        const uint64_t cpu0 = ThreadCpuNs();
        if (j.extrap) ExecuteExtrap(*dt, *sc, j, p); else ExecutePair(*dt, *sc, j, p);
        sc->cpu_presenter_ns += ThreadCpuNs() - cpu0;
        { std::lock_guard<std::mutex> l(p->mu); p->busy = false; }
        p->idle_cv.notify_all();
    }
}

void StartPresenter(DeviceDispatch& dt, SwapchainState& sc) {
    auto* p = new Presenter;
    sc.presenter = p;
    p->th = std::thread(PresenterMain, &dt, &sc, p);
    FG_LOG("presenter: thread started (lead %d us)", GetConfig().pace_lead_us);
}

bool StopPresenter(SwapchainState& sc) {    // true: the thread was detached and still uses `sc`, its GPU state and the device dispatch table
    Presenter* p = sc.presenter;
    if (!p) return false;
    { std::lock_guard<std::mutex> l(p->mu); p->stop = true; p->q.clear(); }
    p->cv.notify_all();
    if (p->th.joinable()) {
        // a presenter that is stuck inside a driver call must not hang the game's exit: wait a few seconds, then leave the thread behind
        std::unique_lock<std::mutex> l(p->mu);
        if (!p->idle_cv.wait_for(l, std::chrono::seconds(3), [&] { return !p->busy; })) {
            l.unlock();
            FG_LOG("shutdown: the presenter thread is still busy after 3 s, detaching it");
            p->th.detach();
            sc.presenter = nullptr;
            return true;                                                      // the Presenter object is leaked on purpose: the thread still uses it
        }
        l.unlock();
        p->th.join();
    }
    sc.presenter = nullptr;
    delete p;
    return false;
}

// Waits until every queued tick has been presented. Needed before the game's own present goes out, to keep the order.
void DrainPresenter(SwapchainState& sc) {
    Presenter* p = sc.presenter;
    if (!p) return;
    std::unique_lock<std::mutex> l(p->mu);
    if (!p->idle_cv.wait_for(l, std::chrono::seconds(2), [&] { return p->q.empty() && !p->busy; })) FG_LOG("presenter: drain timed out");
}
} // namespace

VKAPI_ATTR VkResult VKAPI_CALL Hook_QueuePresentKHR(VkQueue queue, const VkPresentInfoKHR* pPresentInfo) {
    DeviceDispatch* dt = LookupDevice(GetKey(queue));          // a queue shares its device's key
    if (!dt) return VK_ERROR_DEVICE_LOST;
    const Config& cfg = GetConfig();

    SwapchainState* sc = pPresentInfo->swapchainCount == 1 ? FindSwapchain(pPresentInfo->pSwapchains[0]) : nullptr;
    if (!sc) {                                                  // several swapchains in one present: pass through untouched
        std::mutex* qm = QueueMutex(queue);
        std::unique_lock<std::mutex> lk;
        if (qm) lk = std::unique_lock<std::mutex>(*qm);
        return dt->QueuePresentKHR(queue, pPresentInfo);
    }

    struct PresentTimer { SwapchainState* s; uint64_t t0, c0; ~PresentTimer() { s->wait_present_ns += NowNs() - t0; s->cpu_hook_ns += ThreadCpuNs() - c0; } } present_timer{sc, NowNs(), ThreadCpuNs()};     // everything below is the game thread inside our present

    // Optional base-rate cap: space the game's presents 1/cap apart (it makes Game Mode testable). By default the wait happens AFTER the present, so the
    // game starts its next frame as late as it can and still presents on the grid: it samples input just before rendering and its frame is not stale.
    // (VKIFRAME_CAP_EARLY=1: wait before the present, which makes the intervals exact but adds the whole wait to the latency of every frame.)
    uint64_t cap_slot_ns = 0;
    LivePoll(NowNs());                                          // live.conf: one stat() a second
    const float cap_fps = (Live().enabled.load() || StatsBenchActive()) ? Live().base_fps_cap.load() : 0.0f;   // a benchmark keeps the cap in its "off" seconds, so both states pace the game the same way   // disabled = pure passthrough, no cap either
    if (cap_fps > 0.0f) {
        cap_slot_ns = uint64_t(1e9 / cap_fps);
        const uint64_t now0 = NowNs();
        if (cfg.cap_early) {
            if (sc->cap_next_slot_ns > now0) SleepUntilNs(sc->cap_next_slot_ns);
            sc->cap_next_slot_ns = std::max(sc->cap_next_slot_ns + cap_slot_ns, now0);   // never accumulates debt after a stall
        } else {
            if (sc->cap_return_ns) {                                                       // how long the game took from our return to this present
                const double render = double(now0 - sc->cap_return_ns);
                if (render < double(cap_slot_ns)) sc->cap_render_ema_ns = sc->cap_render_ema_ns > 0.0 ? 0.9 * sc->cap_render_ema_ns + 0.1 * render : render;
            }
            if (sc->cap_next_slot_ns == 0 || now0 > sc->cap_next_slot_ns + cap_slot_ns) sc->cap_next_slot_ns = now0;      // first frame, or after a stall: no debt
            sc->cap_this_slot_ns = sc->cap_next_slot_ns;
            sc->cap_next_slot_ns += cap_slot_ns;
        }
    }

    const uint64_t now = NowNs();
    if (StatsOn()) { if (sc->stat_prev_ns) sc->stat_ft.Add(double(now - sc->stat_prev_ns) / 1e6); sc->stat_prev_ns = now; }   // present-to-present interval of the game
    else sc->stat_prev_ns = 0;
    PairScheduler::Plan plan;
    {
        std::lock_guard<std::mutex> l(sc->mu);
        sc->timing.Update(now);
        ++sc->frames;
        plan = PlanAndCount(*sc, now);
    }

    // ---- frame holding (Stage 3b) ----
    std::unique_lock<std::mutex> flow(sc->flow_mu);
    const AppChainInfo app = ClassifyPresentChain(pPresentInfo);
    const AppChain chain_kind = app.kind;
    sc->last_queue = queue;
    sc->last_present_ns = now;
    if (sc->frames == 1) {                                                              // what the game chains to its presents (once)
        std::string types;
        for (auto* p = static_cast<const VkBaseInStructure*>(pPresentInfo->pNext); p; p = p->pNext) types += " " + std::to_string(int(p->sType));
        FG_LOG("present chain of the first present: [%s ] (%s)", types.c_str(), chain_kind == AppChain::Unsupported ? "unsupported" : chain_kind == AppChain::Carried ? "carried with held frames" : "plain");
    }
    bool holding_possible = sc->hold_candidate && !sc->hold_failed && chain_kind != AppChain::Unsupported;
    if (sc->hold_candidate && chain_kind == AppChain::Unsupported && !sc->skip_logged) {
        sc->skip_logged = true;
        FG_LOG("hold: off, the application's present chain uses present id 2 / present timing (or a malformed id/fence/mode struct), which cannot be carried to a later present");
    }
    if (holding_possible && !sc->gpu) {
        const uint32_t family = QueueFamilyOf(queue);
        VkQueueFamilyProperties qf[16]{}; uint32_t nq = 16;
        dt->instance->GetPhysicalDeviceQueueFamilyProperties(dt->physical_device, &nq, qf);
        if (family == UINT32_MAX || family >= nq || !(qf[family].queueFlags & VK_QUEUE_GRAPHICS_BIT)) {
            FG_LOG("hold: present queue family %u is unknown or cannot blit, frame holding is off", family);
            sc->hold_failed = true;
        } else if (!(sc->gpu = GpuInit(*dt, *sc, family))) {
            sc->hold_failed = true;
        } else if (cfg.presenter && sc->gpu->synth_ready && cfg.synth) {
            StartPresenter(*dt, *sc);                          // only worth a thread when synthetic frames need pacing
        }
        holding_possible = !sc->hold_failed;
    }

    VkResult r = VK_SUCCESS;
    auto plain_present = [&] {                                  // the game's own present, after everything queued has gone out
        DrainPresenter(*sc);
        return PresentWithTiming(*dt, queue, *sc, *pPresentInfo, now);
    };
    if (!holding_possible || !sc->gpu) {
        if (sc->gpu && sc->gpu->held.valid) {                    // holding just failed or became impossible: let the held frame go first
            DrainPresenter(*sc);
            (void)PresentHeld(*dt, queue, *sc, now);
        }
        r = plain_present();
    } else if (plan.kind == PairScheduler::Kind::Bypass) {
        if (cfg.extrapolate) { ++sc->job_gen; if (sc->presenter) { { std::lock_guard<std::mutex> l(sc->presenter->mu); } sc->presenter->cv.notify_all(); } }   // predicted frames still queued are stale now
        DrainPresenter(*sc);
        if (sc->gpu->last.valid) { GpuUnpin(*sc->gpu, sc->gpu->last.slot); sc->gpu->last.valid = false; }
        VkResult rf = plan.flush_held || sc->gpu->held.valid ? PresentHeld(*dt, queue, *sc, now) : VK_SUCCESS;
        r = plain_present();                                                            // the game's own present, untouched
        if (rf < 0) r = rf;
    } else {                                                                            // Hold or Pair: capture the new frame, show the old one
        HeldFrame b;
        const bool captured = GpuCapture(*dt, queue, *sc->gpu, *pPresentInfo, b);
        b.present_id = app.id; b.fence = app.fence; b.mode = app.mode; b.has_mode = app.has_mode;                                     // shown later, with the application's own id and fence
        if (!captured) {
            FG_LOG("hold: capture failed, back to passthrough for this swapchain");
            sc->hold_failed = true;
            DrainPresenter(*sc);
            if (sc->gpu->held.valid) (void)PresentHeld(*dt, queue, *sc, now);
            r = plain_present();
        } else if (cfg.extrapolate) {
            // nothing is held back: B goes out as tick 0 of its job, the earlier frame is only a ring copy for the motion estimate
            GpuState& g = *sc->gpu;
            const bool have_prev = g.last.valid;
            HeldFrame a = have_prev ? g.last : b;                                      // the previous frame's reference passes to the job
            if (!have_prev) GpuPin(g, b.slot);                                         // a == b: the job drops both references
            GpuPin(g, b.slot);                                                         // the reference of g.last; the capture's own goes to the job
            g.last = b;
            if (!have_prev) plan.n = std::min(plan.n, 1);
            PresentJob job{a, b, plan, queue, true, ++sc->job_gen};
            if (sc->presenter) {
                { std::lock_guard<std::mutex> l(sc->presenter->mu); sc->presenter->q.push_back(job); }
                sc->presenter->cv.notify_all();
            } else {
                ExecuteExtrap(*dt, *sc, job, nullptr);                                  // synchronous (VKIFRAME_PRESENTER=0)
            }
        } else if (plan.kind == PairScheduler::Kind::Pair && sc->gpu->held.valid) {
            PresentJob job{sc->gpu->held, b, plan, queue};
            HeldFenceRemove(*dt, sc->gpu->held);
            sc->gpu->held.valid = false;                                                // A's reference now belongs to the job
            GpuPin(*sc->gpu, b.slot);                                                   // B stays held for the next pair and is used by this one
            sc->gpu->held = b; HeldFenceAdd(*dt, *sc, b);
            if (sc->presenter) {
                { std::lock_guard<std::mutex> l(sc->presenter->mu); sc->presenter->q.push_back(job); }
                sc->presenter->cv.notify_all();
            } else {
                ExecutePair(*dt, *sc, job, nullptr);                                    // synchronous (VKIFRAME_PRESENTER=0)
            }
        } else {
            r = PresentHeld(*dt, queue, *sc, now);                                       // Hold: nothing to present yet
            sc->gpu->held = b; HeldFenceAdd(*dt, *sc, b);                                // frame B waits for the next call
        }
    }
    flow.unlock();
    if (const int e = sc->pending_error.exchange(0); e < 0 && r >= 0) r = VkResult(e);    // an error of a presenter-thread present
    if (pPresentInfo->pResults) pPresentInfo->pResults[0] = r;

    {
        std::lock_guard<std::mutex> l(sc->mu);
        if (sc->timing_enabled) DrainPastTimings(*dt, *sc);
        LogTimingStats(*dt, *sc, NowNs());
        LogRates(*sc, now);
    }
    if (sc->frames % 300 == 0) FG_LOG("vkQueuePresentKHR (frame %llu)", (unsigned long long)sc->frames);
    if (cap_slot_ns && !cfg.cap_early) {                        // hold the game back until it should start the frame that will be presented on the next slot
        const double lead = 1.2 * sc->cap_render_ema_ns;
        if (double(sc->cap_next_slot_ns) > lead) { const uint64_t until = uint64_t(double(sc->cap_next_slot_ns) - lead); if (until > NowNs()) SleepUntilNs(until); }
        sc->cap_return_ns = NowNs();
    }
    return r;
}

// ---------------------------------------------------------------- queue locking (Stage 3c-ii)
// The presenter thread submits and presents on the game's queue. Vulkan requires external synchronization per queue, so every
// queue operation of the game takes the same mutex as ours. Image acquires are polled so that no lock is held while waiting.

namespace {
struct QueueLock {
    std::mutex* m;
    explicit QueueLock(VkQueue q) : m(QueueMutex(q)) { if (m) m->lock(); }
    ~QueueLock() { if (m) m->unlock(); }
};

template <class F> VkResult PolledAcquire(uint64_t timeout, F&& once) {
    const uint64_t start = NowNs();
    for (;;) {
        const VkResult r = once();
        if (r != VK_NOT_READY && r != VK_TIMEOUT) return r;
        if (timeout == 0) return VK_NOT_READY;
        if (timeout != UINT64_MAX && NowNs() - start >= timeout) return VK_TIMEOUT;
        timespec nap{0, 100'000}; nanosleep(&nap, nullptr);
    }
}
} // namespace

VKAPI_ATTR VkResult VKAPI_CALL Hook_QueueSubmit(VkQueue q, uint32_t n, const VkSubmitInfo* s, VkFence f) {
    DeviceDispatch* dt = LookupDevice(GetKey(q)); if (!dt) return VK_ERROR_DEVICE_LOST; QueueLock l(q); return dt->QueueSubmit(q, n, s, f);
}
VKAPI_ATTR VkResult VKAPI_CALL Hook_QueueSubmit2(VkQueue q, uint32_t n, const VkSubmitInfo2* s, VkFence f) {
    DeviceDispatch* dt = LookupDevice(GetKey(q)); if (!dt) return VK_ERROR_DEVICE_LOST; QueueLock l(q);
    const auto submit2 = dt->QueueSubmit2 ? dt->QueueSubmit2 : dt->QueueSubmit2KHR;      // the same fallback as Hook_QueueSubmit2KHR, the other way round
    return submit2 ? submit2(q, n, s, f) : VK_ERROR_FEATURE_NOT_PRESENT;
}
VKAPI_ATTR VkResult VKAPI_CALL Hook_QueueSubmit2KHR(VkQueue q, uint32_t n, const VkSubmitInfo2* s, VkFence f) {
    DeviceDispatch* dt = LookupDevice(GetKey(q)); if (!dt) return VK_ERROR_DEVICE_LOST; QueueLock l(q);
    const auto submit2 = dt->QueueSubmit2KHR ? dt->QueueSubmit2KHR : dt->QueueSubmit2;
    return submit2 ? submit2(q, n, s, f) : VK_ERROR_FEATURE_NOT_PRESENT;
}
VKAPI_ATTR VkResult VKAPI_CALL Hook_QueueBindSparse(VkQueue q, uint32_t n, const VkBindSparseInfo* s, VkFence f) {
    DeviceDispatch* dt = LookupDevice(GetKey(q)); if (!dt) return VK_ERROR_DEVICE_LOST; QueueLock l(q); return dt->QueueBindSparse(q, n, s, f);
}
VKAPI_ATTR VkResult VKAPI_CALL Hook_QueueWaitIdle(VkQueue q) {
    DeviceDispatch* dt = LookupDevice(GetKey(q));
    if (!dt) return VK_ERROR_DEVICE_LOST;
    const uint64_t t0 = NowNs();
    QueueLock l(q);
    const uint64_t t1 = NowNs();
    const VkResult r = dt->QueueWaitIdle(q);
    const uint64_t t2 = NowNs();
    if (t2 - t0 > 500'000'000ull) FG_LOG("shutdown: vkQueueWaitIdle took %.0f ms (queue lock %.0f ms, wait %.0f ms)", (t2 - t0) / 1e6, (t1 - t0) / 1e6, (t2 - t1) / 1e6);
    return r;
}
VKAPI_ATTR VkResult VKAPI_CALL Hook_DeviceWaitIdle(VkDevice device) {
    DeviceDispatch* dt = LookupDevice(GetKey(device));
    if (!dt) return VK_ERROR_DEVICE_LOST;
    const uint64_t t0 = NowNs();
    std::vector<std::unique_lock<std::mutex>> locks;
    for (std::mutex* m : DeviceQueueMutexes(GetKey(device))) locks.emplace_back(*m);        // all queues, in a fixed order
    const uint64_t t1 = NowNs();
    const VkResult r = dt->DeviceWaitIdle(device);
    const uint64_t t2 = NowNs();
    if (t2 - t0 > 500'000'000ull) FG_LOG("shutdown: vkDeviceWaitIdle took %.0f ms (queue locks %.0f ms, wait %.0f ms)", (t2 - t0) / 1e6, (t1 - t0) / 1e6, (t2 - t1) / 1e6);
    return r;
}
VKAPI_ATTR VkResult VKAPI_CALL Hook_AcquireNextImageKHR(VkDevice device, VkSwapchainKHR swapchain, uint64_t timeout,
                                                        VkSemaphore sem, VkFence fence, uint32_t* pIndex) {
    DeviceDispatch* dt = LookupDevice(GetKey(device));
    if (!dt) return VK_ERROR_DEVICE_LOST;
    SwapchainState* sc = FindSwapchain(swapchain);
    if (!sc || !sc->gpu || !sc->presenter) return dt->AcquireNextImageKHR(device, swapchain, timeout, sem, fence, pIndex);
    GpuState* g = sc->gpu;
    struct Mark { SwapchainState* s; uint64_t t0; explicit Mark(SwapchainState* x) : s(x), t0(NowNs()) { s->acquire_wait_since_ns = t0; } ~Mark() { s->acquire_wait_since_ns = 0; s->wait_acquire_ns += NowNs() - t0; } } mark(sc);
    return PolledAcquire(timeout, [&] {
        std::lock_guard<std::mutex> l(g->acquire_mu);           // the presenter acquires hidden images on the same swapchain
        return dt->AcquireNextImageKHR(device, swapchain, 0, sem, fence, pIndex);
    });
}
VKAPI_ATTR VkResult VKAPI_CALL Hook_AcquireNextImage2KHR(VkDevice device, const VkAcquireNextImageInfoKHR* info, uint32_t* pIndex) {
    DeviceDispatch* dt = LookupDevice(GetKey(device));
    if (!dt) return VK_ERROR_DEVICE_LOST;
    SwapchainState* sc = FindSwapchain(info->swapchain);
    if (!sc || !sc->gpu || !sc->presenter) return dt->AcquireNextImage2KHR(device, info, pIndex);
    GpuState* g = sc->gpu;
    struct Mark { SwapchainState* s; uint64_t t0; explicit Mark(SwapchainState* x) : s(x), t0(NowNs()) { s->acquire_wait_since_ns = t0; } ~Mark() { s->acquire_wait_since_ns = 0; s->wait_acquire_ns += NowNs() - t0; } } mark(sc);
    VkAcquireNextImageInfoKHR once = *info; once.timeout = 0;
    return PolledAcquire(info->timeout, [&] {
        std::lock_guard<std::mutex> l(g->acquire_mu);
        return dt->AcquireNextImage2KHR(device, &once, pIndex);
    });
}

// Releases the held frame of a swapchain when the game waits for something that only that frame's presentation can complete:
// its present id (vkWaitForPresentKHR) or its present fence (vkWaitForFences / vkGetFenceStatus). The ticks of the pair after it
// are skipped; the next pair starts fresh.
namespace {
VkResult FlushHeld(DeviceDispatch& dt, SwapchainState& sc, uint64_t present_id, VkFence fence, const char* why) {
    std::lock_guard<std::mutex> fl(sc.flow_mu);
    if (!sc.gpu || !sc.last_queue) return VK_SUCCESS;
    GpuState& g = *sc.gpu;
    DrainPresenter(sc);
    const bool hit = g.held.valid && ((fence && g.held.fence == fence) || (present_id && g.held.present_id != 0 && g.held.present_id <= present_id));
    if (!hit) return VK_SUCCESS;
    static std::atomic<int> logged{0};
    if (logged++ < 5) FG_LOG("hold: the game waits for the held frame (%s), presenting it now", why);
    return PresentHeld(dt, sc.last_queue, sc, NowNs());
}

// Swapchains with a held frame whose application fence is among fences[]
void FlushForFences(DeviceDispatch& dt, uint32_t n, const VkFence* fences, const char* why) {
    std::vector<SwapchainState*> hit;
    {
        std::lock_guard<std::mutex> l(dt.fence_mu);
        if (dt.held_fences.empty()) return;
        for (uint32_t i = 0; i < n; ++i)
            for (auto& e : dt.held_fences)
                if (e.first == uint64_t(reinterpret_cast<uintptr_t>(fences[i]))) hit.push_back(e.second);
    }
    for (uint32_t i = 0; i < n; ++i)
        for (SwapchainState* sc : hit) (void)FlushHeld(dt, *sc, 0, fences[i], why);
}
} // namespace

VKAPI_ATTR VkResult VKAPI_CALL Hook_WaitForFences(VkDevice device, uint32_t n, const VkFence* fences, VkBool32 all, uint64_t timeout) {
    DeviceDispatch* dt = LookupDevice(GetKey(device));
    if (!dt) return VK_ERROR_DEVICE_LOST;
    FlushForFences(*dt, n, fences, "vkWaitForFences");
    return dt->WaitForFences(device, n, fences, all, timeout);
}
VKAPI_ATTR VkResult VKAPI_CALL Hook_GetFenceStatus(VkDevice device, VkFence fence) {
    DeviceDispatch* dt = LookupDevice(GetKey(device));
    if (!dt) return VK_ERROR_DEVICE_LOST;
    FlushForFences(*dt, 1, &fence, "vkGetFenceStatus");
    return dt->GetFenceStatus(device, fence);
}

// vkWaitForPresentKHR for the id of the frame we are holding would wait for a present that only the game's next present
// triggers (DXVK waits for every present it makes). The frame is not lost, only shown one frame late, so the wait is answered
// at once and the game goes on to render the next frame. Waits for older ids are real: those frames are on screen or queued
// on the presenter thread. A game that then stops presenting gets its held frame from the idle flush in the presenter thread.
VKAPI_ATTR VkResult VKAPI_CALL Hook_WaitForPresentKHR(VkDevice device, VkSwapchainKHR swapchain, uint64_t presentId, uint64_t timeout) {
    DeviceDispatch* dt = LookupDevice(GetKey(device));
    if (!dt) return VK_ERROR_DEVICE_LOST;
    SwapchainState* sc = FindSwapchain(swapchain);
    if (!sc) return dt->WaitForPresentKHR(device, swapchain, presentId, timeout);
    {
        std::lock_guard<std::mutex> fl(sc->flow_mu);
        if (sc->gpu && sc->gpu->held.valid && sc->gpu->held.present_id != 0 && presentId >= sc->gpu->held.present_id) {
            static std::atomic<int> logged{0};
            if (logged++ < 3) FG_LOG("hold: vkWaitForPresentKHR(%llu) is for the held frame, answered early (it is shown with the next present)", (unsigned long long)presentId);
            return VK_SUCCESS;
        }
    }
    sc->wait_present_id = presentId; sc->wait_present_since_ns = NowNs();
    struct Done { SwapchainState* s; ~Done() { s->wait_present_since_ns = 0; } } done{sc};
    return dt->WaitForPresentKHR(device, swapchain, presentId, timeout);
}
