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
static bool k_surface_max_small = false;

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
    *c = {}; c->minImageCount = 2; c->maxImageCount = k_surface_max_small ? 3 : 0;
    c->supportedUsageFlags = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT; return VK_SUCCESS;
}
static void VKAPI_CALL m_FormatProps(VkPhysicalDevice, VkFormat, VkFormatProperties* p) {
    p->optimalTilingFeatures = VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT;
}
static void VKAPI_CALL m_QFProps(VkPhysicalDevice, uint32_t* n, VkQueueFamilyProperties* p) { *n = 1; p[0] = {}; p[0].queueFlags = VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT; p[0].queueCount = 1; }
static void VKAPI_CALL m_MemProps(VkPhysicalDevice, VkPhysicalDeviceMemoryProperties* p) { *p = {}; p->memoryTypeCount = 1; p->memoryTypes[0].propertyFlags = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT; }

static VkResult VKAPI_CALL m_CreateSwapchain(VkDevice, const VkSwapchainCreateInfoKHR* ci, const VkAllocationCallbacks*, VkSwapchainKHR* s) {
    r_usage = ci->imageUsage; r_min_images = ci->minImageCount; *s = (VkSwapchainKHR)(uintptr_t)0x77; return VK_SUCCESS;
}
static void VKAPI_CALL m_DestroySwapchain(VkDevice, VkSwapchainKHR, const VkAllocationCallbacks*) {}
static void VKAPI_CALL m_GetQueue(VkDevice, uint32_t, uint32_t, VkQueue* q) { *q = (VkQueue)&g_q; }
static VkResult VKAPI_CALL m_SwapImages(VkDevice, VkSwapchainKHR, uint32_t* n, VkImage* p) { if (!p) { *n = 8; return VK_SUCCESS; } for (uint32_t i = 0; i < *n; ++i) p[i] = (VkImage)(uintptr_t)(1000 + i); return VK_SUCCESS; }
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
static VkResult VKAPI_CALL m_Submit2(VkQueue, uint32_t, const VkSubmitInfo2* si, VkFence) {
    Submit s; for (uint32_t i = 0; i < si->waitSemaphoreInfoCount; ++i) s.waits.push_back(si->pWaitSemaphoreInfos[i].semaphore);
    CHECK(si->signalSemaphoreInfoCount <= 1); s.signal = si->signalSemaphoreInfoCount ? si->pSignalSemaphoreInfos[0].semaphore : VK_NULL_HANDLE; if (s.signal) r_submits.push_back(s); r_events.push_back("S"); return VK_SUCCESS;
}
static void VKAPI_CALL m_Barrier2(VkCommandBuffer, const VkDependencyInfo*) { ++r_barriers; }
static void VKAPI_CALL m_Blit(VkCommandBuffer, VkImage, VkImageLayout, VkImage, VkImageLayout, uint32_t, const VkImageBlit*, VkFilter) { ++r_blits; }
static VkResult VKAPI_CALL m_Present(VkQueue, const VkPresentInfoKHR* pi) {
    CHECK(pi->swapchainCount == 1 && pi->waitSemaphoreCount <= 1);
    uint64_t pid = 0; for (auto* c = (const VkBaseInStructure*)pi->pNext; c; c = c->pNext) if (c->sType == VK_STRUCTURE_TYPE_PRESENT_ID_KHR) { auto* x = (const VkPresentIdKHR*)c; CHECK(x->swapchainCount == 1); pid = x->pPresentIds[0]; }
    VkFence fence = VK_NULL_HANDLE; for (auto* c = (const VkBaseInStructure*)pi->pNext; c; c = c->pNext) if (c->sType == VK_STRUCTURE_TYPE_SWAPCHAIN_PRESENT_FENCE_INFO_KHR) { auto* x = (const VkSwapchainPresentFenceInfoKHR*)c; CHECK(x->swapchainCount == 1); fence = x->pFences[0]; }
    int mode = -1; for (auto* c = (const VkBaseInStructure*)pi->pNext; c; c = c->pNext) if (c->sType == VK_STRUCTURE_TYPE_SWAPCHAIN_PRESENT_MODE_INFO_KHR) { auto* x = (const VkSwapchainPresentModeInfoKHR*)c; CHECK(x->swapchainCount == 1); mode = int(x->pPresentModes[0]); }
    r_presents.push_back({pi->waitSemaphoreCount ? pi->pWaitSemaphores[0] : VK_NULL_HANDLE, pi->pImageIndices[0], pid, fence, mode}); r_events.push_back("P"); return VK_SUCCESS;
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
static void VKAPI_CALL m_UpdateSets(VkDevice, uint32_t n, const VkWriteDescriptorSet*, uint32_t, const VkCopyDescriptorSet*) { ++r_desc_updates; }
static void VKAPI_CALL m_BindPipe(VkCommandBuffer, VkPipelineBindPoint, VkPipeline) {}
static void VKAPI_CALL m_BindSets(VkCommandBuffer, VkPipelineBindPoint, VkPipelineLayout, uint32_t, uint32_t, const VkDescriptorSet*, uint32_t, const uint32_t*) {}
static std::vector<float> r_extrap_flags;
static void VKAPI_CALL m_PushConst(VkCommandBuffer, VkPipelineLayout, VkShaderStageFlags, uint32_t, uint32_t size, const void* v) { if (size == 40) { float f[10]; std::memcpy(f, v, 40); r_t.push_back(f[0]); r_extrap_flags.push_back(f[9]); } }
static void VKAPI_CALL m_Dispatch(VkCommandBuffer, uint32_t x, uint32_t y, uint32_t) { (void)x; (void)y; ++r_dispatches; }       // 640x480 / 16x16
static VkResult VKAPI_CALL m_Acquire(VkDevice, VkSwapchainKHR, uint64_t timeout, VkSemaphore sem, VkFence, uint32_t* idx) {
    CHECK(timeout <= 1'000'000);   // the game's acquire polls with 0; the presenter waits at most ACQUIRE_WAIT_US (1 ms) outside the capture mutex
    if (k_acquire_fail) return VK_ERROR_DEVICE_LOST;
    ++r_acquires; r_acq_sems.push_back(sem); *idx = 4 + (g_acq_next++ % 4); return VK_SUCCESS;
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

int main(int, char** argv) {
    ClearFramegenEnv();
    setenv("VKIFRAME_EXTRAPOLATE", "1", 1);
    void* lib = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL); CHECK(lib);
    Env e = Bring(lib);
    VkQueue q; ((PFN_vkGetDeviceQueue)e.gdpa(e.dev, "vkGetDeviceQueue"))(e.dev, 0, 0, &q);
    auto csc = (PFN_vkCreateSwapchainKHR)e.gdpa(e.dev, "vkCreateSwapchainKHR");
    VkSwapchainCreateInfoKHR sci{VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR}; sci.minImageCount = 3; sci.imageFormat = VK_FORMAT_B8G8R8A8_SRGB;
    sci.imageExtent = {640, 480}; sci.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT; VkSwapchainKHR sc{};
    CHECK(csc(e.dev, &sci, nullptr, &sc) == VK_SUCCESS);
    auto qp = (PFN_vkQueuePresentKHR)e.gdpa(e.dev, "vkQueuePresentKHR");

    // Extrapolation on the mock driver: nothing may be held back, every game frame must be presented (in order, with its own present id), and predicted
    // frames come out of the interpolation pass with the extrapolation flag set. What they look like is a Deck test.
    std::set<VkSemaphore> game_sems; uint32_t frame = 0;
    auto present_frame = [&](useconds_t gap_us) {
        usleep(gap_us);
        VkSemaphore game_sem = (VkSemaphore)(uintptr_t)(5000 + frame); uint32_t idx = frame % 4; ++frame; game_sems.insert(game_sem);
        uint64_t app_id = frame; VkPresentIdKHR pid{VK_STRUCTURE_TYPE_PRESENT_ID_KHR}; pid.swapchainCount = 1; pid.pPresentIds = &app_id;
        VkPresentInfoKHR pi{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR, &pid}; pi.waitSemaphoreCount = 1; pi.pWaitSemaphores = &game_sem; pi.swapchainCount = 1; pi.pSwapchains = &sc; pi.pImageIndices = &idx;
        VkResult res = VK_NOT_READY; pi.pResults = &res;
        CHECK(qp(q, &pi) == VK_SUCCESS && res == VK_SUCCESS);
    };
    for (int i = 0; i < 60; ++i) present_frame(25'000);
    usleep(150'000);                                                                   // let the presenter thread finish the last job
    std::vector<uint64_t> ids;
    for (auto& p : r_presents) { if (p.id) { CHECK(p.image < 4); ids.push_back(p.id); } else CHECK(p.image >= 4); }   // native frames: the game's images and ids; predicted: hidden images
    CHECK(ids.size() == size_t(frame));                                                 // every game frame was presented, none is still held back
    CHECK(r_presents.size() > ids.size());                                              // plus the predicted frames
    for (size_t i = 1; i < ids.size(); ++i) CHECK(ids[i] == ids[i - 1] + 1);           // in order, none skipped or repeated
    CHECK(!r_t.empty());                                                                // predicted frames were made (the mock runs the flow passes as no-ops)
    for (size_t i = 0; i < r_t.size(); ++i) { CHECK(r_extrap_flags[i] == 1.0f); CHECK(r_t[i] > 0.0f && r_t[i] <= 1.0f); }   // every interpolation push is an extrapolation with s in (0, 1]
    CHECK(r_acquires > 0);                                                              // predicted frames use hidden images

    ((PFN_vkDestroySwapchainKHR)e.gdpa(e.dev, "vkDestroySwapchainKHR"))(e.dev, sc, nullptr);
    std::printf("PASS: extrapolation mock driver (%zu presents for %u frames)\n", r_presents.size(), frame);
}
