[index](../STAGE2.md) | [previous](01-overview-objectives.md) | [next](03-presenting-with-timing-information.md)

### A. Device creation: enable what the driver supports

The layer cannot assume the extensions exist. It checks the physical device, queries the feature bits, and enables only what is supported. Structures the application already put in the `pNext` chain are never duplicated.

```cpp
static bool DeviceHasExtension(InstanceDispatch& inst, VkPhysicalDevice pd, const char* name) {
    uint32_t n = 0;
    inst.EnumerateDeviceExtensionProperties(pd, nullptr, &n, nullptr);
    std::vector<VkExtensionProperties> props(n);
    inst.EnumerateDeviceExtensionProperties(pd, nullptr, &n, props.data());
    return std::any_of(props.begin(), props.end(), [&](auto& p) { return !strcmp(p.extensionName, name); });
}

static bool ChainHas(const void* pNext, VkStructureType type) {
    for (auto* s = static_cast<const VkBaseInStructure*>(pNext); s; s = s->pNext)
        if (s->sType == type) return true;
    return false;
}

VKAPI_ATTR VkResult VKAPI_CALL Hook_CreateDevice(VkPhysicalDevice pd, const VkDeviceCreateInfo* pCreateInfo,
                                                 const VkAllocationCallbacks* pAllocator, VkDevice* pDevice) {
    InstanceDispatch* inst = LookupInstance(GetKey(pd));
    auto* link = FindDeviceLink(pCreateInfo);
    PFN_vkGetDeviceProcAddr gdpa = link->u.pLayerInfo->pfnNextGetDeviceProcAddr;
    link->u.pLayerInfo = link->u.pLayerInfo->pNext;

    VkDeviceCreateInfo ci = *pCreateInfo;
    std::vector<const char*> exts(ci.ppEnabledExtensionNames, ci.ppEnabledExtensionNames + ci.enabledExtensionCount);

    // Query feature support for both extensions.
    VkPhysicalDevicePresentTimingFeaturesEXT supTiming{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRESENT_TIMING_FEATURES_EXT};
    VkPhysicalDevicePresentId2FeaturesKHR    supId2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRESENT_ID_2_FEATURES_KHR};
    supTiming.pNext = &supId2;
    VkPhysicalDeviceFeatures2 q{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, &supTiming};
    const bool have_exts = DeviceHasExtension(*inst, pd, VK_EXT_PRESENT_TIMING_EXTENSION_NAME) &&
                           DeviceHasExtension(*inst, pd, VK_KHR_PRESENT_ID_2_EXTENSION_NAME);
    if (have_exts) inst->GetPhysicalDeviceFeatures2(pd, &q);
    const bool timing_ok = have_exts && supTiming.presentTiming && supTiming.presentAtAbsoluteTime && supId2.presentId2
                           && !ChainHas(ci.pNext, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRESENT_TIMING_FEATURES_EXT);

    VkPhysicalDevicePresentTimingFeaturesEXT enTiming = supTiming;   // enable exactly what is supported
    VkPhysicalDevicePresentId2FeaturesKHR    enId2    = supId2;
    if (timing_ok) {
        enId2.pNext    = const_cast<void*>(ci.pNext);                // prepend to the app's chain
        enTiming.pNext = &enId2;
        ci.pNext       = &enTiming;
        for (const char* e : {VK_EXT_PRESENT_TIMING_EXTENSION_NAME, VK_KHR_PRESENT_ID_2_EXTENSION_NAME})
            if (std::none_of(exts.begin(), exts.end(), [&](auto* x) { return !strcmp(x, e); })) exts.push_back(e);
        ci.enabledExtensionCount   = uint32_t(exts.size());
        ci.ppEnabledExtensionNames = exts.data();
    }

    VkResult r = inst->CreateDevice(pd, &ci, pAllocator, pDevice);
    if (r != VK_SUCCESS) return r;

    DeviceDispatch* dt = RegisterDevice(GetKey(*pDevice), *pDevice, pd, gdpa, inst);
    dt->timing_supported = timing_ok;                               // new bool member of DeviceDispatch
    FG_LOG("present timing %s", timing_ok ? "ENABLED" : "unavailable (vblank-paced fallback)");
    return VK_SUCCESS;
}
```

(If the application itself already requests present timing, the layer leaves its chain alone and treats timing as unavailable; Stage 6 hides the extension from applications to avoid that case.)

### B. Swapchain creation

```cpp
VKAPI_ATTR VkResult VKAPI_CALL Hook_CreateSwapchainKHR(VkDevice device, const VkSwapchainCreateInfoKHR* pCreateInfo,
                                                       const VkAllocationCallbacks* pAllocator, VkSwapchainKHR* pSwapchain) {
    DeviceDispatch* dt = LookupDevice(GetKey(device));
    VkSwapchainCreateInfoKHR ci = *pCreateInfo;

    // ... minImageCount handling from Stage 1 ...

    if (dt->timing_supported) {
        ci.flags |= VK_SWAPCHAIN_CREATE_PRESENT_TIMING_BIT_EXT | VK_SWAPCHAIN_CREATE_PRESENT_ID_2_BIT_KHR;
        if (ci.presentMode != VK_PRESENT_MODE_FIFO_KHR) {
            FG_LOG("forcing FIFO present mode (timing needs FIFO)");
            ci.presentMode = VK_PRESENT_MODE_FIFO_KHR;
        }
    }

    VkResult r = dt->CreateSwapchainKHR(device, &ci, pAllocator, pSwapchain);
    if (r == VK_SUCCESS && dt->timing_supported) {
        auto& sc = RegisterSwapchain(*pSwapchain);                 // per-swapchain state, keyed by handle
        sc.image_count = ci.minImageCount;
        dt->SetSwapchainPresentTimingQueueSizeEXT(device, *pSwapchain, ci.minImageCount * 4);
        sc.time_domain_id = PickTimeDomain(dt, device, *pSwapchain);
        sc.timing_enabled = true;
    }
    return r;
}
```

> A surface may refuse `FIFO`-only or the present-id2 swapchain flag. `FIFO` is mandatory for every surface, but if `vkCreateSwapchainKHR` fails with the extra flags, retry once without them and mark the device `timing_supported = false`.

### C. Choosing a time domain

Targets are expressed in a *time domain* identified by an ID. The layer needs one that it can convert from `CLOCK_MONOTONIC`:

```cpp
// Pseudocode of PickTimeDomain():
// 1. vkGetSwapchainTimeDomainPropertiesEXT(device, sc, &props, &counter) twice (count, then fill).
// 2. If a reported domain is VK_TIME_DOMAIN_CLOCK_MONOTONIC_KHR, use its timeDomainId directly:
//        target_ns = monotonic_ns
// 3. Otherwise use VK_TIME_DOMAIN_SWAPCHAIN_LOCAL_EXT (or PRESENT_STAGE_LOCAL_EXT, which every
//    implementation must support) and keep a calibration pair, refreshed about once a second:
//        vkGetCalibratedTimestampsKHR(...) with VkSwapchainCalibratedTimestampInfoEXT
//        offset = domain_ns - monotonic_ns
//    then target_ns = monotonic_ns + offset.
```

