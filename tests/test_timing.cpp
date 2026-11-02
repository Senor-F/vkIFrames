// Mock-driver test for Stage 2: extension/feature injection, per-surface checks, swapchain flags, forced FIFO,
// present chain (ids, target time, stage), result draining, queue-full retry. No GPU needed.
#include "clean_env.hpp"
#include <vulkan/vulkan.h>
#include <vulkan/vk_layer.h>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <string>
#include <vector>
#include <dlfcn.h>
#include <time.h>
#include <unistd.h>
#define CHECK(c) do { if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); std::exit(1); } } while (0)

struct Fake { void* dispatch; };
static Fake g_inst{(void*)0x1111}, g_pd{(void*)0x1111}, g_dev{(void*)0x2222}, g_q{(void*)0x2222};

// ---- knobs per scenario
static bool k_abs = true, k_dev_exts = true, k_features = true, k_surface_ok = true, k_flags_fail = false, k_queue_full_once = false;
// ---- records
static std::vector<std::string> r_inst_exts, r_dev_exts;
static bool r_timing_feat, r_id2_feat; static VkSwapchainCreateFlagsKHR r_sc_flags; static VkPresentModeKHR r_sc_mode;
static int r_sc_calls, r_present_calls, r_present_with_chain, r_queue_size;
static int r_retired_reuse; static std::vector<VkSwapchainKHR> r_retired; static bool k_setup_fail = false, k_reject_changes = false; static VkSwapchainKHR r_old_sc{}; static uint32_t r_sc_min_images, k_app_images; static VkImageUsageFlags r_sc_usage; static std::vector<VkSwapchainKHR> r_destroyed_sc;
static uint64_t r_last_id, r_last_target; static bool r_ids_increase = true; static VkPresentStageFlagsEXT r_last_stage;
static int r_pending; static uint64_t r_pending_target;

static uint64_t now_ns() { timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return uint64_t(t.tv_sec) * 1000000000ull + t.tv_nsec; }

