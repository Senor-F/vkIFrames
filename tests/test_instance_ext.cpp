// Mock-loader test for vkCreateInstance: the extensions the layer adds for surface apps (get_physical_device_properties2 for 1.0 apps,
// get_surface_capabilities2) are tried on the instance and dropped when the implementation reports them missing. The layers below are never
// asked for a function with a null instance, except vkCreateInstance: Mesa's device_select layer crashes on any other name. No GPU needed.
#include <vulkan/vulkan.h>
#include <vulkan/vk_layer.h>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <set>
#include <string>
#include <vector>
#include <dlfcn.h>
#define CHECK(c) do { if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); std::exit(1); } } while (0)

struct Fake { void* dispatch; };
static Fake g_inst{(void*)0x1111};
static std::set<std::string> g_supported;                       // instance extensions the mock implementation offers
static std::vector<std::vector<std::string>> g_attempts;        // the extension names of every vkCreateInstance that reached it
static VkResult g_force_error = VK_SUCCESS;
static bool g_force_only_with_extras = false;                    // g_force_error only for requests that carry an extension the app did not ask for

static VkResult VKAPI_CALL m_CreateInstance(const VkInstanceCreateInfo* ci, const VkAllocationCallbacks*, VkInstance* p) {
    std::vector<std::string> names;
    for (uint32_t i = 0; i < ci->enabledExtensionCount; ++i) names.push_back(ci->ppEnabledExtensionNames[i]);
    g_attempts.push_back(names);
    if (g_force_error != VK_SUCCESS) {
        bool extras = false;
        for (auto& n : names) if (n == VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME || n == VK_KHR_GET_SURFACE_CAPABILITIES_2_EXTENSION_NAME) extras = true;
        if (!g_force_only_with_extras || extras) return g_force_error;
    }
    for (auto& n : names) if (!g_supported.count(n)) return VK_ERROR_EXTENSION_NOT_PRESENT;
    *p = (VkInstance)&g_inst; return VK_SUCCESS;
}
static void VKAPI_CALL m_DestroyInstance(VkInstance, const VkAllocationCallbacks*) {}
static PFN_vkVoidFunction VKAPI_CALL next_gipa(VkInstance inst, const char* n) {
    if (!inst && std::strcmp(n, "vkCreateInstance") != 0) { std::printf("FAIL: the layer asked the next layer for %s with a null instance\n", n); std::exit(1); }
    if (!std::strcmp(n, "vkCreateInstance")) return (PFN_vkVoidFunction)m_CreateInstance;
    if (!std::strcmp(n, "vkDestroyInstance")) return (PFN_vkVoidFunction)m_DestroyInstance;
    return nullptr;
}

static bool has(const std::vector<std::string>& v, const char* n) { for (auto& s : v) if (s == n) return true; return false; }

