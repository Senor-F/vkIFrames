#pragma once
// Live settings: ~/.config/vkiframes/live.conf (written by the Decky plugin) is polled about once a second from the present path.
// The environment gives the startup values; a key in the file overrides them, removing the file reverts. Every change is written to live.log.
#include <atomic>
#include <cstdint>

struct LiveSettings {
    std::atomic<int>   enabled{1};           // ENABLED: 0 = bypass, native frames pass straight through (also no base-fps cap)
    std::atomic<float> target_fps{60.0f};    // TARGET_FPS
    std::atomic<int>   max_synthetic{3};     // MAX_SYNTHETIC (up to kLiveMaxSynthetic: the swapchain is sized for that many)
    std::atomic<float> base_fps_cap{0.0f};   // BASE_FPS_CAP
    std::atomic<float> min_gain{1.12f};      // MIN_GAIN
    std::atomic<int>   phase_lock{1};        // PHASE_LOCK
    std::atomic<float> phase_offset{0.5f};   // PHASE_OFFSET
    std::atomic<int>   interp_bidir{0};        // INTERP_BIDIR (takes effect from the next pair)
};
constexpr int kLiveMaxSynthetic = 10;        // hard upper bound of MAX_SYNTHETIC (experimental mode)
constexpr int kLiveMaxSyntheticNormal = 4;   // the most without experimental mode
// The most generated frames this game run can use: 10 when EXPERIMENTAL=1 was set at game start (env or live.conf), else 4. The swapchain is sized for it.
int LiveMaxSyntheticLimit();

// Without VKIFRAME_LOG_FILE the layer's own output goes to live.log (0600, in a 0700 folder; the game's stderr is not touched). Returns false on failure.
bool LiveLogOpen();
LiveSettings& Live();                         // initialised from the environment on first use
void LivePoll(uint64_t now_ns);               // cheap: one stat() per second; re-reads the file when it changed
// About once a second: native fps, shown fps, generated frames per native frame and the mode; written to status.json next to live.conf
void LiveWriteStatus(double native_fps, double shown_fps, int k, const char* mode, double refresh_hz = 0.0);  // refresh_hz: the swapchain's refresh rate, 0 = unknown
