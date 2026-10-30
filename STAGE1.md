# Stage 1: Vulkan Layer Skeleton & Swapchain Interception

## 1. Overview & Objectives

Stage 1 establishes a minimal, correct Vulkan layer: manifest, loader negotiation, instance/device chaining, dispatch tables, and hooks on swapchain creation and presentation. It does **not** modify pixels and must not change the behavior of the host application, other than raising the swapchain image count.

Two rules from here on apply to every stage:

1. **Chain properly.** `vkCreateInstance` and `vkCreateDevice` receive a link-info structure from the loader; the layer must take the next layer's `GetInstanceProcAddr` / `GetDeviceProcAddr` from it and advance the chain.
2. **Call down the chain.** The layer never calls bare `vk*` symbols; it calls function pointers stored in its dispatch tables. Bare calls go back through the loader and re-enter this layer.

---

## 2. Repository Layout (Stage 1 additions)

```text
vkIFrames/
├── CMakeLists.txt
├── manifest/
│   └── VkLayer_frame_gen.json.in
├── include/
│   └── vk_iframes_layer.hpp      # dispatch tables, GetKey, shared declarations
└── src/
    ├── layer_dispatch.cpp        # registries, proc-addr tables, loader negotiation
    └── layer_main.cpp            # hooked entry points
```

---

## 3. Configuration & Metadata

### A. Layer Manifest Template (`manifest/VkLayer_frame_gen.json.in`)

```json
{
  "file_format_version": "1.1.2",
  "layer": {
    "name": "VK_LAYER_CUSTOM_frame_gen",
    "type": "GLOBAL",
    "library_path": "@FRAMEGEN_LIBRARY_PATH@",
    "api_version": "1.3.0",
    "implementation_version": "1",
    "description": "vkIFrames dynamic frame interpolation layer",
    "functions": {
      "vkNegotiateLoaderLayerInterfaceVersion": "FrameGen_NegotiateLoaderLayerInterfaceVersion",
      "vkGetInstanceProcAddr": "FrameGen_GetInstanceProcAddr",
      "vkGetDeviceProcAddr": "FrameGen_GetDeviceProcAddr"
    }
  }
}
```

This is an **explicit** layer (enabled via environment variables). Stage 6 adds an implicit variant with `enable_environment` / `disable_environment`.

### B. Build System (`CMakeLists.txt`)

A layer needs the Vulkan **headers**, not the loader library, so it links only `Vulkan::Headers`.

```cmake
cmake_minimum_required(VERSION 3.21)
project(vkIFrames VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
set(CMAKE_CXX_VISIBILITY_PRESET hidden)       # only the FRAMEGEN_EXPORT symbols are public
set(CMAKE_VISIBILITY_INLINES_HIDDEN ON)

# A layer never links the Vulkan loader, so only the headers are needed (no libvulkan dev package).
find_package(VulkanHeaders CONFIG QUIET)                       # provides Vulkan::Headers when Vulkan-Headers is installed with CMake files
if(NOT TARGET Vulkan::Headers)
    find_path(FRAMEGEN_VULKAN_INCLUDE_DIR vulkan/vk_layer.h HINTS ${Vulkan_INCLUDE_DIR} ENV VULKAN_SDK PATH_SUFFIXES include)
    if(NOT FRAMEGEN_VULKAN_INCLUDE_DIR)
        message(FATAL_ERROR "Vulkan headers not found. Install Vulkan-Headers (recent enough for VK_EXT_present_timing) "
                            "or pass -DFRAMEGEN_VULKAN_INCLUDE_DIR=<dir containing vulkan/vk_layer.h>.")
    endif()
    add_library(Vulkan::Headers INTERFACE IMPORTED)
    set_target_properties(Vulkan::Headers PROPERTIES INTERFACE_INCLUDE_DIRECTORIES "${FRAMEGEN_VULKAN_INCLUDE_DIR}")
endif()

add_library(vkIFrames SHARED
    src/layer_main.cpp
    src/layer_dispatch.cpp
)
target_include_directories(vkIFrames PRIVATE include)
target_link_libraries(vkIFrames PRIVATE Vulkan::Headers)
target_link_options(vkIFrames PRIVATE -Wl,--no-undefined)

# Manifest next to the library. The path is relative to the manifest file.
# (configure_file does not evaluate generator expressions, so use a plain name.)
set(FRAMEGEN_LIBRARY_PATH "./libvkIFrames.so")
configure_file(
    manifest/VkLayer_frame_gen.json.in
    ${CMAKE_BINARY_DIR}/VkLayer_frame_gen.json
    @ONLY
)
```

