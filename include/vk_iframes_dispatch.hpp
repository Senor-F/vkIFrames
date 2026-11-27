#pragma once
// Dispatch tables: function lists, InstanceDispatch/DeviceDispatch, and the instance/device/queue/swapchain registries.

#include <vulkan/vulkan.h>
#include <vulkan/vk_layer.h>

#include <cstdint>
#include <memory>
#include <mutex>
#include <utility>
#include <vector>

struct SwapchainState;

// The loader's dispatch pointer is the first word of every dispatchable handle.
// Handles created from one instance/device share it, so it is the registry key
// (a VkQueue has the same key as its VkDevice, a VkPhysicalDevice the same as its VkInstance).
template <typename T>
inline void* GetKey(T handle) { return *reinterpret_cast<void**>(handle); }

// ---- Functions the layer calls down the chain. Later stages append names. ----
// Extension functions resolve to null unless the extension is enabled on the instance / device.
#define FRAMEGEN_INSTANCE_FUNCS(X)                  \
    X(DestroyInstance)                              \
    X(CreateDevice)                                 \
    X(EnumerateDeviceExtensionProperties)           \
    X(GetPhysicalDeviceSurfaceCapabilitiesKHR)      \
    X(GetPhysicalDeviceMemoryProperties)            \
    X(GetPhysicalDeviceQueueFamilyProperties)       \
    X(GetPhysicalDeviceFeatures2)                   \
    X(GetPhysicalDeviceFeatures2KHR)                \
    X(GetPhysicalDeviceSurfaceCapabilities2KHR)     \
    X(GetPhysicalDeviceFormatProperties)            \
    X(GetPhysicalDeviceProperties)

#define FRAMEGEN_DEVICE_FUNCS(X)                    \
    X(DestroyDevice)                                \
    X(CreateSwapchainKHR)                           \
    X(DestroySwapchainKHR)                          \
    X(QueuePresentKHR)                              \
    X(SetSwapchainPresentTimingQueueSizeEXT)        \
    X(GetSwapchainTimeDomainPropertiesEXT)          \
    X(GetSwapchainTimingPropertiesEXT)              \
    X(GetPastPresentationTimingEXT)                 \
    X(GetCalibratedTimestampsKHR)                   \
    X(GetDeviceQueue)                               \
    X(GetDeviceQueue2)                              \
    X(GetSwapchainImagesKHR)                        \
    X(DeviceWaitIdle)                               \
    X(CreateImage)                                  \
    X(DestroyImage)                                 \
    X(GetImageMemoryRequirements)                   \
    X(AllocateMemory)                               \
    X(FreeMemory)                                   \
    X(BindImageMemory)                              \
    X(CreateCommandPool)                            \
    X(DestroyCommandPool)                           \
    X(AllocateCommandBuffers)                       \
    X(BeginCommandBuffer)                           \
    X(EndCommandBuffer)                             \
    X(ResetCommandBuffer)                           \
    X(CreateSemaphore)                              \
    X(DestroySemaphore)                             \
    X(CreateFence)                                  \
    X(DestroyFence)                                 \
    X(WaitForFences)                                \
    X(ResetFences)                                  \
    X(QueueSubmit2)                                 \
    X(QueueSubmit2KHR)                              \
    X(CmdPipelineBarrier2)                          \
    X(CmdPipelineBarrier2KHR)                       \
    X(CmdBlitImage)                                 \
    X(AcquireNextImageKHR)                          \
    X(AcquireNextImage2KHR)                         \
    X(WaitForPresentKHR)                            \
    X(GetFenceStatus)                               \
    X(QueueSubmit)                                  \
    X(QueueBindSparse)                              \
    X(QueueWaitIdle)                                \
    X(CreateImageView)                              \
    X(DestroyImageView)                             \
    X(CreateShaderModule)                           \
    X(DestroyShaderModule)                          \
    X(CreateDescriptorSetLayout)                    \
    X(DestroyDescriptorSetLayout)                   \
    X(CreatePipelineLayout)                         \
    X(DestroyPipelineLayout)                        \
    X(CreateComputePipelines)                       \
    X(DestroyPipeline)                              \
    X(CreateDescriptorPool)                         \
    X(DestroyDescriptorPool)                        \
    X(AllocateDescriptorSets)                       \
    X(UpdateDescriptorSets)                         \
    X(CmdBindPipeline)                              \
    X(CmdBindDescriptorSets)                        \
    X(CmdPushConstants)                             \
    X(CmdDispatch)                                  \
    X(CreateQueryPool)                              \
    X(DestroyQueryPool)                             \
    X(CmdResetQueryPool)                            \
    X(CmdWriteTimestamp)                            \
    X(GetQueryPoolResults)                          \
    X(CreateBuffer)                                 \
    X(DestroyBuffer)                                \
    X(GetBufferMemoryRequirements)                  \
    X(BindBufferMemory)                             \
    X(MapMemory)                                    \
    X(UnmapMemory)                                  \
    X(CmdCopyImageToBuffer)