static VkResult VKAPI_CALL m_EnumInstExt(const char*, uint32_t* n, VkExtensionProperties* p) {
    if (!p) { *n = 3; return VK_SUCCESS; }
    std::strcpy(p[0].extensionName, VK_KHR_SURFACE_EXTENSION_NAME); std::strcpy(p[1].extensionName, VK_KHR_GET_SURFACE_CAPABILITIES_2_EXTENSION_NAME);
    std::strcpy(p[2].extensionName, VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME); return VK_SUCCESS;
}
static VkResult VKAPI_CALL m_CreateInstance(const VkInstanceCreateInfo* ci, const VkAllocationCallbacks*, VkInstance* p) {
    r_inst_exts.clear(); for (uint32_t i = 0; i < ci->enabledExtensionCount; ++i) r_inst_exts.push_back(ci->ppEnabledExtensionNames[i]);
    *p = (VkInstance)&g_inst; return VK_SUCCESS;
}
static void VKAPI_CALL m_DestroyInstance(VkInstance, const VkAllocationCallbacks*) {}
static VkResult VKAPI_CALL m_EnumDevExt(VkPhysicalDevice, const char*, uint32_t* n, VkExtensionProperties* p) {
    const char* names[] = {VK_KHR_SWAPCHAIN_EXTENSION_NAME, VK_EXT_PRESENT_TIMING_EXTENSION_NAME, VK_KHR_PRESENT_ID_2_EXTENSION_NAME, VK_KHR_CALIBRATED_TIMESTAMPS_EXTENSION_NAME};
    uint32_t cnt = k_dev_exts ? 4 : 1;
    if (!p) { *n = cnt; return VK_SUCCESS; }
    for (uint32_t i = 0; i < cnt && i < *n; ++i) std::strcpy(p[i].extensionName, names[i]);
    return VK_SUCCESS;
}
static void VKAPI_CALL m_Features2(VkPhysicalDevice, VkPhysicalDeviceFeatures2* f) {
    for (auto* b = (VkBaseOutStructure*)f->pNext; b; b = b->pNext) {
        if (b->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRESENT_TIMING_FEATURES_EXT) { auto* t = (VkPhysicalDevicePresentTimingFeaturesEXT*)b; t->presentTiming = t->presentAtAbsoluteTime = k_features; }
        if (b->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRESENT_ID_2_FEATURES_KHR) ((VkPhysicalDevicePresentId2FeaturesKHR*)b)->presentId2 = k_features;
    }
}
static VkResult VKAPI_CALL m_CreateDevice(VkPhysicalDevice, const VkDeviceCreateInfo* ci, const VkAllocationCallbacks*, VkDevice* p) {
    r_dev_exts.clear(); for (uint32_t i = 0; i < ci->enabledExtensionCount; ++i) r_dev_exts.push_back(ci->ppEnabledExtensionNames[i]);
    r_timing_feat = r_id2_feat = false;
    for (auto* b = (const VkBaseInStructure*)ci->pNext; b; b = b->pNext) {
        if (b->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRESENT_TIMING_FEATURES_EXT) r_timing_feat = ((const VkPhysicalDevicePresentTimingFeaturesEXT*)b)->presentTiming;
        if (b->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRESENT_ID_2_FEATURES_KHR) r_id2_feat = ((const VkPhysicalDevicePresentId2FeaturesKHR*)b)->presentId2;
    }
    *p = (VkDevice)&g_dev; return VK_SUCCESS;
}
static VkResult VKAPI_CALL m_Caps(VkPhysicalDevice, VkSurfaceKHR, VkSurfaceCapabilitiesKHR* c) { *c = {}; c->minImageCount = 2; return VK_SUCCESS; }
static VkResult VKAPI_CALL m_Caps2(VkPhysicalDevice, const VkPhysicalDeviceSurfaceInfo2KHR*, VkSurfaceCapabilities2KHR* c) {
    for (auto* b = (VkBaseOutStructure*)c->pNext; b; b = b->pNext) {
        if (b->sType == VK_STRUCTURE_TYPE_PRESENT_TIMING_SURFACE_CAPABILITIES_EXT) {
            auto* t = (VkPresentTimingSurfaceCapabilitiesEXT*)b;
            t->presentTimingSupported = k_surface_ok; t->presentAtAbsoluteTimeSupported = k_surface_ok && k_abs; t->presentStageQueries = VK_PRESENT_STAGE_IMAGE_FIRST_PIXEL_VISIBLE_BIT_EXT;
        }
        if (b->sType == VK_STRUCTURE_TYPE_SURFACE_CAPABILITIES_PRESENT_ID_2_KHR) ((VkSurfaceCapabilitiesPresentId2KHR*)b)->presentId2Supported = k_surface_ok;
    }
    return VK_SUCCESS;
}

