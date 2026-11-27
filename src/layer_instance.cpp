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
