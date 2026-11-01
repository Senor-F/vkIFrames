#pragma once
// VKIFRAME_DUMP: host-visible buffers allocated once at start, so that no allocation, fence wait or CPU copy happens inside the
// capture window. The window only records vkCmdCopyImageToBuffer into them (inside a command buffer that is submitted anyway);
// after the window one wait covers every copy and the files are written straight from the mapped memory.
// Not thread safe: the caller holds gpu_mu.
#include "vk_iframes_layer.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

struct DumpLayout {
    size_t a_bytes{0};      // one native frame
    size_t b_bytes{0};
    size_t fwd_bytes{0}, bwd_bytes{0}, mask_bytes{0};
    int    synth{0};        // synthetic frames (each as large as a native frame)
};

struct DumpPool {
    struct Buf {
        VkBuffer       buf{};
        VkDeviceMemory mem{};
        uint8_t*       ptr{};      // persistently mapped
        size_t         bytes{0};
        std::string    name;       // file name, set when the copy is recorded
        bool           recorded{false};
    };

    bool ready{false};
    bool cached{false};            // every buffer is in a host-cached memory type (reading the files back from it is fast)
    int  frames{0};                // native frames the pool holds (the window length that was actually allocated)
    std::vector<Buf> bufs;         // [0, frames) A frames, then B, fwd, bwd, mask, then the synthetic frames

    Buf& A(int k)     { return bufs[size_t(k)]; }
    Buf& B()          { return bufs[size_t(frames)]; }
    Buf& Fwd()        { return bufs[size_t(frames) + 1]; }
    Buf& Bwd()        { return bufs[size_t(frames) + 2]; }
    Buf& Mask()       { return bufs[size_t(frames) + 3]; }
    Buf& Synth(int k) { return bufs[size_t(frames) + 4 + size_t(k)]; }

    // Allocates for `wanted` frames; if that fails, for half as many, down to 5, and logs each step. All or nothing for a size:
    // a failed attempt frees everything it allocated. Returns false (ready stays false) when even 5 frames do not fit.
    bool Create(const DeviceDispatch& dt, const DumpLayout& layout, int wanted);
    void Destroy(const DeviceDispatch& dt);           // unmaps and frees every buffer; safe to call twice
    void ClearRecorded();                             // forgets which copies were recorded (a submit failed)
    size_t TotalBytes() const;

private:
    bool AllocateAll(const DeviceDispatch& dt, const DumpLayout& layout, int frames);
    bool AllocateOne(const DeviceDispatch& dt, Buf& b, size_t bytes, const VkPhysicalDeviceMemoryProperties& mp, size_t index);
};
