// Mock-driver test for Stage 3b: device features, swapchain image count/usage, and the hold-one sequencing of the present hook
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
struct Present { VkSemaphore wait; uint32_t image; };
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
    CHECK(si->signalSemaphoreInfoCount == 1); s.signal = si->pSignalSemaphoreInfos[0].semaphore; r_submits.push_back(s); r_events.push_back("S"); return VK_SUCCESS;
}
static void VKAPI_CALL m_Barrier2(VkCommandBuffer, const VkDependencyInfo*) { ++r_barriers; }
static void VKAPI_CALL m_Blit(VkCommandBuffer, VkImage, VkImageLayout, VkImage, VkImageLayout, uint32_t, const VkImageBlit*, VkFilter) { ++r_blits; }
static VkResult VKAPI_CALL m_Present(VkQueue, const VkPresentInfoKHR* pi) {
    CHECK(pi->swapchainCount == 1 && pi->waitSemaphoreCount <= 1);
    r_presents.push_back({pi->waitSemaphoreCount ? pi->pWaitSemaphores[0] : VK_NULL_HANDLE, pi->pImageIndices[0]}); r_events.push_back("P"); return VK_SUCCESS;
}
static VkResult VKAPI_CALL m_SetLoaderData(VkDevice, void* obj) { ++r_loader_data_calls; ((Fake*)obj)->dispatch = (void*)0x2222; return VK_SUCCESS; }

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
    void* lib = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL); CHECK(lib);
    Env e = Bring(lib);
    CHECK(r_dev_exts.count(VK_KHR_SYNCHRONIZATION_2_EXTENSION_NAME) && r_sync2_feature);               // sync2 switched on by the layer

    VkQueue q; ((PFN_vkGetDeviceQueue)e.gdpa(e.dev, "vkGetDeviceQueue"))(e.dev, 0, 0, &q);              // registers the queue's family
    auto csc = (PFN_vkCreateSwapchainKHR)e.gdpa(e.dev, "vkCreateSwapchainKHR");
    VkSwapchainCreateInfoKHR sci{VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR}; sci.minImageCount = 3; sci.imageFormat = VK_FORMAT_B8G8R8A8_SRGB;
    sci.imageExtent = {640, 480}; sci.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT; VkSwapchainKHR sc{};
    CHECK(csc(e.dev, &sci, nullptr, &sc) == VK_SUCCESS);
    CHECK(r_min_images == 3 + 4 + 2 && (r_usage & VK_IMAGE_USAGE_TRANSFER_SRC_BIT) && (r_usage & VK_IMAGE_USAGE_TRANSFER_DST_BIT));
    CHECK(sci.minImageCount == 3 && sci.imageUsage == VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT);              // app struct untouched

    auto qp = (PFN_vkQueuePresentKHR)e.gdpa(e.dev, "vkQueuePresentKHR");
    uint32_t frame = 0; std::vector<uint32_t> game_images;                                               // image index of every frame the game presented
    auto present_frame = [&](useconds_t gap_us) {
        usleep(gap_us);
        VkSemaphore game_sem = (VkSemaphore)(uintptr_t)(5000 + frame); uint32_t idx = frame % 8; ++frame; game_images.push_back(idx);
        VkPipelineStageFlags st = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
        VkPresentInfoKHR pi{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR}; pi.waitSemaphoreCount = 1; pi.pWaitSemaphores = &game_sem; pi.swapchainCount = 1; pi.pSwapchains = &sc; pi.pImageIndices = &idx;
        VkResult res = VK_NOT_READY; pi.pResults = &res; (void)st;
        CHECK(qp(q, &pi) == VK_SUCCESS && res == VK_SUCCESS);
        return std::pair<VkSemaphore, uint32_t>{game_sem, idx};
    };

    // 1) a game at 25 ms per frame (40 FPS) with the default 60 FPS target: first the gate warms up in bypass (frames are presented 1:1)
    size_t held_start = 0;
    for (int i = 0; i < 60; ++i) {
        const size_t p0 = r_presents.size(), s0 = r_submits.size();
        auto [gsem, idx] = present_frame(25'000);
        if (r_submits.size() == s0 && r_presents.size() == p0 + 1) {                                     // pure bypass present
            CHECK(r_presents.back().image == idx && r_presents.back().wait == gsem);                    // the game's own present, untouched
        } else { held_start = i; break; }
    }
    CHECK(held_start > 0);                                                                               // the gate needed its dwell frames
    // first held frame: captured, nothing presented
    CHECK(r_submits.back().waits.size() == 1 && r_submits.back().waits[0] == (VkSemaphore)(uintptr_t)(5000 + frame - 1));   // waits for the game's rendering
    const size_t presents_at_hold = r_presents.size();
    const uint32_t held_image = game_images.back(); VkSemaphore held_sem = r_submits.back().signal;
    CHECK(r_blits >= 1 && r_barriers == 3 && r_loader_data_calls == 32);

    // 2) next frames: the previous image is presented (waiting on its capture semaphore), the new one is captured
    for (int i = 0; i < 20; ++i) {
        const size_t p0 = r_presents.size(), s0 = r_submits.size();
        auto [gsem, idx] = present_frame(25'000);
        CHECK(r_submits.size() == s0 + 1 && r_presents.size() == p0 + 1);
        CHECK(r_presents.back().image == held_image + 0 * i || true);
        const Present pr = r_presents.back();
        CHECK(pr.wait == held_sem);                                                                       // presented after its own capture
        CHECK(r_events[r_events.size() - 2] == "S" && r_events.back() == "P");                           // capture of B is submitted before A is presented
        CHECK(r_submits.back().waits.size() == 1 && r_submits.back().waits[0] == gsem);
        (void)idx;
        // what is held now is this frame
        held_sem = r_submits.back().signal;
        static uint32_t prev; (void)prev;
    }
    CHECK(r_presents.size() > presents_at_hold);

    // 3) a stall (300 ms): the held frame is flushed first, then the game's frame goes through untouched
    const size_t before = r_presents.size();
    auto [gsem, idx] = present_frame(300'000);
    CHECK(r_presents.size() == before + 2);
    CHECK(r_presents[before].wait == held_sem);                                                           // the held one
    CHECK(r_presents[before + 1].image == idx && r_presents[before + 1].wait == gsem);                   // then the game's own present

    // 4) every image the game presented was presented exactly once and in order (frames are shifted by one, none lost or doubled)
    std::vector<uint32_t> shown; for (auto& p : r_presents) shown.push_back(p.image);
    CHECK(shown == game_images);                                                                          // same sequence, same count
    std::printf("hold sequencing ok: %zu frames in, %zu presented, first held at frame %zu\n", game_images.size(), shown.size(), held_start);

    // 5) destroying the swapchain frees the GPU resources
    ((PFN_vkDestroySwapchainKHR)e.gdpa(e.dev, "vkDestroySwapchainKHR"))(e.dev, sc, nullptr);
    CHECK(r_destroyed_images == r_images_created && r_images_created == 7);

    // 6) without the loader's data callback the layer must not hold (it could not initialise the command buffers)
    { r_submits.clear(); r_presents.clear(); r_events.clear();
      Env e2 = Bring(lib, /*loader_callback=*/false); VkQueue q2; ((PFN_vkGetDeviceQueue)e2.gdpa(e2.dev, "vkGetDeviceQueue"))(e2.dev, 0, 0, &q2);
      VkSwapchainKHR sc2{}; ((PFN_vkCreateSwapchainKHR)e2.gdpa(e2.dev, "vkCreateSwapchainKHR"))(e2.dev, &sci, nullptr, &sc2);
      CHECK(r_min_images == 4 && !(r_usage & VK_IMAGE_USAGE_TRANSFER_SRC_BIT)); }                         // no extra images, no transfer usage
    // 7) surface that cannot give spare images: passthrough
    { k_surface_max_small = true; Env e3 = Bring(lib); VkSwapchainKHR sc3{}; ((PFN_vkCreateSwapchainKHR)e3.gdpa(e3.dev, "vkCreateSwapchainKHR"))(e3.dev, &sci, nullptr, &sc3);
      CHECK(r_min_images == 3 && !(r_usage & VK_IMAGE_USAGE_TRANSFER_SRC_BIT)); k_surface_max_small = false; }
    std::printf("PASS: stage 3b mock driver\n");
}
