#pragma once
// Settings: log macros, Config, VKIFRAME_* env and number parsing, machine defaults (GetConfig reads them once).

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#define FRAMEGEN_EXPORT extern "C" __attribute__((visibility("default")))
bool LiveLogOpen();   // live_config_io.cpp
int  LiveMaxSyntheticLimit();   // live_config.cpp: 10 when VKIFRAME_EXPERIMENTAL was set at game start, else 4
inline std::atomic<FILE*> g_fg_log{nullptr};              // the layer's log stream (live.log), null = stderr; swapped when live.log is rotated while the layer runs
inline FILE* FgLog() { FILE* f = g_fg_log.load(std::memory_order_acquire); return f ? f : stderr; }
#define FG_LOG(...) do { FILE* fg_f_ = FgLog(); std::fprintf(fg_f_, "[FrameGen] " __VA_ARGS__); std::fputc('\n', fg_f_); } while (0)

// ---- Configuration (environment variables, read once) ----
struct Config {
    float target_fps{120.0f};    // VKIFRAME_TARGET_FPS (the value for "any other machine"; GetConfig() replaces it by the machine's own, MachineDefaultsFor)
    bool  phase_lock{true};      // VKIFRAME_PHASE_LOCK: align one tick per pair with the native frame when the ratio is near an integer
    float synth_margin{0.15f};   // VKIFRAME_SYNTH_MARGIN (0..0.45): every tick of an interpolated pair is a synthetic frame, with t kept at least this far from 0 and 1, so no native frame is shown as is (0 = off)
    float phase_offset{0.5f};    // VKIFRAME_PHASE_OFFSET (0..1): with the phase lock, 0 puts a tick on the native frame, 0.5 puts the ticks between (t = 0.25 / 0.75 at 45 -> 90)
    float min_gain{1.01f};       // VKIFRAME_MIN_GAIN: interpolate only above this base-time / target-time ratio
    int   max_synthetic{3};      // VKIFRAME_MAX_SYNTHETIC: most synthetic frames between two natives
    float base_fps_cap{60.0f};   // VKIFRAME_BASE_FPS_CAP (the "any other machine" value, see target_fps) (0 = off): the layer limits the game's present rate itself
    bool  hold{true};            // VKIFRAME_HOLD=0: keep the Stage 3a dry run only, never hold a frame
    bool  synth{true};           // VKIFRAME_SYNTH=0: hold frames but never create synthetic ones (Stage 3b behaviour)
    bool  pace_sleep{false};     // VKIFRAME_PACE_SLEEP=1: sleep in the game thread until each tick (experimental, vblank-paced path)
    bool  presenter{true};       // VKIFRAME_PRESENTER=0: present the ticks from the game thread (Stage 3c-i behaviour, no pacing thread)
    int   cmd_buffers{8};        // VKIFRAME_CMD_BUFFERS (4..32): command buffer entries per swapchain; a GPU-bound game makes the game thread wait on the fence of the oldest one
    int   acquire_wait_us{1000}; // VKIFRAME_ACQUIRE_WAIT_US: the presenter waits this long per try for a free swapchain image without holding the capture mutex; 0 = old polling under the mutex
    int   pace_lead_us{2000};    // VKIFRAME_PACE_LEAD_US: the presenter calls present this long before a tick is due (vblank-paced path)
    bool  absolute{false};       // VKIFRAME_ABSOLUTE=1: give each tick an absolute target time when the surface supports it (untested path)
    bool  flow{false};           // VKIFRAME_FLOW=1: estimate the optical flow of every pair (Stage 4; the blend still ignores it until Stage 5)
    bool  interpolate{false};    // VKIFRAME_OPTICAL_FLOW (old names VKIFRAME_INTERPOLATE, VKIFRAME_WARP; default 0 = the plain cross-fade, 1 = optical flow): build the in-between frames by interpolating along the flow instead of the plain blend (Stage 5; implies VKIFRAME_FLOW; about 5 ms of GPU time per game frame in the scene we measured on a Steam Deck)
    int   tickmark{0};           // VKIFRAME_TICKMARK: 1 = 64x64 square top-left, red for the first half of a pair, green for the second; 2 = also a bar along the bottom that steps 6 px every displayed frame (120 steps/s at 60->120); 3 = the bar steps 12 px once per pair (60 steps/s, both frames of a pair identical). 2 vs 3 shows by eye whether 120 distinct frames arrive
    int   flow_level{2};         // VKIFRAME_FLOW_LEVEL: pyramid level of the finished flow field, 1 = half resolution (finest, most expensive), 2 = quarter (default), 3 = eighth
    float interp_guide{0.08f};     // VKIFRAME_INTERP_GUIDE: luma sigma of the colour-guided vector weighting (0 = plain bilinear)
    float conf_lo{0.06f};        // VKIFRAME_CONF_LO / VKIFRAME_CONF_HI: match cost where the interpolation starts / finishes fading to the cross-fade (HI <= LO = off)
    float conf_hi{0.14f};
    float pan_lo{0.15f};         // VKIFRAME_PAN_LO / VKIFRAME_PAN_HI: when this share of the flow field is occluded or badly matched the frame starts to cross-fade, fully at the high value (a fast pan; captures: calm 0.04-0.10, pan 0.84; pan_hi <= pan_lo = off)
    float pan_hi{0.35f};
    float ui_mask{0.08f};        // VKIFRAME_UI_MASK_THRESHOLD: static-UI mask; where the 3x3 mean of |A-B| (luma) stays below this the pixel cross-fades instead of warping (0 = off; 0.03 suits film-grain content)
    // VKIFRAME_CAP_MODE=late|early|hybrid (experiment; default late). late: sleep after the present so the game starts its next frame as late as it can (lowest latency, the
    // game's render-time jitter reaches the present interval); early: sleep before the present until the grid time (exact intervals, up to a slot of added latency);
    // hybrid: late sleep with a jitter-aware lead plus a short hold before the present until the grid time (exact intervals, latency = the margin).
    enum class CapMode { Late, Early, Hybrid };
    CapMode cap_mode{CapMode::Late};
    bool  stats_frames{false};   // VKIFRAME_STATS_FRAMES=1 (with stats on): every frame time of each second goes to stats-frames.jsonl as well
    bool  cap_early{false};      // VKIFRAME_CAP_EARLY=1: VKIFRAME_BASE_FPS_CAP waits before the present (exact intervals, but the wait adds to every frame's latency); default: after it
    bool  onesided_fill{false};  // VKIFRAME_ONESIDED_FILL=1 (experimental; needs VKIFRAME_EXPERIMENTAL=1 too; read at game start): at occluded pixels take the colour from the one frame that shows the background instead of blending both. Offline it was worse than the default in 21 of 25 triplets on hard pixels
    bool  extrapolate{false};    // VKIFRAME_EXTRAPOLATE=1: no frame is held back; the newest frame is shown at once and the slots until the next one show frames predicted from it (implies flow and warp)
    bool  interp_bidir{true};      // VKIFRAME_INTERP_BIDIR=1 (old name VKIFRAME_WARP_BIDIR): two interpolations (A forward, B backward), each trusted by its own flow; needs the full-resolution backward flow, so it turns the backward skip off
    float cand_thr{0.0f};        // VKIFRAME_INTERP_CAND (off by default: on real game footage it made ghosting worse): at occluded / badly matched pixels, pick the neighbouring vector that makes A and B agree (value = largest accepted luma difference; 0 = off)
    int   flow_bwd_skip{1};      // VKIFRAME_FLOW_BWD_SKIP: 0 = always refine the backward flow at the finest level; 1 (default) = skip it with VKIFRAME_FLOW_LEVEL=1 (the occlusion mask reads the next coarser backward field, about 1 ms cheaper); 2 = skip it at any level (about 0.9 ms at level 2, mask from a 160x90 field)
    bool  flow_subpixel{true};   // VKIFRAME_FLOW_SUBPIXEL=0: whole-pixel vectors at the finished flow level (default: parabola fit, fractional vectors)
    char  dump_dir[256]{"/tmp/vkIFrames_dump"};   // VKIFRAME_DUMP_DIR: where VKIFRAME_DUMP writes (under Steam/Proton /tmp is private to the game's container: use a folder under the home directory)
    int   dump_frames{5};        // VKIFRAME_DUMP_FRAMES=M (5 to 60, default 5): how many consecutive native frames VKIFRAME_DUMP keeps in memory and writes after the last one (about 7.4 MB each at 1280x720); times.txt lists their times
    int   dump_pair{0};          // VKIFRAME_DUMP=N: write A, B, the flow, the mask and the next two synthetic frames of pair N to /tmp/vkIFrames_dump (debugging). Needs the optical flow on (VKIFRAME_OPTICAL_FLOW=1): pairs are counted by the flow, so with it off nothing is captured or written (gpu_frames.cpp DumpInit)
    bool  debug_flow{false};     // VKIFRAME_DEBUG_FLOW=1: show the motion field instead of the synthetic frames (implies VKIFRAME_FLOW)
    bool  debug_ring{false};     // VKIFRAME_DEBUG_RING=1: present the ring-buffer copy of each held frame (shows that capture works)
    bool  timing_test{false};    // VKIFRAME_TIMING_TEST=1: schedule every present 2 smoothed frames ahead (Stage 2 test)
};
// Startup-only keys (EXTRAPOLATE, UI_MASK_THRESHOLD, FLOW_LEVEL, OPTICAL_FLOW) can also come from live.conf (the Decky plugin); the file wins over the environment.
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
// Defaults by machine (owner table, 2026-11-02), read from the DMI board name: Steam Deck OLED ("Galileo", 90 Hz panel) cap 45 / target 90, Steam Deck LCD ("Jupiter",
// 60 Hz) cap 30 / target 60, any other machine cap 60 / target 120. VKIFRAME_BASE_FPS_CAP, VKIFRAME_TARGET_FPS and live.conf override them. The Decky plugin
// (decky/main.py) and decky/settings_schema.json ("machine_defaults") carry the same table; tests/test_defaults.cpp compares the three.
struct MachineDefaults { float base_fps_cap; float target_fps; };
// The file read is /sys/class/dmi/id/product_name; VKIFRAME_DMI_FILE=<path> names another one (a test hook: the tests point it at a file that is not a Deck, so
// their results do not depend on the machine they run on). It changes only these defaults.
inline const char* DmiProductFile() {
    const char* v = std::getenv("VKIFRAME_DMI_FILE");
    return v && *v ? v : "/sys/class/dmi/id/product_name";
}
inline MachineDefaults MachineDefaultsFor(const char* dmi_file = DmiProductFile()) {
    char name[64] = {};
    // open without blocking and only a regular file is read (a FIFO named by VKIFRAME_DMI_FILE would block fopen forever on the game's first Vulkan call)
    const int fd = ::open(dmi_file, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
    if (fd >= 0) {
        struct stat st {};
        if (::fstat(fd, &st) == 0 && S_ISREG(st.st_mode)) { const ssize_t n = ::read(fd, name, sizeof name - 1); if (n < 0) name[0] = 0; }
        ::close(fd);
    }
    if (std::strncmp(name, "Galileo", 7) == 0) return {45.0f, 90.0f};
    if (std::strncmp(name, "Jupiter", 7) == 0) return {30.0f, 60.0f};
    return {60.0f, 120.0f};
}
inline float DefaultTargetFps(const char* dmi_file = DmiProductFile()) { return MachineDefaultsFor(dmi_file).target_fps; }
inline float DefaultBaseFpsCap(const char* dmi_file = DmiProductFile()) { return MachineDefaultsFor(dmi_file).base_fps_cap; }
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
// Renamed twice (WARP -> INTERPOLATE -> OPTICAL_FLOW): the newest name wins, then the older ones.
inline const char* EnvVar3(const char* name, const char* old_name, const char* oldest_name) {
    if (const char* v = EnvVar(name)) return v;
    if (const char* v = EnvVar(old_name)) return v;
    return EnvVar(oldest_name);
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
// VKIFRAME_DUMP counts pairs by their optical flow, so with the flow off (the default) no pool is made and nothing is captured: say so (one log line, see DumpInit).
inline const char* DumpNeedsFlowNote(int dump_pair, bool flow_on) {
    return dump_pair > 0 && !flow_on ? "VKIFRAME_DUMP needs VKIFRAME_OPTICAL_FLOW=1; nothing will be captured" : nullptr;
}
