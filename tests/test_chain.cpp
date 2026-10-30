// Mock-loader test for Stage 1: negotiation, instance/device chaining, proc-addr routing, swapchain image-count hook, present forwarding.
#include <vulkan/vulkan.h>
#include <vulkan/vk_layer.h>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <dlfcn.h>
#define CHECK(c) do { if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); std::exit(1); } } while (0)

struct Fake { void* dispatch; };             // loader dispatch word first, like a real dispatchable handle
static Fake g_inst{(void*)0x1111}, g_pd{(void*)0x1111}, g_dev{(void*)0x2222}, g_q{(void*)0x2222};
static int  g_inst_destroyed, g_dev_destroyed, g_present_calls, g_sc_created, g_sc_destroyed, g_unknown_calls;
static uint32_t g_seen_min_images; static uint32_t g_surface_max = 0;

static VkResult VKAPI_CALL m_CreateInstance(const VkInstanceCreateInfo*, const VkAllocationCallbacks*, VkInstance* p) { *p = (VkInstance)&g_inst; return VK_SUCCESS; }
static void VKAPI_CALL m_DestroyInstance(VkInstance, const VkAllocationCallbacks*) { ++g_inst_destroyed; }
static VkResult VKAPI_CALL m_CreateDevice(VkPhysicalDevice, const VkDeviceCreateInfo*, const VkAllocationCallbacks*, VkDevice* p) { *p = (VkDevice)&g_dev; return VK_SUCCESS; }
static void VKAPI_CALL m_DestroyDevice(VkDevice, const VkAllocationCallbacks*) { ++g_dev_destroyed; }
static VkResult VKAPI_CALL m_Caps(VkPhysicalDevice, VkSurfaceKHR, VkSurfaceCapabilitiesKHR* c) { *c = {}; c->minImageCount = 2; c->maxImageCount = g_surface_max; return VK_SUCCESS; }
static VkResult VKAPI_CALL m_CreateSwapchain(VkDevice, const VkSwapchainCreateInfoKHR* ci, const VkAllocationCallbacks*, VkSwapchainKHR* s) { g_seen_min_images = ci->minImageCount; ++g_sc_created; *s = (VkSwapchainKHR)(uintptr_t)0x77; return VK_SUCCESS; }
static void VKAPI_CALL m_DestroySwapchain(VkDevice, VkSwapchainKHR, const VkAllocationCallbacks*) { ++g_sc_destroyed; }
static VkResult VKAPI_CALL m_Present(VkQueue, const VkPresentInfoKHR*) { ++g_present_calls; return VK_SUCCESS; }
static void VKAPI_CALL m_Unknown() { ++g_unknown_calls; }

static PFN_vkVoidFunction VKAPI_CALL next_gipa(VkInstance, const char* n) {
#define R(x) if (!strcmp(n, "vk" #x)) return (PFN_vkVoidFunction)m_##x;
    R(CreateInstance) R(DestroyInstance) R(CreateDevice)
    if (!strcmp(n, "vkGetPhysicalDeviceSurfaceCapabilitiesKHR")) return (PFN_vkVoidFunction)m_Caps;
    if (!strcmp(n, "vkFooBar")) return (PFN_vkVoidFunction)m_Unknown;
    return nullptr;                         // other instance functions the layer asks for may be null in this mock
}
static PFN_vkVoidFunction VKAPI_CALL next_gdpa(VkDevice, const char* n) {
    R(DestroyDevice)
    if (!strcmp(n, "vkCreateSwapchainKHR")) return (PFN_vkVoidFunction)m_CreateSwapchain;
    if (!strcmp(n, "vkDestroySwapchainKHR")) return (PFN_vkVoidFunction)m_DestroySwapchain;
    if (!strcmp(n, "vkQueuePresentKHR")) return (PFN_vkVoidFunction)m_Present;
    if (!strcmp(n, "vkDeviceFoo")) return (PFN_vkVoidFunction)m_Unknown;
    return nullptr;
}

