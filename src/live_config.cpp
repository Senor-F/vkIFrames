#include "live_config.hpp"

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

namespace {

enum Key { kEnabled, kTargetFps, kMaxSynthetic, kBaseFpsCap, kMinGain, kPhaseLock, kPhaseOffset, kInterpBidir, kCmdBuffers, kSynthMargin, kStats, kBench, kKeys };

struct KeyInfo { const char* name; bool toggle; double lo, hi; };   // ranges as in settings_schema.json on the decky-plugin branch
constexpr KeyInfo kInfo[kKeys] = {
    {"ENABLED", true, 0, 1}, {"TARGET_FPS", false, 30, 1000}, {"MAX_SYNTHETIC", false, 1, kLiveMaxSyntheticNormal}, {"BASE_FPS_CAP", false, 0, 90},
    {"MIN_GAIN", false, 1.01, 1.5}, {"PHASE_LOCK", true, 0, 1}, {"PHASE_OFFSET", false, 0.0, 0.999}, {"INTERP_BIDIR", true, 0, 1}, {"CMD_BUFFERS", false, 4, 32}, {"SYNTH_MARGIN", false, 0.0, 0.45},
    {"STATS", true, 0, 1}, {"BENCH", false, 0, 4e12},     // BENCH: a request token (any new number starts one benchmark run), see stats.hpp
};

// Empty when neither XDG_CONFIG_HOME nor HOME is set: then there are no live settings and no live.log (no shared /tmp fallback).
std::string ConfDir() {
    if (const char* x = secure_getenv("XDG_CONFIG_HOME"); x && *x == '/') return std::string(x) + "/vkiframes";
    if (const char* h = secure_getenv("HOME"); h && *h == '/') return std::string(h) + "/.config/vkiframes";
    return "";
}
std::string ConfPath() { if (const char* v = EnvVar("LIVE_CONF"); v && *v) return v; const std::string d = ConfDir(); return d.empty() ? d : d + "/live.conf"; }
std::string LogPath()  { if (const char* v = EnvVar("LIVE_LOG");  v && *v) return v; const std::string d = ConfDir(); return d.empty() ? d : d + "/live.log"; }

// Reads a small settings file: no symlink, a regular file owned by this user. Empty string when it cannot be used.
bool ReadOwnFile(const std::string& path, std::string& text) {
    text.clear();
    if (path.empty()) return false;
    const int fd = ::open(path.c_str(), O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
    if (fd < 0) return false;
    struct stat st;
    if (::fstat(fd, &st) != 0 || !S_ISREG(st.st_mode) || st.st_uid != ::geteuid()) { ::close(fd); errno = EACCES; return false; }
    char buf[4096]; ssize_t n;
    while ((n = ::read(fd, buf, sizeof buf)) > 0 && text.size() < 65536) text.append(buf, size_t(n));
    ::close(fd);
    return true;
}


// Creates DIR (and ~/.config above it) with mode 0700; an existing directory of this user is tightened to 0700. False if it cannot be used.
bool MakePrivateDir(const std::string& dir) {
    if (dir.empty()) return false;
    const size_t slash = dir.rfind('/');
    if (slash != std::string::npos && slash > 0) (void)::mkdir(dir.substr(0, slash).c_str(), 0700);   // ~/.config may not exist yet
    if (::mkdir(dir.c_str(), 0700) != 0 && errno != EEXIST) return false;
    struct stat st;
    if (::lstat(dir.c_str(), &st) != 0 || !S_ISDIR(st.st_mode) || st.st_uid != ::geteuid()) return false;
    if ((st.st_mode & 077) != 0) (void)::chmod(dir.c_str(), 0700);
    return true;
}

// Opens PATH for appending: created 0600, a symlink is refused. Its directory is made private first when it is the layer's own.
FILE* OpenPrivateAppend(const std::string& path) {
    if (path.empty()) return nullptr;
    if (path == ConfDir() + "/live.log" && !MakePrivateDir(ConfDir())) return nullptr;
    const int fd = ::open(path.c_str(), O_WRONLY | O_APPEND | O_CREAT | O_NOFOLLOW | O_CLOEXEC, 0600);
    if (fd < 0) return nullptr;
    // open() applies the mode only when it creates the file: one that an older version left at 0644 is tightened (our own files in our own folder only).
    struct stat st;
    const std::string dir = ConfDir();
    if (!dir.empty() && path.rfind(dir + "/", 0) == 0 && ::fstat(fd, &st) == 0 && S_ISREG(st.st_mode) && st.st_uid == ::geteuid() && (st.st_mode & 077) != 0) (void)::fchmod(fd, 0600);
    FILE* f = ::fdopen(fd, "a");
    if (!f) ::close(fd);
    return f;
}

std::mutex g_log_mu;
FILE* g_log = nullptr;
bool g_shared = false;                 // stderr is live.log: LogLine writes there, no second handle
// One line in live.log (verbose, with wall-clock time) and, shorter, in the layer's own log.
void LogLine(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
void LogLine(const char* fmt, ...) {
    char msg[600];
    va_list ap; va_start(ap, fmt); std::vsnprintf(msg, sizeof msg, fmt, ap); va_end(ap);
    std::lock_guard<std::mutex> l(g_log_mu);
    if (g_shared) {                                         // the layer's log stream already is live.log: one line, with the wall-clock time
        timespec ts; clock_gettime(CLOCK_REALTIME, &ts);
        tm t; localtime_r(&ts.tv_sec, &t);
        std::fprintf(FgLog(), "%04d-%02d-%02d %02d:%02d:%02d.%03ld [FrameGen] live: %s\n", t.tm_year + 1900, t.tm_mon + 1, t.tm_mday, t.tm_hour, t.tm_min, t.tm_sec, ts.tv_nsec / 1000000, msg);
        return;
    }
    FG_LOG("live: %s", msg);
    if (!g_log) {
        g_log = OpenPrivateAppend(LogPath());
        if (!g_log) return;
        std::setvbuf(g_log, nullptr, _IOLBF, 0);
    }
    timespec ts; clock_gettime(CLOCK_REALTIME, &ts);
    tm t; localtime_r(&ts.tv_sec, &t);
    std::fprintf(g_log, "%04d-%02d-%02d %02d:%02d:%02d.%03ld pid %d  %s\n", t.tm_year + 1900, t.tm_mon + 1, t.tm_mday, t.tm_hour, t.tm_min, t.tm_sec, ts.tv_nsec / 1000000, int(::getpid()), msg);
}

double Get(const LiveSettings& s, int k) {
    switch (k) {
        case kEnabled: return s.enabled; case kTargetFps: return s.target_fps; case kMaxSynthetic: return s.max_synthetic; case kBaseFpsCap: return s.base_fps_cap;
        case kMinGain: return s.min_gain; case kPhaseLock: return s.phase_lock; case kPhaseOffset: return s.phase_offset; case kCmdBuffers: return s.cmd_buffers; case kSynthMargin: return s.synth_margin; case kStats: return s.stats; case kBench: return double(s.bench_token.load()); default: return s.interp_bidir;
    }
}
void Set(LiveSettings& s, int k, double v) {
    switch (k) {
        case kEnabled: s.enabled = int(v); break; case kTargetFps: s.target_fps = float(v); break; case kMaxSynthetic: s.max_synthetic = int(v); break;
        case kBaseFpsCap: s.base_fps_cap = float(v); break; case kMinGain: s.min_gain = float(v); break; case kPhaseLock: s.phase_lock = int(v); break;
        case kPhaseOffset: s.phase_offset = float(v); break; case kCmdBuffers: s.cmd_buffers = int(v); break; case kSynthMargin: s.synth_margin = float(v); break; case kStats: s.stats = int(v); break; case kBench: s.bench_token = (long long)v; break; default: s.interp_bidir = int(v); break;
    }
}
std::string Show(int k, double v) { char b[40]; if (k == kBench) std::snprintf(b, sizeof b, "%lld", (long long)v); else if (kInfo[k].toggle || k == kMaxSynthetic || k == kCmdBuffers) std::snprintf(b, sizeof b, "%d", int(v)); else std::snprintf(b, sizeof b, "%g", v); return b; }

double g_startup[kKeys];            // values from the environment (what removing the file goes back to)
bool   g_have_file = false;
long long g_mtime_ns = 0; long long g_size = -1; unsigned long long g_rev = 0;
unsigned long long g_gen = 0;      // counts the loads that changed a setting (the benchmark stops when it moves)
long long g_bench_seen = 0;        // the BENCH token already handled (or present when the game started)
uint64_t g_next_poll = 0;
std::mutex g_poll_mu;

// Parses VALUE for key k; true when usable, with the clamped number in out and a note when it was clamped.
bool ParseValue(int k, const std::string& raw, double& out, std::string& note) {
    char* end = nullptr;
    errno = 0;
    double v = std::strtod(raw.c_str(), &end);
    while (end && (*end == ' ' || *end == '\t' || *end == '\r')) ++end;
    if (raw.empty() || end == raw.c_str() || (end && *end) || errno == ERANGE || !std::isfinite(v)) {
        if (kInfo[k].toggle && (raw == "true" || raw == "on" || raw == "yes")) { out = 1; return true; }
        if (kInfo[k].toggle && (raw == "false" || raw == "off" || raw == "no")) { out = 0; return true; }
        return false;
    }
    if (kInfo[k].toggle) { out = v != 0.0 ? 1 : 0; if (v != 0.0 && v != 1.0) note = "any non-zero value counts as 1"; return true; }
    if (k == kMaxSynthetic || k == kBench || k == kCmdBuffers) v = std::round(v);
    const double hi = (k == kMaxSynthetic) ? double(LiveMaxSyntheticLimit()) : kInfo[k].hi;   // experimental mode (set at game start) allows up to 10
    const double c = std::clamp(v, kInfo[k].lo, hi);
    if (c != v) { char b[120]; std::snprintf(b, sizeof b, "clamped to the range %g..%g", kInfo[k].lo, hi); note = b; }
    out = (k == kMaxSynthetic || k == kBench || k == kCmdBuffers) ? c : double(float(c));      // the settings are floats: compare as floats, or 1.12 reads as a change from 1.12f
    return true;
}

// Old spellings of renamed keys (warp -> interpolation) still work in live.conf.
void NormalizeKey(std::string& key) {
    static const char* const kOld[][2] = {{"WARP_BIDIR", "INTERP_BIDIR"}, {"WARP_GUIDE", "INTERP_GUIDE"}, {"WARP_CAND", "INTERP_CAND"}, {"WARP", "INTERPOLATE"}};
    for (const auto& o : kOld) if (key == o[0]) { key = o[1]; return; }
}

void Trim(std::string& s) { while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r' || s.back() == '\n')) s.pop_back(); size_t i = 0; while (i < s.size() && (s[i] == ' ' || s[i] == '\t')) ++i; s.erase(0, i); }

void Apply(const char* why, const std::string& text, bool present) {
    LiveSettings& live = Live();
    double want[kKeys];
    for (int k = 0; k < kKeys; ++k) want[k] = g_startup[k];
    bool from_file[kKeys] = {};
    unsigned lines = 0, comments = 0, bad = 0, unknown = 0, startup = 0;
    if (present) {
        size_t pos = 0; int lineno = 0;
        while (pos <= text.size()) {
            size_t nl = text.find('\n', pos); if (nl == std::string::npos) nl = text.size();
            std::string line = text.substr(pos, nl - pos); pos = nl + 1; ++lineno;
            Trim(line);
            if (line.empty() || line[0] == '#') { ++comments; continue; }
            ++lines;
            const size_t eq = line.find('=');
            if (eq == std::string::npos) { ++bad; LogLine("line %d ignored (no '='): \"%.60s\"", lineno, line.c_str()); continue; }
            std::string key = line.substr(0, eq), val = line.substr(eq + 1); Trim(key); Trim(val);
            if (key == "REV") { g_rev = std::strtoull(val.c_str(), nullptr, 10); continue; }
            for (const char* pfx : {"VKIFRAME_", "FRAMEGEN_"}) if (key.rfind(pfx, 0) == 0) { key.erase(0, std::strlen(pfx)); break; }
            NormalizeKey(key);
            int k = -1;
            for (int i = 0; i < kKeys; ++i) if (key == kInfo[i].name) k = i;
            if (k < 0) {
                bool startup_only = false;
                for (const char* sk : {"INTERPOLATE", "WARP", "EXTRAPOLATE", "UI_MASK_THRESHOLD", "FLOW_LEVEL", "EXPERIMENTAL"}) if (key == sk) startup_only = true;
                if (startup_only) { ++startup; continue; }     // read once when the game starts (logged then), not an error
                ++unknown; LogLine("line %d: unknown key \"%.40s\" ignored", lineno, key.c_str()); continue;
            }
            double v; std::string note;
            if (!ParseValue(k, val, v, note)) { ++bad; LogLine("line %d: %s=\"%.40s\" is not a valid value, keeping %s", lineno, kInfo[k].name, val.c_str(), Show(k, Get(live, k)).c_str()); continue; }
            if (!note.empty()) LogLine("line %d: %s=%s %s -> %s", lineno, kInfo[k].name, val.c_str(), note.c_str(), Show(k, v).c_str());
            want[k] = v; from_file[k] = true;
        }
    }
    int changed = 0;
    for (int k = 0; k < kKeys; ++k) {
        const double old = Get(live, k);
        if (old == want[k]) continue;
        ++changed;
        LogLine("CHANGE %s: %s -> %s (%s)", kInfo[k].name, Show(k, old).c_str(), Show(k, want[k]).c_str(), from_file[k] ? "live.conf" : "back to the startup value");
        Set(live, k, want[k]);
    }
    if (changed) ++g_gen;
    LogLine("%s: REV %llu, %u setting line(s), %u comment/blank, %u invalid, %u unknown, %u start-of-game only (not applied now); %d value(s) changed", why, g_rev, lines, comments, bad, unknown, startup, changed);
    if (changed) {
        char sum[300]; int n = 0;
        for (int k = 0; k < kKeys; ++k) n += std::snprintf(sum + n, sizeof sum - size_t(n), "%s%s=%s", k ? " " : "", kInfo[k].name, Show(k, Get(live, k)).c_str());
        LogLine("now: %s", sum);
    }
}

}  // namespace

// Settings that cannot change while a game runs (they size buffers or pick shaders) are read from live.conf once, when the game starts.
const char* StartupFileValue(const char* name) {
    static const char* const kStartupKeys[] = {"EXTRAPOLATE", "UI_MASK_THRESHOLD", "FLOW_LEVEL", "INTERPOLATE", "EXPERIMENTAL"};
    bool allowed = false;
    for (const char* k : kStartupKeys) if (std::strcmp(name, k) == 0) allowed = true;
    if (!allowed) return nullptr;                       // also keeps LIVE_CONF / LIVE_LOG lookups out of the loader below
    static const std::map<std::string, std::string> values = [] {
        std::map<std::string, std::string> m;
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
            NormalizeKey(key);
            for (const char* k : kStartupKeys) if (key == k && !val.empty()) m[key] = val;
        }
        for (auto& kv : m) LogLine("startup setting %s=%s taken from live.conf (read once when the game starts: a change needs a game restart)", kv.first.c_str(), kv.second.c_str());
        return m;
    }();
    auto it = values.find(name);
    return it == values.end() ? nullptr : it->second.c_str();
}

bool LiveLogOpen() {
    const std::string path = LogPath();
    if (path.empty()) return false;                      // no HOME / XDG_CONFIG_HOME: the layer keeps logging to stderr, there are no live settings
    if (path == ConfDir() + "/live.log" && !MakePrivateDir(ConfDir())) return false;
    struct stat st;
    if (::lstat(path.c_str(), &st) == 0 && S_ISREG(st.st_mode) && st.st_size > 8 * 1024 * 1024) (void)std::rename(path.c_str(), (path + ".1").c_str());   // one old log is kept
    FILE* f = OpenPrivateAppend(path);
    if (!f) return false;
    std::setvbuf(f, nullptr, _IOLBF, 0);
    std::lock_guard<std::mutex> l(g_log_mu);
    g_fg_log = f;                                        // only the layer's own output goes there; the game's stderr is left alone
    g_shared = true;
    return true;
}

int LiveMaxSyntheticLimit() {
    static const int limit = [] {
        const char* v = StartupFileValue("EXPERIMENTAL");     // live.conf wins over the environment, like the other start-of-game keys
        if (!v) v = EnvVar("EXPERIMENTAL");
        const bool on = v && (std::atoi(v) != 0 || std::strcmp(v, "true") == 0 || std::strcmp(v, "on") == 0);
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
        if (const char* v = EnvVar("STATS")) l->stats = std::atoi(v) != 0 ? 1 : 0;
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
    std::string text;
    if (ReadOwnFile(ConfPath(), text)) {
    } else { LogLine("live.conf could not be opened (errno %d), keeping the current values", errno); return; }
    Apply(first ? "live.conf found at startup" : "live.conf changed", text, true);
    if (first) g_bench_seen = Live().bench_token.load();      // a token left in the file from before is not a request
}

int LiveKeyTable(LiveKeyRow* rows, int max) {
    const Config d{};
    const double defs[kKeys] = {1, double(d.target_fps), double(d.max_synthetic), double(d.base_fps_cap), double(d.min_gain), d.phase_lock ? 1.0 : 0.0, double(d.phase_offset), d.interp_bidir ? 1.0 : 0.0, double(d.cmd_buffers), double(d.synth_margin), 0, 0};
    int n = 0;
    for (int k = 0; k < kKeys && n < max; ++k) { if (k == kBench) continue; rows[n++] = {kInfo[k].name, kInfo[k].toggle, kInfo[k].lo, kInfo[k].hi, defs[k]}; }
    return n;
}

unsigned long long LiveGeneration() { return g_gen; }
bool LiveBenchRequest(long long* token) {
    const long long t = Live().bench_token.load();
    if (t == 0 || t == g_bench_seen) return false;
    *token = t; return true;
}
void LiveBenchAck(long long token) { g_bench_seen = token; }

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

// One line appended to stats.jsonl (0600, private folder, no symlink). One old file is kept when it grows past 8 MB.
bool LiveStatsAppend(const std::string& line) {
    static std::mutex mu;
    static FILE* f = nullptr;
    static unsigned n = 0;
    std::lock_guard<std::mutex> l(mu);
    const std::string dir = ConfDir(), path = dir + "/stats.jsonl";
    if (dir.empty() || !MakePrivateDir(dir)) return false;
    if (f && (++n % 600) == 0) {                                  // about every ten minutes: check the size
        struct stat st;
        if (::fstat(::fileno(f), &st) == 0 && st.st_size > 8 * 1024 * 1024) { std::fclose(f); f = nullptr; (void)std::rename(path.c_str(), (path + ".1").c_str()); }
    }
    if (!f) f = OpenPrivateAppend(path);
    if (!f) return false;
    std::fputs(line.c_str(), f); std::fputc('\n', f); std::fflush(f);
    return true;
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
    std::fprintf(f, "{\"native_fps\":%.1f,\"output_fps\":%.1f,\"k\":%d,\"mode\":\"%s\",\"enabled\":%d,\"refresh_hz\":%.2f,\"bench\":\"%s\"}\n", native_fps, shown_fps, k, mode, Live().enabled.load(), refresh_hz, bench ? bench : "");
    std::fclose(f);
    (void)std::rename(tmp.c_str(), fin.c_str());
}