struct InstanceDispatch {
    PFN_vkGetInstanceProcAddr GetInstanceProcAddr{};
    uint32_t api_version{VK_API_VERSION_1_0};   // what the application asked for
    bool     props2{false};                     // physical-device-properties2 is usable (core 1.1 or the KHR extension)
    bool     surface_caps2{false};              // VK_KHR_get_surface_capabilities2 is enabled (the app's or ours)
#define X(name) PFN_vk##name name{};
    FRAMEGEN_INSTANCE_FUNCS(X)
#undef X
};

struct DeviceDispatch {
    PFN_vkGetDeviceProcAddr GetDeviceProcAddr{};
    VkDevice         device{};
    VkPhysicalDevice physical_device{};
    InstanceDispatch* instance{};
    PFN_vkSetDeviceLoaderData set_loader_data{};   // the loader must initialise the dispatch word of every dispatchable object we create
    bool             sync2{false};              // synchronization2 is enabled and its entry points resolve (needed to capture and hold)
    std::vector<uint32_t> queue_families;       // families the device was created with
    std::mutex       fence_mu;                  // held_fences: application present fences of frames the layer is holding
    std::vector<std::pair<uint64_t, std::weak_ptr<struct SwapchainState>>> held_fences;   // weak: a fence-wait thread takes a shared_ptr under fence_mu, so the state outlives its flush even when the swapchain is destroyed meanwhile
    bool             timing_supported{false};   // present timing + present id 2 + calibrated timestamps are enabled on this device
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
void LeakSwapchainState(VkSwapchainKHR handle);   // forget without freeing: a detached presenter thread still uses it
void LeakDevice(void* key);                       // same, for the device dispatch table
void UnregisterInstance(void* key);
void UnregisterDevice(void* key);

SwapchainState* RegisterSwapchain(VkSwapchainKHR handle);   // replaces any state under the same handle
SwapchainState* FindSwapchain(VkSwapchainKHR handle);   // no ownership: only for the thread that owns the swapchain's use (present, acquire, destroy)
std::shared_ptr<SwapchainState> FindSwapchainShared(VkSwapchainKHR handle);
void            DestroySwapchainState(VkSwapchainKHR handle);

// Every queue gets a mutex: vkQueueSubmit/Present/... on one queue must not overlap between the game thread and our presenter.
void     RegisterQueue(VkQueue queue, uint32_t family, void* device_key);
std::mutex* QueueMutex(VkQueue queue);                       // nullptr for an unknown queue
std::vector<std::mutex*> DeviceQueueMutexes(void* device_key);
void     UnregisterQueues(void* device_key);
void     LeakQueues(void* device_key);   // forget the queue infos (and their mutexes) without freeing: a detached presenter thread may hold one
uint32_t QueueFamilyOf(VkQueue queue);                      // UINT32_MAX when unknown
