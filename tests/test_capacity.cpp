// Mock-driver test for Stage 3c: synthetic frames (compute blend into hidden images) and the failure fallback
// (what is submitted, what is presented, in which order, with which semaphores). No GPU needed.
#include "clean_env.hpp"
#include <vulkan/vulkan.h>
#include <vulkan/vk_layer.h>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <string>
#include <vector>
#include <map>
#include <set>
#include <dlfcn.h>
#include <unistd.h>
#include <fstream>
#include <sstream>
#include <string>
#define CHECK(c) do { if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); std::exit(1); } } while (0)

struct Fake { void* dispatch; };
static Fake g_inst{(void*)0x1111}, g_pd{(void*)0x1111}, g_dev{(void*)0x2222}, g_q{(void*)0x2222};
static std::vector<Fake*> g_cbs;

// ---- records
struct Submit { std::vector<VkSemaphore> waits; VkSemaphore signal; };
struct Present { VkSemaphore wait; uint32_t image; uint64_t id; VkFence fence; int mode; };
static std::vector<Submit> r_submits; static std::vector<Present> r_presents;
static std::vector<std::string> r_events;                      // "S" submit, "P" present, in call order
static VkImageUsageFlags r_usage; static uint32_t r_min_images; static std::set<std::string> r_dev_exts; static bool r_sync2_feature;
static int r_loader_data_calls, r_blits, r_barriers, r_images_created, r_destroyed_images;
static uint64_t g_next_handle = 100;
static bool k_surface_max_small = false; static uint32_t k_surface_max = 0, g_granted = 8;

#define H(x) ((x)(uintptr_t)(g_next_handle++))
static VkResult VKAPI_CALL m_EnumInstExt(const char*, uint32_t* n, VkExtensionProperties* p) {
    if (!p) { *n = 2; return VK_SUCCESS; }
    std::strcpy(p[0].extensionName, VK_KHR_SURFACE_EXTENSION_NAME); std::strcpy(p[1].extensionName, VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME); return VK_SUCCESS;
}
static VkResult VKAPI_CALL m_CreateInstance(const VkInstanceCreateInfo*, const VkAllocationCallbacks*, VkInstance* p) { *p = (VkInstance)&g_inst; return VK_SUCCESS; }
static void VKAPI_CALL m_DestroyInstance(VkInstance, const VkAllocationCallbacks*) {}
static VkResult VKAPI_CALL m_EnumDevExt(VkPhysicalDevice, const char*, uint32_t* n, VkExtensionProperties* p) {
    if (!p) { *n = 2; return VK_SUCCESS; }
    std::strcpy(p[0].extensionName, VK_KHR_SWAPCHAIN_EXTENSION_NAME); std::strcpy(p[1].extensionName, VK_KHR_SYNCHRONIZATION_2_EXTENSION_NAME); return VK_SUCCESS;
}
static void VKAPI_CALL m_Features2(VkPhysicalDevice, VkPhysicalDeviceFeatures2* f) {
    for (auto* b = (VkBaseOutStructure*)f->pNext; b; b = b->pNext)
        if (b->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SYNCHRONIZATION_2_FEATURES) ((VkPhysicalDeviceSynchronization2Features*)b)->synchronization2 = VK_TRUE;
}
static VkResult VKAPI_CALL m_CreateDevice(VkPhysicalDevice, const VkDeviceCreateInfo* ci, const VkAllocationCallbacks*, VkDevice* p) {
    r_dev_exts.clear(); for (uint32_t i = 0; i < ci->enabledExtensionCount; ++i) r_dev_exts.insert(ci->ppEnabledExtensionNames[i]);
    r_sync2_feature = false;
    for (auto* b = (const VkBaseInStructure*)ci->pNext; b; b = b->pNext)
        if (b->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SYNCHRONIZATION_2_FEATURES) r_sync2_feature = ((const VkPhysicalDeviceSynchronization2Features*)b)->synchronization2;
    *p = (VkDevice)&g_dev; return VK_SUCCESS;
}
static VkResult VKAPI_CALL m_Caps(VkPhysicalDevice, VkSurfaceKHR, VkSurfaceCapabilitiesKHR* c) {
    *c = {}; c->minImageCount = 2; c->maxImageCount = k_surface_max;
    c->supportedUsageFlags = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT; return VK_SUCCESS;
}
static void VKAPI_CALL m_FormatProps(VkPhysicalDevice, VkFormat, VkFormatProperties* p) {
    p->optimalTilingFeatures = VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT;
}
static void VKAPI_CALL m_QFProps(VkPhysicalDevice, uint32_t* n, VkQueueFamilyProperties* p) { *n = 1; p[0] = {}; p[0].queueFlags = VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT; p[0].queueCount = 1; }
static void VKAPI_CALL m_MemProps(VkPhysicalDevice, VkPhysicalDeviceMemoryProperties* p) { *p = {}; p->memoryTypeCount = 1; p->memoryTypes[0].propertyFlags = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT; }