---

## 4. Implementation Details

### A. Dispatch Tables & Shared Declarations (`include/vk_iframes_layer.hpp`)

Function lists are X-macros, so adding a hooked or used function in a later stage is a one-line change.

```cpp
#pragma once

#include <vulkan/vulkan.h>
#include <vulkan/vk_layer.h>

#include <cstdio>

#define FRAMEGEN_EXPORT extern "C" __attribute__((visibility("default")))
#define FG_LOG(...) do { std::fprintf(stderr, "[FrameGen] " __VA_ARGS__); std::fputc('\n', stderr); } while (0)

// The loader's dispatch pointer is the first word of every dispatchable handle.
// Handles created from one instance/device share it, so it is the registry key
// (a VkQueue has the same key as its VkDevice, a VkPhysicalDevice the same as its VkInstance).
template <typename T>
inline void* GetKey(T handle) { return *reinterpret_cast<void**>(handle); }

// ---- Functions the layer calls down the chain. Later stages append names. ----
#define FRAMEGEN_INSTANCE_FUNCS(X)                  \
    X(DestroyInstance)                              \
    X(CreateDevice)                                 \
    X(EnumerateDeviceExtensionProperties)           \
    X(GetPhysicalDeviceSurfaceCapabilitiesKHR)      \
    X(GetPhysicalDeviceMemoryProperties)            \
    X(GetPhysicalDeviceQueueFamilyProperties)

#define FRAMEGEN_DEVICE_FUNCS(X)                    \
    X(DestroyDevice)                                \
    X(CreateSwapchainKHR)                           \
    X(DestroySwapchainKHR)                          \
    X(QueuePresentKHR)

struct InstanceDispatch {
    PFN_vkGetInstanceProcAddr GetInstanceProcAddr{};
#define X(name) PFN_vk##name name{};
    FRAMEGEN_INSTANCE_FUNCS(X)
#undef X
};

struct DeviceDispatch {
    PFN_vkGetDeviceProcAddr GetDeviceProcAddr{};
    VkDevice         device{};
    VkPhysicalDevice physical_device{};
    InstanceDispatch* instance{};
#define X(name) PFN_vk##name name{};
    FRAMEGEN_DEVICE_FUNCS(X)
#undef X
};

// ---- Registries (layer_dispatch.cpp) ----
InstanceDispatch* RegisterInstance(void* key, VkInstance instance, PFN_vkGetInstanceProcAddr gipa);
DeviceDispatch*   RegisterDevice(void* key, VkDevice device, VkPhysicalDevice physical_device,
                                 PFN_vkGetDeviceProcAddr gdpa, InstanceDispatch* instance);
InstanceDispatch* LookupInstance(void* key);
DeviceDispatch*   LookupDevice(void* key);
void UnregisterInstance(void* key);
void UnregisterDevice(void* key);

// ---- Hooks (layer_main.cpp) ----
VKAPI_ATTR VkResult VKAPI_CALL Hook_CreateInstance(const VkInstanceCreateInfo*, const VkAllocationCallbacks*, VkInstance*);
VKAPI_ATTR void     VKAPI_CALL Hook_DestroyInstance(VkInstance, const VkAllocationCallbacks*);
VKAPI_ATTR VkResult VKAPI_CALL Hook_CreateDevice(VkPhysicalDevice, const VkDeviceCreateInfo*, const VkAllocationCallbacks*, VkDevice*);
VKAPI_ATTR void     VKAPI_CALL Hook_DestroyDevice(VkDevice, const VkAllocationCallbacks*);
VKAPI_ATTR VkResult VKAPI_CALL Hook_CreateSwapchainKHR(VkDevice, const VkSwapchainCreateInfoKHR*, const VkAllocationCallbacks*, VkSwapchainKHR*);
VKAPI_ATTR void     VKAPI_CALL Hook_DestroySwapchainKHR(VkDevice, VkSwapchainKHR, const VkAllocationCallbacks*);
VKAPI_ATTR VkResult VKAPI_CALL Hook_QueuePresentKHR(VkQueue, const VkPresentInfoKHR*);

// ---- Exported entry points ----
FRAMEGEN_EXPORT VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL FrameGen_GetInstanceProcAddr(VkInstance, const char*);
FRAMEGEN_EXPORT VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL FrameGen_GetDeviceProcAddr(VkDevice, const char*);
FRAMEGEN_EXPORT VKAPI_ATTR VkResult VKAPI_CALL FrameGen_NegotiateLoaderLayerInterfaceVersion(VkNegotiateLayerInterface*);
```

