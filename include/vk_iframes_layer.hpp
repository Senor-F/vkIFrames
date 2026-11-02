#pragma once

#include <vulkan/vulkan.h>
#include <vulkan/vk_layer.h>

#include <algorithm>
#include <atomic>
#include <mutex>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <memory>
#include <string>
#include <vector>

#include "output_clock.hpp"
#include "stats.hpp"
#include "timing_engine.hpp"

#define FRAMEGEN_EXPORT extern "C" __attribute__((visibility("default")))
bool LiveLogOpen();   // live_config.cpp
int  LiveMaxSyntheticLimit();   // live_config.cpp: 10 when VKIFRAME_EXPERIMENTAL was set at game start, else 4
inline std::atomic<FILE*> g_fg_log{nullptr};              // the layer's log stream (live.log), null = stderr; swapped when live.log is rotated while the layer runs
inline FILE* FgLog() { FILE* f = g_fg_log.load(std::memory_order_acquire); return f ? f : stderr; }
#define FG_LOG(...) do { FILE* fg_f_ = FgLog(); std::fprintf(fg_f_, "[FrameGen] " __VA_ARGS__); std::fputc('\n', fg_f_); } while (0)

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

// ---- Configuration (environment variables, read once) ----
struct Config {
    float target_fps{60.0f};     // VKIFRAME_TARGET_FPS
    bool  phase_lock{true};      // VKIFRAME_PHASE_LOCK: align one tick per pair with the native frame when the ratio is near an integer
    float synth_margin{0.15f};   // VKIFRAME_SYNTH_MARGIN (0..0.45): every tick of an interpolated pair is a synthetic frame, with t kept at least this far from 0 and 1, so no native frame is shown as is (0 = off)
    float phase_offset{0.5f};    // VKIFRAME_PHASE_OFFSET (0..1): with the phase lock, 0 puts a tick on the native frame, 0.5 puts the ticks between (t = 0.25 / 0.75 at 45 -> 90)
    float min_gain{1.01f};       // VKIFRAME_MIN_GAIN: interpolate only above this base-time / target-time ratio
    int   max_synthetic{3};      // VKIFRAME_MAX_SYNTHETIC: most synthetic frames between two natives
    float base_fps_cap{45.0f};   // VKIFRAME_BASE_FPS_CAP (0 = off): the layer limits the game's present rate itself
    bool  hold{true};            // VKIFRAME_HOLD=0: keep the Stage 3a dry run only, never hold a frame
    bool  synth{true};           // VKIFRAME_SYNTH=0: hold frames but never create synthetic ones (Stage 3b behaviour)
    bool  pace_sleep{false};     // VKIFRAME_PACE_SLEEP=1: sleep in the game thread until each tick (experimental, vblank-paced path)
    bool  presenter{true};       // VKIFRAME_PRESENTER=0: present the ticks from the game thread (Stage 3c-i behaviour, no pacing thread)
    int   cmd_buffers{8};        // VKIFRAME_CMD_BUFFERS (4..32): command buffer entries per swapchain; a GPU-bound game makes the game thread wait on the fence of the oldest one
    int   acquire_wait_us{1000}; // VKIFRAME_ACQUIRE_WAIT_US: the presenter waits this long per try for a free swapchain image without holding the capture mutex; 0 = old polling under the mutex
    int   pace_lead_us{2000};    // VKIFRAME_PACE_LEAD_US: the presenter calls present this long before a tick is due (vblank-paced path)
    bool  absolute{false};       // VKIFRAME_ABSOLUTE=1: give each tick an absolute target time when the surface supports it (untested path)
    bool  flow{false};           // VKIFRAME_FLOW=1: estimate the optical flow of every pair (Stage 4; the blend still ignores it until Stage 5)
    bool  interpolate{true};     // VKIFRAME_INTERPOLATE (old name VKIFRAME_WARP; default 1, =0 for the plain cross-fade): build the in-between frames by interpolating along the flow instead of the plain blend (Stage 5; implies VKIFRAME_FLOW)
    int   tickmark{0};           // VKIFRAME_TICKMARK: 1 = 64x64 square top-left, red for the first half of a pair, green for the second; 2 = also a bar along the bottom that steps 6 px every displayed frame (120 steps/s at 60->120); 3 = the bar steps 12 px once per pair (60 steps/s, both frames of a pair identical). 2 vs 3 shows by eye whether 120 distinct frames arrive
    int   flow_level{2};         // VKIFRAME_FLOW_LEVEL: pyramid level of the finished flow field, 1 = half resolution (finest, most expensive), 2 = quarter (default), 3 = eighth
    float interp_guide{0.08f};     // VKIFRAME_INTERP_GUIDE: luma sigma of the colour-guided vector weighting (0 = plain bilinear)
    float conf_lo{0.06f};        // VKIFRAME_CONF_LO / VKIFRAME_CONF_HI: match cost where the interpolation starts / finishes fading to the cross-fade (HI <= LO = off)
    float conf_hi{0.14f};
    float pan_lo{0.15f};         // VKIFRAME_PAN_LO / VKIFRAME_PAN_HI: when this share of the flow field is occluded or badly matched the frame starts to cross-fade, fully at the high value (a fast pan; captures: calm 0.04-0.10, pan 0.84; pan_hi <= pan_lo = off)
    float pan_hi{0.35f};
    float ui_mask{0.08f};        // VKIFRAME_UI_MASK_THRESHOLD: static-UI mask; where the 3x3 mean of |A-B| (luma) stays below this the pixel cross-fades instead of warping (0 = off; 0.03 suits film-grain content)
    bool  cap_early{false};      // VKIFRAME_CAP_EARLY=1: VKIFRAME_BASE_FPS_CAP waits before the present (exact intervals, but the wait adds to every frame's latency); default: after it
    bool  onesided_fill{false};  // VKIFRAME_ONESIDED_FILL=1 (experimental; needs VKIFRAME_EXPERIMENTAL=1 too; read at game start): at occluded pixels take the colour from the one frame that shows the background instead of blending both. Offline it was worse than the default in 21 of 25 triplets on hard pixels
    bool  extrapolate{false};    // VKIFRAME_EXTRAPOLATE=1: no frame is held back; the newest frame is shown at once and the slots until the next one show frames predicted from it (implies flow and warp)
    bool  interp_bidir{true};      // VKIFRAME_INTERP_BIDIR=1 (old name VKIFRAME_WARP_BIDIR): two interpolations (A forward, B backward), each trusted by its own flow; needs the full-resolution backward flow, so it turns the backward skip off
    float cand_thr{0.0f};        // VKIFRAME_INTERP_CAND (off by default: on real game footage it made ghosting worse): at occluded / badly matched pixels, pick the neighbouring vector that makes A and B agree (value = largest accepted luma difference; 0 = off)
    int   flow_bwd_skip{1};      // VKIFRAME_FLOW_BWD_SKIP: 0 = always refine the backward flow at the finest level; 1 (default) = skip it with VKIFRAME_FLOW_LEVEL=1 (the occlusion mask reads the next coarser backward field, about 1 ms cheaper); 2 = skip it at any level (about 0.9 ms at level 2, mask from a 160x90 field)
    bool  flow_subpixel{true};   // VKIFRAME_FLOW_SUBPIXEL=0: whole-pixel vectors at the finished flow level (default: parabola fit, fractional vectors)
    char  dump_dir[256]{"/tmp/vkIFrames_dump"};   // VKIFRAME_DUMP_DIR: where VKIFRAME_DUMP writes (under Steam/Proton /tmp is private to the game's container: use a folder under the home directory)
    int   dump_frames{5};        // VKIFRAME_DUMP_FRAMES=M (5 to 60, default 5): how many consecutive native frames VKIFRAME_DUMP keeps in memory and writes after the last one (about 7.4 MB each at 1280x720); times.txt lists their times
    int   dump_pair{0};          // VKIFRAME_DUMP=N: write A, B, the flow, the mask and the next two synthetic frames of pair N to /tmp/vkIFrames_dump (debugging)
    bool  debug_flow{false};     // VKIFRAME_DEBUG_FLOW=1: show the motion field instead of the synthetic frames (implies VKIFRAME_FLOW)
    bool  debug_ring{false};     // VKIFRAME_DEBUG_RING=1: present the ring-buffer copy of each held frame (shows that capture works)
    bool  timing_test{false};    // VKIFRAME_TIMING_TEST=1: schedule every present 2 smoothed frames ahead (Stage 2 test)
};
// Startup-only keys (EXTRAPOLATE, UI_MASK_THRESHOLD, FLOW_LEVEL, INTERPOLATE) can also come from live.conf (the Decky plugin); the file wins over the environment.
const char* StartupFileValue(const char* name);   // live_config.cpp; nullptr when the file does not set it
// Settings come from VKIFRAME_<NAME>; the old VKIFRAME_<NAME> spelling still works when the new one is not set.
inline const char* EnvVar(const char* name) {
    if (const char* f = StartupFileValue(name)) return f;
    char buf[96];
    std::snprintf(buf, sizeof buf, "VKIFRAME_%s", name);
    if (const char* v = std::getenv(buf)) return v;
    std::snprintf(buf, sizeof buf, "FRAMEGEN_%s", name);
    return std::getenv(buf);
}
// Default target: 90 fps on the Steam Deck OLED (board name "Galileo", 90 Hz panel), 60 everywhere else. VKIFRAME_TARGET_FPS and live.conf override it.
inline float DefaultTargetFps(const char* dmi_file = "/sys/class/dmi/id/product_name") {
    char name[64] = {};
    if (FILE* f = std::fopen(dmi_file, "r")) { if (!std::fgets(name, sizeof name, f)) name[0] = 0; std::fclose(f); }
    return std::strncmp(name, "Galileo", 7) == 0 ? 90.0f : 60.0f;
}
// The target the layer works to: TARGET_FPS, but not above the display's refresh rate. A frame shown above a fixed refresh rate only fills the present
// queue (more latency, nothing more on screen). A variable-refresh display and an unknown refresh rate (refresh_ns == 0) are not limited.
inline double EffectiveTargetFps(double target_fps, uint64_t refresh_ns, bool vrr) {
    if (vrr || refresh_ns == 0) return target_fps;
    const double hz = 1e9 / double(refresh_ns);
    return target_fps > hz * 1.0005 ? hz : target_fps;      // 0.05 % slack: a 90.00 target on a 89.99 Hz report is not "above"
}
// The layer's GPU work in one window (flow once per pair, interpolation once per generated frame, in ms) against the time the window lasted. 1.0 = the layer alone
// would fill the whole GPU; since the work per game frame is compared with the game's frame time, this is the same ratio for any frame rate.
inline double LayerCostRatio(double flow_ms_total, double interp_ms_total, double window_s) {
    return window_s > 0.0 ? (flow_ms_total + interp_ms_total) / (window_s * 1000.0) : 0.0;
}
// A setting that was renamed: the new name wins, the old one still works.
inline const char* EnvVar2(const char* name, const char* old_name) {
    if (const char* v = EnvVar(name)) return v;
    return EnvVar(old_name);
}
// A number from the environment or a settings file: only digits, a sign, a point and an exponent (no hex float, no inf, no nan, no blanks inside), the whole
// string, and finite. Anything else is not a number (the setting keeps its default) instead of being turned into 0 or into a huge value by atof.
inline bool ParseFiniteNumber(const char* v, double& out) {
    if (!v || !*v) return false;
    const char* p = v; while (*p == ' ' || *p == '\t') ++p;
    const char* b = p;
    for (; *p && *p != ' ' && *p != '\t' && *p != '\r' && *p != '\n'; ++p) if (!((*p >= '0' && *p <= '9') || *p == '+' || *p == '-' || *p == '.' || *p == 'e' || *p == 'E')) return false;
    const char* e = p; while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') ++p;
    if (*p || e == b) return false;
    std::string t(b, e);
    char* end = nullptr; errno = 0;
    const double x = std::strtod(t.c_str(), &end);
    if (*end || errno == ERANGE || !(x - x == 0.0)) return false;           // trailing text, out of range, inf or nan
    out = x; return true;
}
// The text of a rejected setting for the log: at most 40 characters, control characters replaced (the value may come from live.conf: a start-of-game key).
inline std::string ShownValue(const char* v) {
    std::string t(v ? v : "", v ? std::min<size_t>(std::strlen(v), 40) : 0);
    for (char& c : t) if (static_cast<unsigned char>(c) < 0x20 || c == 0x7f) c = '?';
    return t;
}
inline void EnvNumberValue(const char* name, const char* v, double lo, double hi, float& dst) {
    double x;
    if (ParseFiniteNumber(v, x)) dst = float(std::clamp(x, lo, hi));
    else FG_LOG("VKIFRAME_%s=\"%s\" is not a usable number: ignored, the default stays", name, ShownValue(v).c_str());
}
inline void EnvNumber(const char* name, double lo, double hi, float& dst) { if (const char* v = EnvVar(name)) EnvNumberValue(name, v, lo, hi, dst); }
// Whole-number settings: the same rule as the other numbers (nan, inf, hex, text after the number are not numbers: ignored and logged), a fraction is cut off, the
// result stays inside lo..hi.
inline void EnvIntValue(const char* name, const char* v, int lo, int hi, int& dst) {
    double x;
    if (ParseFiniteNumber(v, x)) dst = int(std::clamp(std::trunc(x), double(lo), double(hi)));
    else FG_LOG("VKIFRAME_%s=\"%s\" is not a usable number: ignored, the default stays", name, ShownValue(v).c_str());
}
inline void EnvInt(const char* name, int lo, int hi, int& dst) { if (const char* v = EnvVar(name)) EnvIntValue(name, v, lo, hi, dst); }
// On/off settings: a number (non-zero = on) or true/on/yes, false/off/no, as in live.conf. False when the text is neither.
inline bool ParseFlag(const char* v, bool& out) {
    double x;
    if (ParseFiniteNumber(v, x)) { out = x != 0.0; return true; }
    if (!v) return false;
    const char* b = v; while (*b == ' ' || *b == '\t') ++b;                      // blanks around the word are ignored, like around a number
    const char* e = b + std::strlen(b); while (e > b && (e[-1] == ' ' || e[-1] == '\t' || e[-1] == '\r' || e[-1] == '\n')) --e;
    const std::string w(b, e);
    if (w == "true" || w == "on" || w == "yes") { out = true; return true; }
    if (w == "false" || w == "off" || w == "no") { out = false; return true; }
    return false;
}
inline void EnvFlagValue(const char* name, const char* v, bool& dst) {
    if (!ParseFlag(v, dst)) FG_LOG("VKIFRAME_%s=\"%s\" is not a usable value (a number, or true/false, on/off, yes/no): ignored, the default stays", name, ShownValue(v).c_str());
}
inline void EnvFlag(const char* name, bool& dst) { if (const char* v = EnvVar(name)) EnvFlagValue(name, v, dst); }
// VKIFRAME_COST_WARN: the ratio (layer GPU time / game frame time) above which the cost warning counts a window. The same number rule as the other settings;
// missing, not a number, or not above 0: 1.0.
inline double CostWarnFromEnv(const char* v) {
    double x;
    if (v && ParseFiniteNumber(v, x) && x > 0.0) return x;
    if (v) FG_LOG("VKIFRAME_COST_WARN=\"%s\" is not a usable number above 0: ignored, the default 1.0 stays", ShownValue(v).c_str());
    return 1.0;
}
inline const Config& GetConfig() {
    static const Config c = [] {
        Config c;
        c.target_fps = DefaultTargetFps();
        // VKIFRAME_LOG_FILE=<path>: append everything the layer prints (and the game's own stderr) to that file; for runs without the launch script
        if (const char* v = EnvVar("LOG_FILE"); v && *v) { FILE* r_ = std::freopen(v, "a", stderr); (void)r_; }
        else (void)LiveLogOpen();                     // default: one log, ~/.config/vkiframes/live.log, shared with the settings changes
        EnvNumber("TARGET_FPS", 1.0, 1000.0, c.target_fps);
        EnvNumber("SYNTH_MARGIN", 0.0, 0.45, c.synth_margin);
        EnvNumber("PHASE_OFFSET", 0.0, 0.999, c.phase_offset);
        EnvFlag("PHASE_LOCK", c.phase_lock);
        EnvNumber("MIN_GAIN", 1.01, 2.0, c.min_gain);
        EnvInt("MAX_SYNTHETIC", 1, 10, c.max_synthetic);
        EnvNumber("BASE_FPS_CAP", 0.0, 90.0, c.base_fps_cap);
        if (c.base_fps_cap < 1.0f) c.base_fps_cap = 0.0f;                      // below 1 fps is not a cap anybody means: off (a 1/x second sleep per frame would freeze the game)
        EnvFlag("HOLD", c.hold);
        EnvFlag("SYNTH", c.synth);
        EnvFlag("PACE_SLEEP", c.pace_sleep);
        EnvFlag("PRESENTER", c.presenter);
        EnvInt("CMD_BUFFERS", 4, 32, c.cmd_buffers);
        EnvInt("ACQUIRE_WAIT_US", 0, 20000, c.acquire_wait_us);
        EnvInt("PACE_LEAD_US", 0, 20000, c.pace_lead_us);
        EnvFlag("ABSOLUTE", c.absolute);
        EnvFlag("FLOW", c.flow);
        EnvFlag("DEBUG_FLOW", c.debug_flow);
        EnvInt("TICKMARK", 0, 3, c.tickmark);
        EnvInt("FLOW_LEVEL", 1, 3, c.flow_level);
        if (const char* v = EnvVar2("INTERP_GUIDE", "WARP_GUIDE")) EnvNumberValue("INTERP_GUIDE", v, 0.0, 1.0, c.interp_guide);
        EnvNumber("CONF_LO", 0.0, 10.0, c.conf_lo);
        EnvNumber("CONF_HI", 0.0, 10.0, c.conf_hi);
        EnvNumber("PAN_LO", 0.0, 1.0, c.pan_lo);
        EnvNumber("PAN_HI", 0.0, 1.0, c.pan_hi);
        EnvNumber("UI_MASK_THRESHOLD", 0.0, 0.5, c.ui_mask);
        EnvFlag("CAP_EARLY", c.cap_early);
        EnvFlag("EXTRAPOLATE", c.extrapolate);
        if (const char* v = EnvVar2("INTERP_BIDIR", "WARP_BIDIR")) EnvFlagValue("INTERP_BIDIR", v, c.interp_bidir);
        if (const char* v = EnvVar2("INTERP_CAND", "WARP_CAND")) EnvNumberValue("INTERP_CAND", v, 0.0, 1.0, c.cand_thr);
        EnvInt("FLOW_BWD_SKIP", 0, 2, c.flow_bwd_skip);
        EnvFlag("FLOW_SUBPIXEL", c.flow_subpixel);
        if (const char* v = EnvVar("DUMP_DIR"))         { std::snprintf(c.dump_dir, sizeof c.dump_dir, "%s", v); }
        EnvInt("DUMP", 0, 1000000000, c.dump_pair);
        EnvInt("DUMP_FRAMES", 5, 60, c.dump_frames);
        if (const char* v = EnvVar2("INTERPOLATE", "WARP")) EnvFlagValue("INTERPOLATE", v, c.interpolate);
        EnvFlag("ONESIDED_FILL", c.onesided_fill);
        if (c.onesided_fill && LiveMaxSyntheticLimit() <= 4) { c.onesided_fill = false; FG_LOG("VKIFRAME_ONESIDED_FILL is experimental: it needs VKIFRAME_EXPERIMENTAL=1 as well; ignored"); }
        if (c.extrapolate) { c.interpolate = true; c.debug_flow = false; }
        if (c.debug_flow || c.interpolate) c.flow = true;
        EnvFlag("DEBUG_RING", c.debug_ring);
        EnvFlag("TIMING_TEST", c.timing_test);
        return c;
    }();
    return c;
}

