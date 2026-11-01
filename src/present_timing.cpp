#include "present_timing.hpp"
#include "live_config.hpp"

#include <cmath>
#include <vector>

namespace {

const char* DomainName(VkTimeDomainKHR d) {
    switch (int(d)) {
        case VK_TIME_DOMAIN_DEVICE_KHR:                   return "device";
        case VK_TIME_DOMAIN_CLOCK_MONOTONIC_KHR:          return "clock_monotonic";
        case VK_TIME_DOMAIN_CLOCK_MONOTONIC_RAW_KHR:      return "clock_monotonic_raw";
        case VK_TIME_DOMAIN_QUERY_PERFORMANCE_COUNTER_KHR:return "qpc";
        case VK_TIME_DOMAIN_PRESENT_STAGE_LOCAL_EXT:      return "present_stage_local";
        case VK_TIME_DOMAIN_SWAPCHAIN_LOCAL_EXT:          return "swapchain_local";
        default:                                          return "other";
    }
}

// domain time = CLOCK_MONOTONIC time + offset, sampled together so the pair is consistent.
void RefreshCalibration(const DeviceDispatch& dt, SwapchainState& sc, uint64_t now_ns) {
    sc.last_calib_ns = now_ns;
    if (!dt.GetCalibratedTimestampsKHR) { sc.calib_ok = false; return; }
    VkSwapchainCalibratedTimestampInfoEXT si{VK_STRUCTURE_TYPE_SWAPCHAIN_CALIBRATED_TIMESTAMP_INFO_EXT};
    si.swapchain = sc.handle; si.presentStage = sc.stage; si.timeDomainId = sc.time_domain_id;
    VkCalibratedTimestampInfoKHR infos[2]{};
    infos[0].sType = VK_STRUCTURE_TYPE_CALIBRATED_TIMESTAMP_INFO_KHR; infos[0].pNext = &si; infos[0].timeDomain = sc.time_domain;
    infos[1].sType = VK_STRUCTURE_TYPE_CALIBRATED_TIMESTAMP_INFO_KHR; infos[1].timeDomain = VK_TIME_DOMAIN_CLOCK_MONOTONIC_KHR;
    uint64_t ts[2]{}, deviation = 0;
    if (dt.GetCalibratedTimestampsKHR(dt.device, 2, infos, ts, &deviation) != VK_SUCCESS) { sc.calib_ok = false; return; }
    sc.domain_offset_ns = int64_t(ts[0]) - int64_t(ts[1]);
    sc.calib_ok = true;
}

} // namespace