### B. Registries, Proc-Addr Tables, Negotiation (`src/layer_dispatch.cpp`)

```cpp
#include "vk_iframes_layer.hpp"

#include <cstring>
#include <memory>
#include <mutex>
#include <unordered_map>

namespace {
std::mutex g_mutex;
std::unordered_map<void*, std::unique_ptr<InstanceDispatch>> g_instances;
std::unordered_map<void*, std::unique_ptr<DeviceDispatch>>   g_devices;

struct HookEntry { const char* name; PFN_vkVoidFunction fn; bool device_level; };

#define HOOK(n, dev) { "vk" #n, reinterpret_cast<PFN_vkVoidFunction>(Hook_##n), dev }
const HookEntry kHooks[] = {
    HOOK(CreateInstance,       false),
    HOOK(DestroyInstance,      false),
    HOOK(CreateDevice,         false),
    HOOK(DestroyDevice,        true),
    HOOK(CreateSwapchainKHR,   true),
    HOOK(DestroySwapchainKHR,  true),
    HOOK(QueuePresentKHR,      true),
    { "vkGetInstanceProcAddr", reinterpret_cast<PFN_vkVoidFunction>(FrameGen_GetInstanceProcAddr), false },
    { "vkGetDeviceProcAddr",   reinterpret_cast<PFN_vkVoidFunction>(FrameGen_GetDeviceProcAddr),   true  },
};
#undef HOOK

PFN_vkVoidFunction FindHook(const char* name, bool device_only) {
    for (const auto& h : kHooks)
        if ((!device_only || h.device_level) && std::strcmp(h.name, name) == 0) return h.fn;
    return nullptr;
}
} // namespace

InstanceDispatch* RegisterInstance(void* key, VkInstance instance, PFN_vkGetInstanceProcAddr gipa) {
    auto t = std::make_unique<InstanceDispatch>();
    t->GetInstanceProcAddr = gipa;
#define X(name) t->name = reinterpret_cast<PFN_vk##name>(gipa(instance, "vk" #name));
    FRAMEGEN_INSTANCE_FUNCS(X)
#undef X
    std::lock_guard lock(g_mutex);
    return (g_instances[key] = std::move(t)).get();
}

DeviceDispatch* RegisterDevice(void* key, VkDevice device, VkPhysicalDevice pd,
                               PFN_vkGetDeviceProcAddr gdpa, InstanceDispatch* instance) {
    auto t = std::make_unique<DeviceDispatch>();
    t->GetDeviceProcAddr = gdpa;
    t->device = device;
    t->physical_device = pd;
    t->instance = instance;
#define X(name) t->name = reinterpret_cast<PFN_vk##name>(gdpa(device, "vk" #name));
    FRAMEGEN_DEVICE_FUNCS(X)
#undef X
    std::lock_guard lock(g_mutex);
    return (g_devices[key] = std::move(t)).get();
}

InstanceDispatch* LookupInstance(void* key) { std::lock_guard l(g_mutex); auto it = g_instances.find(key); return it == g_instances.end() ? nullptr : it->second.get(); }
DeviceDispatch*   LookupDevice(void* key)   { std::lock_guard l(g_mutex); auto it = g_devices.find(key);   return it == g_devices.end()   ? nullptr : it->second.get(); }
void UnregisterInstance(void* key) { std::lock_guard l(g_mutex); g_instances.erase(key); }
void UnregisterDevice(void* key)   { std::lock_guard l(g_mutex); g_devices.erase(key); }

FRAMEGEN_EXPORT VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL
FrameGen_GetInstanceProcAddr(VkInstance instance, const char* name) {
    if (auto fn = FindHook(name, /*device_only=*/false)) return fn;   // apps may fetch device functions here too
    if (!instance) return nullptr;
    InstanceDispatch* d = LookupInstance(GetKey(instance));
    return d ? d->GetInstanceProcAddr(instance, name) : nullptr;
}

FRAMEGEN_EXPORT VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL
FrameGen_GetDeviceProcAddr(VkDevice device, const char* name) {
    if (auto fn = FindHook(name, /*device_only=*/true)) return fn;
    DeviceDispatch* d = LookupDevice(GetKey(device));
    return d ? d->GetDeviceProcAddr(device, name) : nullptr;
}

FRAMEGEN_EXPORT VKAPI_ATTR VkResult VKAPI_CALL
FrameGen_NegotiateLoaderLayerInterfaceVersion(VkNegotiateLayerInterface* pVersionStruct) {
    if (!pVersionStruct || pVersionStruct->sType != LAYER_NEGOTIATE_INTERFACE_STRUCT)
        return VK_ERROR_INITIALIZATION_FAILED;
    if (pVersionStruct->loaderLayerInterfaceVersion >= 2) {
        pVersionStruct->pfnGetInstanceProcAddr       = FrameGen_GetInstanceProcAddr;
        pVersionStruct->pfnGetDeviceProcAddr         = FrameGen_GetDeviceProcAddr;
        pVersionStruct->pfnGetPhysicalDeviceProcAddr = nullptr;
    }
    if (pVersionStruct->loaderLayerInterfaceVersion > CURRENT_LOADER_LAYER_INTERFACE_VERSION)
        pVersionStruct->loaderLayerInterfaceVersion = CURRENT_LOADER_LAYER_INTERFACE_VERSION;
    return VK_SUCCESS;
}
```