// ---- Per-swapchain state ----
struct GpuState;
// A pointer that is written by one thread and read by others (#33): the same syntax as a plain pointer, loads and stores are atomic (acquire/release, so
// what the pointer leads to is visible to the thread that reads it).
template <class T> struct AtomicPtr {
    std::atomic<T*> p{nullptr};
    AtomicPtr& operator=(T* v) { p.store(v, std::memory_order_release); return *this; }
    operator T*() const { return p.load(std::memory_order_acquire); }
    T* operator->() const { return p.load(std::memory_order_acquire); }
    T& operator*() const { return *p.load(std::memory_order_acquire); }
};

struct SwapchainState : std::enable_shared_from_this<SwapchainState> {
    VkSwapchainKHR handle{};
    std::mutex flow_mu;                       // the hold/pair/flush logic of one swapchain (present hook, fence and present-wait hooks)
    std::atomic<uint64_t> last_present_ns{0}, acquire_wait_since_ns{0}, wait_present_since_ns{0}, wait_present_id{0};   // watchdog
    std::mutex mu;                            // guards rates and the timing state below (game thread and presenter thread both touch them)
    std::atomic<int> pending_error{0};        // first error of a presenter-thread present, returned to the game by its next present
    std::atomic<struct Presenter*> presenter{nullptr};   // Stage 3c-ii: thread that presents the planned ticks at their times; written by the game thread (flow_mu), read without it by the acquire hooks
    // present timing (Stage 2)
    bool     timing_enabled{false};
    VkPresentStageFlagsEXT stage{0};          // the one present stage we query and target
    uint64_t time_domain_id{0};
    VkTimeDomainKHR time_domain{VK_TIME_DOMAIN_CLOCK_MONOTONIC_KHR};
    bool     domain_monotonic{true};          // the chosen domain is CLOCK_MONOTONIC: no conversion needed
    bool     absolute_ok{false};              // the surface accepts absolute target times; if not, timing is feedback only
    bool     calib_ok{false};                 // calib_ok, domain_offset_ns and last_calib_ns: every access after the swapchain is created is under mu (PresentWithTiming)
    int64_t  domain_offset_ns{0};             // domain time - CLOCK_MONOTONIC, refreshed about once a second
    uint64_t last_calib_ns{0};
    uint64_t next_present_id{1};              // must increase, never 0
    uint32_t image_count{0};
    uint64_t refresh_ns{0};                   // refresh duration, 0 = unknown (with a variable-refresh display: the shortest refresh)
    bool     refresh_vrr{false};              // the display is variable-refresh (refreshInterval == UINT64_MAX): TARGET_FPS is not limited then
    float    clamp_logged_fps{0.0f};          // the limited target last written to the log, so the note appears once per change
    unsigned cost_over_windows{0};            // consecutive seconds in which the layer's GPU work was over its budget
    bool     cost_warned{false};              // the cost warning was written (once per swapchain)
    // timing and rate control
    TimingEngine timing;
    std::atomic<uint64_t> job_gen{0};         // VKIFRAME_EXTRAPOLATE: number of the newest presenter job; ticks of an older job are cancelled
    std::atomic<uint64_t> ex_planned{0}, ex_cancelled{0}, ex_late_sum_ns{0}, ex_late_max_ns{0}, ex_late_n{0};   // VKIFRAME_EXTRAPOLATE window counters: predicted ticks planned / not presented, lateness of tick 0
    PairScheduler sched;                      // Stage 3: gate, output clock, per-pair plan
    struct Rates { uint64_t synthetic{0}, shown{0}, native{0}, output{0}, bypass{0}, pair{0}, hold{0}, carrier{0}, dropped{0}, last_ns{0}; } rates;   // window counters
    uint64_t cap_next_slot_ns{0}, cap_this_slot_ns{0}, cap_return_ns{0};   // VKIFRAME_BASE_FPS_CAP: next slot of the grid, when the game was handed back to its thread
    double   cap_render_ema_ns{0.0};          // how long the game takes between our return and its next present
    std::atomic<uint64_t> cpu_hook_ns{0}, cpu_presenter_ns{0};   // CPU time (not waiting) of the game thread inside our present, and of the presenter thread (window totals)
    std::atomic<uint64_t> wait_acquire_ns{0}, wait_present_ns{0}, native_cnt{0};   // game-thread waits (window totals, see LogRates)
    stats::FrameTimes stat_ft;                // VKIFRAME_STATS: present-to-present intervals of this second (only filled while StatsOn())
    uint64_t stat_prev_ns{0};
    // frame holding (Stage 3b)
    bool     hold_candidate{false};           // surface, format, usage and image count allow it
    int      synth_capacity{10};              // most synthetic frames per native frame this swapchain can hold (images granted); k_max never exceeds it
    bool     synth_failed{false};             // creating a synthetic frame failed: held frames continue without synthetic ones
    bool     skip_logged{false};              // "frame holding skipped for this present chain" was logged once
    std::atomic<VkQueue> last_queue{};        // the queue the game presents on (for flushing a held frame from vkWaitForPresentKHR); written by the game thread, read by the presenter thread's idle flush
    bool     hold_failed{false};              // a GPU step failed: back to passthrough for this swapchain
    VkExtent2D extent{};
    VkFormat   format{VK_FORMAT_UNDEFINED};
    AtomicPtr<GpuState> gpu;                  // created at the first present (needs the queue family); written by the game thread, read by the presenter thread's idle flush
    uint64_t frames{0};
    uint64_t last_log_ns{0};
    // statistics, windowed over the log interval except for the totals
    struct Stats {
        uint64_t results_total{0}, queue_full_total{0};
        uint64_t n_err{0};  double sum_err_ms{0}, max_abs_err_ms{0};
        uint64_t n_int{0};  double sum_int_ms{0}, sum_int2_ms{0};
        uint64_t last_actual_ns{0};
    } st;
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

// ---- Hooks (layer_main.cpp) ----
VKAPI_ATTR VkResult VKAPI_CALL Hook_CreateInstance(const VkInstanceCreateInfo*, const VkAllocationCallbacks*, VkInstance*);
VKAPI_ATTR void     VKAPI_CALL Hook_DestroyInstance(VkInstance, const VkAllocationCallbacks*);
VKAPI_ATTR VkResult VKAPI_CALL Hook_CreateDevice(VkPhysicalDevice, const VkDeviceCreateInfo*, const VkAllocationCallbacks*, VkDevice*);
VKAPI_ATTR void     VKAPI_CALL Hook_DestroyDevice(VkDevice, const VkAllocationCallbacks*);
VKAPI_ATTR VkResult VKAPI_CALL Hook_CreateSwapchainKHR(VkDevice, const VkSwapchainCreateInfoKHR*, const VkAllocationCallbacks*, VkSwapchainKHR*);
VKAPI_ATTR void     VKAPI_CALL Hook_DestroySwapchainKHR(VkDevice, VkSwapchainKHR, const VkAllocationCallbacks*);
VKAPI_ATTR VkResult VKAPI_CALL Hook_QueuePresentKHR(VkQueue, const VkPresentInfoKHR*);
VKAPI_ATTR VkResult VKAPI_CALL Hook_QueueSubmit(VkQueue, uint32_t, const VkSubmitInfo*, VkFence);
VKAPI_ATTR VkResult VKAPI_CALL Hook_QueueSubmit2(VkQueue, uint32_t, const VkSubmitInfo2*, VkFence);
VKAPI_ATTR VkResult VKAPI_CALL Hook_QueueSubmit2KHR(VkQueue, uint32_t, const VkSubmitInfo2*, VkFence);
VKAPI_ATTR VkResult VKAPI_CALL Hook_QueueBindSparse(VkQueue, uint32_t, const VkBindSparseInfo*, VkFence);
VKAPI_ATTR VkResult VKAPI_CALL Hook_QueueWaitIdle(VkQueue);
VKAPI_ATTR VkResult VKAPI_CALL Hook_DeviceWaitIdle(VkDevice);
VKAPI_ATTR VkResult VKAPI_CALL Hook_AcquireNextImageKHR(VkDevice, VkSwapchainKHR, uint64_t, VkSemaphore, VkFence, uint32_t*);
VKAPI_ATTR VkResult VKAPI_CALL Hook_WaitForFences(VkDevice, uint32_t, const VkFence*, VkBool32, uint64_t);
VKAPI_ATTR VkResult VKAPI_CALL Hook_GetFenceStatus(VkDevice, VkFence);
VKAPI_ATTR VkResult VKAPI_CALL Hook_WaitForPresentKHR(VkDevice, VkSwapchainKHR, uint64_t, uint64_t);
VKAPI_ATTR VkResult VKAPI_CALL Hook_AcquireNextImage2KHR(VkDevice, const VkAcquireNextImageInfoKHR*, uint32_t*);
VKAPI_ATTR void     VKAPI_CALL Hook_GetDeviceQueue(VkDevice, uint32_t, uint32_t, VkQueue*);
VKAPI_ATTR void     VKAPI_CALL Hook_GetDeviceQueue2(VkDevice, const VkDeviceQueueInfo2*, VkQueue*);

// ---- Exported entry points ----
FRAMEGEN_EXPORT VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL FrameGen_GetInstanceProcAddr(VkInstance, const char*);
FRAMEGEN_EXPORT VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL FrameGen_GetDeviceProcAddr(VkDevice, const char*);
FRAMEGEN_EXPORT VKAPI_ATTR VkResult VKAPI_CALL FrameGen_NegotiateLoaderLayerInterfaceVersion(VkNegotiateLayerInterface*);