static VkResult VKAPI_CALL m_CreateSwapchain(VkDevice, const VkSwapchainCreateInfoKHR* ci, const VkAllocationCallbacks*, VkSwapchainKHR* s) {
    ++r_sc_calls; r_sc_flags = ci->flags; r_sc_mode = ci->presentMode; r_old_sc = ci->oldSwapchain;
    if (ci->oldSwapchain) {                                     // Vulkan: oldSwapchain is retired by the call, also when it fails; a retired one must not be used again (VUID ...-oldSwapchain-01933)
        for (auto h : r_retired) if (h == ci->oldSwapchain) ++r_retired_reuse;
        r_retired.push_back(ci->oldSwapchain);
    } r_sc_min_images = ci->minImageCount; r_sc_usage = ci->imageUsage;
    if (k_flags_fail && (ci->flags & VK_SWAPCHAIN_CREATE_PRESENT_TIMING_BIT_EXT)) return VK_ERROR_INITIALIZATION_FAILED;
    if (k_reject_changes && (ci->flags != 0 || ci->minImageCount != k_app_images)) return VK_ERROR_INITIALIZATION_FAILED;     // a driver that only takes the game's own request
    *s = (VkSwapchainKHR)(uintptr_t)(0x77 + r_sc_calls); return VK_SUCCESS;
}
static void VKAPI_CALL m_DestroySwapchain(VkDevice, VkSwapchainKHR s, const VkAllocationCallbacks*) { r_destroyed_sc.push_back(s); }
static VkResult VKAPI_CALL m_SetQueue(VkDevice, VkSwapchainKHR, uint32_t n) { r_queue_size = int(n); return k_setup_fail ? VK_ERROR_INITIALIZATION_FAILED : VK_SUCCESS; }
static VkResult VKAPI_CALL m_Domains(VkDevice, VkSwapchainKHR, VkSwapchainTimeDomainPropertiesEXT* p, uint64_t* c) {
    *c = 1;
    if (!p->pTimeDomains) { p->timeDomainCount = 1; return VK_SUCCESS; }
    p->pTimeDomains[0] = VK_TIME_DOMAIN_CLOCK_MONOTONIC_KHR; p->pTimeDomainIds[0] = 42; p->timeDomainCount = 1; return VK_SUCCESS;
}
static VkResult VKAPI_CALL m_Calib(VkDevice, uint32_t n, const VkCalibratedTimestampInfoKHR*, uint64_t* ts, uint64_t* dev) { for (uint32_t i = 0; i < n; ++i) ts[i] = now_ns(); *dev = 0; return VK_SUCCESS; }
static VkResult VKAPI_CALL m_Past(VkDevice, const VkPastPresentationTimingInfoEXT*, VkPastPresentationTimingPropertiesEXT* p) {
    uint32_t n = 0;
    while (r_pending > 0 && n < p->presentationTimingCount) {
        auto& t = p->pPresentationTimings[n]; t.targetTime = r_pending_target; t.reportComplete = VK_TRUE; t.presentStageCount = 1;
        t.pPresentStages[0].stage = VK_PRESENT_STAGE_IMAGE_FIRST_PIXEL_VISIBLE_BIT_EXT; t.pPresentStages[0].time = r_pending_target ? r_pending_target + 500000 : now_ns();
        ++n; --r_pending;
    }
    p->presentationTimingCount = n; return VK_SUCCESS;
}
static VkResult VKAPI_CALL m_Present(VkQueue, const VkPresentInfoKHR* pi) {
    ++r_present_calls;
    if (k_queue_full_once) { k_queue_full_once = false; return VK_ERROR_PRESENT_TIMING_QUEUE_FULL_EXT; }
    const VkPresentId2KHR* id2 = nullptr; const VkPresentTimingsInfoEXT* ti = nullptr;
    for (auto* b = (const VkBaseInStructure*)pi->pNext; b; b = b->pNext) {
        if (b->sType == VK_STRUCTURE_TYPE_PRESENT_ID_2_KHR) id2 = (const VkPresentId2KHR*)b;
        if (b->sType == VK_STRUCTURE_TYPE_PRESENT_TIMINGS_INFO_EXT) ti = (const VkPresentTimingsInfoEXT*)b;
    }
    if (id2 && ti) {
        ++r_present_with_chain; CHECK(id2->swapchainCount == 1 && ti->swapchainCount == 1);
        if (id2->pPresentIds[0] <= r_last_id) r_ids_increase = false;
        r_last_id = id2->pPresentIds[0]; r_last_target = ti->pTimingInfos[0].targetTime; r_last_stage = ti->pTimingInfos[0].presentStageQueries;
        CHECK(ti->pTimingInfos[0].timeDomainId == 42 && r_last_id != 0);
        ++r_pending; r_pending_target = r_last_target;
    }
    return VK_SUCCESS;
}

