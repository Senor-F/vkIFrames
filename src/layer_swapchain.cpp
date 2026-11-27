// Layer hooks: swapchain creation (with the extra images the layer needs) and destruction.
#include "layer_internal.hpp"
#include "live_config.hpp"

#include <algorithm>
#include <cstring>
#include <string>
#include <vector>

using namespace fg_layer;

// ---------------------------------------------------------------- swapchain

namespace {
// The fields of a swapchain request that the layer changes (and the rest of the structure is the same object): flags, usage, image count, present mode, old swapchain.
bool SameCreateInfo(const VkSwapchainCreateInfoKHR& a, const VkSwapchainCreateInfoKHR& b) {
    return a.flags == b.flags && a.imageUsage == b.imageUsage && a.minImageCount == b.minImageCount && a.presentMode == b.presentMode && a.oldSwapchain == b.oldSwapchain;
}
}

VkResult Impl_CreateSwapchainKHR(VkDevice device,
                                                       const VkSwapchainCreateInfoKHR* pCreateInfo,
                                                       const VkAllocationCallbacks* pAllocator,
                                                       VkSwapchainKHR* pSwapchain) {
    DeviceDispatch* dt = LookupDevice(GetKey(device));
    if (!dt) return VK_ERROR_INITIALIZATION_FAILED;
    const Config& cfg = GetConfig();

    VkSwapchainCreateInfoKHR ci = *pCreateInfo;
    const uint32_t app_min_images = ci.minImageCount;

    // Frame holding needs spare images (1 held + hidden acquires later), transfer usage on the swapchain images, and blit
    // support for the swapchain format and the ring format. Anything missing leaves this swapchain in passthrough.
    bool hold = cfg.hold && dt->sync2 && !(ci.flags & VK_SWAPCHAIN_CREATE_PROTECTED_BIT_KHR);
    VkSurfaceCapabilitiesKHR caps{};
    const bool have_caps = dt->instance->GetPhysicalDeviceSurfaceCapabilitiesKHR(dt->physical_device, ci.surface, &caps) == VK_SUCCESS;
    if (hold && have_caps) {
        const VkImageUsageFlags need_usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        VkFormatProperties fp{}, rp{};
        dt->instance->GetPhysicalDeviceFormatProperties(dt->physical_device, ci.imageFormat, &fp);
        dt->instance->GetPhysicalDeviceFormatProperties(dt->physical_device, GpuState::kRingFormat, &rp);
        const VkFormatFeatureFlags blit = VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT;
        const VkFormatFeatureFlags ring = blit | VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT;
        if ((caps.supportedUsageFlags & need_usage) != need_usage) { FG_LOG("hold: surface does not allow transfer usage"); hold = false; }
        else if ((fp.optimalTilingFeatures & blit) != blit)         { FG_LOG("hold: swapchain format %d cannot be blitted", int(ci.imageFormat)); hold = false; }
        else if ((rp.optimalTilingFeatures & ring) != ring)         { FG_LOG("hold: ring format cannot be used here"); hold = false; }
    } else if (hold) hold = false;

    int synth_capacity = kLiveMaxSyntheticNormal;                  // C for this swapchain (set below when the surface is known)
    if (have_caps) {
        // Images that exist at once beyond what the game expects: 1 held + up to k_max queued synthetic + 1 hidden acquire.
        uint32_t wanted = std::max(ci.minImageCount, kMinImageCount);
        synth_capacity = LiveSynthCapacityFor(cfg.max_synthetic);          // C: MAX_SYNTHETIC at game start (live.conf over the environment), at least 4, at most the limit EXPERIMENTAL allows
        if (hold) wanted = std::max(wanted, app_min_images + uint32_t(synth_capacity) + 2);
        if (caps.maxImageCount != 0) wanted = std::min(wanted, caps.maxImageCount);
        if (wanted != ci.minImageCount) {
            FG_LOG("vkCreateSwapchainKHR: minImageCount %u -> %u", ci.minImageCount, wanted);
            ci.minImageCount = wanted;
        }
        if (hold && ci.minImageCount < app_min_images + 1) { FG_LOG("hold: no spare image available (surface max %u)", caps.maxImageCount); hold = false; }
    }
    if (hold) ci.imageUsage |= VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;

    // Per-surface check: this surface must support present timing and present id 2, otherwise the flags would fail creation.
    VkPresentStageFlagsEXT stage_mask = 0;
    bool try_timing = false, surface_absolute = false;
    if (dt->timing_supported && dt->instance->GetPhysicalDeviceSurfaceCapabilities2KHR &&
        !(ci.flags & (VK_SWAPCHAIN_CREATE_PRESENT_TIMING_BIT_EXT | VK_SWAPCHAIN_CREATE_PRESENT_ID_2_BIT_KHR))) {
        VkPresentTimingSurfaceCapabilitiesEXT tcaps{VK_STRUCTURE_TYPE_PRESENT_TIMING_SURFACE_CAPABILITIES_EXT};
        VkSurfaceCapabilitiesPresentId2KHR    icaps{VK_STRUCTURE_TYPE_SURFACE_CAPABILITIES_PRESENT_ID_2_KHR};
        tcaps.pNext = &icaps;
        VkSurfaceCapabilities2KHR c2{VK_STRUCTURE_TYPE_SURFACE_CAPABILITIES_2_KHR};
        c2.pNext = &tcaps;
        VkPhysicalDeviceSurfaceInfo2KHR si{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SURFACE_INFO_2_KHR};
        si.surface = ci.surface;
        const VkResult cr = dt->instance->GetPhysicalDeviceSurfaceCapabilities2KHR(dt->physical_device, &si, &c2);
        FG_LOG("present timing: surface query result=%d timing=%u absolute=%u relative=%u id2=%u stages=0x%x", int(cr),
               tcaps.presentTimingSupported, tcaps.presentAtAbsoluteTimeSupported, tcaps.presentAtRelativeTimeSupported,
               icaps.presentId2Supported, unsigned(tcaps.presentStageQueries));
        if (cr == VK_SUCCESS &&
            tcaps.presentTimingSupported && icaps.presentId2Supported) {
            // Without absolute targets (e.g. a Wayland compositor lacking commit timing) timing still reports when frames
            // reached the screen; pacing then falls back to vblank (FIFO), exactly as DESIGN section 4.6 plans.
            if (!tcaps.presentAtAbsoluteTimeSupported) FG_LOG("present timing: surface has no absolute target times; feedback only, pacing stays vblank-driven");
            surface_absolute = tcaps.presentAtAbsoluteTimeSupported;
            stage_mask = tcaps.presentStageQueries;
            try_timing = stage_mask != 0;
        } else {
            FG_LOG("present timing: not supported on this surface");
        }
    }

    const VkPresentModeKHR app_mode = ci.presentMode;
    if (try_timing) {
        ci.flags |= VK_SWAPCHAIN_CREATE_PRESENT_TIMING_BIT_EXT | VK_SWAPCHAIN_CREATE_PRESENT_ID_2_BIT_KHR;
        if (ci.presentMode != VK_PRESENT_MODE_FIFO_KHR) {
            FG_LOG("present timing: forcing FIFO (app asked for mode %d)", int(ci.presentMode));
            ci.presentMode = VK_PRESENT_MODE_FIFO_KHR;
        }
    }

    VkResult r = dt->CreateSwapchainKHR(device, &ci, pAllocator, pSwapchain);
    if (r != VK_SUCCESS && try_timing) {
        FG_LOG("present timing: swapchain creation failed with the flags (%d), retrying without", int(r));
        ci.flags = pCreateInfo->flags; ci.presentMode = app_mode;
        ci.oldSwapchain = VK_NULL_HANDLE;     // the failed call already retired the game's oldSwapchain (Vulkan: "even if creation of the new swapchain fails"), and VUID-VkSwapchainCreateInfoKHR-oldSwapchain-01933 wants a non-retired one
        try_timing = false;
        r = dt->CreateSwapchainKHR(device, &ci, pAllocator, pSwapchain);
    }
    if (r != VK_SUCCESS && !SameCreateInfo(ci, *pCreateInfo)) {
        // Whatever we changed (usage bits, image count, timing flags, present mode) may be what the driver refuses: the game's own request is tried as it is, and that
        // swapchain is not touched by the layer at all (no state is registered: its presents pass straight through).
        FG_LOG("vkCreateSwapchainKHR failed with the layer's changes (%d): creating the game's swapchain unmodified", int(r));
        VkSwapchainCreateInfoKHR as_asked = *pCreateInfo;
        as_asked.oldSwapchain = VK_NULL_HANDLE;                      // already retired by the failed attempts above (see there), so it is not passed again
        r = dt->CreateSwapchainKHR(device, &as_asked, pAllocator, pSwapchain);
        if (r == VK_SUCCESS) LiveSetSynthCapacity(0);
        return r;
    }
    if (r != VK_SUCCESS) return r;

    // A replacement for the swapchain just created: made from `info` with the first one as oldSwapchain (the game's own oldSwapchain is already retired by the first
    // creation and must not be used twice), the first one is destroyed. False when the driver refuses: the first swapchain then stays. Creating the replacement
    // retires the first one even when it fails (Vulkan retires oldSwapchain whether or not creation succeeds), so the game then gets a retired swapchain: its next
    // acquire or present returns VK_ERROR_OUT_OF_DATE_KHR and it recreates its swapchain, which is the path every game has for a resize. A driver that refuses a
    // swapchain made exactly as the game asked is unusual enough that nothing better is done here.
    auto replace_with = [&](const VkSwapchainCreateInfoKHR& info) {
        VkSwapchainCreateInfoKHR again = info;
        again.oldSwapchain = *pSwapchain;
        VkSwapchainKHR made = VK_NULL_HANDLE;
        if (dt->CreateSwapchainKHR(device, &again, pAllocator, &made) != VK_SUCCESS) return false;
        DestroySwapchainState(*pSwapchain);
        dt->DestroySwapchainKHR(device, *pSwapchain, pAllocator);
        *pSwapchain = made;
        return true;
    };

    SwapchainState* sc = RegisterSwapchain(*pSwapchain);
    sc->hold_candidate = hold; sc->extent = ci.imageExtent; sc->format = ci.imageFormat;
    if (hold) {
        // What this swapchain can really hold: C limited by the images the driver granted (1 held + C queued synthetic + 1 hidden acquire beyond the app's own).
        uint32_t granted = 0;
        if (dt->GetSwapchainImagesKHR && dt->GetSwapchainImagesKHR(device, *pSwapchain, &granted, nullptr) >= 0 && granted > 0) {
            const int fit = int(granted) - int(app_min_images) - 2;
            if (fit < 1) {
                // Not even one synthetic frame has an image (held frame + one queued + one spare for the acquire): holding would only add a frame of latency and the first
                // synthetic tick would wait for an image that cannot be released. Pass-through for this swapchain: hold_candidate is read only by the present path
                // (before any GpuInit, capture or presenter thread exist), so turning it off here is safe.
                FG_LOG("synthetic frames: swapchain built for %d, but only %u images granted (the app asked for %u): not even one synthetic frame fits, frame holding is off for this swapchain", synth_capacity, granted, app_min_images);
                sc->hold_candidate = false; sc->synth_capacity = 0;
                LiveSetSynthCapacity(0);
                // Pass-through: the swapchain is made again exactly as the game asked (its flags, present mode, image count and usage), not with the layer's extra images,
                // usage bits, timing flags and forced FIFO that serve nothing now. If the driver refuses that, the swapchain just created stays.
                if (replace_with(*pCreateInfo)) FG_LOG("pass-through: the swapchain was created again as the game asked");
                return VK_SUCCESS;
            }
            sc->synth_capacity = std::clamp(std::min(synth_capacity, fit), 1, kLiveMaxSynthetic);
            FG_LOG("synthetic frames: swapchain built for %d, %u images granted (the app asked for %u): capacity %d%s", synth_capacity, granted, app_min_images, sc->synth_capacity,
                   sc->synth_capacity < synth_capacity ? " (fewer than wanted: a higher MAX_SYNTHETIC is clamped to it)" : "");
        } else sc->synth_capacity = synth_capacity;
        LiveSetSynthCapacity(sc->synth_capacity);
    }
    if (try_timing) {
        if (!SetupSwapchainTiming(*dt, *sc, stage_mask, ci.minImageCount * 4)) {
            FG_LOG("present timing: setup failed, presenting without timing");
            // Without timing the timing flags and the forced FIFO serve nothing: the swapchain is made again with the game's flags and present mode (the rest of the layer's
            // changes stay, they belong to frame holding). If the driver refuses, the first swapchain stays as it is.
            VkSwapchainCreateInfoKHR plain_timing = ci;
            plain_timing.flags = pCreateInfo->flags; plain_timing.presentMode = app_mode;
            const uint32_t keep_capacity = uint32_t(sc->synth_capacity);
            const bool keep_hold = sc->hold_candidate;
            if (replace_with(plain_timing)) {
                sc = RegisterSwapchain(*pSwapchain);
                sc->hold_candidate = keep_hold; sc->extent = ci.imageExtent; sc->format = ci.imageFormat; sc->synth_capacity = int(keep_capacity);
                FG_LOG("present timing: the swapchain was created again without the timing flags, with the game's present mode");
            }
        }
        sc->absolute_ok = sc->timing_enabled && surface_absolute;
    }
    return VK_SUCCESS;
}