static int r_creates; static VkSwapchainKHR r_old_last{}; static VkPresentModeKHR r_mode_last; static VkSwapchainCreateFlagsKHR r_flags_last; static std::vector<VkSwapchainKHR> r_destroyed_sc;
static VkResult VKAPI_CALL m_CreateSwapchain(VkDevice, const VkSwapchainCreateInfoKHR* ci, const VkAllocationCallbacks*, VkSwapchainKHR* s) {
    ++r_creates; r_old_last = ci->oldSwapchain; r_mode_last = ci->presentMode; r_flags_last = ci->flags;
    r_usage = ci->imageUsage; r_min_images = ci->minImageCount; g_granted = k_surface_max && ci->minImageCount > k_surface_max ? k_surface_max : ci->minImageCount; *s = (VkSwapchainKHR)(uintptr_t)(0x77 + r_creates); return VK_SUCCESS;
}
static void VKAPI_CALL m_DestroySwapchain(VkDevice, VkSwapchainKHR s, const VkAllocationCallbacks*) { r_destroyed_sc.push_back(s); }
static void VKAPI_CALL m_GetQueue(VkDevice, uint32_t, uint32_t, VkQueue* q) { *q = (VkQueue)&g_q; }
static VkResult VKAPI_CALL m_SwapImages(VkDevice, VkSwapchainKHR, uint32_t* n, VkImage* p) { if (!p) { *n = g_granted; return VK_SUCCESS; } for (uint32_t i = 0; i < *n && i < g_granted; ++i) p[i] = (VkImage)(uintptr_t)(1000 + i); return VK_SUCCESS; }
static VkResult VKAPI_CALL m_WaitIdle(VkDevice) { return VK_SUCCESS; }
static VkResult VKAPI_CALL m_CreateImage(VkDevice, const VkImageCreateInfo*, const VkAllocationCallbacks*, VkImage* i) { ++r_images_created; *i = H(VkImage); return VK_SUCCESS; }
static void VKAPI_CALL m_DestroyImage(VkDevice, VkImage, const VkAllocationCallbacks*) { ++r_destroyed_images; }
static void VKAPI_CALL m_ImgReq(VkDevice, VkImage, VkMemoryRequirements* r) { *r = {}; r->size = 4096; r->memoryTypeBits = 1; }
static VkResult VKAPI_CALL m_Alloc(VkDevice, const VkMemoryAllocateInfo*, const VkAllocationCallbacks*, VkDeviceMemory* m) { *m = H(VkDeviceMemory); return VK_SUCCESS; }
static void VKAPI_CALL m_Free(VkDevice, VkDeviceMemory, const VkAllocationCallbacks*) {}
static VkResult VKAPI_CALL m_Bind(VkDevice, VkImage, VkDeviceMemory, VkDeviceSize) { return VK_SUCCESS; }
static VkResult VKAPI_CALL m_CreatePool(VkDevice, const VkCommandPoolCreateInfo*, const VkAllocationCallbacks*, VkCommandPool* p) { *p = H(VkCommandPool); return VK_SUCCESS; }
static void VKAPI_CALL m_DestroyPool(VkDevice, VkCommandPool, const VkAllocationCallbacks*) {}
static VkResult VKAPI_CALL m_AllocCb(VkDevice, const VkCommandBufferAllocateInfo* ai, VkCommandBuffer* p) {
    for (uint32_t i = 0; i < ai->commandBufferCount; ++i) { auto* f = new Fake{nullptr}; g_cbs.push_back(f); p[i] = (VkCommandBuffer)f; } return VK_SUCCESS;
}
static VkResult VKAPI_CALL m_Begin(VkCommandBuffer, const VkCommandBufferBeginInfo*) { return VK_SUCCESS; }
static VkResult VKAPI_CALL m_End(VkCommandBuffer) { return VK_SUCCESS; }
static VkResult VKAPI_CALL m_ResetCb(VkCommandBuffer, VkCommandBufferResetFlags) { return VK_SUCCESS; }
static VkResult VKAPI_CALL m_CreateSem(VkDevice, const VkSemaphoreCreateInfo*, const VkAllocationCallbacks*, VkSemaphore* s) { *s = H(VkSemaphore); return VK_SUCCESS; }
static void VKAPI_CALL m_DestroySem(VkDevice, VkSemaphore, const VkAllocationCallbacks*) {}
static VkResult VKAPI_CALL m_CreateFence(VkDevice, const VkFenceCreateInfo*, const VkAllocationCallbacks*, VkFence* f) { *f = H(VkFence); return VK_SUCCESS; }
static void VKAPI_CALL m_DestroyFence(VkDevice, VkFence, const VkAllocationCallbacks*) {}
static VkResult VKAPI_CALL m_WaitFences(VkDevice, uint32_t, const VkFence*, VkBool32, uint64_t) { return VK_SUCCESS; }
static VkResult VKAPI_CALL m_ResetFences(VkDevice, uint32_t, const VkFence*) { return VK_SUCCESS; }
static bool k_bad_index = false;   // the next acquire returns an index the swapchain does not have
static bool k_submit_fail_hidden = false; static VkSemaphore k_last_acq_sem = VK_NULL_HANDLE; static int r_failed_submits = 0;   // fail the next submit that waits on the last acquire semaphore
static VkResult VKAPI_CALL m_Submit2(VkQueue, uint32_t, const VkSubmitInfo2* si, VkFence) {
    if (k_submit_fail_hidden) for (uint32_t i = 0; i < si->waitSemaphoreInfoCount; ++i)
        if (si->pWaitSemaphoreInfos[i].semaphore == k_last_acq_sem) { k_submit_fail_hidden = false; ++r_failed_submits; return VK_ERROR_OUT_OF_DEVICE_MEMORY; }
    Submit s; for (uint32_t i = 0; i < si->waitSemaphoreInfoCount; ++i) s.waits.push_back(si->pWaitSemaphoreInfos[i].semaphore);
    CHECK(si->signalSemaphoreInfoCount == 1); s.signal = si->pSignalSemaphoreInfos[0].semaphore; r_submits.push_back(s); r_events.push_back("S"); return VK_SUCCESS;
}
static void VKAPI_CALL m_Barrier2(VkCommandBuffer, const VkDependencyInfo*) { ++r_barriers; }
static void VKAPI_CALL m_Blit(VkCommandBuffer, VkImage, VkImageLayout, VkImage, VkImageLayout, uint32_t, const VkImageBlit*, VkFilter) { ++r_blits; }
static VkResult VKAPI_CALL m_Present(VkQueue, const VkPresentInfoKHR* pi) {
    CHECK(pi->swapchainCount == 1 && pi->waitSemaphoreCount <= 1);
    uint64_t pid = 0; for (auto* c = (const VkBaseInStructure*)pi->pNext; c; c = c->pNext) if (c->sType == VK_STRUCTURE_TYPE_PRESENT_ID_KHR) { auto* x = (const VkPresentIdKHR*)c; CHECK(x->swapchainCount == 1); pid = x->pPresentIds[0]; }
    VkFence fence = VK_NULL_HANDLE; for (auto* c = (const VkBaseInStructure*)pi->pNext; c; c = c->pNext) if (c->sType == VK_STRUCTURE_TYPE_SWAPCHAIN_PRESENT_FENCE_INFO_KHR) { auto* x = (const VkSwapchainPresentFenceInfoKHR*)c; CHECK(x->swapchainCount == 1); fence = x->pFences[0]; }
    int mode = -1; for (auto* c = (const VkBaseInStructure*)pi->pNext; c; c = c->pNext) if (c->sType == VK_STRUCTURE_TYPE_SWAPCHAIN_PRESENT_MODE_INFO_KHR) { auto* x = (const VkSwapchainPresentModeInfoKHR*)c; CHECK(x->swapchainCount == 1); mode = int(x->pPresentModes[0]); }
    r_presents.push_back({pi->waitSemaphoreCount ? pi->pWaitSemaphores[0] : VK_NULL_HANDLE, pi->pImageIndices[0], pid, fence, mode}); r_events.push_back("P"); if (pi->pResults) pi->pResults[0] = VK_SUCCESS; return VK_SUCCESS;   // a real driver reports per swapchain
}
static VkResult VKAPI_CALL m_SetLoaderData(VkDevice, void* obj) { ++r_loader_data_calls; ((Fake*)obj)->dispatch = (void*)0x2222; return VK_SUCCESS; }