static PFN_vkVoidFunction VKAPI_CALL next_gipa(VkInstance, const char* n) {
#define R(x, f) if (!std::strcmp(n, "vk" #x)) return (PFN_vkVoidFunction)f;
    R(CreateInstance, m_CreateInstance) R(DestroyInstance, m_DestroyInstance) R(CreateDevice, m_CreateDevice)
    R(EnumerateInstanceExtensionProperties, m_EnumInstExt) R(EnumerateDeviceExtensionProperties, m_EnumDevExt)
    R(GetPhysicalDeviceFeatures2, m_Features2) R(GetPhysicalDeviceFeatures2KHR, m_Features2) R(GetPhysicalDeviceSurfaceCapabilitiesKHR, m_Caps)
    R(GetPhysicalDeviceSurfaceCapabilities2KHR, m_Caps2)
    return nullptr;
}
static PFN_vkVoidFunction VKAPI_CALL next_gdpa(VkDevice, const char* n) {
    R(CreateSwapchainKHR, m_CreateSwapchain) R(DestroySwapchainKHR, m_DestroySwapchain) R(QueuePresentKHR, m_Present)
    R(SetSwapchainPresentTimingQueueSizeEXT, m_SetQueue) R(GetSwapchainTimeDomainPropertiesEXT, m_Domains)
    R(GetPastPresentationTimingEXT, m_Past) R(GetCalibratedTimestampsKHR, m_Calib)
    return nullptr;
}
static bool has(const std::vector<std::string>& v, const char* s) { for (auto& e : v) if (e == s) return true; return false; }

struct Env { PFN_vkGetInstanceProcAddr gipa; PFN_vkGetDeviceProcAddr gdpa; VkInstance inst; VkDevice dev; };

static Env Bring(void* lib, const std::vector<const char*>& inst_exts, uint32_t api, const std::vector<const char*>& dev_exts, const void* dev_pnext = nullptr) {
    auto negotiate = (PFN_vkNegotiateLoaderLayerInterfaceVersion)dlsym(lib, "FrameGen_NegotiateLoaderLayerInterfaceVersion");
    VkNegotiateLayerInterface nl{}; nl.sType = LAYER_NEGOTIATE_INTERFACE_STRUCT; nl.loaderLayerInterfaceVersion = 2; CHECK(negotiate(&nl) == VK_SUCCESS);
    Env e{nl.pfnGetInstanceProcAddr, nl.pfnGetDeviceProcAddr, {}, {}};
    VkLayerInstanceLink il{nullptr, next_gipa, nullptr};
    VkLayerInstanceCreateInfo lici{}; lici.sType = VK_STRUCTURE_TYPE_LOADER_INSTANCE_CREATE_INFO; lici.function = VK_LAYER_LINK_INFO; lici.u.pLayerInfo = &il;
    VkApplicationInfo ai{VK_STRUCTURE_TYPE_APPLICATION_INFO}; ai.apiVersion = api;
    VkInstanceCreateInfo ici{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, &lici}; ici.pApplicationInfo = &ai;
    ici.enabledExtensionCount = uint32_t(inst_exts.size()); ici.ppEnabledExtensionNames = inst_exts.data();
    auto ci = (PFN_vkCreateInstance)e.gipa(VK_NULL_HANDLE, "vkCreateInstance");
    CHECK(ci(&ici, nullptr, &e.inst) == VK_SUCCESS);
    VkLayerDeviceLink dl{nullptr, nullptr, next_gdpa};
    VkLayerDeviceCreateInfo ldci{}; ldci.sType = VK_STRUCTURE_TYPE_LOADER_DEVICE_CREATE_INFO; ldci.function = VK_LAYER_LINK_INFO; ldci.u.pLayerInfo = &dl;
    ldci.pNext = dev_pnext;
    VkDeviceCreateInfo dci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, &ldci};
    dci.enabledExtensionCount = uint32_t(dev_exts.size()); dci.ppEnabledExtensionNames = dev_exts.data();
    auto cd = (PFN_vkCreateDevice)e.gipa(e.inst, "vkCreateDevice");
    CHECK(cd((VkPhysicalDevice)&g_pd, &dci, nullptr, &e.dev) == VK_SUCCESS);
    return e;
}
static void Reset() { k_abs = true; k_dev_exts = k_features = k_surface_ok = true; k_flags_fail = k_queue_full_once = k_setup_fail = k_reject_changes = false; r_destroyed_sc.clear(); r_retired.clear(); r_retired_reuse = 0; r_old_sc = {}; r_sc_calls = r_present_calls = r_present_with_chain = 0; r_last_id = 0; r_ids_increase = true; r_pending = 0; r_sc_flags = 0; r_queue_size = 0; }