bool SetupSwapchainTiming(const DeviceDispatch& dt, SwapchainState& sc, VkPresentStageFlagsEXT stage_mask, uint32_t queue_size) {
    sc.timing_enabled = false;
    if (!dt.SetSwapchainPresentTimingQueueSizeEXT || !dt.GetSwapchainTimeDomainPropertiesEXT || !dt.GetPastPresentationTimingEXT) {
        FG_LOG("present timing: entry points missing");
        return false;
    }

    // The one present stage we ask about and aim at: when the first pixel is visible, else the first one the surface offers.
    for (VkPresentStageFlagsEXT bit : {VkPresentStageFlagsEXT(VK_PRESENT_STAGE_IMAGE_FIRST_PIXEL_VISIBLE_BIT_EXT),
                                       VkPresentStageFlagsEXT(VK_PRESENT_STAGE_IMAGE_FIRST_PIXEL_OUT_BIT_EXT),
                                       VkPresentStageFlagsEXT(VK_PRESENT_STAGE_REQUEST_DEQUEUED_BIT_EXT),
                                       VkPresentStageFlagsEXT(VK_PRESENT_STAGE_QUEUE_OPERATIONS_END_BIT_EXT)})
        if (stage_mask & bit) { sc.stage = bit; break; }
    if (!sc.stage) { FG_LOG("present timing: the surface offers no present stage to query"); return false; }

    VkResult r = dt.SetSwapchainPresentTimingQueueSizeEXT(dt.device, sc.handle, queue_size);
    if (r != VK_SUCCESS) { FG_LOG("present timing: vkSetSwapchainPresentTimingQueueSizeEXT failed (%d)", int(r)); return false; }

    // Time domains: count first, then fill.
    VkSwapchainTimeDomainPropertiesEXT dp{VK_STRUCTURE_TYPE_SWAPCHAIN_TIME_DOMAIN_PROPERTIES_EXT};
    uint64_t counter = 0;
    r = dt.GetSwapchainTimeDomainPropertiesEXT(dt.device, sc.handle, &dp, &counter);
    if ((r != VK_SUCCESS && r != VK_INCOMPLETE) || dp.timeDomainCount == 0) {
        FG_LOG("present timing: no time domains reported (%d)", int(r));
        return false;
    }
    std::vector<VkTimeDomainKHR> domains(dp.timeDomainCount);
    std::vector<uint64_t> ids(dp.timeDomainCount);
    dp.pTimeDomains = domains.data(); dp.pTimeDomainIds = ids.data();
    r = dt.GetSwapchainTimeDomainPropertiesEXT(dt.device, sc.handle, &dp, &counter);
    if (r != VK_SUCCESS && r != VK_INCOMPLETE) { FG_LOG("present timing: time domain query failed (%d)", int(r)); return false; }
    domains.resize(dp.timeDomainCount); ids.resize(dp.timeDomainCount);

    int pick = -1;
    for (size_t i = 0; i < domains.size(); ++i) {
        FG_LOG("present timing: time domain %zu = %s (id %llu)", i, DomainName(domains[i]), (unsigned long long)ids[i]);
        if (domains[i] == VK_TIME_DOMAIN_CLOCK_MONOTONIC_KHR) pick = int(i);          // exact match: no conversion
    }
    if (pick < 0)
        for (VkTimeDomainKHR want : {VK_TIME_DOMAIN_SWAPCHAIN_LOCAL_EXT, VK_TIME_DOMAIN_PRESENT_STAGE_LOCAL_EXT})
            for (size_t i = 0; i < domains.size() && pick < 0; ++i) if (domains[i] == want) pick = int(i);
    if (pick < 0) { FG_LOG("present timing: no usable time domain"); return false; }

    sc.time_domain = domains[pick]; sc.time_domain_id = ids[pick];
    sc.domain_monotonic = sc.time_domain == VK_TIME_DOMAIN_CLOCK_MONOTONIC_KHR;
    if (!sc.domain_monotonic) {
        RefreshCalibration(dt, sc, NowNs());
        if (!sc.calib_ok) FG_LOG("present timing: cannot calibrate %s against CLOCK_MONOTONIC; targets will be 'as soon as possible'", DomainName(sc.time_domain));
    }
    sc.image_count = queue_size / 4;
    sc.timing_enabled = true;
    FG_LOG("present timing: using domain %s (id %llu), stage 0x%x, queue size %u", DomainName(sc.time_domain),
           (unsigned long long)sc.time_domain_id, unsigned(sc.stage), queue_size);
    return true;
}