static int r_dispatches, r_acquires, r_desc_updates, r_pipelines, r_image_views; static std::vector<float> r_t; static bool k_acquire_fail = false;
static std::vector<VkSemaphore> r_acq_sems; static uint32_t g_acq_next = 0;
static VkResult VKAPI_CALL m_CreateView(VkDevice, const VkImageViewCreateInfo*, const VkAllocationCallbacks*, VkImageView* v) { ++r_image_views; *v = H(VkImageView); return VK_SUCCESS; }
static void VKAPI_CALL m_DestroyView(VkDevice, VkImageView, const VkAllocationCallbacks*) {}
static VkResult VKAPI_CALL m_CreateShader(VkDevice, const VkShaderModuleCreateInfo*, const VkAllocationCallbacks*, VkShaderModule* m) { *m = H(VkShaderModule); return VK_SUCCESS; }
static void VKAPI_CALL m_DestroyShader(VkDevice, VkShaderModule, const VkAllocationCallbacks*) {}
static VkResult VKAPI_CALL m_CreateSetLayout(VkDevice, const VkDescriptorSetLayoutCreateInfo*, const VkAllocationCallbacks*, VkDescriptorSetLayout* l) { *l = H(VkDescriptorSetLayout); return VK_SUCCESS; }
static void VKAPI_CALL m_DestroySetLayout(VkDevice, VkDescriptorSetLayout, const VkAllocationCallbacks*) {}
static VkResult VKAPI_CALL m_CreatePipeLayout(VkDevice, const VkPipelineLayoutCreateInfo*, const VkAllocationCallbacks*, VkPipelineLayout* l) { *l = H(VkPipelineLayout); return VK_SUCCESS; }
static void VKAPI_CALL m_DestroyPipeLayout(VkDevice, VkPipelineLayout, const VkAllocationCallbacks*) {}
static VkResult VKAPI_CALL m_CreateCompute(VkDevice, VkPipelineCache, uint32_t n, const VkComputePipelineCreateInfo*, const VkAllocationCallbacks*, VkPipeline* p) { for (uint32_t i = 0; i < n; ++i) { p[i] = H(VkPipeline); ++r_pipelines; } return VK_SUCCESS; }
static void VKAPI_CALL m_DestroyPipeline(VkDevice, VkPipeline, const VkAllocationCallbacks*) {}
static VkResult VKAPI_CALL m_CreateDescPool(VkDevice, const VkDescriptorPoolCreateInfo*, const VkAllocationCallbacks*, VkDescriptorPool* p) { *p = H(VkDescriptorPool); return VK_SUCCESS; }
static void VKAPI_CALL m_DestroyDescPool(VkDevice, VkDescriptorPool, const VkAllocationCallbacks*) {}
static VkResult VKAPI_CALL m_AllocSets(VkDevice, const VkDescriptorSetAllocateInfo* ai, VkDescriptorSet* s) { for (uint32_t i = 0; i < ai->descriptorSetCount; ++i) s[i] = H(VkDescriptorSet); return VK_SUCCESS; }
static void VKAPI_CALL m_UpdateSets(VkDevice, uint32_t n, const VkWriteDescriptorSet*, uint32_t, const VkCopyDescriptorSet*) { CHECK(n == 3); ++r_desc_updates; }
static void VKAPI_CALL m_BindPipe(VkCommandBuffer, VkPipelineBindPoint, VkPipeline) {}
static void VKAPI_CALL m_BindSets(VkCommandBuffer, VkPipelineBindPoint, VkPipelineLayout, uint32_t, uint32_t, const VkDescriptorSet*, uint32_t, const uint32_t*) {}
static void VKAPI_CALL m_PushConst(VkCommandBuffer, VkPipelineLayout, VkShaderStageFlags, uint32_t, uint32_t size, const void* v) { CHECK(size == 4); float f; std::memcpy(&f, v, 4); r_t.push_back(f); }
static void VKAPI_CALL m_Dispatch(VkCommandBuffer, uint32_t x, uint32_t y, uint32_t) { CHECK(x == 40 && y == 30); ++r_dispatches; }       // 640x480 / 16x16
static VkResult VKAPI_CALL m_Acquire(VkDevice, VkSwapchainKHR, uint64_t timeout, VkSemaphore sem, VkFence, uint32_t* idx) {
    CHECK(timeout <= 1'000'000);   // the game's acquire polls with 0; the presenter waits at most ACQUIRE_WAIT_US (1 ms) outside the capture mutex
    if (k_acquire_fail) return VK_ERROR_DEVICE_LOST;
    ++r_acquires; r_acq_sems.push_back(sem); k_last_acq_sem = sem; *idx = 4 + (g_acq_next++ % (g_granted > 5 ? g_granted - 4 : 1));
    if (k_bad_index) { k_bad_index = false; *idx = 99; }
    return VK_SUCCESS;
}