void Impl_DestroySwapchainKHR(VkDevice device, VkSwapchainKHR swapchain,
                                                    const VkAllocationCallbacks* pAllocator) {
    DeviceDispatch* dt = LookupDevice(GetKey(device));
    if (!dt) return;
    StatsBenchAbort("the game closed its swapchain");           // a benchmark run stops and restores ENABLED
    FG_LOG("shutdown: vkDestroySwapchainKHR");
    const uint64_t t0 = NowNs();
    if (swapchain) {
        if (SwapchainState* sc = FindSwapchain(swapchain)) {
            if (sc->timing_enabled) { std::lock_guard<std::mutex> l(sc->mu); DrainPastTimings(*dt, *sc); }   // under mu like every other caller: the presenter thread may be in its own drain
            bool detached;
            {   // flow_mu: a thread that flushes the held frame for a fence or a present id (Hook_WaitForFences, Hook_GetFenceStatus, Hook_WaitForPresentKHR) holds it for the whole
                // flush. Destroy waits for that flush here; after it those threads find no GPU state (sc->gpu is null) and do nothing. The state itself stays allocated
                // for as long as a flush holds a reference to it (FlushForFences).
                std::lock_guard<std::mutex> fl(sc->flow_mu);
                detached = StopPresenter(*sc);                                    // no job may touch the swapchain while it is destroyed
                FG_LOG("shutdown: presenter stopped (%.0f ms)", (NowNs() - t0) / 1e6);
                HeldFencesDrop(*dt, sc);                                          // a later wait on the held frame's fence must not reach this state
                if (!detached && sc->gpu) {
                    GpuState* g = sc->gpu;
                    sc->gpu = nullptr;                                            // the held frame is simply dropped with its swapchain
                    GpuDestroy(*dt, g);
                    FG_LOG("shutdown: GPU resources destroyed (%.0f ms)", (NowNs() - t0) / 1e6);
                }
            }
            if (detached) {
                // The presenter thread is stuck in a driver call and still holds the swapchain state, its GPU state and the device dispatch table:
                // none of them is freed (this is shutdown, a small leak); vkDestroyDevice leaves the dispatch table alone too.
                FG_LOG("shutdown: the presenter thread is still running; its swapchain state, GPU state and dispatch table are left allocated");
                g_presenter_detached = true;
                LeakSwapchainState(swapchain);
            } else {
                DestroySwapchainState(swapchain);
            }
        } else DestroySwapchainState(swapchain);
    }
    t_guard_forwarded = true;
    dt->DestroySwapchainKHR(device, swapchain, pAllocator);
    FG_LOG("shutdown: swapchain destroyed (%.0f ms)", (NowNs() - t0) / 1e6);
}