int main(int argc, char** argv) {
    void* lib = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL); CHECK(lib);
    auto negotiate = (PFN_vkNegotiateLoaderLayerInterfaceVersion)dlsym(lib, "FrameGen_NegotiateLoaderLayerInterfaceVersion"); CHECK(negotiate);

    VkNegotiateLayerInterface nl{}; nl.sType = LAYER_NEGOTIATE_INTERFACE_STRUCT; nl.loaderLayerInterfaceVersion = 7;
    CHECK(negotiate(&nl) == VK_SUCCESS);
    CHECK(nl.loaderLayerInterfaceVersion == CURRENT_LOADER_LAYER_INTERFACE_VERSION && nl.pfnGetInstanceProcAddr && nl.pfnGetDeviceProcAddr);
    VkNegotiateLayerInterface bad{}; CHECK(negotiate(&bad) == VK_ERROR_INITIALIZATION_FAILED);

    // vkCreateInstance with a loader link chain
    VkLayerInstanceLink ilink{nullptr, next_gipa, nullptr};
    VkLayerInstanceCreateInfo lici{}; lici.sType = VK_STRUCTURE_TYPE_LOADER_INSTANCE_CREATE_INFO; lici.function = VK_LAYER_LINK_INFO; lici.u.pLayerInfo = &ilink;
    VkInstanceCreateInfo ici{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, &lici};
    auto create_instance = (PFN_vkCreateInstance)nl.pfnGetInstanceProcAddr(VK_NULL_HANDLE, "vkCreateInstance"); CHECK(create_instance);
    VkInstance inst{}; CHECK(create_instance(&ici, nullptr, &inst) == VK_SUCCESS);
    CHECK(inst == (VkInstance)&g_inst);
    CHECK(lici.u.pLayerInfo == nullptr);                                   // chain advanced (ilink.pNext was null)

    auto gipa = nl.pfnGetInstanceProcAddr;
    CHECK(gipa(inst, "vkQueuePresentKHR") != nullptr);                     // device fn is also served by the layer from GIPA
    auto unk = (void(*)())gipa(inst, "vkFooBar"); CHECK(unk == (void(*)())m_Unknown);   // unhooked names pass down
    CHECK(gipa(inst, "vkGetInstanceProcAddr") == (PFN_vkVoidFunction)nl.pfnGetInstanceProcAddr);

    // vkCreateDevice
    VkLayerDeviceLink dlink{nullptr, nullptr, next_gdpa};
    VkLayerDeviceCreateInfo ldci{}; ldci.sType = VK_STRUCTURE_TYPE_LOADER_DEVICE_CREATE_INFO; ldci.function = VK_LAYER_LINK_INFO; ldci.u.pLayerInfo = &dlink;
    VkDeviceCreateInfo dci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, &ldci};
    auto create_device = (PFN_vkCreateDevice)gipa(inst, "vkCreateDevice");
    VkDevice dev{}; CHECK(create_device((VkPhysicalDevice)&g_pd, &dci, nullptr, &dev) == VK_SUCCESS);
    auto gdpa = nl.pfnGetDeviceProcAddr;
    CHECK(gdpa(dev, "vkDeviceFoo") == (PFN_vkVoidFunction)m_Unknown);

    // Swapchain: 3 -> 4; clamped by surface max; never lowered; max 0 = unlimited
    auto csc = (PFN_vkCreateSwapchainKHR)gdpa(dev, "vkCreateSwapchainKHR"); CHECK(csc);
    VkSwapchainCreateInfoKHR sci{VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR}; VkSwapchainKHR sc{};
    sci.minImageCount = 3; g_surface_max = 0; CHECK(csc(dev, &sci, nullptr, &sc) == VK_SUCCESS); CHECK(g_seen_min_images == 4);
    sci.minImageCount = 3; g_surface_max = 3; csc(dev, &sci, nullptr, &sc); CHECK(g_seen_min_images == 3);
    sci.minImageCount = 6; g_surface_max = 0; csc(dev, &sci, nullptr, &sc); CHECK(g_seen_min_images == 6);
    CHECK(sci.minImageCount == 6);                                          // the app's struct is not modified (const copy)

    auto qp = (PFN_vkQueuePresentKHR)gdpa(dev, "vkQueuePresentKHR"); VkPresentInfoKHR pi{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
    for (int i = 0; i < 600; ++i) CHECK(qp((VkQueue)&g_q, &pi) == VK_SUCCESS);
    CHECK(g_present_calls == 600);

    ((PFN_vkDestroySwapchainKHR)gdpa(dev, "vkDestroySwapchainKHR"))(dev, sc, nullptr); CHECK(g_sc_destroyed == 1);
    ((PFN_vkDestroyDevice)gdpa(dev, "vkDestroyDevice"))(dev, nullptr); CHECK(g_dev_destroyed == 1);
    ((PFN_vkDestroyInstance)gipa(inst, "vkDestroyInstance"))(inst, nullptr); CHECK(g_inst_destroyed == 1);
    std::printf("PASS: negotiate, instance/device chaining, proc-addr routing, swapchain count (3->4, clamp, no-lower), 600 presents forwarded, clean destroy\n");
}
