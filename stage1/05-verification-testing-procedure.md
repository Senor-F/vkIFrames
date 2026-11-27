[index](../STAGE1.md) | [previous](04-hooks.md)

## 5. Verification & Testing Procedure

> **Already verified without a GPU.** The code of §4 compiles and links (`-Wl,--no-undefined`, C++20) against Vulkan-Headers `cde2207` (Oct 2026), and exports exactly the three `FrameGen_*` symbols. `tests/test_chain.cpp` loads the built library with a mock loader and driver and checks: version negotiation (including rejecting a bad struct), instance and device chaining, proc-addr routing (hooked names, unhooked pass-through), the swapchain image count (3 → 4, clamped to the surface maximum, never lowered, the application's struct untouched), 600 presents forwarded, and clean destroy. Build and run it with:
> ```bash
> g++ -std=c++20 -I<Vulkan-Headers>/include tests/test_chain.cpp -o test_chain -ldl && ./test_chain build/libvkIFrames.so
> ```
> The steps below still need a real GPU driver and the Vulkan loader. They are manual in this repository (`scripts/test_stage1.sh`, which automated them in the first repository, was not carried over); `scripts/check_env.sh` reports what the machine is missing first, and `ctest --test-dir build` runs the mock-loader test.

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