uint64_t ToDomainTime(const DeviceDispatch& dt, SwapchainState& sc, uint64_t mono_ns) {
    if (mono_ns == 0) return 0;
    if (sc.domain_monotonic) return mono_ns;
    if (mono_ns > sc.last_calib_ns + 1'000'000'000ull) RefreshCalibration(dt, sc, mono_ns);   // about once a second
    if (!sc.calib_ok) return 0;
    return uint64_t(int64_t(mono_ns) + sc.domain_offset_ns);
}

const void* BuildTimingChain(TimingChain& c, SwapchainState& sc, uint64_t target_time, const void* app_pnext) {
    c.present_id = sc.next_present_id++;
    c.id2 = {VK_STRUCTURE_TYPE_PRESENT_ID_2_KHR, app_pnext, 1, &c.present_id};

    c.info = {};
    c.info.sType = VK_STRUCTURE_TYPE_PRESENT_TIMING_INFO_EXT;
    c.info.targetTime = target_time;
    c.info.timeDomainId = sc.time_domain_id;
    c.info.presentStageQueries = sc.stage;                          // report when it reached this stage
    c.info.targetTimeDomainPresentStage = sc.stage;                 // the stage targetTime refers to
    // Show the image at the refresh cycle nearest the target instead of strictly not before it.
    c.info.flags = target_time ? VK_PRESENT_TIMING_INFO_PRESENT_AT_NEAREST_REFRESH_CYCLE_BIT_EXT : 0;

    c.timings = {VK_STRUCTURE_TYPE_PRESENT_TIMINGS_INFO_EXT, &c.id2, 1, &c.info};
    return &c.timings;
}

void DrainPastTimings(const DeviceDispatch& dt, SwapchainState& sc) {
    if (!sc.timing_enabled || !dt.GetPastPresentationTimingEXT) return;
    constexpr uint32_t kBatch = 8;
    for (int rounds = 0; rounds < 16; ++rounds) {                           // bounded: never spin in the game's present call
        VkPastPresentationTimingEXT t[kBatch]{};
        VkPresentStageTimeEXT stages[kBatch]{};
        for (uint32_t i = 0; i < kBatch; ++i) {
            t[i].sType = VK_STRUCTURE_TYPE_PAST_PRESENTATION_TIMING_EXT;
            t[i].presentStageCount = 1; t[i].pPresentStages = &stages[i];
        }
        VkPastPresentationTimingInfoEXT info{VK_STRUCTURE_TYPE_PAST_PRESENTATION_TIMING_INFO_EXT};
        info.swapchain = sc.handle;
        VkPastPresentationTimingPropertiesEXT props{VK_STRUCTURE_TYPE_PAST_PRESENTATION_TIMING_PROPERTIES_EXT};
        props.presentationTimingCount = kBatch; props.pPresentationTimings = t;

        VkResult r = dt.GetPastPresentationTimingEXT(dt.device, &info, &props);
        if ((r != VK_SUCCESS && r != VK_INCOMPLETE) || props.presentationTimingCount == 0) return;

        for (uint32_t i = 0; i < props.presentationTimingCount; ++i) {
            if (!t[i].reportComplete || t[i].presentStageCount < 1) continue;
            const uint64_t actual = stages[i].time;
            ++sc.st.results_total;
            if (t[i].targetTime) {                                          // error against the requested time, same domain
                const double err_ms = (double(int64_t(actual) - int64_t(t[i].targetTime))) / 1e6;
                ++sc.st.n_err; sc.st.sum_err_ms += err_ms; sc.st.max_abs_err_ms = std::max(sc.st.max_abs_err_ms, std::fabs(err_ms));
            }
            if (sc.st.last_actual_ns && actual > sc.st.last_actual_ns) {    // spacing between consecutive presents
                const double d = double(actual - sc.st.last_actual_ns) / 1e6;
                ++sc.st.n_int; sc.st.sum_int_ms += d; sc.st.sum_int2_ms += d * d;
            }
            sc.st.last_actual_ns = actual;
        }
        if (props.presentationTimingCount < kBatch) return;
    }
}

void LogTimingStats(const DeviceDispatch& dt, SwapchainState& sc, uint64_t now_ns) {
    if (sc.last_log_ns == 0) { sc.last_log_ns = now_ns; return; }
    if (now_ns - sc.last_log_ns < 1'000'000'000ull) return;
    sc.last_log_ns = now_ns;

    FG_LOG_ROUTINE("base FPS (smoothed) %.2f | delta %.2f ms | jitter %.2f ms", sc.timing.SmoothedFps(), sc.timing.SmoothedNs() / 1e6, sc.timing.JitterNs() / 1e6);
    if (!sc.timing_enabled) return;

    if (sc.refresh_ns == 0 && dt.GetSwapchainTimingPropertiesEXT) {          // may not be known until the first presents finished
        VkSwapchainTimingPropertiesEXT tp{VK_STRUCTURE_TYPE_SWAPCHAIN_TIMING_PROPERTIES_EXT};
        uint64_t counter = 0;
        if (dt.GetSwapchainTimingPropertiesEXT(dt.device, sc.handle, &tp, &counter) == VK_SUCCESS) {
            sc.refresh_ns = tp.refreshDuration;
            sc.refresh_vrr = tp.refreshInterval == UINT64_MAX;                       // "the presentation engine is operating in VRR mode"; 0 = it cannot tell, treated as fixed
            if (sc.refresh_ns) FG_LOG("display: refresh %.2f Hz (%s)", 1e9 / double(sc.refresh_ns), sc.refresh_vrr ? "variable refresh: TARGET_FPS is not limited" : "TARGET_FPS is limited to it");
        }
    }
    auto& s = sc.st;
    const double int_mean = s.n_int ? s.sum_int_ms / s.n_int : 0.0;
    const double int_sd   = s.n_int ? std::sqrt(std::max(0.0, s.sum_int2_ms / s.n_int - int_mean * int_mean)) : 0.0;
    FG_LOG_ROUTINE("timing: results=%llu err_mean_ms=%.3f err_abs_max_ms=%.3f interval_mean_ms=%.3f interval_sd_ms=%.3f refresh_ms=%.3f queue_full=%llu domain=%s",
           (unsigned long long)s.results_total, s.n_err ? s.sum_err_ms / s.n_err : 0.0, s.max_abs_err_ms, int_mean, int_sd,
           sc.refresh_ns / 1e6, (unsigned long long)s.queue_full_total, DomainName(sc.time_domain));
    s.n_err = 0; s.sum_err_ms = 0; s.max_abs_err_ms = 0; s.n_int = 0; s.sum_int_ms = 0; s.sum_int2_ms = 0;   // window; totals stay
}
