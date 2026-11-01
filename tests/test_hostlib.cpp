// The layer carries its own C++ runtime: loaded into a process whose runtime is replaced (hoststd: operator new/delete counted), it must not use the host's
// allocator, and its exports must be exactly the three entry points. A mock implementation below the layer; nothing here allocates while the layer runs.
#include <vulkan/vulkan.h>
#include <vulkan/vk_layer.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dlfcn.h>
#include <initializer_list>
#define CHECK(c) do { if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); std::exit(1); } } while (0)

extern "C" unsigned long hoststd_new_calls();
struct Fake { void* dispatch; };
static Fake g_inst{(void*)0x1111};
static VkResult VKAPI_CALL m_CreateInstance(const VkInstanceCreateInfo*, const VkAllocationCallbacks*, VkInstance* p) { *p = (VkInstance)&g_inst; return VK_SUCCESS; }
static void VKAPI_CALL m_DestroyInstance(VkInstance, const VkAllocationCallbacks*) {}
static PFN_vkVoidFunction VKAPI_CALL next_gipa(VkInstance inst, const char* n) {
    if (!inst && std::strcmp(n, "vkCreateInstance") != 0) return nullptr;
    if (!std::strcmp(n, "vkCreateInstance")) return (PFN_vkVoidFunction)m_CreateInstance;
    if (!std::strcmp(n, "vkDestroyInstance")) return (PFN_vkVoidFunction)m_DestroyInstance;
    return nullptr;
}

int main(int, char** argv) {
    { int* p = new int(1); delete p; }                                       // the interposition works: the test's own allocation reaches hoststd
    const unsigned long c0 = hoststd_new_calls();
    CHECK(c0 > 0);

    void* lib = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL); CHECK(lib);
    // the library does not export a single C++ runtime symbol: operator new, a std:: template instance, the exception machinery
    for (const char* sym : {"_Znwm", "_ZdlPv", "_ZSt9terminatev", "__cxa_throw", "_ZNSt6vectorIiSaIiEE17_M_default_appendEm", "_Znam"}) CHECK(!dlsym(lib, sym));
    auto negotiate = (PFN_vkNegotiateLoaderLayerInterfaceVersion)dlsym(lib, "FrameGen_NegotiateLoaderLayerInterfaceVersion"); CHECK(negotiate);
    CHECK(dlsym(lib, "FrameGen_GetInstanceProcAddr") && dlsym(lib, "FrameGen_GetDeviceProcAddr"));
    VkNegotiateLayerInterface nl{}; nl.sType = LAYER_NEGOTIATE_INTERFACE_STRUCT; nl.loaderLayerInterfaceVersion = 2; CHECK(negotiate(&nl) == VK_SUCCESS);
    auto create_instance = (PFN_vkCreateInstance)nl.pfnGetInstanceProcAddr(VK_NULL_HANDLE, "vkCreateInstance"); CHECK(create_instance);
    auto destroy = (PFN_vkDestroyInstance)nl.pfnGetInstanceProcAddr((VkInstance)&g_inst, "vkDestroyInstance");

    static const char* exts[] = {VK_KHR_SURFACE_EXTENSION_NAME};               // a surface app: the layer builds its extension list (a std::vector) and registers the instance
    VkLayerInstanceLink ilink{nullptr, next_gipa, nullptr};
    VkLayerInstanceCreateInfo lici{}; lici.sType = VK_STRUCTURE_TYPE_LOADER_INSTANCE_CREATE_INFO; lici.function = VK_LAYER_LINK_INFO; lici.u.pLayerInfo = &ilink;
    VkApplicationInfo ai{VK_STRUCTURE_TYPE_APPLICATION_INFO}; ai.apiVersion = VK_API_VERSION_1_0;
    VkInstanceCreateInfo ici{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, &lici}; ici.pApplicationInfo = &ai; ici.enabledExtensionCount = 1; ici.ppEnabledExtensionNames = exts;
    const unsigned long before = hoststd_new_calls();
    VkInstance inst{};
    CHECK(create_instance(&ici, nullptr, &inst) == VK_SUCCESS);
    CHECK(destroy != nullptr);
    destroy((VkInstance)&g_inst, nullptr);
    const unsigned long after = hoststd_new_calls();
    std::printf("host allocator calls made while the layer created and destroyed an instance: %lu\n", after - before);
    CHECK(after == before);
    std::printf("PASS: the layer's C++ runtime is its own\n");
    return 0;
}
