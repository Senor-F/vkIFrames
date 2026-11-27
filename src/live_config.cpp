// Live settings: the key table, live.conf parsing and polling, the startup values and the synth capacity
#include "live_config.hpp"
#include "live_config_internal.hpp"

#include "vk_iframes_layer.hpp"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstdarg>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <map>
#include <mutex>
#include <string>
#include <utility>

using namespace liveconf;

namespace {

enum Key { kEnabled, kTargetFps, kMaxSynthetic, kBaseFpsCap, kMinGain, kPhaseLock, kPhaseOffset, kInterpBidir, kCmdBuffers, kSynthMargin, kStats, kKeys };

struct KeyInfo { const char* name; bool toggle; double lo, hi; };   // ranges as in settings_schema.json on the decky-plugin branch
constexpr KeyInfo kInfo[kKeys] = {
    {"ENABLED", true, 0, 1}, {"TARGET_FPS", false, 30, 600}, {"MAX_SYNTHETIC", false, 1, kLiveMaxSyntheticNormal}, {"BASE_FPS_CAP", false, 0, 240},
    {"MIN_GAIN", false, 1.01, 1.5}, {"PHASE_LOCK", true, 0, 1}, {"PHASE_OFFSET", false, 0.0, 0.999}, {"INTERP_BIDIR", true, 0, 1}, {"CMD_BUFFERS", false, 4, 32}, {"SYNTH_MARGIN", false, 0.0, 0.45},
    {"STATS", true, 0, 1},
};

double Get(const LiveSettings& s, int k) {
    switch (k) {
        case kEnabled: return s.enabled; case kTargetFps: return s.target_fps; case kMaxSynthetic: return s.max_synthetic; case kBaseFpsCap: return s.base_fps_cap;
        case kMinGain: return s.min_gain; case kPhaseLock: return s.phase_lock; case kPhaseOffset: return s.phase_offset; case kCmdBuffers: return s.cmd_buffers; case kSynthMargin: return s.synth_margin; case kStats: return s.stats; default: return s.interp_bidir;
    }
}
void Set(LiveSettings& s, int k, double v) {
    switch (k) {
        case kEnabled: s.enabled = int(v); break; case kTargetFps: s.target_fps = float(v); break; case kMaxSynthetic: s.max_synthetic = int(v); break;
        case kBaseFpsCap: s.base_fps_cap = float(v); break; case kMinGain: s.min_gain = float(v); break; case kPhaseLock: s.phase_lock = int(v); break;
        case kPhaseOffset: s.phase_offset = float(v); break; case kCmdBuffers: s.cmd_buffers = int(v); break; case kSynthMargin: s.synth_margin = float(v); break; case kStats: s.stats = int(v); break; default: s.interp_bidir = int(v); break;
    }
}
std::string Show(int k, double v) { char b[40]; if (kInfo[k].toggle || k == kMaxSynthetic || k == kCmdBuffers) std::snprintf(b, sizeof b, "%d", int(v)); else std::snprintf(b, sizeof b, "%g", v); return b; }

std::atomic<int> g_synth_cap{0};   // what the swapchain can hold (0 = unknown): the live MAX_SYNTHETIC never goes above it
std::atomic<int> g_cap_note_value{0}, g_cap_note_cap{0};     // the last "above the capacity" note that was logged: logged once per (value, capacity)
double g_startup[kKeys];            // values from the environment (what removing the file goes back to)
bool   g_have_file = false;
long long g_mtime_ns = 0; long long g_size = -1; unsigned long long g_rev = 0;
unsigned long long g_gen = 0;      // counts the loads that changed a setting (the benchmark stops when it moves)
std::atomic<long long> g_bench_seen{0};   // the bench.req token already handled (or present when the game started); read on the stats path, written there and by the poll
std::atomic<long long> g_bench_cur{0};    // the token bench.req holds now (0: none, or a cancel)
std::atomic<bool> g_bench_running{false};   // set by stats.cpp while a benchmark run is going (LiveBenchRunning)
std::atomic<bool> g_bench_cancel{false};     // bench.req changed to 0 while a non-zero token was there: stop a running benchmark (consumed by LiveBenchCancel)
long long g_req_mtime_ns = 0; long long g_req_size = -1; unsigned long long g_req_ino = 0; bool g_req_have = false;
uint64_t g_next_poll = 0;
std::mutex g_poll_mu;

// Parses VALUE for key k; true when usable, with the clamped number in out and a note when it was clamped.
bool ParseValue(int k, const std::string& raw, double& out, std::string& note) {
    double v = 0.0;
    if (raw.find('\0') != std::string::npos || !ParseFiniteNumber(raw.c_str(), v)) {      // digits, sign, point, exponent only: no hex float, inf, nan, no text after the number
        if (kInfo[k].toggle && (raw == "true" || raw == "on" || raw == "yes")) { out = 1; return true; }
        if (kInfo[k].toggle && (raw == "false" || raw == "off" || raw == "no")) { out = 0; return true; }
        return false;
    }
    if (kInfo[k].toggle) { out = v != 0.0 ? 1 : 0; if (v != 0.0 && v != 1.0) note = "any non-zero value counts as 1"; return true; }
    if (k == kMaxSynthetic || k == kCmdBuffers) v = std::round(v);
    double hi = (k == kMaxSynthetic) ? double(LiveMaxSyntheticLimit()) : kInfo[k].hi;   // experimental mode (set at game start) allows up to 10
    bool over_capacity = false;
    if (k == kMaxSynthetic && g_synth_cap.load() > 0 && v > g_synth_cap.load() && v <= hi) {   // more than this game's swapchain was built for
        const int cap = g_synth_cap.load();
        over_capacity = true;
        if (g_cap_note_value.load() != int(v) || g_cap_note_cap.load() != cap) {
            g_cap_note_value = int(v); g_cap_note_cap = cap;
            char b[160]; std::snprintf(b, sizeof b, "is above the %d this game's swapchain was built for: using %d until the game is restarted", cap, cap); note = b;
        }
        hi = double(cap);
    } else if (k == kMaxSynthetic && g_synth_cap.load() > 0) hi = std::min(hi, double(g_synth_cap.load()));
    if (k == kBaseFpsCap && v > 0.0 && v < 1.0) { out = 0.0; note = "below 1 fps is not a cap: off"; return true; }
    const double c = std::clamp(v, kInfo[k].lo, hi);
    if (c != v && !over_capacity) { char b[120]; std::snprintf(b, sizeof b, "clamped to the range %g..%g", kInfo[k].lo, hi); note = b; }
    out = (k == kMaxSynthetic || k == kCmdBuffers) ? c : double(float(c));      // the settings are floats: compare as floats, or 1.12 reads as a change from 1.12f
    return true;
}

// Old spellings of renamed keys (warp -> interpolate -> optical flow) still work in live.conf; the key is then handled under its new name.
void NormalizeKey(std::string& key) {
    static const char* const kOld[][2] = {{"WARP_BIDIR", "INTERP_BIDIR"}, {"WARP_GUIDE", "INTERP_GUIDE"}, {"WARP_CAND", "INTERP_CAND"}, {"WARP", "OPTICAL_FLOW"}, {"INTERPOLATE", "OPTICAL_FLOW"}};
    for (const auto& o : kOld) if (key == o[0]) { key = o[1]; return; }
}

void Trim(std::string& s) { while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r' || s.back() == '\n')) s.pop_back(); size_t i = 0; while (i < s.size() && (s[i] == ' ' || s[i] == '\t')) ++i; s.erase(0, i); }

double g_prev_want[kKeys];          // what the previous Apply decided for each key (the file's value, or the startup value)
bool   g_prev_init = false;

// What one load of live.conf found: the wanted values and the line counters. Note() logs at most kMaxNotes lines per load: a hostile or broken file must
// not turn one poll into thousands of log lines.
struct ApplyCtx {
    double want[kKeys];
    bool from_file[kKeys] = {};
    unsigned lines = 0, comments = 0, bad = 0, unknown = 0, startup = 0, notes = 0;
    static constexpr unsigned kMaxNotes = 10;
    template <class... A> void Note(A&&... args) { if (++notes <= kMaxNotes) LogLine(std::forward<A>(args)...); }
};

// REV is only logged: a whole number, anything else is ignored (and said so) instead of read as a prefix or as 0.
void ParseRev(const std::string& val, ApplyCtx& c) {
    errno = 0; char* end = nullptr;
    const bool digits = !val.empty() && val.size() <= 20 && val.find_first_not_of("0123456789") == std::string::npos;
    const unsigned long long r = digits ? std::strtoull(val.c_str(), &end, 10) : 0;
    if (digits && errno == 0 && end && !*end) g_rev = r;
    else c.Note("REV=\"%s\" is not a whole number: ignored (it is only logged)", ShownValue(val.c_str()).c_str());
}

// Splits one line into key and value. False when the line is blank, a comment or unusable (counted and noted).
bool SplitLine(std::string& line, int lineno, std::string& key, std::string& val, ApplyCtx& c) {
    Trim(line);
    if (line.empty() || line[0] == '#') { ++c.comments; return false; }
    ++c.lines;
    if (line.find('\0') != std::string::npos) { ++c.bad; c.Note("line %d ignored (contains a NUL byte)", lineno); return false; }
    const size_t eq = line.find('=');
    if (eq == std::string::npos) { ++c.bad; c.Note("line %d ignored (no '='): \"%.60s\"", lineno, line.c_str()); return false; }
    key = line.substr(0, eq); val = line.substr(eq + 1); Trim(key); Trim(val);
    return true;
}

// The key's index; -1 for an unknown key, -2 for a setting that is only read when the game starts (logged then, not an error).
int ClassifyKey(const std::string& key) {
    for (int i = 0; i < kKeys; ++i) if (key == kInfo[i].name) return i;
    for (const char* sk : {"OPTICAL_FLOW", "EXTRAPOLATE", "UI_MASK_THRESHOLD", "FLOW_LEVEL", "EXPERIMENTAL", "ONESIDED_FILL"}) if (key == sk) return -2;
    return -1;
}

void ParseLine(std::string line, int lineno, const LiveSettings& live, ApplyCtx& c) {
    std::string key, val;
    if (!SplitLine(line, lineno, key, val, c)) return;
    if (key == "REV") { ParseRev(val, c); return; }
    for (const char* pfx : {"VKIFRAME_", "FRAMEGEN_"}) if (key.rfind(pfx, 0) == 0) { key.erase(0, std::strlen(pfx)); break; }
    NormalizeKey(key);
    if (key == "BENCH") {                       // an older plugin or script: a benchmark request is now the file bench.req, never a setting
        static bool noted = false;
        if (!noted) { noted = true; c.Note("line %d: BENCH in live.conf is not used any more (a benchmark request is the file bench.req); ignored", lineno); }
        return;
    }
    const int k = ClassifyKey(key);
    if (k == -2) { ++c.startup; return; }
    if (k < 0) { ++c.unknown; c.Note("line %d: unknown key \"%.40s\" ignored", lineno, key.c_str()); return; }
    double v; std::string note;
    if (!ParseValue(k, val, v, note)) { ++c.bad; c.Note("line %d: %s=\"%.40s\" is not a valid value, keeping %s", lineno, kInfo[k].name, val.c_str(), Show(k, Get(live, k)).c_str()); return; }
    if (!note.empty()) c.Note("line %d: %s=%s %s -> %s", lineno, kInfo[k].name, val.c_str(), note.c_str(), Show(k, v).c_str());
    c.want[k] = v; c.from_file[k] = true;
}

// Writes the wanted values into the live settings, logging each change; returns how many changed.
int ApplyChanges(LiveSettings& live, const ApplyCtx& c) {
    const double* want = c.want;
    int changed = 0;
    // A running benchmark switches ENABLED itself, so the live value differs from the file's in its "off" seconds. A write that changes nothing (the plugin
    // rewriting live.conf at a loader restart or a panel change) must not count as a change of ENABLED: it is compared with the file's previous value instead.
    // When the user does change something, ENABLED follows the file as before and the run stops.
    bool others_changed = false;
    for (int k = 0; k < kKeys; ++k) if (k != kEnabled && Get(live, k) != want[k]) others_changed = true;
    const bool keep_bench_enabled = g_bench_running.load() && !others_changed && want[kEnabled] == g_prev_want[kEnabled];
    for (int k = 0; k < kKeys; ++k) {
        const double old = Get(live, k);
        if (old == want[k]) continue;
        if (k == kEnabled && keep_bench_enabled) continue;
        ++changed;
        LogLine("CHANGE %s: %s -> %s (%s)", kInfo[k].name, Show(k, old).c_str(), Show(k, want[k]).c_str(), c.from_file[k] ? "live.conf" : "back to the startup value");
        Set(live, k, want[k]);
    }
    for (int k = 0; k < kKeys; ++k) g_prev_want[k] = want[k];
    return changed;
}

void Apply(const char* why, const std::string& text, bool present) {
    LiveSettings& live = Live();
    ApplyCtx c;
    for (int k = 0; k < kKeys; ++k) c.want[k] = g_startup[k];
    if (!g_prev_init) { for (int k = 0; k < kKeys; ++k) g_prev_want[k] = g_startup[k]; g_prev_init = true; }
    if (present) {
        size_t pos = 0; int lineno = 0;
        while (pos <= text.size()) {
            size_t nl = text.find('\n', pos); if (nl == std::string::npos) nl = text.size();
            ParseLine(text.substr(pos, nl - pos), ++lineno, live, c);
            pos = nl + 1;
        }
    }
    if (c.notes > ApplyCtx::kMaxNotes) LogLine("... and %u more notes about this file not shown", c.notes - ApplyCtx::kMaxNotes);
    const int changed = ApplyChanges(live, c);
    if (changed) ++g_gen;
    LogLine("%s: REV %llu, %u setting line(s), %u comment/blank, %u invalid, %u unknown, %u start-of-game only (not applied now); %d value(s) changed", why, g_rev, c.lines, c.comments, c.bad, c.unknown, c.startup, changed);
    if (changed) {
        char sum[300]; int n = 0;
        for (int k = 0; k < kKeys; ++k) n += std::snprintf(sum + n, sizeof sum - size_t(n), "%s%s=%s", k ? " " : "", kInfo[k].name, Show(k, Get(live, k)).c_str());
        LogLine("now: %s", sum);
    }
}

// bench.req: one line with a number, the benchmark request token (see stats.hpp). It is not a setting: writing it changes no live value, so it can never
// stop a running benchmark by itself. A new non-zero number asks for one run; 0 stops a running one. A file that is there at the start of the game is not a request.
// Polled with live.conf, once a second: one stat(), the file is read when its mtime, size or inode changed. Same rules as live.conf: no symlink, own file.
void PollBenchReq(bool first) {
    const std::string conf = ConfPath();                        // next to live.conf (a test or a setup that moves live.conf moves the request file with it)
    if (conf.empty()) return;
    const size_t slash = conf.rfind('/');
    const std::string path = (slash == std::string::npos ? std::string(".") : conf.substr(0, slash)) + "/bench.req";
    struct stat st;
    if (::stat(path.c_str(), &st) != 0) {
        if (g_req_have) { g_req_have = false; g_req_size = -1; g_bench_cur = 0; }          // removed: no request, nothing to cancel
        return;
    }
    const long long mt = (long long)st.st_mtim.tv_sec * 1'000'000'000ll + st.st_mtim.tv_nsec;
    if (g_req_have && mt == g_req_mtime_ns && (long long)st.st_size == g_req_size && (unsigned long long)st.st_ino == g_req_ino) return;
    g_req_have = true; g_req_mtime_ns = mt; g_req_size = (long long)st.st_size; g_req_ino = (unsigned long long)st.st_ino;
    std::string text;
    if (!ReadOwnFile(path, text)) { LogLine("bench.req could not be used (errno %d), ignored", errno); return; }
    if (text.find('\0') != std::string::npos) { LogLine("bench.req contains a NUL byte, ignored"); return; }
    Trim(text);
    const size_t nl = text.find('\n'); if (nl != std::string::npos) text.erase(nl);
    Trim(text);
    char* end = nullptr; errno = 0;
    const long long v = std::strtoll(text.c_str(), &end, 10);
    if (text.empty() || end == text.c_str() || *end || errno == ERANGE || v < 0 || v > 4000000000000ll) {
        LogLine("bench.req: \"%.40s\" is not a request number (0 to 4e12), ignored", text.c_str());
        return;
    }
    const long long old = g_bench_cur.load();
    g_bench_cur = v;
    if (first) { g_bench_seen = v; return; }                      // there before the game started: not a request
    if (v == 0 && old != 0) { g_bench_cancel = true; LogLine("bench.req: 0, a running benchmark is stopped"); }
    else if (v != 0 && v != g_bench_seen.load()) LogLine("bench.req: request %lld", v);
}

}  // namespace

// Settings that cannot change while a game runs (they size buffers or pick shaders) are read from live.conf once, when the game starts.
const char* StartupFileValue(const char* name) {
    static const char* const kStartupKeys[] = {"EXTRAPOLATE", "UI_MASK_THRESHOLD", "FLOW_LEVEL", "OPTICAL_FLOW", "EXPERIMENTAL", "ONESIDED_FILL"};
    bool allowed = false;
    for (const char* k : kStartupKeys) if (std::strcmp(name, k) == 0) allowed = true;
    if (!allowed) return nullptr;                       // also keeps LIVE_CONF / LIVE_LOG lookups out of the loader below
    static const std::map<std::string, std::string> values = [] {
        std::map<std::string, std::string> m;
        std::map<std::string, int> rank;
        const char* e = std::getenv("VKIFRAME_LIVE_CONF");
        if (!e || !*e) e = std::getenv("FRAMEGEN_LIVE_CONF");
        const std::string path = e && *e ? std::string(e) : (ConfDir().empty() ? std::string() : ConfDir() + "/live.conf");
        std::string text;
        if (!ReadOwnFile(path, text)) return m;
        size_t pos = 0;
        while (pos <= text.size()) {
            size_t nl = text.find('\n', pos); if (nl == std::string::npos) nl = text.size();
            std::string line = text.substr(pos, nl - pos); pos = nl + 1;
            Trim(line);
            const size_t eq = line.find('=');
            if (line.empty() || line[0] == '#' || eq == std::string::npos) continue;
            std::string key = line.substr(0, eq), val = line.substr(eq + 1); Trim(key); Trim(val);
            for (const char* pfx : {"VKIFRAME_", "FRAMEGEN_"}) if (key.rfind(pfx, 0) == 0) { key.erase(0, std::strlen(pfx)); break; }
            const std::string spelled = key;
            NormalizeKey(key);
            const int age = spelled == "WARP" ? 0 : spelled == "INTERPOLATE" ? 1 : 2;       // the newest spelling of a renamed key wins when a file has more than one
            for (const char* k : kStartupKeys)
                if (key == k && !val.empty() && (!rank.count(key) || age >= rank[key])) { m[key] = val; rank[key] = age; }
        }
        for (auto& kv : m) LogLine("startup setting %s=%s taken from live.conf (read once when the game starts: a change needs a game restart)", kv.first.c_str(), kv.second.c_str());
        return m;
    }();
    auto it = values.find(name);
    return it == values.end() ? nullptr : it->second.c_str();
}

int LiveRequestedMaxSynthetic() {
    int request = 0;
    std::string text;
    if (!ReadOwnFile(ConfPath(), text)) return 0;
    size_t pos = 0;
    while (pos <= text.size()) {
        size_t nl = text.find('\n', pos); if (nl == std::string::npos) nl = text.size();
        std::string line = text.substr(pos, nl - pos); pos = nl + 1;
        Trim(line);
        const size_t eq = line.find('=');
        if (line.empty() || line[0] == '#' || eq == std::string::npos) continue;
        std::string key = line.substr(0, eq), val = line.substr(eq + 1); Trim(key); Trim(val);
        for (const char* pfx : {"VKIFRAME_", "FRAMEGEN_"}) if (key.rfind(pfx, 0) == 0) { key.erase(0, std::strlen(pfx)); break; }
        if (key != "MAX_SYNTHETIC") continue;
        double x;                                                  // the same rule as the live parser: a line it ignores (5x, 0x4, nan) does not set the request either; the last valid line counts
        if (val.find('\0') != std::string::npos || !ParseFiniteNumber(val.c_str(), x)) continue;
        request = int(std::clamp(std::round(x), 1.0, double(kLiveMaxSynthetic)));
    }
    return request;
}

int LiveSynthCapacityFor(int env_request) {
    const int file = LiveRequestedMaxSynthetic();
    const int request = file > 0 ? file : env_request;
    return std::min(LiveMaxSyntheticLimit(), std::max(request, kLiveMaxSyntheticNormal));
}

void LiveSetSynthCapacity(int c_eff) {
    g_synth_cap = c_eff;
    if (c_eff <= 0) return;
    LiveSettings& live = Live();
    if (live.max_synthetic.load() > c_eff) {
        LogLine("MAX_SYNTHETIC %d is above the %d this game's swapchain was built for: using %d until the game is restarted", live.max_synthetic.load(), c_eff, c_eff);
        live.max_synthetic = c_eff;
    }
}
int LiveSynthCapacity() { return g_synth_cap.load(); }

int LiveMaxSyntheticLimit() {
    static const int limit = [] {
        const char* v = StartupFileValue("EXPERIMENTAL");     // live.conf wins over the environment, like the other start-of-game keys
        if (!v) v = EnvVar("EXPERIMENTAL");
        bool on = false;
        if (v && !ParseFlag(v, on)) { LogLine("EXPERIMENTAL=\"%s\" is not a usable value: ignored, experimental mode stays off", ShownValue(v).c_str()); on = false; }
        return on ? kLiveMaxSynthetic : kLiveMaxSyntheticNormal;
    }();
    return limit;
}

LiveSettings& Live() {
    static LiveSettings* s = [] {
        auto* l = new LiveSettings;
        const Config& c = GetConfig();
        l->enabled = 1; l->target_fps = c.target_fps; l->max_synthetic = std::min(c.max_synthetic, LiveMaxSyntheticLimit()); l->base_fps_cap = c.base_fps_cap; l->cmd_buffers = c.cmd_buffers; l->synth_margin = c.synth_margin;
        l->min_gain = c.min_gain; l->phase_lock = c.phase_lock ? 1 : 0; l->phase_offset = c.phase_offset; l->interp_bidir = c.interp_bidir ? 1 : 0;
        if (const char* v = EnvVar("STATS")) { bool on = false; if (ParseFlag(v, on)) l->stats = on ? 1 : 0; }
        for (int k = 0; k < kKeys; ++k) g_startup[k] = Get(*l, k);
        return l;
    }();
    return *s;
}

void LivePoll(uint64_t now_ns) {
    if (now_ns < g_next_poll) return;                       // racy read is fine: at worst two threads poll once
    std::unique_lock<std::mutex> lk(g_poll_mu, std::try_to_lock);
    if (!lk.owns_lock()) return;
    const bool first = g_next_poll == 0;
    g_next_poll = now_ns + 1'000'000'000ull;
    (void)Live();
    PollBenchReq(first);
    if (first) {
        char sum[300]; int n = 0;
        for (int k = 0; k < kKeys; ++k) n += std::snprintf(sum + n, sizeof sum - size_t(n), "%s%s=%s", k ? " " : "", kInfo[k].name, Show(k, g_startup[k]).c_str());
        LogLine("session start; startup values from the environment: %s; watching %s", sum, "live.conf");
    }
    struct stat st;
    if (::stat(ConfPath().c_str(), &st) != 0) {
        if (g_have_file) { g_have_file = false; g_size = -1; g_mtime_ns = 0; Apply("live.conf removed", "", false); }
        return;
    }
    const long long mt = (long long)st.st_mtim.tv_sec * 1'000'000'000ll + st.st_mtim.tv_nsec;
    if (g_have_file && mt == g_mtime_ns && (long long)st.st_size == g_size) return;
    g_have_file = true; g_mtime_ns = mt; g_size = (long long)st.st_size;
    std::string text; bool cut = false;
    if (!ReadOwnFile(ConfPath(), text, &cut)) { LogLine("live.conf could not be opened (errno %d), keeping the current values", errno); return; }
    if (cut) LogLine("live.conf is bigger than %zu bytes: only the complete lines inside the first %zu bytes are used", kMaxSettingsBytes, kMaxSettingsBytes);
    Apply(first ? "live.conf found at startup" : "live.conf changed", text, true);
}

int LiveKeyTable(LiveKeyRow* rows, int max) {
    const Config d{};
    const double defs[kKeys] = {1, double(d.target_fps), double(d.max_synthetic), double(d.base_fps_cap), double(d.min_gain), d.phase_lock ? 1.0 : 0.0, double(d.phase_offset), d.interp_bidir ? 1.0 : 0.0, double(d.cmd_buffers), double(d.synth_margin), 0};
    int n = 0;
    for (int k = 0; k < kKeys && n < max; ++k) { rows[n++] = {kInfo[k].name, kInfo[k].toggle, kInfo[k].lo, kInfo[k].hi, defs[k]}; }
    return n;
}

unsigned long long LiveGeneration() { return g_gen; }
bool LiveBenchRequest(long long* token) {
    const long long t = g_bench_cur.load();
    if (t == 0 || t == g_bench_seen.load()) return false;
    *token = t; return true;
}
void LiveBenchAck(long long token) { g_bench_seen = token; }
void LiveBenchRunning(bool on) { g_bench_running = on; }
bool LiveBenchCancel() { return g_bench_cancel.exchange(false); }

// Atomic private file next to live.conf: written to a temp name (O_EXCL, 0600, no symlink) and renamed over NAME.
bool LiveWritePrivate(const char* name, const std::string& text) {
    static std::mutex mu;
    std::lock_guard<std::mutex> l(mu);
    const std::string dir = ConfDir();
    if (dir.empty() || !MakePrivateDir(dir)) return false;
    const std::string tmp = dir + "/" + name + ".tmp", fin = dir + "/" + name;
    (void)::unlink(tmp.c_str());
    const int fd = ::open(tmp.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
    if (fd < 0) return false;
    FILE* f = ::fdopen(fd, "w");
    if (!f) { ::close(fd); return false; }
    const bool ok = std::fwrite(text.data(), 1, text.size(), f) == text.size();
    std::fclose(f);
    if (!ok || std::rename(tmp.c_str(), fin.c_str()) != 0) { (void)::unlink(tmp.c_str()); return false; }
    return true;
}

// One line appended to a file of the config folder (0600, private folder, no symlink). One old file is kept when it grows past the cap (8 MiB).
namespace {
bool AppendCapped(const char* name, FILE*& f, unsigned& n, const std::string& line, bool allow_rotate) {
    const std::string dir = ConfDir(), path = dir + "/" + name;
    if (dir.empty() || !MakePrivateDir(dir)) return false;
    if (allow_rotate && f && (++n % 60) == 0) {                                   // about every minute: check the size
        struct stat st;
        if (::fstat(::fileno(f), &st) == 0 && st.st_size > g_cap_bytes) { std::fclose(f); f = nullptr; (void)std::rename(path.c_str(), (path + ".1").c_str()); }
    }
    if (!f) {
        struct stat st;                                           // a file left over from an earlier run is checked before the first append
        if (allow_rotate && ::lstat(path.c_str(), &st) == 0 && S_ISREG(st.st_mode) && st.st_size > g_cap_bytes) (void)std::rename(path.c_str(), (path + ".1").c_str());
        f = OpenPrivateAppend(path);
    }
    if (!f) return false;
    std::fputs(line.c_str(), f); std::fputc('\n', f); std::fflush(f);
    return true;
}
}  // namespace

// One line appended to stats.jsonl.
bool LiveStatsAppend(const std::string& line, bool allow_rotate) {
    static std::mutex mu; static FILE* f = nullptr; static unsigned n = 0;
    std::lock_guard<std::mutex> l(mu);
    return AppendCapped("stats.jsonl", f, n, line, allow_rotate);
}

// One line appended to stats-frames.jsonl (VKIFRAME_STATS_FRAMES=1: every frame time of a second).
bool LiveFramesAppend(const std::string& line) {
    static std::mutex mu; static FILE* f = nullptr; static unsigned n = 0;
    std::lock_guard<std::mutex> l(mu);
    return AppendCapped("stats-frames.jsonl", f, n, line, true);
}

void LiveWriteStatus(double native_fps, double shown_fps, int k, const char* mode, double refresh_hz, const char* bench) {
    static std::mutex mu;
    std::lock_guard<std::mutex> l(mu);
    const std::string dir = ConfDir(), tmp = dir + "/status.json.tmp", fin = dir + "/status.json";
    if (dir.empty() || !MakePrivateDir(dir)) return;
    (void)::unlink(tmp.c_str());
    const int fd = ::open(tmp.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
    if (fd < 0) return;
    FILE* f = ::fdopen(fd, "w");
    if (!f) { ::close(fd); return; }
    std::fprintf(f, "{\"native_fps\":%.1f,\"output_fps\":%.1f,\"k\":%d,\"mode\":\"%s\",\"enabled\":%d,\"refresh_hz\":%.2f,\"bench\":\"%s\",\"synth_capacity\":%d}\n", native_fps, shown_fps, k, mode, Live().enabled.load(), refresh_hz, bench ? bench : "", g_synth_cap.load());
    std::fclose(f);
    (void)std::rename(tmp.c_str(), fin.c_str());
}
