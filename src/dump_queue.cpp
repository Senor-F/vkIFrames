#include "dump_queue.hpp"
#include "vk_iframes_layer.hpp"

#include <cstdio>
#include <fcntl.h>
#include <new>
#include <stdexcept>
#include <sys/stat.h>
#include <unistd.h>

bool DumpQueue::Add(const char* path, const uint8_t* p, size_t n) noexcept {
    try { items.push_back({path, std::vector<uint8_t>(p, p + n)}); return true; }
    catch (const std::bad_alloc&) { return false; }
    catch (const std::length_error&) { return false; }
}

bool DumpQueue::AddTime(int k, int64_t ns) noexcept {
    try { char line[96]; std::snprintf(line, sizeof line, "pair%d_A %lld\n", k, (long long)ns); times += line; return true; }
    catch (const std::bad_alloc&) { return false; }
    catch (const std::length_error&) { return false; }
}

void DumpQueue::Flush(const char* dir) {
    size_t written = 0, total = items.size();
    if (!times.empty()) {
        char tp[512]; std::snprintf(tp, sizeof tp, "%s/times.txt", dir);
        if (Add(tp, reinterpret_cast<const uint8_t*>(times.data()), times.size())) ++total;
        else FG_LOG("dump: times.txt FAILED (out of memory)");
        times.clear();
    }
    for (const auto& it : items) {
        const int dfd = ::open(it.path.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_NOFOLLOW | O_CLOEXEC, 0600);     // screenshots: owner only, no symlinks
        FILE* f = dfd >= 0 ? ::fdopen(dfd, "wb") : nullptr;
        if (!f) { if (dfd >= 0) ::close(dfd); FG_LOG("dump: %s: FAILED to open", it.path.c_str()); continue; }
        const bool full = std::fwrite(it.data.data(), 1, it.data.size(), f) == it.data.size();
        const bool closed = std::fclose(f) == 0;
        if (full && closed) ++written; else FG_LOG("dump: %s: FAILED to write", it.path.c_str());
    }
    if (written == total) FG_LOG("dump: capture complete, %zu of %zu files written", written, total);
    else FG_LOG("dump: capture FAILED, %zu of %zu files written", written, total);
    items.clear(); items.shrink_to_fit(); synth_left = 0; pairs_queued = 0;
}

void DumpQueue::Drop(const char* why) {
    if (!items.empty()) FG_LOG("dump: %zu queued images not written (%s)", items.size(), why);
    items.clear(); items.shrink_to_fit(); times.clear(); synth_left = 0; pairs_queued = 0;
}

void DumpQueue::MaybeFlush(const char* dir, int frames, bool force) {
    if (items.empty()) return;
    if (force || (pairs_queued >= frames && synth_left == 0)) Flush(dir);
}