static PFN_vkVoidFunction VKAPI_CALL next_gipa(VkInstance, const char* n) {
#define R(x, f) if (!std::strcmp(n, "vk" #x)) return (PFN_vkVoidFunction)f;
    R(CreateInstance, m_CreateInstance) R(DestroyInstance, m_DestroyInstance) R(CreateDevice, m_CreateDevice)
    R(EnumerateInstanceExtensionProperties, m_EnumInstExt) R(EnumerateDeviceExtensionProperties, m_EnumDevExt)
    R(GetPhysicalDeviceFeatures2, m_Features2) R(GetPhysicalDeviceFeatures2KHR, m_Features2) R(GetPhysicalDeviceSurfaceCapabilitiesKHR, m_Caps)
    R(GetPhysicalDeviceFormatProperties, m_FormatProps) R(GetPhysicalDeviceQueueFamilyProperties, m_QFProps) R(GetPhysicalDeviceMemoryProperties, m_MemProps)
    return nullptr;
}
static PFN_vkVoidFunction VKAPI_CALL next_gdpa(VkDevice, const char* n) {
    R(CreateSwapchainKHR, m_CreateSwapchain) R(DestroySwapchainKHR, m_DestroySwapchain) R(QueuePresentKHR, m_Present) R(GetDeviceQueue, m_GetQueue)
    R(GetSwapchainImagesKHR, m_SwapImages) R(DeviceWaitIdle, m_WaitIdle) R(CreateImage, m_CreateImage) R(DestroyImage, m_DestroyImage)
    R(GetImageMemoryRequirements, m_ImgReq) R(AllocateMemory, m_Alloc) R(FreeMemory, m_Free) R(BindImageMemory, m_Bind)
    R(CreateCommandPool, m_CreatePool) R(DestroyCommandPool, m_DestroyPool) R(AllocateCommandBuffers, m_AllocCb) R(BeginCommandBuffer, m_Begin)
    R(EndCommandBuffer, m_End) R(ResetCommandBuffer, m_ResetCb) R(CreateSemaphore, m_CreateSem) R(DestroySemaphore, m_DestroySem)
    R(CreateFence, m_CreateFence) R(DestroyFence, m_DestroyFence) R(WaitForFences, m_WaitFences) R(ResetFences, m_ResetFences)
    R(CreateImageView, m_CreateView) R(DestroyImageView, m_DestroyView) R(CreateShaderModule, m_CreateShader) R(DestroyShaderModule, m_DestroyShader)
    R(CreateDescriptorSetLayout, m_CreateSetLayout) R(DestroyDescriptorSetLayout, m_DestroySetLayout) R(CreatePipelineLayout, m_CreatePipeLayout) R(DestroyPipelineLayout, m_DestroyPipeLayout)
    R(CreateComputePipelines, m_CreateCompute) R(DestroyPipeline, m_DestroyPipeline) R(CreateDescriptorPool, m_CreateDescPool) R(DestroyDescriptorPool, m_DestroyDescPool)
    R(AllocateDescriptorSets, m_AllocSets) R(UpdateDescriptorSets, m_UpdateSets) R(CmdBindPipeline, m_BindPipe) R(CmdBindDescriptorSets, m_BindSets)
    R(CmdPushConstants, m_PushConst) R(CmdDispatch, m_Dispatch) R(AcquireNextImageKHR, m_Acquire)
    R(QueueSubmit2KHR, m_Submit2) R(CmdPipelineBarrier2KHR, m_Barrier2) R(CmdBlitImage, m_Blit)
    return nullptr;
}

