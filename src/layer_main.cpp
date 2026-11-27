// All hooks: instance/device/swapchain create, present hook, presenter thread, hold-one.
#include "vk_iframes_layer.hpp"
#include "live_config.hpp"
#include "gpu_frames.hpp"
#include "present_timing.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <exception>
#include <mutex>
#include <new>
#include <thread>
#include <unistd.h>
#include <cstring>
#include <string>
#include <vector>

#include "layer_internal.hpp"
using namespace fg_layer;
// Set by the hook bodies for the guard below (see "host out of memory" at the end of this file): the next layer's destroy was already called; the game's frame
// was already consumed by the layer; the swapchain of the present in progress.
namespace fg_layer { std::atomic<bool> g_presenter_detached{false}; thread_local bool t_guard_forwarded = false, t_guard_present_consumed = false; thread_local SwapchainState* t_guard_present_sc = nullptr; }



// ---------------------------------------------------------------- queue locking (Stage 3c-ii)
// The presenter thread submits and presents on the game's queue. Vulkan requires external synchronization per queue, so every
// queue operation of the game takes the same mutex as ours. Image acquires are polled so that no lock is held while waiting.

namespace {
struct QueueLock {
    std::mutex* m;
    explicit QueueLock(VkQueue q) : m(QueueMutex(q)) { if (m) m->lock(); }
    ~QueueLock() { if (m) m->unlock(); }
};

template <class F> VkResult PolledAcquire(uint64_t timeout, F&& once) {
    const uint64_t start = NowNs();
    for (;;) {
        const VkResult r = once();
        if (r != VK_NOT_READY && r != VK_TIMEOUT) return r;
        if (timeout == 0) return VK_NOT_READY;
        if (timeout != UINT64_MAX && NowNs() - start >= timeout) return VK_TIMEOUT;
        timespec nap{0, 100'000}; nanosleep(&nap, nullptr);
    }
}
} // namespace

VKAPI_ATTR VkResult VKAPI_CALL Hook_QueueSubmit(VkQueue q, uint32_t n, const VkSubmitInfo* s, VkFence f) {
    DeviceDispatch* dt = LookupDevice(GetKey(q)); if (!dt) return VK_ERROR_DEVICE_LOST; QueueLock l(q); return dt->QueueSubmit(q, n, s, f);
}
VKAPI_ATTR VkResult VKAPI_CALL Hook_QueueSubmit2(VkQueue q, uint32_t n, const VkSubmitInfo2* s, VkFence f) {
    DeviceDispatch* dt = LookupDevice(GetKey(q)); if (!dt) return VK_ERROR_DEVICE_LOST; QueueLock l(q);
    const auto submit2 = dt->QueueSubmit2 ? dt->QueueSubmit2 : dt->QueueSubmit2KHR;      // the same fallback as Hook_QueueSubmit2KHR, the other way round
    return submit2 ? submit2(q, n, s, f) : VK_ERROR_FEATURE_NOT_PRESENT;
}
VKAPI_ATTR VkResult VKAPI_CALL Hook_QueueSubmit2KHR(VkQueue q, uint32_t n, const VkSubmitInfo2* s, VkFence f) {
    DeviceDispatch* dt = LookupDevice(GetKey(q)); if (!dt) return VK_ERROR_DEVICE_LOST; QueueLock l(q);
    const auto submit2 = dt->QueueSubmit2KHR ? dt->QueueSubmit2KHR : dt->QueueSubmit2;
    return submit2 ? submit2(q, n, s, f) : VK_ERROR_FEATURE_NOT_PRESENT;
}
VKAPI_ATTR VkResult VKAPI_CALL Hook_QueueBindSparse(VkQueue q, uint32_t n, const VkBindSparseInfo* s, VkFence f) {
    DeviceDispatch* dt = LookupDevice(GetKey(q)); if (!dt) return VK_ERROR_DEVICE_LOST; QueueLock l(q); return dt->QueueBindSparse(q, n, s, f);
}
VKAPI_ATTR VkResult VKAPI_CALL Hook_QueueWaitIdle(VkQueue q) {
    DeviceDispatch* dt = LookupDevice(GetKey(q));
    if (!dt) return VK_ERROR_DEVICE_LOST;
    const uint64_t t0 = NowNs();
    QueueLock l(q);
    const uint64_t t1 = NowNs();
    const VkResult r = dt->QueueWaitIdle(q);
    const uint64_t t2 = NowNs();
    if (t2 - t0 > 500'000'000ull) FG_LOG("shutdown: vkQueueWaitIdle took %.0f ms (queue lock %.0f ms, wait %.0f ms)", (t2 - t0) / 1e6, (t1 - t0) / 1e6, (t2 - t1) / 1e6);
    return r;
}
VKAPI_ATTR VkResult VKAPI_CALL Hook_DeviceWaitIdle(VkDevice device) {
    DeviceDispatch* dt = LookupDevice(GetKey(device));
    if (!dt) return VK_ERROR_DEVICE_LOST;
    const uint64_t t0 = NowNs();
    std::vector<std::unique_lock<std::mutex>> locks;
    for (std::mutex* m : DeviceQueueMutexes(GetKey(device))) locks.emplace_back(*m);        // all queues, in a fixed order
    const uint64_t t1 = NowNs();
    const VkResult r = dt->DeviceWaitIdle(device);
    const uint64_t t2 = NowNs();
    if (t2 - t0 > 500'000'000ull) FG_LOG("shutdown: vkDeviceWaitIdle took %.0f ms (queue locks %.0f ms, wait %.0f ms)", (t2 - t0) / 1e6, (t1 - t0) / 1e6, (t2 - t1) / 1e6);
    return r;
}
// About the raw SwapchainState pointers in the acquire and present-wait hooks below: the swapchain is externally synchronized in vkAcquireNextImageKHR,
// vkAcquireNextImage2KHR (VkAcquireNextImageInfoKHR::swapchain), vkWaitForPresentKHR and vkDestroySwapchainKHR (externsync in vk.xml), so the game cannot destroy a
// swapchain while another thread is inside one of these calls, and the pointer from FindSwapchain stays valid for the whole call. The fence hooks are different (a fence
// is not synchronized with its swapchain) and hold shared ownership, see FlushForFences.
VkResult Impl_AcquireNextImageKHR(VkDevice device, VkSwapchainKHR swapchain, uint64_t timeout,
                                                        VkSemaphore sem, VkFence fence, uint32_t* pIndex) {
    DeviceDispatch* dt = LookupDevice(GetKey(device));
    if (!dt) return VK_ERROR_DEVICE_LOST;
    SwapchainState* sc = FindSwapchain(swapchain);
    if (!sc || !sc->gpu || !sc->presenter) { t_guard_forwarded = true; return dt->AcquireNextImageKHR(device, swapchain, timeout, sem, fence, pIndex); }
    GpuState* g = sc->gpu;
    struct Mark { SwapchainState* s; uint64_t t0; explicit Mark(SwapchainState* x) : s(x), t0(NowNs()) { s->acquire_wait_since_ns = t0; } ~Mark() { s->acquire_wait_since_ns = 0; s->wait_acquire_ns += NowNs() - t0; } } mark(sc);
    return PolledAcquire(timeout, [&] {
        std::lock_guard<std::mutex> l(g->acquire_mu);           // the presenter acquires hidden images on the same swapchain
        const VkResult r = dt->AcquireNextImageKHR(device, swapchain, 0, sem, fence, pIndex);
        if (r == VK_SUCCESS || r == VK_SUBOPTIMAL_KHR) t_guard_forwarded = true;     // an image was acquired and the semaphore or fence will be signalled: the guard must not acquire again
        return r;
    });
}
VkResult Impl_AcquireNextImage2KHR(VkDevice device, const VkAcquireNextImageInfoKHR* info, uint32_t* pIndex) {
    DeviceDispatch* dt = LookupDevice(GetKey(device));
    if (!dt) return VK_ERROR_DEVICE_LOST;
    SwapchainState* sc = FindSwapchain(info->swapchain);
    if (!sc || !sc->gpu || !sc->presenter) { t_guard_forwarded = true; return dt->AcquireNextImage2KHR(device, info, pIndex); }
    GpuState* g = sc->gpu;
    struct Mark { SwapchainState* s; uint64_t t0; explicit Mark(SwapchainState* x) : s(x), t0(NowNs()) { s->acquire_wait_since_ns = t0; } ~Mark() { s->acquire_wait_since_ns = 0; s->wait_acquire_ns += NowNs() - t0; } } mark(sc);
    VkAcquireNextImageInfoKHR once = *info; once.timeout = 0;
    return PolledAcquire(info->timeout, [&] {
        std::lock_guard<std::mutex> l(g->acquire_mu);
        const VkResult r = dt->AcquireNextImage2KHR(device, &once, pIndex);
        if (r == VK_SUCCESS || r == VK_SUBOPTIMAL_KHR) t_guard_forwarded = true;
        return r;
    });
}

// ---------------------------------------------------------------- host out of memory
// An exception (std::bad_alloc from a vector, string or map, std::system_error from a thread or a lock) must never leave a hook: it would run into the game's C
// code and end the process. Every entry point that allocates catches it here, logs once (a few lines at most), and falls back: a create hook fails with
// VK_ERROR_OUT_OF_HOST_MEMORY, a destroy hook goes on to the next layer's destroy (unless that was already called), a wait or acquire hook calls the real function,
// and a present forwards the game's own present when the layer has not touched the frame yet and fails with VK_ERROR_OUT_OF_HOST_MEMORY otherwise (never two presents).
// Nothing is locked while a fallback runs: the locks of the failed body were released while the exception unwound. This changes only the failure path.
namespace fg_layer {
std::atomic<int> g_guard_notes{0};
void GuardNote(const char* hook, const char* what) noexcept {
    if (g_guard_notes.fetch_add(1) >= 5) return;
    try { FG_LOG("%s: %s: the layer steps aside for this call (out of host memory or an internal error)", hook, what); } catch (...) {}
}
// Runs body(); true when it returned normally. On an exception: logs and returns false.
template <class F> bool Run(const char* hook, F&& body) noexcept {
    try { body(); return true; }
    catch (const std::bad_alloc&) { GuardNote(hook, "std::bad_alloc"); }
    catch (const std::exception& e) { GuardNote(hook, e.what()); }
    catch (...) { GuardNote(hook, "unknown exception"); }
    return false;
}
// A fallback must not throw out of the hook either.
template <class F> void Quiet(F&& f) noexcept { try { f(); } catch (...) {} }
} // namespace

VKAPI_ATTR VkResult VKAPI_CALL Hook_CreateInstance(const VkInstanceCreateInfo* ci, const VkAllocationCallbacks* a, VkInstance* out) {
    VkResult r = VK_ERROR_OUT_OF_HOST_MEMORY;
    (void)Run("vkCreateInstance", [&] { r = Impl_CreateInstance(ci, a, out); });
    return r;
}
VKAPI_ATTR VkResult VKAPI_CALL Hook_CreateDevice(VkPhysicalDevice pd, const VkDeviceCreateInfo* ci, const VkAllocationCallbacks* a, VkDevice* out) {
    VkResult r = VK_ERROR_OUT_OF_HOST_MEMORY;
    (void)Run("vkCreateDevice", [&] { r = Impl_CreateDevice(pd, ci, a, out); });
    return r;
}
VKAPI_ATTR VkResult VKAPI_CALL Hook_CreateSwapchainKHR(VkDevice d, const VkSwapchainCreateInfoKHR* ci, const VkAllocationCallbacks* a, VkSwapchainKHR* out) {
    VkResult r = VK_ERROR_OUT_OF_HOST_MEMORY;
    const VkSwapchainKHR before = out ? *out : VK_NULL_HANDLE;
    if (Run("vkCreateSwapchainKHR", [&] { r = Impl_CreateSwapchainKHR(d, ci, a, out); })) return r;
    if (out && *out != before && *out != VK_NULL_HANDLE) {         // the driver had created a swapchain before the failure: the game gets none, so it must not stay alive
        const VkSwapchainKHR made = *out; *out = VK_NULL_HANDLE;
        Quiet([&] { Hook_DestroySwapchainKHR(d, made, a); });
    }
    return VK_ERROR_OUT_OF_HOST_MEMORY;
}

// The destroy fallbacks below are the plain driver destroy: if the body failed before its own call to the next layer, this skips the device idle wait, the
// layer's state is not cleaned (it is leaked) and a presenter thread is not stopped. That is acceptable on the out-of-memory path: the game is closing the object.
VKAPI_ATTR void VKAPI_CALL Hook_DestroyInstance(VkInstance instance, const VkAllocationCallbacks* a) {
    void* key = GetKey(instance);                                 // read before the handle dies
    t_guard_forwarded = false;
    if (Run("vkDestroyInstance", [&] { Impl_DestroyInstance(instance, a); })) return;
    if (!t_guard_forwarded) Quiet([&] { if (InstanceDispatch* id = LookupInstance(key)) { id->DestroyInstance(instance, a); } });
}
VKAPI_ATTR void VKAPI_CALL Hook_DestroyDevice(VkDevice device, const VkAllocationCallbacks* a) {
    void* key = GetKey(device);
    t_guard_forwarded = false;
    if (Run("vkDestroyDevice", [&] { Impl_DestroyDevice(device, a); })) return;
    if (!t_guard_forwarded) Quiet([&] { if (DeviceDispatch* dd = LookupDevice(key)) { dd->DestroyDevice(device, a); } });
}
VKAPI_ATTR void VKAPI_CALL Hook_DestroySwapchainKHR(VkDevice device, VkSwapchainKHR swapchain, const VkAllocationCallbacks* a) {
    void* key = GetKey(device);
    t_guard_forwarded = false;
    if (Run("vkDestroySwapchainKHR", [&] { Impl_DestroySwapchainKHR(device, swapchain, a); })) return;
    if (!t_guard_forwarded) Quiet([&] { if (DeviceDispatch* dt = LookupDevice(key)) { dt->DestroySwapchainKHR(device, swapchain, a); } });
}

VKAPI_ATTR VkResult VKAPI_CALL Hook_AcquireNextImageKHR(VkDevice device, VkSwapchainKHR swapchain, uint64_t timeout, VkSemaphore sem, VkFence fence, uint32_t* pIndex) {
    VkResult r = VK_ERROR_OUT_OF_HOST_MEMORY;
    t_guard_forwarded = false;
    if (Run("vkAcquireNextImageKHR", [&] { r = Impl_AcquireNextImageKHR(device, swapchain, timeout, sem, fence, pIndex); })) return r;
    if (t_guard_forwarded) return VK_ERROR_OUT_OF_HOST_MEMORY;          // the real acquire already ran and signalled: a second one would signal the semaphore twice
    Quiet([&] { if (DeviceDispatch* dt = LookupDevice(GetKey(device))) r = dt->AcquireNextImageKHR(device, swapchain, timeout, sem, fence, pIndex); });
    return r;
}
VKAPI_ATTR VkResult VKAPI_CALL Hook_AcquireNextImage2KHR(VkDevice device, const VkAcquireNextImageInfoKHR* info, uint32_t* pIndex) {
    VkResult r = VK_ERROR_OUT_OF_HOST_MEMORY;
    t_guard_forwarded = false;
    if (Run("vkAcquireNextImage2KHR", [&] { r = Impl_AcquireNextImage2KHR(device, info, pIndex); })) return r;
    if (t_guard_forwarded) return VK_ERROR_OUT_OF_HOST_MEMORY;
    Quiet([&] { if (DeviceDispatch* dt = LookupDevice(GetKey(device))) r = dt->AcquireNextImage2KHR(device, info, pIndex); });
    return r;
}
VKAPI_ATTR VkResult VKAPI_CALL Hook_WaitForFences(VkDevice device, uint32_t n, const VkFence* fences, VkBool32 all, uint64_t timeout) {
    VkResult r = VK_ERROR_OUT_OF_HOST_MEMORY;
    if (Run("vkWaitForFences", [&] { r = Impl_WaitForFences(device, n, fences, all, timeout); })) return r;
    Quiet([&] { if (DeviceDispatch* dt = LookupDevice(GetKey(device))) r = dt->WaitForFences(device, n, fences, all, timeout); });
    return r;
}
VKAPI_ATTR VkResult VKAPI_CALL Hook_GetFenceStatus(VkDevice device, VkFence fence) {
    VkResult r = VK_ERROR_OUT_OF_HOST_MEMORY;
    if (Run("vkGetFenceStatus", [&] { r = Impl_GetFenceStatus(device, fence); })) return r;
    Quiet([&] { if (DeviceDispatch* dt = LookupDevice(GetKey(device))) r = dt->GetFenceStatus(device, fence); });
    return r;
}
VKAPI_ATTR VkResult VKAPI_CALL Hook_WaitForPresentKHR(VkDevice device, VkSwapchainKHR swapchain, uint64_t presentId, uint64_t timeout) {
    VkResult r = VK_ERROR_OUT_OF_HOST_MEMORY;
    if (Run("vkWaitForPresentKHR", [&] { r = Impl_WaitForPresentKHR(device, swapchain, presentId, timeout); })) return r;
    Quiet([&] { if (DeviceDispatch* dt = LookupDevice(GetKey(device))) r = dt->WaitForPresentKHR ? dt->WaitForPresentKHR(device, swapchain, presentId, timeout) : VK_ERROR_EXTENSION_NOT_PRESENT; });
    return r;
}

VKAPI_ATTR VkResult VKAPI_CALL Hook_QueuePresentKHR(VkQueue queue, const VkPresentInfoKHR* pPresentInfo) {
    VkResult r = VK_ERROR_OUT_OF_HOST_MEMORY;
    t_guard_present_consumed = false; t_guard_present_sc = nullptr;
    if (Run("vkQueuePresentKHR", [&] { r = Impl_QueuePresentKHR(queue, pPresentInfo); })) return r;
    SwapchainState* sc = t_guard_present_sc;
    const bool consumed = t_guard_present_consumed;
    if (sc) Quiet([&] { std::lock_guard<std::mutex> fl(sc->flow_mu); sc->hold_failed = true; });   // no more holding on this swapchain; a held frame goes out with the next present
    if (!consumed) {                                              // the layer has not touched the frame: the game's own present, as if the layer were not there
        Quiet([&] {
            if (DeviceDispatch* dt = LookupDevice(GetKey(queue))) {
                std::mutex* qm = QueueMutex(queue);
                std::unique_lock<std::mutex> lk;
                if (qm) lk = std::unique_lock<std::mutex>(*qm);
                r = dt->QueuePresentKHR(queue, pPresentInfo);
            }
        });
        return r;
    }
    if (pPresentInfo && pPresentInfo->pResults && pPresentInfo->swapchainCount >= 1) pPresentInfo->pResults[0] = VK_ERROR_OUT_OF_HOST_MEMORY;
    return VK_ERROR_OUT_OF_HOST_MEMORY;                           // the frame may be held or queued already: presenting it again could show it twice
}
