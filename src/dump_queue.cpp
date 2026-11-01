#include "dump_queue.hpp"
#include "vk_iframes_layer.hpp"

#include <cstdio>
#include <ctime>
#include <fcntl.h>
#include <new>
#include <stdexcept>
#include <sys/stat.h>
#include <unistd.h>

bool DumpQueue::AddView(const char* path, const uint8_t* p, size_t n) noexcept {
    try { items.push_back({path, p, n}); return true; }
    catch (const std::bad_alloc&) { return false; }
    catch (const std::length_error&) { return false; }
}

bool DumpQueue::AddTime(int k, int64_t ns) noexcept {
    try { char line[96]; std::snprintf(line, sizeof line, "pair%d_A %lld\n", k, (long long)ns); times += line; return true; }
    catch (const std::bad_alloc&) { return false; }
    catch (const std::length_error&) { return false; }
}

// Local time of day (the user's own zone, never named) with milliseconds, for the lines that mark the end of the window and of the flush.
static void WallStamp(char* out, size_t n) {
    timespec ts; clock_gettime(CLOCK_REALTIME, &ts);
    tm t; localtime_r(&ts.tv_sec, &t);
    std::snprintf(out, n, "%02d:%02d:%02d.%03ld", t.tm_hour, t.tm_min, t.tm_sec, ts.tv_nsec / 1000000);
}

void DumpQueue::Flush(const char* dir) {
    timespec t0; clock_gettime(CLOCK_MONOTONIC, &t0);
    size_t written = 0, total = items.size();
    if (!times.empty()) {
        char tp[512]; std::snprintf(tp, sizeof tp, "%s/times.txt", dir);
        if (AddView(tp, reinterpret_cast<const uint8_t*>(times.data()), times.size())) ++total;   // views `times`, which stays alive until the loop below ends
        else FG_LOG("dump: times.txt FAILED (out of memory)");
    }
    char stamp[24]; WallStamp(stamp, sizeof stamp);
    FG_LOG("dump: window over at %s, writing %zu files", stamp, total);          // the flush below stalls the game for a moment: this line marks where
    for (const auto& it : items) {
        const int dfd = ::open(it.path.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_NOFOLLOW | O_CLOEXEC, 0600);     // screenshots: owner only, no symlinks
        FILE* f = dfd >= 0 ? ::fdopen(dfd, "wb") : nullptr;
        if (!f) { if (dfd >= 0) ::close(dfd); FG_LOG("dump: %s: FAILED to open", it.path.c_str()); continue; }
        const bool full = std::fwrite(it.ptr, 1, it.n, f) == it.n;
        const bool closed = std::fclose(f) == 0;
        if (full && closed) ++written; else FG_LOG("dump: %s: FAILED to write", it.path.c_str());
    }
    timespec t1; clock_gettime(CLOCK_MONOTONIC, &t1);
    const long long ms = (long long)(t1.tv_sec - t0.tv_sec) * 1000 + (t1.tv_nsec - t0.tv_nsec) / 1000000;
    WallStamp(stamp, sizeof stamp);
    if (written == total) FG_LOG("dump: capture complete, %zu of %zu files written in %lld ms (at %s)", written, total, ms, stamp);
    else FG_LOG("dump: capture FAILED, %zu of %zu files written in %lld ms (at %s)", written, total, ms, stamp);
    items.clear(); items.shrink_to_fit(); times.clear(); synth_left = 0; pairs_queued = 0;
}

void DumpQueue::Drop(const char* why) {
    if (!items.empty()) FG_LOG("dump: %zu queued images not written (%s)", items.size(), why);
    items.clear(); items.shrink_to_fit(); times.clear(); synth_left = 0; pairs_queued = 0;
}
