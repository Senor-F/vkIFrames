[index](../STAGE1.md) | [next](02-dispatch-tables-shared-declarations.md)

# Stage 1: Vulkan Layer Skeleton & Swapchain Interception

> **Implementation note (brought up to date).** Stage 1 is done and the sketches below are the first design; the shipped code differs from them in these ways. The header `include/vk_iframes_layer.hpp` now also holds the `Config` (the `VKIFRAME_*` settings), the swapchain state and the GPU-side declarations, and the logging macro `FG_LOG` writes the `[FrameGen]` lines to the layer's log file (`~/.config/vkiframes/live.log` by default, `VKIFRAME_LOG_FILE` to change it) instead of straight to stderr. The hook table in `src/layer_dispatch.cpp` has grown from the five Stage 1 entry points to: `CreateInstance`, `DestroyInstance`, `CreateDevice`, `DestroyDevice`, `CreateSwapchainKHR`, `DestroySwapchainKHR`, `QueuePresentKHR`, plus the later stages' `QueueSubmit`, `QueueSubmit2(KHR)`, `QueueBindSparse`, `QueueWaitIdle`, `DeviceWaitIdle`, `AcquireNextImageKHR`, `AcquireNextImage2KHR`, `WaitForPresentKHR`, `WaitForFences`, `GetFenceStatus`, `GetDeviceQueue` and `GetDeviceQueue2`. The registries are never destroyed (#47). The swapchain image count is raised to at least 4 as here, and further when frames are held (STAGE3.md §4.B), so the log reads `minImageCount 3 -> 8` rather than `3 -> 4` in a hold run; the `3 -> 4` of the test below is what a swapchain without holding gets. The mock-loader test is `tests/test_chain.cpp`, run by `ctest` as `mock_loader`; the shell scripts of the first repository (`scripts/test_stage1.sh` and the report file it wrote) were not carried over, `scripts/check_env.sh` was.

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