int main(int, char** argv) {
    void* lib = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL); CHECK(lib);
    auto negotiate = (PFN_vkNegotiateLoaderLayerInterfaceVersion)dlsym(lib, "FrameGen_NegotiateLoaderLayerInterfaceVersion"); CHECK(negotiate);
    VkNegotiateLayerInterface nl{}; nl.sType = LAYER_NEGOTIATE_INTERFACE_STRUCT; nl.loaderLayerInterfaceVersion = 2; CHECK(negotiate(&nl) == VK_SUCCESS);
    auto create_instance = (PFN_vkCreateInstance)nl.pfnGetInstanceProcAddr(VK_NULL_HANDLE, "vkCreateInstance"); CHECK(create_instance);
    auto destroy = (PFN_vkDestroyInstance)nl.pfnGetInstanceProcAddr((VkInstance)&g_inst, "vkDestroyInstance");

    const char* P2 = VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME; const char* C2 = VK_KHR_GET_SURFACE_CAPABILITIES_2_EXTENSION_NAME;
    const char* SURF = VK_KHR_SURFACE_EXTENSION_NAME;
    auto run = [&](uint32_t api, std::vector<const char*> app_exts, std::set<std::string> supported, VkResult force = VK_SUCCESS) {
        g_supported = std::move(supported); g_attempts.clear(); g_force_error = force;
        VkLayerInstanceLink ilink{nullptr, next_gipa, nullptr};
        VkLayerInstanceCreateInfo lici{}; lici.sType = VK_STRUCTURE_TYPE_LOADER_INSTANCE_CREATE_INFO; lici.function = VK_LAYER_LINK_INFO; lici.u.pLayerInfo = &ilink;
        VkApplicationInfo ai{VK_STRUCTURE_TYPE_APPLICATION_INFO}; ai.apiVersion = api;
        VkInstanceCreateInfo ici{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, &lici}; ici.pApplicationInfo = &ai;
        ici.enabledExtensionCount = uint32_t(app_exts.size()); ici.ppEnabledExtensionNames = app_exts.data();
        VkInstance inst{}; VkResult r = create_instance(&ici, nullptr, &inst);
        if (r == VK_SUCCESS) destroy((VkInstance)&g_inst, nullptr);
        return r;
    };

    // 1.0 surface app, both offered: one attempt, both added
    CHECK(run(VK_API_VERSION_1_0, {SURF}, {SURF, P2, C2}) == VK_SUCCESS);
    CHECK(g_attempts.size() == 1 && has(g_attempts[0], P2) && has(g_attempts[0], C2) && has(g_attempts[0], SURF));
    // only properties2 offered: the first attempt fails, the second has properties2 alone (caps2 is never added without it)
    CHECK(run(VK_API_VERSION_1_0, {SURF}, {SURF, P2}) == VK_SUCCESS);
    CHECK(g_attempts.size() == 2 && has(g_attempts[1], P2) && !has(g_attempts[1], C2));
    // neither offered: three attempts, the last is the app's list unchanged
    CHECK(run(VK_API_VERSION_1_0, {SURF}, {SURF}) == VK_SUCCESS);
    CHECK(g_attempts.size() == 3 && g_attempts[2].size() == 1 && g_attempts[2][0] == SURF);
    // a 1.1 app already has the core functions: only caps2 is tried, then dropped
    CHECK(run(VK_API_VERSION_1_1, {SURF}, {SURF, C2}) == VK_SUCCESS);
    CHECK(g_attempts.size() == 1 && has(g_attempts[0], C2) && !has(g_attempts[0], P2));
    CHECK(run(VK_API_VERSION_1_1, {SURF}, {SURF}) == VK_SUCCESS);
    CHECK(g_attempts.size() == 2 && g_attempts[1].size() == 1);
    // an app with no surface is left alone
    CHECK(run(VK_API_VERSION_1_0, {}, {}) == VK_SUCCESS);
    CHECK(g_attempts.size() == 1 && g_attempts[0].empty());
    // an app that already asks for both: nothing added, nothing retried
    CHECK(run(VK_API_VERSION_1_0, {SURF, P2, C2}, {SURF, P2, C2}) == VK_SUCCESS);
    CHECK(g_attempts.size() == 1 && g_attempts[0].size() == 3);
    // an extension the app itself asked for and nobody offers: the app gets the error, after the layer's own fallbacks
    CHECK(run(VK_API_VERSION_1_0, {SURF, "VK_FOO_not_there"}, {SURF}) == VK_ERROR_EXTENSION_NOT_PRESENT);
    CHECK(g_attempts.size() == 3);
    // a layer or driver below answers an extension it does not want with another code: the requests carrying the extras fail with it, the app's own
    // list works, so the game's vkCreateInstance succeeds after the fallbacks (1.0 surface app: three attempts, the last is the app's list unchanged)
    g_force_only_with_extras = true;
    CHECK(run(VK_API_VERSION_1_0, {SURF}, {SURF, P2, C2}, VK_ERROR_INITIALIZATION_FAILED) == VK_SUCCESS);
    CHECK(g_attempts.size() == 3 && g_attempts[2].size() == 1 && g_attempts[2][0] == SURF);
    CHECK(run(VK_API_VERSION_1_1, {SURF}, {SURF, P2, C2}, VK_ERROR_INCOMPATIBLE_DRIVER) == VK_SUCCESS);
    CHECK(g_attempts.size() == 2 && g_attempts[1].size() == 1);
    g_force_only_with_extras = false;
    // everything fails with that code: the last try's code comes back, and the last try is the app's own list unchanged
    CHECK(run(VK_API_VERSION_1_0, {SURF}, {SURF, P2, C2}, VK_ERROR_INITIALIZATION_FAILED) == VK_ERROR_INITIALIZATION_FAILED);
    CHECK(g_attempts.size() == 3 && g_attempts[2].size() == 1 && g_attempts[2][0] == SURF);
    // an app with no surface gets no extras and a single attempt, whatever the error
    CHECK(run(VK_API_VERSION_1_0, {}, {}, VK_ERROR_INITIALIZATION_FAILED) == VK_ERROR_INITIALIZATION_FAILED);
    CHECK(g_attempts.size() == 1 && g_attempts[0].empty());
    std::printf("PASS: instance extensions are tried and dropped, no null-instance call below the layer\n");
}
