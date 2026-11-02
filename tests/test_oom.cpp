// Host out-of-memory test (no GPU needed): the layer's entry points must survive a failing allocation. The test replaces the global operator new so that the
// Nth allocation of the calling thread throws std::bad_alloc, and sweeps N over one call of each kind (create instance and device, create swapchain, the first
// present with the GPU setup, a steady-state present while a frame is held, destroy swapchain, device and instance). Every call must return a valid VkResult
// (VK_SUCCESS or VK_ERROR_OUT_OF_HOST_MEMORY), never abort or hang, never present one game frame twice, and the driver's destroy must run exactly once.
// The layer is loaded from a copy built with the dynamic C++ runtime (vkIFrames_oom): the static runtime of the release library has its own operator new.
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
#include <atomic>
#include <thread>
#include <mutex>
#include <chrono>
#include <pthread.h>
#include <new>
#include <cstdlib>
#define CHECK(c) do { if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); std::exit(1); } } while (0)

struct Fake { void* dispatch; };
static Fake g_inst{(void*)0x1111}, g_pd{(void*)0x1111}, g_dev{(void*)0x2222}, g_q{(void*)0x2222};
static std::vector<Fake*> g_cbs;
static thread_local bool t_waiter = false;                       // the thread that waits on the held frame's fence
static std::atomic<int> g_waiter_state{0};                       // 1: the waiter's flush is inside the driver's present, 2: it left
static std::atomic<bool> g_destroy_done{false}, g_violation{false};   // the driver's vkDestroySwapchainKHR ran; it ran while the waiter was still inside the layer
static std::atomic<bool> g_probe{false}, g_go{false}, g_in_idle{false}, g_overlap{false};
static pthread_t g_main_thread;


// ---- allocation failure injection (this thread: the Nth allocation throws; any thread: g_global_at counts down)
enum { MODE_OFF = 0, MODE_COUNT = 1, MODE_FAIL = 2 };
static thread_local int t_mode = MODE_OFF; static thread_local long t_n = 0, t_at = 0;
static std::atomic<long> g_global_at{0};
static void* Alloc(size_t n) {
    if (t_mode == MODE_COUNT) ++t_n;
    else if (t_mode == MODE_FAIL && ++t_n == t_at) { t_mode = MODE_OFF; throw std::bad_alloc(); }
    if (g_global_at.load() > 0 && g_global_at.fetch_sub(1) == 1) throw std::bad_alloc();
    void* p = std::malloc(n ? n : 1); if (!p) throw std::bad_alloc(); return p;
}
void* operator new(size_t n) { return Alloc(n); }
void* operator new[](size_t n) { return Alloc(n); }
void* operator new(size_t n, const std::nothrow_t&) noexcept { try { return Alloc(n); } catch (...) { return nullptr; } }
void* operator new[](size_t n, const std::nothrow_t&) noexcept { try { return Alloc(n); } catch (...) { return nullptr; } }
void operator delete(void* p) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete(void* p, size_t) noexcept { std::free(p); }
void operator delete[](void* p, size_t) noexcept { std::free(p); }