### C. Hooks (`src/layer_main.cpp`)

```cpp
#include "vk_iframes_layer.hpp"

#include <algorithm>
#include <atomic>

namespace {
constexpr uint32_t kMinImageCount = 4;

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
} // namespace

VKAPI_ATTR VkResult VKAPI_CALL Hook_CreateInstance(const VkInstanceCreateInfo* pCreateInfo,
                                                   const VkAllocationCallbacks* pAllocator,
                                                   VkInstance* pInstance) {
    auto* link = FindInstanceLink(pCreateInfo);
    if (!link || !link->u.pLayerInfo) return VK_ERROR_INITIALIZATION_FAILED;

    PFN_vkGetInstanceProcAddr gipa = link->u.pLayerInfo->pfnNextGetInstanceProcAddr;
    link->u.pLayerInfo = link->u.pLayerInfo->pNext;               // advance the chain for the next layer

    auto create = reinterpret_cast<PFN_vkCreateInstance>(gipa(VK_NULL_HANDLE, "vkCreateInstance"));
    if (!create) return VK_ERROR_INITIALIZATION_FAILED;

    VkResult r = create(pCreateInfo, pAllocator, pInstance);
    if (r != VK_SUCCESS) return r;

    RegisterInstance(GetKey(*pInstance), *pInstance, gipa);
    FG_LOG("instance created");
    return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL Hook_DestroyInstance(VkInstance instance, const VkAllocationCallbacks* pAllocator) {
    void* key = GetKey(instance);                                  // read before the handle dies
    auto destroy = LookupInstance(key)->DestroyInstance;
    destroy(instance, pAllocator);
    UnregisterInstance(key);
}

VKAPI_ATTR VkResult VKAPI_CALL Hook_CreateDevice(VkPhysicalDevice physicalDevice,
                                                 const VkDeviceCreateInfo* pCreateInfo,
                                                 const VkAllocationCallbacks* pAllocator,
                                                 VkDevice* pDevice) {
    InstanceDispatch* inst = LookupInstance(GetKey(physicalDevice));
    auto* link = FindDeviceLink(pCreateInfo);
    if (!inst || !link || !link->u.pLayerInfo) return VK_ERROR_INITIALIZATION_FAILED;

    PFN_vkGetDeviceProcAddr gdpa = link->u.pLayerInfo->pfnNextGetDeviceProcAddr;
    link->u.pLayerInfo = link->u.pLayerInfo->pNext;

    // Stages 2-4 will build a modified VkDeviceCreateInfo here (extensions, features, extra queue).
    VkResult r = inst->CreateDevice(physicalDevice, pCreateInfo, pAllocator, pDevice);
    if (r != VK_SUCCESS) return r;

    RegisterDevice(GetKey(*pDevice), *pDevice, physicalDevice, gdpa, inst);
    FG_LOG("device created");
    return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL Hook_DestroyDevice(VkDevice device, const VkAllocationCallbacks* pAllocator) {
    void* key = GetKey(device);
    auto destroy = LookupDevice(key)->DestroyDevice;
    destroy(device, pAllocator);
    UnregisterDevice(key);
}

VKAPI_ATTR VkResult VKAPI_CALL Hook_CreateSwapchainKHR(VkDevice device,
                                                       const VkSwapchainCreateInfoKHR* pCreateInfo,
                                                       const VkAllocationCallbacks* pAllocator,
                                                       VkSwapchainKHR* pSwapchain) {
    DeviceDispatch* dt = LookupDevice(GetKey(device));

    VkSwapchainCreateInfoKHR ci = *pCreateInfo;

    // Raise the image count toward kMinImageCount, but never beyond what the surface allows
    // (maxImageCount == 0 means "no limit").
    VkSurfaceCapabilitiesKHR caps{};
    if (dt->instance->GetPhysicalDeviceSurfaceCapabilitiesKHR(dt->physical_device, ci.surface, &caps) == VK_SUCCESS) {
        uint32_t wanted = std::max(ci.minImageCount, kMinImageCount);
        if (caps.maxImageCount != 0) wanted = std::min(wanted, caps.maxImageCount);
        if (wanted != ci.minImageCount) {
            FG_LOG("vkCreateSwapchainKHR: minImageCount %u -> %u", ci.minImageCount, wanted);
            ci.minImageCount = wanted;
        }
    }

    return dt->CreateSwapchainKHR(device, &ci, pAllocator, pSwapchain);
}

VKAPI_ATTR void VKAPI_CALL Hook_DestroySwapchainKHR(VkDevice device, VkSwapchainKHR swapchain,
                                                    const VkAllocationCallbacks* pAllocator) {
    DeviceDispatch* dt = LookupDevice(GetKey(device));
    FG_LOG("vkDestroySwapchainKHR");
    dt->DestroySwapchainKHR(device, swapchain, pAllocator);
}

VKAPI_ATTR VkResult VKAPI_CALL Hook_QueuePresentKHR(VkQueue queue, const VkPresentInfoKHR* pPresentInfo) {
    DeviceDispatch* dt = LookupDevice(GetKey(queue));          // a queue shares its device's key

    static std::atomic<uint64_t> frame_count{0};
    uint64_t n = ++frame_count;
    if (n % 300 == 0) FG_LOG("vkQueuePresentKHR (frame %llu)", static_cast<unsigned long long>(n));

    return dt->QueuePresentKHR(queue, pPresentInfo);
}
```

