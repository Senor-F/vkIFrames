#pragma once
// VKIFRAME_DUMP: the files of the capture window are written together after the window, so no file write happens inside it.
// The queue holds views of memory the caller owns (the mapped buffers of DumpPool); it copies nothing. Not thread safe: the caller holds gpu_mu.
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

struct DumpQueue {
    struct Item { std::string path; const uint8_t* ptr{nullptr}; size_t n{0}; };
    std::vector<Item> items;
    std::string       times;                 // times.txt: "pair<k>_A <steady clock ns>" per queued native frame
    int               pairs_queued{0};       // native frames A queued so far
    int               synth_left{0};         // synthetic frames still to record
    int               frames{5};             // native frames in the window (what the pool could allocate)

    bool AddView(const char* path, const uint8_t* p, size_t n) noexcept;     // false when out of memory
    bool AddTime(int k, int64_t ns) noexcept;
    // Writes every queued view (0600, no symlinks) and times.txt into dir, logs "capture complete, X of Y files written" (FAILED when X != Y), clears.
    void Flush(const char* dir);
    // The window ended some other way (game exit, engine off, swapchain recreated): logs what was dropped and clears.
    void Drop(const char* why);
    bool Ready() const { return pairs_queued >= frames && synth_left == 0; }   // all A frames and synthetic frames are recorded
};
