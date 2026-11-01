#pragma once
// VKIFRAME_DUMP: the images of the capture window are read back into host memory and written together after the window,
// so no file write happens inside it. Not thread safe: the caller holds gpu_mu.
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

struct DumpQueue {
    struct Item { std::string path; std::vector<uint8_t> data; };
    std::vector<Item> items;
    std::string       times;                 // times.txt: "pair<k>_A <steady clock ns>" per queued native frame
    int               pairs_queued{0};       // native frames A queued so far
    int               synth_left{0};         // synthetic frames still to read back

    bool Add(const char* path, const uint8_t* p, size_t n) noexcept;     // false when out of memory
    bool AddTime(int k, int64_t ns) noexcept;
    // Writes every queued file (0600, no symlinks) and times.txt into dir, logs "capture complete, X of Y files written" (FAILED when X != Y), clears.
    void Flush(const char* dir);
    // The window ended some other way (game exit, engine off, swapchain recreated): logs what was dropped and clears.
    void Drop(const char* why);
    void MaybeFlush(const char* dir, int frames, bool force);   // flushes once all `frames` A frames and the synthetic frames are queued, or when forced
};