static std::atomic<uint64_t> g_next_sc{0};                                   // every swapchain gets its own handle, as a driver does
static std::atomic<int> g_sc_created{0}, g_sc_destroyed_total{0};
static std::atomic<int> g_driver_destroys{0}, g_driver_device_destroys{0}, g_driver_instance_destroys{0};
// ---- records
struct Submit { std::vector<VkSemaphore> waits; VkSemaphore signal; };
struct Present { VkSemaphore wait; uint32_t image; uint64_t id; VkFence fence; int mode; };
static std::mutex g_rec_mu;                                       // the presenter thread records presents and submits while the main thread reads and clears them
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
static void VKAPI_CALL m_DestroyInstance(VkInstance, const VkAllocationCallbacks*) { ++g_driver_instance_destroys; }
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
    r_usage = ci->imageUsage; r_min_images = ci->minImageCount; *s = (VkSwapchainKHR)(uintptr_t)(0x77 + g_next_sc++); ++g_sc_created; return VK_SUCCESS;
}
static void VKAPI_CALL m_DestroyDevice(VkDevice, const VkAllocationCallbacks*) { ++g_driver_device_destroys; }
static void VKAPI_CALL m_DestroySwapchain(VkDevice, VkSwapchainKHR, const VkAllocationCallbacks*) { g_destroy_done = true; ++g_driver_destroys; ++g_sc_destroyed_total; }
static void VKAPI_CALL m_GetQueue(VkDevice, uint32_t, uint32_t, VkQueue* q) { *q = (VkQueue)&g_q; }
static VkResult VKAPI_CALL m_SwapImages(VkDevice, VkSwapchainKHR, uint32_t* n, VkImage* p) { if (!p) { *n = 8; return VK_SUCCESS; } for (uint32_t i = 0; i < *n; ++i) p[i] = (VkImage)(uintptr_t)(1000 + i); return VK_SUCCESS; }
static VkResult VKAPI_CALL m_WaitIdle(VkDevice) {
    if (g_probe) { g_in_idle = true; g_go = true; usleep(200'000); g_in_idle = false; }       // a game thread submits to the queue now
    return VK_SUCCESS;
}
static VkResult VKAPI_CALL m_QueueSubmit1(VkQueue, uint32_t, const VkSubmitInfo*, VkFence) { if (g_in_idle) g_overlap = true; return VK_SUCCESS; }
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
static VkResult VKAPI_CALL m_GetFenceStatus(VkDevice, VkFence) { return VK_SUCCESS; }
static VkResult VKAPI_CALL m_WaitForPresent(VkDevice, VkSwapchainKHR, uint64_t, uint64_t) { return VK_SUCCESS; }
static VkResult VKAPI_CALL m_ResetFences(VkDevice, uint32_t, const VkFence*) { return VK_SUCCESS; }
static VkResult VKAPI_CALL m_Submit2(VkQueue, uint32_t, const VkSubmitInfo2* si, VkFence) {
    Submit s; for (uint32_t i = 0; i < si->waitSemaphoreInfoCount; ++i) s.waits.push_back(si->pWaitSemaphoreInfos[i].semaphore);
    CHECK(si->signalSemaphoreInfoCount == 1); s.signal = si->pSignalSemaphoreInfos[0].semaphore; { std::lock_guard<std::mutex> rl(g_rec_mu); r_submits.push_back(s); r_events.push_back("S"); } return VK_SUCCESS;
}
static void VKAPI_CALL m_Barrier2(VkCommandBuffer, const VkDependencyInfo*) { ++r_barriers; }
static void VKAPI_CALL m_Blit(VkCommandBuffer, VkImage, VkImageLayout, VkImage, VkImageLayout, uint32_t, const VkImageBlit*, VkFilter) { ++r_blits; }
static VkResult VKAPI_CALL m_Present(VkQueue, const VkPresentInfoKHR* pi) {
    if (t_waiter && g_waiter_state == 0) {
        g_waiter_state = 1;
        for (int i = 0; i < 600 && !g_destroy_done; ++i) usleep(1000);
        if (g_destroy_done) g_violation = true;                    // the swapchain was destroyed (driver call made) while this thread was still in the layer
        g_waiter_state = 2;
    }
    CHECK(pi->swapchainCount == 1 && pi->waitSemaphoreCount <= 1);
    uint64_t pid = 0; for (auto* c = (const VkBaseInStructure*)pi->pNext; c; c = c->pNext) if (c->sType == VK_STRUCTURE_TYPE_PRESENT_ID_KHR) { auto* x = (const VkPresentIdKHR*)c; CHECK(x->swapchainCount == 1); pid = x->pPresentIds[0]; }
    VkFence fence = VK_NULL_HANDLE; for (auto* c = (const VkBaseInStructure*)pi->pNext; c; c = c->pNext) if (c->sType == VK_STRUCTURE_TYPE_SWAPCHAIN_PRESENT_FENCE_INFO_KHR) { auto* x = (const VkSwapchainPresentFenceInfoKHR*)c; CHECK(x->swapchainCount == 1); fence = x->pFences[0]; }
    int mode = -1; for (auto* c = (const VkBaseInStructure*)pi->pNext; c; c = c->pNext) if (c->sType == VK_STRUCTURE_TYPE_SWAPCHAIN_PRESENT_MODE_INFO_KHR) { auto* x = (const VkSwapchainPresentModeInfoKHR*)c; CHECK(x->swapchainCount == 1); mode = int(x->pPresentModes[0]); }
    { std::lock_guard<std::mutex> rl(g_rec_mu); r_presents.push_back({pi->waitSemaphoreCount ? pi->pWaitSemaphores[0] : VK_NULL_HANDLE, pi->pImageIndices[0], pid, fence, mode}); r_events.push_back("P"); }
    if (pi->pResults) pi->pResults[0] = VK_SUCCESS; return VK_SUCCESS;
}
static VkResult VKAPI_CALL m_SetLoaderData(VkDevice, void* obj) { ++r_loader_data_calls; ((Fake*)obj)->dispatch = (void*)0x2222; return VK_SUCCESS; }


static std::atomic<int> r_dispatches{0}, r_acquires{0}, r_desc_updates{0}, r_pipelines{0}, r_image_views{0};      // written by the presenter thread, read by the main thread
static std::vector<float> r_t; static bool k_acquire_fail = false;
static std::vector<VkSemaphore> r_acq_sems; static std::atomic<uint32_t> g_acq_next{0};
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
    R(CreateSwapchainKHR, m_CreateSwapchain) R(DestroySwapchainKHR, m_DestroySwapchain) R(DestroyDevice, m_DestroyDevice) R(QueuePresentKHR, m_Present) R(GetDeviceQueue, m_GetQueue)
    R(GetSwapchainImagesKHR, m_SwapImages) R(DeviceWaitIdle, m_WaitIdle) R(CreateImage, m_CreateImage) R(DestroyImage, m_DestroyImage)
    R(GetImageMemoryRequirements, m_ImgReq) R(AllocateMemory, m_Alloc) R(FreeMemory, m_Free) R(BindImageMemory, m_Bind)
    R(CreateCommandPool, m_CreatePool) R(DestroyCommandPool, m_DestroyPool) R(AllocateCommandBuffers, m_AllocCb) R(BeginCommandBuffer, m_Begin)
    R(EndCommandBuffer, m_End) R(ResetCommandBuffer, m_ResetCb) R(CreateSemaphore, m_CreateSem) R(DestroySemaphore, m_DestroySem)
    R(CreateFence, m_CreateFence) R(DestroyFence, m_DestroyFence) R(WaitForFences, m_WaitFences) R(GetFenceStatus, m_GetFenceStatus) R(WaitForPresentKHR, m_WaitForPresent) R(ResetFences, m_ResetFences)
    R(CreateImageView, m_CreateView) R(DestroyImageView, m_DestroyView) R(CreateShaderModule, m_CreateShader) R(DestroyShaderModule, m_DestroyShader)
    R(CreateDescriptorSetLayout, m_CreateSetLayout) R(DestroyDescriptorSetLayout, m_DestroySetLayout) R(CreatePipelineLayout, m_CreatePipeLayout) R(DestroyPipelineLayout, m_DestroyPipeLayout)
    R(CreateComputePipelines, m_CreateCompute) R(DestroyPipeline, m_DestroyPipeline) R(CreateDescriptorPool, m_CreateDescPool) R(DestroyDescriptorPool, m_DestroyDescPool)
    R(AllocateDescriptorSets, m_AllocSets) R(UpdateDescriptorSets, m_UpdateSets) R(CmdBindPipeline, m_BindPipe) R(CmdBindDescriptorSets, m_BindSets)
    R(CmdPushConstants, m_PushConst) R(CmdDispatch, m_Dispatch) R(AcquireNextImageKHR, m_Acquire)
    R(QueueSubmit, m_QueueSubmit1) R(QueueSubmit2KHR, m_Submit2) R(CmdPipelineBarrier2KHR, m_Barrier2) R(CmdBlitImage, m_Blit)
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


static const VkResult kOom = VK_ERROR_OUT_OF_HOST_MEMORY;
static bool Valid(VkResult r) { return r == VK_SUCCESS || r == kOom; }

int main(int, char** argv) {
    std::setvbuf(stdout, nullptr, _IONBF, 0);                                       // a failure shows what was running
    ClearFramegenEnv();
    setenv("VKIFRAME_LOG_FILE", "/dev/null", 1);                                     // the layer's lines are not the point here
    g_main_thread = pthread_self();
    void* lib = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL); CHECK(lib);
    long oom_seen = 0;

    // ---- create instance and device: a failing allocation gives VK_SUCCESS or VK_ERROR_OUT_OF_HOST_MEMORY, and a clean bring-up still works afterwards
    {
        auto negotiate = (PFN_vkNegotiateLoaderLayerInterfaceVersion)dlsym(lib, "FrameGen_NegotiateLoaderLayerInterfaceVersion");
        VkNegotiateLayerInterface nl{}; nl.sType = LAYER_NEGOTIATE_INTERFACE_STRUCT; nl.loaderLayerInterfaceVersion = 2; CHECK(negotiate(&nl) == VK_SUCCESS);
        auto make_instance = [&](VkInstance* out) {
            VkLayerInstanceLink il{nullptr, next_gipa, nullptr};
            VkLayerInstanceCreateInfo lici{}; lici.sType = VK_STRUCTURE_TYPE_LOADER_INSTANCE_CREATE_INFO; lici.function = VK_LAYER_LINK_INFO; lici.u.pLayerInfo = &il;
            VkApplicationInfo ai{VK_STRUCTURE_TYPE_APPLICATION_INFO}; ai.apiVersion = VK_API_VERSION_1_1;
            const char* iexts[] = {VK_KHR_SURFACE_EXTENSION_NAME};
            VkInstanceCreateInfo ici{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, &lici}; ici.pApplicationInfo = &ai; ici.enabledExtensionCount = 1; ici.ppEnabledExtensionNames = iexts;
            return ((PFN_vkCreateInstance)nl.pfnGetInstanceProcAddr(VK_NULL_HANDLE, "vkCreateInstance"))(&ici, nullptr, out);
        };
        long k = 0;
        { VkInstance in{}; t_mode = MODE_COUNT; t_n = 0; CHECK(make_instance(&in) == VK_SUCCESS); k = t_n; t_mode = MODE_OFF;
          ((PFN_vkDestroyInstance)nl.pfnGetInstanceProcAddr(in, "vkDestroyInstance"))(in, nullptr); }
        CHECK(k > 3);
        for (long n = 1; n <= k + 1; ++n) {
            VkInstance in{}; t_mode = MODE_FAIL; t_n = 0; t_at = n;
            const VkResult r = make_instance(&in); t_mode = MODE_OFF;
            CHECK(Valid(r)); if (r == kOom) ++oom_seen;
            if (r == VK_SUCCESS) ((PFN_vkDestroyInstance)nl.pfnGetInstanceProcAddr(in, "vkDestroyInstance"))(in, nullptr);
        }
    }
    Env e = Bring(lib);                                                              // after all that, a clean bring-up works
    {   // devices: the same sweep over vkCreateDevice
        auto cd = (PFN_vkCreateDevice)e.gipa(e.inst, "vkCreateDevice");
        auto make_device = [&](VkDevice* out) {                                       // a fresh link chain for every call, as the loader gives
            VkLayerDeviceLink dl{nullptr, nullptr, next_gdpa};
            VkLayerDeviceCreateInfo ldci{}; ldci.sType = VK_STRUCTURE_TYPE_LOADER_DEVICE_CREATE_INFO; ldci.function = VK_LAYER_LINK_INFO; ldci.u.pLayerInfo = &dl;
            VkLayerDeviceCreateInfo cbk{}; cbk.sType = VK_STRUCTURE_TYPE_LOADER_DEVICE_CREATE_INFO; cbk.function = VK_LOADER_DATA_CALLBACK; cbk.u.pfnSetDeviceLoaderData = m_SetLoaderData; ldci.pNext = &cbk;
            float prio = 1; VkDeviceQueueCreateInfo qci{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO}; qci.queueCount = 1; qci.pQueuePriorities = &prio;
            const char* dexts[] = {VK_KHR_SWAPCHAIN_EXTENSION_NAME};
            VkDeviceCreateInfo dci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, &ldci}; dci.enabledExtensionCount = 1; dci.ppEnabledExtensionNames = dexts; dci.queueCreateInfoCount = 1; dci.pQueueCreateInfos = &qci;
            return cd((VkPhysicalDevice)&g_pd, &dci, nullptr, out);
        };
        long k = 0;
        { VkDevice d2{}; t_mode = MODE_COUNT; t_n = 0; CHECK(make_device(&d2) == VK_SUCCESS); k = t_n; t_mode = MODE_OFF;
          ((PFN_vkDestroyDevice)e.gdpa(d2, "vkDestroyDevice"))(d2, nullptr); }
        CHECK(k > 3);
        for (long n = 1; n <= k + 1; ++n) {
            VkDevice d{}; t_mode = MODE_FAIL; t_n = 0; t_at = n;
            const VkResult r = make_device(&d); t_mode = MODE_OFF;
            if (!Valid(r)) std::printf("create-device N=%ld returned %d\n", n, int(r));
            CHECK(Valid(r)); if (r == kOom) ++oom_seen;
            if (r == VK_SUCCESS) ((PFN_vkDestroyDevice)e.gdpa(d, "vkDestroyDevice"))(d, nullptr);
        }
    }
    e = Bring(lib);
    VkQueue q; ((PFN_vkGetDeviceQueue)e.gdpa(e.dev, "vkGetDeviceQueue"))(e.dev, 0, 0, &q);
    auto csc = (PFN_vkCreateSwapchainKHR)e.gdpa(e.dev, "vkCreateSwapchainKHR");
    auto dsc = (PFN_vkDestroySwapchainKHR)e.gdpa(e.dev, "vkDestroySwapchainKHR");
    auto qp = (PFN_vkQueuePresentKHR)e.gdpa(e.dev, "vkQueuePresentKHR");
    auto wf = (PFN_vkWaitForFences)e.gdpa(e.dev, "vkWaitForFences");
    auto gfs = (PFN_vkGetFenceStatus)e.gdpa(e.dev, "vkGetFenceStatus");
    auto acq = (PFN_vkAcquireNextImageKHR)e.gdpa(e.dev, "vkAcquireNextImageKHR");
    auto wfp = (PFN_vkWaitForPresentKHR)e.gdpa(e.dev, "vkWaitForPresentKHR");
    VkSwapchainCreateInfoKHR sci{VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR}; sci.minImageCount = 3; sci.imageFormat = VK_FORMAT_B8G8R8A8_SRGB;
    sci.imageExtent = {640, 480}; sci.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;

    uint32_t frame = 0; VkFence last_fence = VK_NULL_HANDLE; VkSwapchainKHR sc{};
    auto present_frame = [&](useconds_t gap_us) -> VkResult {
        if (gap_us) usleep(gap_us);
        VkSemaphore game_sem = (VkSemaphore)(uintptr_t)(5000 + frame); uint32_t idx = frame % 4; ++frame;
        uint64_t app_id = frame; VkPresentIdKHR pid{VK_STRUCTURE_TYPE_PRESENT_ID_KHR}; pid.swapchainCount = 1; pid.pPresentIds = &app_id;
        last_fence = (VkFence)(uintptr_t)(7000 + frame);
        VkSwapchainPresentFenceInfoKHR fi{VK_STRUCTURE_TYPE_SWAPCHAIN_PRESENT_FENCE_INFO_KHR}; fi.swapchainCount = 1; fi.pFences = &last_fence; pid.pNext = &fi;
        VkPresentInfoKHR pi{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR, &pid}; pi.waitSemaphoreCount = 1; pi.pWaitSemaphores = &game_sem; pi.swapchainCount = 1; pi.pSwapchains = &sc; pi.pImageIndices = &idx;
        VkResult res = VK_NOT_READY; pi.pResults = &res;
        const VkResult r = qp(q, &pi);
        CHECK(Valid(r)); if (r == VK_SUCCESS) CHECK(res == VK_SUCCESS); else CHECK(res == kOom || res == VK_NOT_READY);
        return r;
    };
    auto no_double_present = [&] {                                                   // a game frame (its present id) reaches the driver at most once
        std::lock_guard<std::mutex> rl(g_rec_mu); std::set<uint64_t> seen; for (auto& p : r_presents) if (p.id) { CHECK(seen.insert(p.id).second); }
    };
    auto sweep = [&](const char* what, auto&& prepare, auto&& op, auto&& after) {      // prepare() -> count run -> N = 1..K+1, each on a fresh prepare()
        prepare(); t_mode = MODE_COUNT; t_n = 0; op(); t_mode = MODE_OFF; const long k = t_n; after(true, VK_SUCCESS);
        (void)k;                                                                       // some hooks do not allocate on the normal path: the sweep is then one plain call
        long ooms = 0;
        for (long n = 1; n <= k + 1; ++n) {
            prepare(); t_mode = MODE_FAIL; t_n = 0; t_at = n; const VkResult r = op(); t_mode = MODE_OFF;
            if (!Valid(r)) std::printf("sweep %s N=%ld returned %d\n", what, n, int(r)); CHECK(Valid(r)); if (r == kOom) { ++ooms; ++oom_seen; }
            after(false, r);
        }
        std::printf("oom sweep %-34s %ld allocations, %ld calls failed with VK_ERROR_OUT_OF_HOST_MEMORY\n", what, k, ooms);
    };

    // ---- create swapchain
    int made = 0;
    sweep("vkCreateSwapchainKHR", [&] {}, [&]() -> VkResult { VkSwapchainKHR s{}; const VkResult r = csc(e.dev, &sci, nullptr, &s); if (r == VK_SUCCESS) { sc = s; ++made; } return r; },
          [&](bool, VkResult r) { if (r == VK_SUCCESS || r == VK_NOT_READY) { if (sc) { dsc(e.dev, sc, nullptr); sc = {}; } } });

    // ---- the first present of a swapchain (GPU state setup), then the swapchain is destroyed: the driver's destroy runs exactly once per swapchain
    auto fresh = [&] { { std::lock_guard<std::mutex> rl(g_rec_mu); r_presents.clear(); r_events.clear(); r_submits.clear(); } CHECK(csc(e.dev, &sci, nullptr, &sc) == VK_SUCCESS); frame = 0; g_driver_destroys = 0; };
    sweep("first vkQueuePresentKHR (GPU setup)", fresh, [&]() -> VkResult { return present_frame(0); },
          [&](bool, VkResult) { present_frame(0); present_frame(0); no_double_present(); dsc(e.dev, sc, nullptr); CHECK(g_driver_destroys == 1); sc = {}; });

    // ---- a steady-state present while the layer holds a frame (gate open after the warm-up), then more presents with no failure
    CHECK(csc(e.dev, &sci, nullptr, &sc) == VK_SUCCESS); frame = 0; { std::lock_guard<std::mutex> rl(g_rec_mu); r_presents.clear(); }
    for (int i = 0; i < 80; ++i) CHECK(present_frame(25'000) == VK_SUCCESS);
    sweep("steady-state vkQueuePresentKHR", [&] { std::lock_guard<std::mutex> rl(g_rec_mu); r_presents.clear(); }, [&]() -> VkResult { return present_frame(25'000); },
          [&](bool, VkResult) { for (int i = 0; i < 3; ++i) present_frame(25'000); no_double_present(); });
    { std::lock_guard<std::mutex> rl(g_rec_mu); CHECK(r_presents.size() > 0); }

    // ---- wait and fence hooks: a failing allocation goes on to the real call
    {
        VkFence f = last_fence;
        sweep("vkWaitForFences", [&] {}, [&]() -> VkResult { return wf(e.dev, 1, &f, VK_TRUE, 1'000'000); }, [&](bool, VkResult) {});
        sweep("vkGetFenceStatus", [&] {}, [&]() -> VkResult { return gfs(e.dev, f); }, [&](bool, VkResult) {});
        sweep("vkWaitForPresentKHR", [&] {}, [&]() -> VkResult { return wfp(e.dev, sc, 1, 1'000'000); }, [&](bool, VkResult) {});
        uint32_t idx = 0;
        sweep("vkAcquireNextImageKHR", [&] {}, [&]() -> VkResult { return acq(e.dev, sc, 0, VK_NULL_HANDLE, VK_NULL_HANDLE, &idx); }, [&](bool, VkResult) {});
    }

    // ---- the presenter thread: a failing allocation in any thread during a stretch of presents must not end the process or leave the game waiting
    for (long n = 1; n <= 24; ++n) {
        g_global_at = n;
        for (int i = 0; i < 6; ++i) { const VkResult r = present_frame(25'000); CHECK(Valid(r)); }
        g_global_at = 0;
        for (int i = 0; i < 4; ++i) present_frame(25'000);
        no_double_present();
    }

    // ---- destroy: the driver's destroy runs exactly once, whatever fails on the way
    dsc(e.dev, sc, nullptr); sc = {};
    sweep("vkDestroySwapchainKHR", fresh, [&]() -> VkResult { for (int i = 0; i < 3; ++i) present_frame(0); g_driver_destroys = 0; t_mode = MODE_OFF; return VK_SUCCESS; }, [&](bool, VkResult) {});
    // (the sweep above only prepares the state; the destroy itself is swept next, armed after the presents)
    for (long n = 1; n <= 60; ++n) {
        fresh(); for (int i = 0; i < 3; ++i) present_frame(0);
        g_driver_destroys = 0; t_mode = MODE_FAIL; t_n = 0; t_at = n; dsc(e.dev, sc, nullptr); t_mode = MODE_OFF;
        CHECK(g_driver_destroys == 1); sc = {};
    }
    {
        long dd = g_driver_device_destroys; auto dev_destroy = (PFN_vkDestroyDevice)e.gdpa(e.dev, "vkDestroyDevice");
        t_mode = MODE_FAIL; t_n = 0; t_at = 1; dev_destroy(e.dev, nullptr); t_mode = MODE_OFF;
        CHECK(g_driver_device_destroys == dd + 1);                                   // the destroy reached the driver once
    }
    CHECK(oom_seen > 20);                                                            // the replacement operator new really reached the layer
    for (Fake* f : g_cbs) delete f;
    std::printf("PASS: host out-of-memory sweeps (%ld failed calls)\n", oom_seen);
    return 0;
}