struct Env { PFN_vkGetInstanceProcAddr gipa; PFN_vkGetDeviceProcAddr gdpa; VkInstance inst; VkDevice dev; };
static Env Bring(void* lib, bool loader_callback = true) {
    auto negotiate = (PFN_vkNegotiateLoaderLayerInterfaceVersion)dlsym(lib, "FrameGen_NegotiateLoaderLayerInterfaceVersion");
    VkNegotiateLayerInterface nl{}; nl.sType = LAYER_NEGOTIATE_INTERFACE_STRUCT; nl.loaderLayerInterfaceVersion = 2; CHECK(negotiate(&nl) == VK_SUCCESS);
    Env e{nl.pfnGetInstanceProcAddr, nl.pfnGetDeviceProcAddr, {}, {}};
    VkLayerInstanceLink il{nullptr, next_gipa, nullptr};
    VkLayerInstanceCreateInfo lici{}; lici.sType = VK_STRUCTURE_TYPE_LOADER_INSTANCE_CREATE_INFO; lici.function = VK_LAYER_LINK_INFO; lici.u.pLayerInfo = &il;
    VkApplicationInfo ai{VK_STRUCTURE_TYPE_APPLICATION_INFO}; ai.apiVersion = VK_API_VERSION_1_1;
    const char* iexts[] = {VK_KHR_SURFACE_EXTENSION_NAME};
    VkInstanceCreateInfo ici{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, &lici}; ici.pApplicationInfo = &ai; ici.enabledExtensionCount = 1; ici.ppEnabledExtensionNames = iexts;
    CHECK(((PFN_vkCreateInstance)e.gipa(VK_NULL_HANDLE, "vkCreateInstance"))(&ici, nullptr, &e.inst) == VK_SUCCESS);
    VkLayerDeviceLink dl{nullptr, nullptr, next_gdpa};
    VkLayerDeviceCreateInfo cb{}; cb.sType = VK_STRUCTURE_TYPE_LOADER_DEVICE_CREATE_INFO; cb.function = VK_LOADER_DATA_CALLBACK; cb.u.pfnSetDeviceLoaderData = m_SetLoaderData;
    VkLayerDeviceCreateInfo ldci{}; ldci.sType = VK_STRUCTURE_TYPE_LOADER_DEVICE_CREATE_INFO; ldci.function = VK_LAYER_LINK_INFO; ldci.u.pLayerInfo = &dl;
    if (loader_callback) { ldci.pNext = &cb; }
    float prio = 1; VkDeviceQueueCreateInfo qci{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO}; qci.queueCount = 1; qci.pQueuePriorities = &prio;
    const char* dexts[] = {VK_KHR_SWAPCHAIN_EXTENSION_NAME};
    VkDeviceCreateInfo dci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, &ldci}; dci.enabledExtensionCount = 1; dci.ppEnabledExtensionNames = dexts; dci.queueCreateInfoCount = 1; dci.pQueueCreateInfos = &qci;
    CHECK(((PFN_vkCreateDevice)e.gipa(e.inst, "vkCreateDevice"))((VkPhysicalDevice)&g_pd, &dci, nullptr, &e.dev) == VK_SUCCESS);
    return e;
}