---

## 5. Verification & Testing Procedure

> **Already verified without a GPU.** The code of §4 compiles and links (`-Wl,--no-undefined`, C++20) against Vulkan-Headers `cde2207` (Oct 2026), and exports exactly the three `FrameGen_*` symbols. `tests/test_chain.cpp` loads the built library with a mock loader and driver and checks: version negotiation (including rejecting a bad struct), instance and device chaining, proc-addr routing (hooked names, unhooked pass-through), the swapchain image count (3 → 4, clamped to the surface maximum, never lowered, the application's struct untouched), 600 presents forwarded, and clean destroy. Build and run it with:
> ```bash
> g++ -std=c++20 -I<Vulkan-Headers>/include tests/test_chain.cpp -o test_chain -ldl && ./test_chain build/libvkIFrames.so
> ```
> The steps below still need a real GPU driver and the Vulkan loader. `scripts/test_stage1.sh` automates all of them (build, mock-loader test, `vkcube` with and without the layer, optional validation run) and writes `build/stage1_report.txt`; `scripts/check_env.sh` reports what the machine is missing first.

### Step 1: Compilation
```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j"$(nproc)"
ls build/libvkIFrames.so build/VkLayer_frame_gen.json
```

### Step 2: Enable the layer
```bash
export VK_LAYER_PATH=$PWD/build
export VK_LOADER_LAYERS_ENABLE=VK_LAYER_CUSTOM_frame_gen
export VK_LOADER_DEBUG=layer        # optional: the loader prints which layers it loads
```
(`VK_INSTANCE_LAYERS` is deprecated in current loaders; use the variables above.)

### Step 3: Run a Vulkan application
```bash
vkcube
```

### Step 4: Success criteria
1. The loader debug output lists `VK_LAYER_CUSTOM_frame_gen`, and logs appear:
   ```text
   [FrameGen] instance created
   [FrameGen] device created
   [FrameGen] vkCreateSwapchainKHR: minImageCount 3 -> 4
   [FrameGen] vkQueuePresentKHR (frame 300)
   ```
2. `vkcube` runs continuously with no crashes and no output from `VK_LAYER_KHRONOS_validation` (run once with it enabled: `VK_LOADER_LAYERS_ENABLE=*validation,VK_LAYER_CUSTOM_frame_gen`).
3. Closing the window destroys the swapchain, device and instance without errors.
