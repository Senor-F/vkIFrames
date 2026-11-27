// Internal declarations shared by the layer_*.cpp files; not exported (scripts/layer.map keeps only the three entry points).
#pragma once
#include "vk_iframes_layer.hpp"
#include "gpu_frames.hpp"
#include "present_timing.hpp"

#include <atomic>

// A pair: A (held until now) and B (just captured), with the ticks the scheduler planned for it. A's ring slot reference was
// handed over from the held frame; B got one more for the job. Both are released when the job is done.
// extrap (VKIFRAME_EXTRAPOLATE): A is the previous frame (ring copy only) and B the new one, which this job presents as its tick 0; the later ticks are
// predictions from B and are dropped once a newer job exists (gen != SwapchainState::job_gen).
struct PresentJob { HeldFrame a, b; PairScheduler::Plan plan; VkQueue queue{}; bool extrap{false}; uint64_t gen{0}; };

// Used by PresenterMain until it moves next to them (removed again in that commit).
#include <condition_variable>
#include <deque>
#include <thread>
struct Presenter {
    std::thread th;
    std::mutex mu;
    std::condition_variable cv, idle_cv;
    std::deque<PresentJob> q;
    bool busy{false};
    std::atomic<bool> stop{false};
};
namespace fg_layer { void NotePresentError(SwapchainState& sc, VkResult r); }

// The bodies of the hooks: layer_main.cpp wraps each in the out-of-memory guard.
VkResult Impl_CreateInstance(const VkInstanceCreateInfo* pCreateInfo, const VkAllocationCallbacks* pAllocator, VkInstance* pInstance);
void Impl_DestroyInstance(VkInstance instance, const VkAllocationCallbacks* pAllocator);
VkResult Impl_CreateDevice(VkPhysicalDevice physicalDevice, const VkDeviceCreateInfo* pCreateInfo, const VkAllocationCallbacks* pAllocator, VkDevice* pDevice);
void Impl_DestroyDevice(VkDevice device, const VkAllocationCallbacks* pAllocator);
VkResult Impl_CreateSwapchainKHR(VkDevice device, const VkSwapchainCreateInfoKHR* pCreateInfo, const VkAllocationCallbacks* pAllocator, VkSwapchainKHR* pSwapchain);
void Impl_DestroySwapchainKHR(VkDevice device, VkSwapchainKHR swapchain, const VkAllocationCallbacks* pAllocator);
VkResult Impl_QueuePresentKHR(VkQueue queue, const VkPresentInfoKHR* pPresentInfo);
VkResult Impl_WaitForFences(VkDevice device, uint32_t n, const VkFence* fences, VkBool32 all, uint64_t timeout);
VkResult Impl_GetFenceStatus(VkDevice device, VkFence fence);
VkResult Impl_WaitForPresentKHR(VkDevice device, VkSwapchainKHR swapchain, uint64_t presentId, uint64_t timeout);
VkResult Impl_AcquireNextImageKHR(VkDevice device, VkSwapchainKHR swapchain, uint64_t timeout, VkSemaphore semaphore, VkFence fence, uint32_t* pImageIndex);
VkResult Impl_AcquireNextImage2KHR(VkDevice device, const VkAcquireNextImageInfoKHR* info, uint32_t* pIndex);

namespace fg_layer {
constexpr uint32_t kMinImageCount = 4;

// State and helpers shared by the hooks and the presenter thread.
extern std::atomic<bool> g_presenter_detached;
extern thread_local bool t_guard_forwarded, t_guard_present_consumed;
extern thread_local SwapchainState* t_guard_present_sc;

// What the application chained to its present. VK_KHR_present_id (one id) and the swapchain-maintenance1 present fence are
// carried along with the held frame and re-attached when it is shown; present id 2, present timing and per-present mode
// structures cannot be re-created for a frame shown later, so those leave the swapchain in passthrough.
enum class AppChain { Plain, Carried, Unsupported };
struct AppChainInfo { AppChain kind{AppChain::Plain}; uint64_t id{0}; VkFence fence{}; VkPresentModeKHR mode{}; bool has_mode{false}; };
const VkBaseInStructure* FindInChain(const void* chain, VkStructureType type);
void GuardNote(const char* hook, const char* what) noexcept;
PairScheduler::Plan PlanAndCount(SwapchainState& sc, uint64_t now);
void LogRates(SwapchainState& sc, uint64_t now);
VkResult PresentWithTiming(DeviceDispatch& dt, VkQueue queue, SwapchainState& sc, const VkPresentInfoKHR& in, uint64_t now, double tick_ns = 0.0);
AppChainInfo ClassifyPresentChain(const VkPresentInfoKHR* pi);
void HeldFenceAdd(DeviceDispatch& dt, SwapchainState& sc, const HeldFrame& h);
void HeldFencesDrop(DeviceDispatch& dt, SwapchainState* sc);
void HeldFenceRemove(DeviceDispatch& dt, const HeldFrame& h);
VkResult PresentFrame(DeviceDispatch& dt, VkQueue queue, SwapchainState& sc, const HeldFrame& h, uint64_t now, double tick_ns = 0.0);
VkResult PresentHeld(DeviceDispatch& dt, VkQueue queue, SwapchainState& sc, uint64_t now);
VkResult PresentSynthetic(DeviceDispatch& dt, VkQueue queue, SwapchainState& sc, const HeldFrame& a, const HeldFrame& b, float t, bool hidden, uint64_t now, double tick_ns, bool extrap = false);
void ExecutePair(DeviceDispatch& dt, SwapchainState& sc, const PresentJob& j, Presenter* pr);
void ExecuteExtrap(DeviceDispatch& dt, SwapchainState& sc, const PresentJob& j, Presenter* pr);
void StartPresenter(DeviceDispatch& dt, SwapchainState& sc);
void DrainPresenter(SwapchainState& sc);
bool StopPresenter(SwapchainState& sc);
void FlushForFences(DeviceDispatch& dt, uint32_t n, const VkFence* fences, const char* why);
} // namespace fg_layer