// Swapchain capacity for synthetic frames: C = min(limit, max(MAX_SYNTHETIC at start, 4)); the swapchain is asked for app_min + C + 2 images; what the driver
// grants limits C further (C_eff = granted - app_min - 2); EXPERIMENTAL only raises the ceiling. argv[2] names the scenario.
static std::string Slurp(const std::string& p) { std::ifstream f(p); std::stringstream s; s << f.rdbuf(); return s.str(); }
int main(int, char** argv) {
    ClearFramegenEnv();
    const std::string sc_name = argv[2];
    char dir[] = "/tmp/vkicap_XXXXXX"; if (!mkdtemp(dir)) return 2;
    const std::string conf = std::string(dir) + "/live.conf", logf = std::string(dir) + "/live.log";
    setenv("VKIFRAME_LIVE_CONF", conf.c_str(), 1); setenv("VKIFRAME_LIVE_LOG", logf.c_str(), 1); setenv("XDG_CONFIG_HOME", dir, 1);
    auto wconf = [&](const std::string& t) { std::ofstream f(conf, std::ios::trunc); f << t; };
    struct Sc { const char* name; int env_max; bool env_exp; const char* conf; uint32_t surface_max; uint32_t want_images; int want_cap; };
    // want_images is what the layer asks for with the app's 3 (3 + C + 2), then limited by the surface; want_cap is C_eff
    const Sc table[] = {
        {"default",          0, false, "",                                        0, 9,  4},     // C = 4: unchanged
        {"exp3",             3, true,  "",                                        0, 9,  4},     // EXPERIMENTAL with MAX_SYNTHETIC 3: no extra images
        {"exp4",             4, true,  "",                                        0, 9,  4},
        {"exp8",             8, true,  "",                                        0, 13, 8},
        {"exp10",           10, true,  "",                                        0, 15, 10},
        {"noexp8",           8, false, "",                                        0, 9,  4},     // above 4 needs EXPERIMENTAL: C stays 4
        {"conf8",            3, true,  "VKIFRAME_MAX_SYNTHETIC=8\n",              0, 13, 8},     // live.conf over the environment
        {"conf8_confexp",    0, false, "VKIFRAME_EXPERIMENTAL=1\nVKIFRAME_MAX_SYNTHETIC=8\n", 0, 13, 8},   // both from live.conf
        {"conf3_env8",       8, true,  "VKIFRAME_MAX_SYNTHETIC=3\n",              0, 9,  4},     // the file says 3, it wins over the environment's 8
        {"conf10_noexp",     0, false, "VKIFRAME_MAX_SYNTHETIC=10\n",             0, 9,  4},
        {"max7",            10, true,  "",                                        7, 7,  2},     // the surface grants only 7: C_eff = 7 - 3 - 2
        {"max5",            10, true,  "",                                        5, 5,  0},     // not even one synthetic frame fits: frame holding is off (pass-through), capacity 0
        {"max6_default",     0, false, "",                                        6, 6,  1},
    };
    const Sc* sc = nullptr; for (const Sc& t : table) if (sc_name == t.name) sc = &t;
    const bool flow_test = sc_name == "flow_full" || sc_name == "flow_small";
    if (!sc && !flow_test) { std::printf("unknown scenario %s\n", sc_name.c_str()); return 2; }
    if (sc) {
        if (sc->env_max) setenv("VKIFRAME_MAX_SYNTHETIC", std::to_string(sc->env_max).c_str(), 1);
        if (sc->env_exp) setenv("VKIFRAME_EXPERIMENTAL", "1", 1);
        if (*sc->conf) wconf(sc->conf);
        k_surface_max = sc->surface_max;
    } else {
        setenv("VKIFRAME_TARGET_FPS", "160", 1); setenv("VKIFRAME_BASE_FPS_CAP", "0", 1);      // 40 fps source against 160: four synthetic frames are wanted
        k_surface_max = sc_name == "flow_small" ? 6 : 0;                                       // 6 images: capacity 1
    }
    void* lib = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL); CHECK(lib);
    Env e = Bring(lib);
    VkQueue q; ((PFN_vkGetDeviceQueue)e.gdpa(e.dev, "vkGetDeviceQueue"))(e.dev, 0, 0, &q);
    auto csc = (PFN_vkCreateSwapchainKHR)e.gdpa(e.dev, "vkCreateSwapchainKHR");
    VkSwapchainCreateInfoKHR sci{VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR}; sci.minImageCount = 3; sci.imageFormat = VK_FORMAT_B8G8R8A8_SRGB;
    sci.imageExtent = {640, 480}; sci.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT; VkSwapchainKHR swc{};
    CHECK(csc(e.dev, &sci, nullptr, &swc) == VK_SUCCESS);
    std::fflush(nullptr);
    const std::string log = Slurp(logf);
    if (sc) {
        CHECK(r_min_images == (sc->want_cap == 0 ? 3u : sc->want_images));
        if (sc->want_cap == 0) {
            // pass-through: the swapchain was made once with the layer's extras, found too small for a synthetic frame, and made again exactly as the game asked
            // (3 images, its usage, flags and present mode), with the first one as oldSwapchain; the first was destroyed and the game got the second
            CHECK(r_creates == 2 && r_min_images == 3 && r_usage == VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT && r_flags_last == 0 && r_mode_last == sci.presentMode);
            CHECK(r_old_last == (VkSwapchainKHR)(uintptr_t)(0x77 + 1) && r_destroyed_sc.size() == 1 && r_destroyed_sc[0] == r_old_last);
            CHECK(swc == (VkSwapchainKHR)(uintptr_t)(0x77 + 2));                                                                      // pass-through: no capacity line, holding off, every present forwarded as it is
            CHECK(log.find("frame holding is off for this swapchain") != std::string::npos);
            CHECK(log.find("capacity ") == std::string::npos);
            auto qp = (PFN_vkQueuePresentKHR)e.gdpa(e.dev, "vkQueuePresentKHR");
            for (uint32_t f = 0; f < 40; ++f) {
                usleep(10'000);
                VkSemaphore game_sem = (VkSemaphore)(uintptr_t)(5000 + f); uint32_t idx = f % 4;
                VkPresentInfoKHR pi{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR}; pi.waitSemaphoreCount = 1; pi.pWaitSemaphores = &game_sem; pi.swapchainCount = 1; pi.pSwapchains = &swc; pi.pImageIndices = &idx;
                VkResult res = VK_NOT_READY; pi.pResults = &res; CHECK(qp(q, &pi) == VK_SUCCESS && res == VK_SUCCESS);
            }
            usleep(200'000);
            CHECK(r_presents.size() == 40);                                                           // one present per game frame, no synthetic frame
            for (size_t i = 0; i < r_presents.size(); ++i) CHECK(r_presents[i].image == uint32_t(i % 4));   // the game's own image indices
            std::printf("capacity %s: ok (pass-through, %zu presents for 40 game frames)\n", sc->name, r_presents.size());
            std::string c0 = std::string("rm -rf ") + dir; if (std::system(c0.c_str()) != 0) (void)0;
            return 0;
        }
        char want[80]; std::snprintf(want, sizeof want, "capacity %d", sc->want_cap);
        CHECK(log.find(want) != std::string::npos);
        CHECK((log.find("fewer than wanted") != std::string::npos) == (sc->surface_max != 0));        // only a surface that limits the image count makes the capacity smaller than wanted
        std::printf("capacity %s: ok (minImageCount asked %u, %s)\n", sc->name, r_min_images, want);
        std::string cmd = std::string("rm -rf ") + dir; if (std::system(cmd.c_str()) != 0) (void)0;
        return 0;
    }
    // flow: how many frames are shown per game frame with the capacity the swapchain has
    auto qp = (PFN_vkQueuePresentKHR)e.gdpa(e.dev, "vkQueuePresentKHR");
    uint32_t frame = 0;
    for (int i = 0; i < 100; ++i) {
        usleep(25'000);
        VkSemaphore game_sem = (VkSemaphore)(uintptr_t)(5000 + frame); uint32_t idx = frame % 4; ++frame;
        VkPresentInfoKHR pi{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR}; pi.waitSemaphoreCount = 1; pi.pWaitSemaphores = &game_sem; pi.swapchainCount = 1; pi.pSwapchains = &swc; pi.pImageIndices = &idx;
        VkResult res = VK_NOT_READY; pi.pResults = &res; CHECK(qp(q, &pi) == VK_SUCCESS && res == VK_SUCCESS);
    }
    usleep(200'000);
    const double per_native = double(r_presents.size()) / double(frame);
    int hidden_max = 0; for (auto& p : r_presents) hidden_max = std::max(hidden_max, int(p.image));
    CHECK(hidden_max < int(g_granted));                                                  // no image the swapchain does not have
    std::printf("capacity %s: %zu frames shown for %u game frames (%.2f per game frame), granted %u images\n", sc_name.c_str(), r_presents.size(), frame, per_native, g_granted);
    if (sc_name == "flow_small") CHECK(per_native <= 2.1);                               // capacity 1: a native frame and at most one synthetic frame
    else CHECK(per_native > 2.6);                                                         // capacity 3 or 4: the wanted three synthetic frames show up
    std::string cmd = std::string("rm -rf ") + dir; if (std::system(cmd.c_str()) != 0) return 1;
    return 0;
}