int main(int, char** argv) {
    ClearFramegenEnv();
    void* lib = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL); CHECK(lib);
    const std::vector<const char*> iexts{VK_KHR_SURFACE_EXTENSION_NAME}, dexts{VK_KHR_SWAPCHAIN_EXTENSION_NAME};

    // --- A: everything supported
    {
        Reset(); Env e = Bring(lib, iexts, VK_API_VERSION_1_3, dexts);
        CHECK(has(r_inst_exts, VK_KHR_GET_SURFACE_CAPABILITIES_2_EXTENSION_NAME));
        CHECK(has(r_dev_exts, VK_EXT_PRESENT_TIMING_EXTENSION_NAME) && has(r_dev_exts, VK_KHR_PRESENT_ID_2_EXTENSION_NAME) && has(r_dev_exts, VK_KHR_CALIBRATED_TIMESTAMPS_EXTENSION_NAME));
        CHECK(has(r_dev_exts, VK_KHR_SWAPCHAIN_EXTENSION_NAME) && r_timing_feat && r_id2_feat);
        auto csc = (PFN_vkCreateSwapchainKHR)e.gdpa(e.dev, "vkCreateSwapchainKHR");
        VkSwapchainCreateInfoKHR sci{VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR}; sci.minImageCount = 3; sci.presentMode = VK_PRESENT_MODE_IMMEDIATE_KHR; VkSwapchainKHR sc{};
        CHECK(csc(e.dev, &sci, nullptr, &sc) == VK_SUCCESS);
        CHECK((r_sc_flags & VK_SWAPCHAIN_CREATE_PRESENT_TIMING_BIT_EXT) && (r_sc_flags & VK_SWAPCHAIN_CREATE_PRESENT_ID_2_BIT_KHR));
        CHECK(r_sc_mode == VK_PRESENT_MODE_FIFO_KHR && sci.presentMode == VK_PRESENT_MODE_IMMEDIATE_KHR);   // forced, app struct untouched
        CHECK(r_queue_size == 16);                                                                            // 4 images x 4
        auto qp = (PFN_vkQueuePresentKHR)e.gdpa(e.dev, "vkQueuePresentKHR");
        VkPresentInfoKHR pi{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR}; pi.swapchainCount = 1; pi.pSwapchains = &sc; uint32_t idx = 0; pi.pImageIndices = &idx;
        for (int i = 0; i < 50; ++i) { CHECK(qp((VkQueue)&g_q, &pi) == VK_SUCCESS); usleep(2000); }
        CHECK(r_present_with_chain == 50 && r_ids_increase && r_last_id == 50 && r_last_target == 0);        // ASAP by default
        CHECK(r_last_stage == VK_PRESENT_STAGE_IMAGE_FIRST_PIXEL_VISIBLE_BIT_EXT);
        // queue full once: retried with a fresh id
        k_queue_full_once = true; int before = r_present_calls; CHECK(qp((VkQueue)&g_q, &pi) == VK_SUCCESS); CHECK(r_present_calls == before + 2);
        // app already uses present ids -> we do not add ours
        VkPresentIdKHR pid{VK_STRUCTURE_TYPE_PRESENT_ID_KHR}; pi.pNext = &pid; int chains = r_present_with_chain; qp((VkQueue)&g_q, &pi); CHECK(r_present_with_chain == chains);
        ((PFN_vkDestroySwapchainKHR)e.gdpa(e.dev, "vkDestroySwapchainKHR"))(e.dev, sc, nullptr);
        std::printf("A ok: instance ext, device exts+features, flags, forced FIFO, queue size, ids, queue-full retry, app ids respected\n");
    }
    // --- B: driver lacks the device extensions / features
    { Reset(); k_dev_exts = false; Env e = Bring(lib, iexts, VK_API_VERSION_1_3, dexts); CHECK(!has(r_dev_exts, VK_EXT_PRESENT_TIMING_EXTENSION_NAME) && !r_timing_feat);
      Reset(); k_features = false; e = Bring(lib, iexts, VK_API_VERSION_1_3, dexts); CHECK(!has(r_dev_exts, VK_EXT_PRESENT_TIMING_EXTENSION_NAME));
      std::printf("B ok: untouched device when unsupported\n"); }
    // --- C: surface does not support it: no flags, no forced FIFO
    {
        Reset(); k_surface_ok = false; Env e = Bring(lib, iexts, VK_API_VERSION_1_3, dexts);
        auto csc = (PFN_vkCreateSwapchainKHR)e.gdpa(e.dev, "vkCreateSwapchainKHR");
        VkSwapchainCreateInfoKHR sci{VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR}; sci.minImageCount = 3; sci.presentMode = VK_PRESENT_MODE_MAILBOX_KHR; VkSwapchainKHR sc{};
        CHECK(csc(e.dev, &sci, nullptr, &sc) == VK_SUCCESS); CHECK(r_sc_flags == 0 && r_sc_mode == VK_PRESENT_MODE_MAILBOX_KHR);
        auto qp = (PFN_vkQueuePresentKHR)e.gdpa(e.dev, "vkQueuePresentKHR"); VkPresentInfoKHR pi{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR}; pi.swapchainCount = 1; pi.pSwapchains = &sc;
        qp((VkQueue)&g_q, &pi); CHECK(r_present_with_chain == 0);
        std::printf("C ok: surface without support is left alone\n");
    }
    // --- D: creation with the flags fails -> retried without, original mode restored
    {
        Reset(); k_flags_fail = true; Env e = Bring(lib, iexts, VK_API_VERSION_1_3, dexts);
        auto csc = (PFN_vkCreateSwapchainKHR)e.gdpa(e.dev, "vkCreateSwapchainKHR");
        VkSwapchainCreateInfoKHR sci{VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR}; sci.minImageCount = 3; sci.presentMode = VK_PRESENT_MODE_MAILBOX_KHR; VkSwapchainKHR sc{};
        sci.oldSwapchain = (VkSwapchainKHR)(uintptr_t)0x5000;                       // a resize: the failed first attempt retires it, the retry must not pass it again
        CHECK(csc(e.dev, &sci, nullptr, &sc) == VK_SUCCESS); CHECK(r_sc_calls == 2 && r_sc_flags == 0 && r_sc_mode == VK_PRESENT_MODE_MAILBOX_KHR);
        CHECK(r_retired_reuse == 0 && r_old_sc == VkSwapchainKHR{});
        std::printf("D ok: retry without flags\n");
    }
    // --- D2: the timing setup fails after the swapchain was created with the flags and the forced FIFO -> made again with the game's flags and mode, the first one destroyed
    {
        Reset(); k_setup_fail = true; Env e = Bring(lib, iexts, VK_API_VERSION_1_3, dexts);
        auto csc = (PFN_vkCreateSwapchainKHR)e.gdpa(e.dev, "vkCreateSwapchainKHR");
        VkSwapchainCreateInfoKHR sci{VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR}; sci.minImageCount = 3; sci.presentMode = VK_PRESENT_MODE_MAILBOX_KHR; VkSwapchainKHR sc{};
        sci.oldSwapchain = (VkSwapchainKHR)(uintptr_t)0x5000;
        CHECK(csc(e.dev, &sci, nullptr, &sc) == VK_SUCCESS);
        CHECK(r_retired_reuse == 0);                                                       // the replacement takes over from the first swapchain (not retired), never from 0x5000 again
        CHECK(r_sc_calls == 2 && r_sc_flags == 0 && r_sc_mode == VK_PRESENT_MODE_MAILBOX_KHR);
        CHECK(r_old_sc == (VkSwapchainKHR)(uintptr_t)(0x77 + 1) && r_destroyed_sc.size() == 1 && r_destroyed_sc[0] == r_old_sc && sc == (VkSwapchainKHR)(uintptr_t)(0x77 + 2));
        auto qp = (PFN_vkQueuePresentKHR)e.gdpa(e.dev, "vkQueuePresentKHR"); VkPresentInfoKHR pi{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR}; pi.swapchainCount = 1; pi.pSwapchains = &sc;
        for (int i = 0; i < 5; ++i) { CHECK(qp((VkQueue)&g_q, &pi) == VK_SUCCESS); usleep(1000); }
        CHECK(r_present_with_chain == 0);                                                  // no timing chain without timing
        std::printf("D2 ok: timing setup failed -> swapchain made again without the flags\n");
    }
    // --- D3: the driver refuses every change the layer makes (flags, image count): the game's own request is made as it is, the layer does not touch that swapchain
    {
        Reset(); k_reject_changes = true; k_app_images = 3; Env e = Bring(lib, iexts, VK_API_VERSION_1_3, dexts);
        auto csc = (PFN_vkCreateSwapchainKHR)e.gdpa(e.dev, "vkCreateSwapchainKHR");
        VkSwapchainCreateInfoKHR sci{VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR}; sci.minImageCount = 3; sci.presentMode = VK_PRESENT_MODE_MAILBOX_KHR; sci.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT; VkSwapchainKHR sc{};
        sci.oldSwapchain = (VkSwapchainKHR)(uintptr_t)0x5000;
        CHECK(csc(e.dev, &sci, nullptr, &sc) == VK_SUCCESS);
        CHECK(r_retired_reuse == 0);                                                       // three attempts, the game's oldSwapchain passed once only
        CHECK(r_sc_calls == 3 && r_sc_flags == 0 && r_sc_mode == VK_PRESENT_MODE_MAILBOX_KHR && r_sc_min_images == 3 && r_sc_usage == VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT && r_old_sc == VkSwapchainKHR{});
        auto qp = (PFN_vkQueuePresentKHR)e.gdpa(e.dev, "vkQueuePresentKHR"); VkPresentInfoKHR pi{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR}; pi.swapchainCount = 1; pi.pSwapchains = &sc;
        for (int i = 0; i < 5; ++i) { CHECK(qp((VkQueue)&g_q, &pi) == VK_SUCCESS); usleep(1000); }
        CHECK(r_present_with_chain == 0);
        std::printf("D3 ok: layer changes refused by the driver -> the game's own swapchain request\n");
    }
    // --- E: app brings its own present timing / old API: layer stays out
    {
        Reset(); Env e = Bring(lib, iexts, VK_API_VERSION_1_3, {VK_KHR_SWAPCHAIN_EXTENSION_NAME, VK_EXT_PRESENT_TIMING_EXTENSION_NAME}); CHECK(!r_id2_feat);
        Reset(); e = Bring(lib, iexts, VK_API_VERSION_1_0, dexts);          // a Vulkan 1.0 app (vkcube) is served through the KHR extensions
        CHECK(has(r_inst_exts, VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME) && has(r_inst_exts, VK_KHR_GET_SURFACE_CAPABILITIES_2_EXTENSION_NAME));
        CHECK(has(r_dev_exts, VK_EXT_PRESENT_TIMING_EXTENSION_NAME) && r_timing_feat && r_id2_feat);
        std::printf("E ok: app-owned timing left alone, Vulkan 1.0 apps served via KHR extensions\n");
    }
    // --- F: surface supports timing but not absolute targets: feedback only, target stays 0 even with VKIFRAME_TIMING_TEST
    {
        setenv("VKIFRAME_TIMING_TEST", "1", 1);
        Reset(); k_abs = false; Env e = Bring(lib, iexts, VK_API_VERSION_1_0, dexts);
        auto csc = (PFN_vkCreateSwapchainKHR)e.gdpa(e.dev, "vkCreateSwapchainKHR");
        VkSwapchainCreateInfoKHR sci{VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR}; sci.minImageCount = 3; VkSwapchainKHR sc{};
        CHECK(csc(e.dev, &sci, nullptr, &sc) == VK_SUCCESS); CHECK(r_sc_flags & VK_SWAPCHAIN_CREATE_PRESENT_TIMING_BIT_EXT);
        auto qp = (PFN_vkQueuePresentKHR)e.gdpa(e.dev, "vkQueuePresentKHR"); VkPresentInfoKHR pi{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR}; pi.swapchainCount = 1; pi.pSwapchains = &sc;
        for (int i = 0; i < 5; ++i) { qp((VkQueue)&g_q, &pi); usleep(1000); }
        CHECK(r_present_with_chain == 5 && r_last_target == 0);
        std::printf("F ok: no absolute targets -> feedback-only timing\n");
    }
    std::printf("PASS: stage 2 mock driver\n");
}
