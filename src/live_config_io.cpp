// Live config I/O: the config folder, safe file reading, live.log and its rotation
#include "live_config.hpp"
#include "live_config_internal.hpp"

#include "vk_iframes_layer.hpp"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <mutex>
#include <string>

namespace liveconf {

// Empty when neither XDG_CONFIG_HOME nor HOME is set: then there are no live settings and no live.log (no shared /tmp fallback).
std::string ConfDir() {
    if (const char* x = secure_getenv("XDG_CONFIG_HOME"); x && *x == '/') return std::string(x) + "/vkiframes";
    if (const char* h = secure_getenv("HOME"); h && *h == '/') return std::string(h) + "/.config/vkiframes";
    return "";
}
std::string ConfPath() { if (const char* v = EnvVar("LIVE_CONF"); v && *v) return v; const std::string d = ConfDir(); return d.empty() ? d : d + "/live.conf"; }
std::string LogPath()  { if (const char* v = EnvVar("LIVE_LOG");  v && *v) return v; const std::string d = ConfDir(); return d.empty() ? d : d + "/live.log"; }

// Reads a small settings file: no symlink, a regular file owned by this user. False when it cannot be used. O_NONBLOCK: a FIFO (or another special file) put in
// the file's place must not block the game's present thread in open(); it is refused after the fstat. At most 64 KiB are used: from a bigger file only the
// complete lines inside the first 64 KiB (a last line cut by the limit is dropped, not read as a shorter value); *truncated says so.
bool ReadOwnFile(const std::string& path, std::string& text, bool* truncated) {
    text.clear();
    if (truncated) *truncated = false;
    if (path.empty()) return false;
    const int fd = ::open(path.c_str(), O_RDONLY | O_NOFOLLOW | O_CLOEXEC | O_NONBLOCK);
    if (fd < 0) return false;
    struct stat st;
    if (::fstat(fd, &st) != 0 || !S_ISREG(st.st_mode) || st.st_uid != ::geteuid()) { ::close(fd); errno = EACCES; return false; }
    char buf[4096]; ssize_t n;
    while (text.size() <= kMaxSettingsBytes && (n = ::read(fd, buf, sizeof buf)) > 0) text.append(buf, size_t(n));
    ::close(fd);
    if (text.size() > kMaxSettingsBytes) {
        text.resize(kMaxSettingsBytes);
        const size_t nl = text.rfind('\n');
        text.resize(nl == std::string::npos ? 0 : nl + 1);
        if (truncated) *truncated = true;
    }
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

long long g_cap_bytes = 8ll * 1024 * 1024;

namespace {
std::mutex g_log_mu;
FILE* g_log = nullptr;
FILE* g_retired_log = nullptr;          // the stream before the last rotation: closed at the next one, so a thread that still holds the old pointer has a whole rotation interval to finish
bool g_shared = false;                 // stderr is live.log: LogLine writes there, no second handle
}  // namespace

// One line in live.log (verbose, with wall-clock time) and, shorter, in the layer's own log.
void LogLine(const char* fmt, ...) {
    char msg[600];
    va_list ap; va_start(ap, fmt); std::vsnprintf(msg, sizeof msg, fmt, ap); va_end(ap);
    // text from a settings file ends up in the message: one line, no control characters (a CR, an escape sequence or a NUL must not forge or hide log lines)
    for (char* c = msg; *c; ++c) if (static_cast<unsigned char>(*c) < 0x20 || *c == 0x7f) *c = '?';
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

}  // namespace liveconf

using namespace liveconf;

bool LiveLogOpen() {
    const std::string path = LogPath();
    if (path.empty()) return false;                      // no HOME / XDG_CONFIG_HOME: the layer keeps logging to stderr, there are no live settings
    if (path == ConfDir() + "/live.log" && !MakePrivateDir(ConfDir())) return false;
    struct stat st;
    if (::lstat(path.c_str(), &st) == 0 && S_ISREG(st.st_mode) && st.st_size > g_cap_bytes) (void)std::rename(path.c_str(), (path + ".1").c_str());   // one old log is kept
    FILE* f = OpenPrivateAppend(path);
    if (!f) return false;
    std::setvbuf(f, nullptr, _IOLBF, 0);
    std::lock_guard<std::mutex> l(g_log_mu);
    g_fg_log.store(f);                                   // only the layer's own output goes there; the game's stderr is left alone
    g_shared = true;
    return true;
}

void LiveLogSetCap(long long bytes) { g_cap_bytes = bytes; }
bool LiveLogIsFile() { return g_shared; }
bool LiveLogAllowAt(std::atomic<uint64_t>& last_ns, uint64_t now_ns) {
    if (g_shared) return true;
    uint64_t last = last_ns.load(std::memory_order_relaxed);
    if (last != 0 && now_ns - last < 60'000'000'000ull) return false;
    return last_ns.compare_exchange_strong(last, now_ns ? now_ns : 1, std::memory_order_relaxed);
}
bool LiveLogAllow(std::atomic<uint64_t>& last_ns) {
    if (g_shared) return true;
    timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    return LiveLogAllowAt(last_ns, uint64_t(ts.tv_sec) * 1'000'000'000ull + uint64_t(ts.tv_nsec));
}

// Called about once a second: when live.log has passed the cap it becomes live.log.1 (the previous .1 goes) and a new live.log is opened; the old stream stays
// open until the next rotation. Costs one fstat.
void LiveLogCap() {
    std::lock_guard<std::mutex> l(g_log_mu);
    FILE* cur = g_fg_log.load();
    if (!g_shared || !cur) return;
    struct stat st;
    if (::fstat(::fileno(cur), &st) != 0 || st.st_size <= g_cap_bytes) return;
    const std::string path = LogPath();
    (void)std::rename(path.c_str(), (path + ".1").c_str());
    FILE* f = OpenPrivateAppend(path);
    if (!f) return;                                          // keep writing to the old file rather than losing the log
    std::setvbuf(f, nullptr, _IOLBF, 0);
    g_fg_log.store(f);
    if (g_retired_log) std::fclose(g_retired_log);
    g_retired_log = cur;
}
