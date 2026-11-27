[index](../README.md) | [previous](01-if-something-goes-wrong.md) | [next](03-live-settings.md)

## How it works

For each native frame the game presents, the layer:

1. **Measures** the game's frame time at `vkQueuePresentKHR` and smooths it, so brief stutters do not make the output rate jump around.
2. **Runs a fixed output clock at the Target FPS.** Every tick presents exactly one frame, either a native frame or a synthetic frame at a fractional time *t* between two native frames, so the output hits the target even when it is not a multiple of the game's rate (40 → 60, 45 → 90). The number of synthetic frames per pair is capped (`VKIFRAME_MAX_SYNTHETIC`). At or above the target the layer bypasses.
3. **Holds one frame** (interpolation mode). To insert frames *between* two native frames, the layer delays the game's own presentation by one frame and presents it on the next one, followed by the synthetic frames. With a near-integer ratio a phase lock keeps the ticks at fixed positions between native frames.
4. **Estimates motion** between the two newest native frames with a hierarchical optical flow (4-level pyramid, forward and backward, sub-pixel refinement) on the GPU, plus a forward/backward check that marks occluded regions.
5. **Interpolates** the frames to each tick's time *t* with a colour-guided vector field. Where the match is poor, a region is occluded or motion is too fast, it fades toward a plain cross-fade; when most of the frame cannot be matched (a fast camera pan) the whole frame cross-fades.
6. **Optionally protects static UI** (`VKIFRAME_UI_MASK_THRESHOLD`): pixels that do not change between two frames cross-fade instead of being dragged along by nearby motion.
7. **Paces** every frame with its own presenter thread against the fixed output clock, and uses `VK_EXT_present_timing` when the driver and compositor offer it.

The math and module breakdown are in [DESIGN.md](../DESIGN.md).

**Where the method comes from.** It is built on standard, published techniques for motion estimation and motion-compensated interpolation (image pyramid, block matching with sub-pixel refinement, forward/backward consistency, flow-guided blending). The layer and the shaders were written for this project. To our knowledge, no code was taken from another frame-generation project.

### Extrapolation mode (lower latency)

Interpolation needs the *next* native frame before it can show anything in between, so it adds about one native frame of delay (68 ms at 15 fps, about 24 ms at 45 fps). With `VKIFRAME_EXTRAPOLATE=1` nothing is held back: the newest native frame is shown on the next output slot and the slots until the following frame show that frame moved forward along its own backward flow. Added delay drops to under one output frame (about 11 ms at 90 Hz). The price is prediction errors: wrong guesses are visible when a real frame replaces them, and appearing or reversing motion cannot be predicted. Opt-in and experimental. Offline, on captured start / stop / reversal frames the prediction rarely overshoots (the fallbacks turn it back into repeating the newest frame), but then it also gains nothing; the gain is on steady motion (error 33–44 % lower). The log line `extrapolation: predicted frames not shown …` counts predicted frames that were cancelled before their slot and how late the native frame went out.

### Trade-offs to know about

- **Added latency.** The layer holds one game frame back so it can interpolate to the next one, plus a small jitter buffer: one to one and a half base frames of delay (about 33 to 50 ms at a 30 fps base, 22 to 33 ms at 45 fps, measured median 41 ms at a 30 fps base). It is set by the base rate, so only a higher base rate lowers it; a base rate a little above the wanted one (for example 32 instead of 30) gains only about 2 ms and breaks the whole-number ratio below. It is inherent: use it only when the base rate is 30 fps or more, or try the extrapolation mode (which holds no frame).
- **Cap your game.** The game should be limited to its intended base rate (in-game limiter, or `VKIFRAME_BASE_FPS_CAP` in Game Mode where Steam's limiter acts below the layer). Choose the base rate as the Target FPS divided by a whole number: 90 with 45 (x2) or 30 (x3), 60 with 30 or 20, 120 with 60 or 40. With another ratio, for example 60 with 32, the generated frames are spaced unevenly, which shows as judder (the layer locks the cadence only when the ratio Target FPS / base rate is within 0.03 of a whole number; for 60 with 30, a base rate between 29.6 and 30.5 fps).
- **Synthetic frame limit.** `VKIFRAME_MAX_SYNTHETIC` (default 3) must be at least target / base − 1, otherwise the output is capped (15 → 90 needs 5).
- **Forced FIFO.** Presentation timing needs the `FIFO` present mode: on a swapchain where the layer uses present timing it creates the swapchain in `FIFO` whatever mode the game asked for (the log says "forcing FIFO"). Without present timing (the game uses present id or present timing itself, or the surface does not offer it) the game's own mode is kept.
- **Early answer to `vkWaitForPresentKHR`.** For the present id of the frame the layer is holding, the layer answers `VK_SUCCESS` at once, before that frame is on screen, because that present only goes out with the game's next one and DXVK waits for every present it makes (a deliberate deviation from the specification). The frame is shown one frame later, or by the idle flush if the game stops presenting. Waits for older ids are real.
- **Artifacts.** Flow estimated from final images (no engine motion vectors) shows artifacts around fast motion, thin geometry such as tree trunks and lines, transparency and disocclusions. The fallbacks reduce them but cannot remove them.
- **GPU cost.** On the Deck at 720p: flow about 5 ms per pair and interpolation about 0.8 ms per synthetic frame. Percentage GPU readings are unreliable on the Deck; the per-stage times in the log are the reference.

---

## Quick start (build from source)

The release archives above need no build. This section is for building the layer yourself.

### 1. Build

Build inside a container with a compiler (on SteamOS: a Distrobox). The repository's Vulkan headers must be recent enough to include `VK_EXT_present_timing`.

```bash
git clone https://github.com/Senor-F/vkIFrames.git && cd vkIFrames
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j"$(nproc)"
ctest --test-dir build          # mock-driver tests, no GPU needed
```

This produces `build/libvkIFrames.so` and `build/VkLayer_frame_gen.json`. Requirements: Linux x86_64 (64-bit games), a Vulkan 1.3 driver, CMake ≥ 3.21, a C++20 compiler, Vulkan-Headers and `glslangValidator` (compiles the shaders). A GPU with a separate compute queue is not required.

**What the library needs at run time:** 64-bit x86-64 Linux only (no 32-bit build) and glibc 2.34 or newer (SteamOS 3.x, Ubuntu 22.04+, Debian 12+); nothing else. The C++ runtime and libgcc are linked into the library (`-static-libstdc++ -static-libgcc`, option `FRAMEGEN_STATIC_RUNTIME`, on by default), so it needs no `libstdc++.so.6` or `libgcc_s.so.1` from the host or from a game's container, and the runtime's symbols are hidden: the library exports exactly three symbols, `FrameGen_NegotiateLoaderLayerInterfaceVersion`, `FrameGen_GetInstanceProcAddr` and `FrameGen_GetDeviceProcAddr` (`scripts/check_so.sh` checks the exports, the imported symbol versions, the glibc floor, the dependencies and that no home folder, user name or host name is in the library; it runs as a test). Sanitizer builds use `-DFRAMEGEN_SANITIZER=address|thread` (below), which turns the static runtime off by itself. Exceptions: the layer's entry points and hooks are C functions and contain no `throw`; the only `try/catch` blocks are around out-of-memory in the debug dump; an out-of-memory elsewhere in the layer throws `std::bad_alloc`, nobody catches it, and `std::terminate` aborts the game (as before; nothing in the layer tries to survive running out of memory).

**Sanitizer builds (development only, never the release binary).** From a clean build folder: `cmake -S . -B build-asan -DFRAMEGEN_SANITIZER=address -DFRAMEGEN_VULKAN_INCLUDE_DIR=<Vulkan-Headers>/include && cmake --build build-asan -j && ctest --test-dir build-asan -j4 --output-on-failure` (AddressSanitizer + UndefinedBehaviorSanitizer; leak reports are off because the layer leaves a few objects on purpose when a presenter thread is detached at exit), and the same with `-DFRAMEGEN_SANITIZER=thread` and `build-tsan` for ThreadSanitizer. Every ctest test then runs with the sanitizer's options; the two tests that need the static runtime (`so_checks`, `static_runtime_isolated`) are not part of those builds. With GCC 11 the thread build works around a libtsan gap (it does not know `pthread_cond_clockwait`, which libstdc++ 11 uses for `wait_for`/`wait_until`, so every condition-variable wait looked like a double lock): CMake tells libstdc++ not to use it, in that build only. `tests/tsan.supp` hides the races on the mock drivers' own recording variables (the mock functions `m_*` run on the layer's presenter thread, the test reads the records afterwards); a race between two layer functions has no such frame and fails the test. The 46 tests of each build pass on main.

**The C++ runtime is bundled.** `libstdc++` and `libgcc` (GCC 11.4, Ubuntu 22.04) are linked into the library, so the layer loads in a Steam Linux Runtime container or a Proton prefix whatever `libstdc++` the game brings, and its runtime symbols stay hidden. The price is that this code is frozen at the version it was built with and does not follow updates of the host's `libstdc++`; `glibc` stays dynamic and does get the host's updates. The layer's own code is small and reads only its settings files and environment, so the exposed part is minimal. A fix in that runtime needs a new release. To see what a library was built with: `readelf -p .comment libvkIFrames.so` (the compiler) and `ldd libvkIFrames.so` (only `libm` and `libc`). To use the host's runtime instead, build with `-DFRAMEGEN_STATIC_RUNTIME=OFF`.

**A hung pacing thread at exit.** When a game closes its swapchain, the layer waits up to 3 seconds for its pacing thread. If the thread is stuck inside a driver call for longer, the layer detaches it and leaves its few objects allocated (the log says so) rather than hang the game; the game's exit is not held up.

### 2. Install on the machine that runs the game (no root, no launch script)

Copy `build/libvkIFrames.so`, the `scripts/` folder and the `manifest/` folder to the target machine (`install.sh` builds the layer manifest from `manifest/VkLayer_frame_gen.json.in`), then:

```bash
chmod +x scripts/*.sh
scripts/install.sh          # copies the library to ~/.local/share/vkiframes/ and writes ~/.local/share/vulkan/implicit_layer.d/VkLayer_frame_gen.json
scripts/uninstall.sh        # removes the manifest and that library copy again
```

### 3. Turn it on per game

In Steam, set the game's launch options, for example for a Deck OLED at 90 Hz with the game capped to 45:

```text
VKIFRAME_ENABLE=1 VKIFRAME_TARGET_FPS=90 VKIFRAME_BASE_FPS_CAP=45 %command%
```

- `VKIFRAME_ENABLE=1` loads the layer, `VKIFRAME_DISABLE=1` forces it off.
- `PROTON_USE_WOW64=1` is for 32-bit games under Proton (the layer is 64-bit). `DXVK_CONFIG="dxvk.enablePresentTiming = False"` is the DXVK setting that worked once for a game whose present chain the layer could not carry (see "Known issues").
- The layer's log (rates, content delay, GPU times, settings changes) goes to `~/.config/vkiframes/live.log` by default; `VKIFRAME_LOG_FILE` writes it to another file instead. `live.log` and `stats.jsonl` are each capped at 8 MiB: past that the file becomes `.1` (one old file is kept, the previous one goes), checked when the layer starts and about once a second while it runs, so the two files and their old copies together stay under about 32 MiB. If `live.log` cannot be opened (no writable config folder) the layer's per-second lines are cut to one a minute, so the game's own stderr (and Steam's log) does not fill up. The Decky plugin caps its lines in the same file the same way. `VKIFRAME_LOG_FILE` (below) has no cap: use it for short runs only.
- By default synthetic frames are plain cross-fades (no optical flow passes, more ghosting); `VKIFRAME_OPTICAL_FLOW=1` builds them along the optical flow.
- The earlier `FRAMEGEN_` spelling of all settings still works; `VKIFRAME_` wins if both are set.

For a quick test of the base frame rate use `VKIFRAME_BASE_FPS_CAP`, an in-game limiter, or `DXVK_FRAME_RATE=30` on Proton.

During development the layer can also be loaded explicitly: `VK_LAYER_PATH=$PWD/build VK_LOADER_LAYERS_ENABLE=VK_LAYER_CUSTOM_frame_gen VKIFRAME_TARGET_FPS=60 vkcube`, or through `scripts/gamemode_launch.sh %command%`, which also writes `build/gamemode_<time>.log`.

### Reading the log

Once per second the layer logs lines such as:

```text
native 45.0 fps -> output 90.0 fps (planned) | shown 90.0 fps | generating | pairs 45 (synthetic 90) bypass 0 carrier-only 0 dropped 0 | content delay 25.4 ms
interpolation: GPU 0.80 ms per synthetic frame | flow: GPU 4.98 ms per pair
```

`dropped` counts planned ticks discarded by the `VKIFRAME_MAX_SYNTHETIC` limit; `content delay` is the latency the layer adds.

---

## Configuration

Environment variables, read once at start. Defaults in the second column.

| Variable | Default | Description |
| :--- | :--- | :--- |
| `VKIFRAME_ENABLE` | unset | `1` turns the installed implicit layer on for that process (loader setting). |
| `VKIFRAME_DISABLE` | unset | `1` forces the layer off. |
| `VKIFRAME_LOG_FILE` | unset (`~/.config/vkiframes/live.log`) | Send the layer's log to this file instead of live.log. **No size cap: for short runs only.** Setting it also redirects the game's own stderr there; without it, only the layer's own lines are written (to live.log) and the game's stderr is untouched. |
| `VKIFRAME_TARGET_FPS` | `120` (`90` on the Steam Deck OLED, `60` on the Steam Deck LCD; the machine is read from the board name) | Output frame rate the layer aims for exactly. At or above it the layer bypasses. |
| `VKIFRAME_BASE_FPS_CAP` | `60` (`45` on the Steam Deck OLED, `30` on the Steam Deck LCD; `0` = off) | The layer limits the game's present rate itself (Game Mode, testing). The default is half of the machine's default target (45 for 90 on the Deck OLED, 30 for 60 on the Deck LCD, 60 for 120 elsewhere); for another target pick exactly half of it, or a third for x3 (30 for 90 Hz); the layer locks the cadence only when the ratio Target FPS / base rate is within 0.03 of a whole number (for 60 with 30: a base rate between 29.6 and 30.5 fps), and the game cannot run faster than the cap, and `0` leaves the game uncapped, then a game that already runs near the target gets few or no generated frames. It waits after each present, so the game samples input as late as it can; `VKIFRAME_CAP_EARLY=1` waits before the present instead (exact intervals, but every frame is older when shown). |
| `VKIFRAME_MAX_SYNTHETIC` | `3` | Most synthetic frames per native frame (1–4; up to 10 with `VKIFRAME_EXPERIMENTAL=1`). |
| `VKIFRAME_CMD_BUFFERS` | `8` | Command buffer entries per swapchain (4..32). A GPU-bound game makes the game thread wait on the fence of the oldest entry (`wait_fence_ms` in the stats); more entries let the layer queue further ahead of the GPU before that happens, at the price of more latency in the layer's own work and a little memory. Live: change it in live.conf or the Decky panel while the game runs. |
| `VKIFRAME_EXPERIMENTAL` | `0` | `1` raises the `MAX_SYNTHETIC` limit from 4 to 10 (untested above 4). It adds no swapchain images by itself: the swapchain is built for `MAX_SYNTHETIC` as set at game start (live.conf over the environment), at least 4, so values above 4 use more images and memory only when you set them and restart the game. Read once when the game starts, from live.conf or the environment. The Decky plugin also uses it to unlock its "More settings". |
| `VKIFRAME_MIN_GAIN` | `1.01` | Interpolate only when the game's frame time exceeds the target frame time by this factor. |
| `VKIFRAME_OPTICAL_FLOW` | `0` (old names `VKIFRAME_INTERPOLATE`, `VKIFRAME_WARP`, still read) | `1`: build synthetic frames along the optical flow (implies `VKIFRAME_FLOW`; costs about 5 ms of GPU time per native frame in the scene we measured (Tomb Raider (2013), 720p, one run); a start-of-game setting). `0` (default): plain cross-fades, no optical flow. |
| `VKIFRAME_EXTRAPOLATE` | `0` | `1`: low-latency mode, no frame held back, slots after each native frame show predicted frames (implies `VKIFRAME_OPTICAL_FLOW`). |
| `VKIFRAME_UI_MASK_THRESHOLD` | `0.08` (`0` = off) | Static-UI mask: pixels whose 3x3 mean of \|A−B\| (luma) stays below this cross-fade instead of interpolating. `0.03` suits film-grain content. |
| `VKIFRAME_ONESIDED_FILL` | `0` | **Experimental, needs `VKIFRAME_EXPERIMENTAL=1`, read at game start.** `1`: behind a moving object, a pixel that the forward/backward check calls occluded is taken from the one frame that shows the background, along the best consistent vector of that frame's flow, instead of from the blend of both. Measured offline (five scenes from the Steam Deck, 1280x720, CPU reference flow, 25 triplets, the best of four variants): worse than the default in 21 of 25 triplets on hard pixels (mean +6.0 %, all pixels +3.0 %), better in 4 (-1.0 to -2.7 %); worse in 90 of 100 comparisons over all four variants on all pixels and 93 of 100 on hard pixels. Provided for testing, not as an improvement. Without `VKIFRAME_EXPERIMENTAL=1` it is ignored and the log says so. |
| `VKIFRAME_INTERP_BIDIR` | `1` (old name `VKIFRAME_WARP_BIDIR`) | `1`: two-sided interpolation (A forward, B backward); costs about 0.37 ms more per synthetic frame. |
| `VKIFRAME_FLOW_LEVEL` | `2` | Resolution of the flow field: 1 = half (most expensive), 2 = quarter (default), 3 = eighth. |
| `VKIFRAME_PHASE_LOCK` / `VKIFRAME_PHASE_OFFSET` | `1` / `0.5` | Keep the output ticks at fixed positions between native frames when the ratio is near an integer. With the lock on the content delay is not constant: at 30 native and 60 target it ranged from 33.6 to 51.7 ms (median 41.0 ms) in one log of an earlier build with `PHASE_OFFSET` 0. In an offline simulation of the scheduler with offset 0 it ranged from 33.6 to 50.2 ms with the lock on and from 33.6 to 34.5 ms with it off; at the default offset 0.5 it ranged from 37.8 to 47.3 ms with small jitter and from 34.5 to 51.2 ms with ±1 ms jitter. The lock trades a steady pass count for a varying delay. The effect on how it feels is not established. |
| `VKIFRAME_SYNTH_MARGIN` | `0.15` | Keep *t* at least this far from 0 and 1, so no native frame is shown as it is. Live: change it in live.conf or the Decky panel while the game runs (from the next pair); bench.json echoes the value. With 0 a tick can sit on the native frame (a sharper frame now and then); it skips the interpolation pass only when the phase lock holds and `VKIFRAME_PHASE_OFFSET` is 0, otherwise only about 4 % of ticks qualify (t < 0.02 after a native frame, or within 0.02 of the next one, which is then shown as the next pair's t = 0). |
| `VKIFRAME_INTERP_GUIDE`, `VKIFRAME_CONF_LO`, `VKIFRAME_CONF_HI` | `0.08`, `0.06`, `0.14` | Colour guidance of the vector weighting and the match-cost range where the interpolation fades to the cross-fade. |
| `VKIFRAME_PAN_LO`, `VKIFRAME_PAN_HI` | `0.15`, `0.35` | Share of unmatched flow where the whole frame starts / finishes cross-fading (fast pans). |
| `VKIFRAME_PRESENTER` | `1` | `0`: present the ticks from the game thread instead of the pacing thread. |
| `VKIFRAME_DUMP`, `VKIFRAME_DUMP_DIR` | off | `VKIFRAME_DUMP=N` (needs optical flow on) writes, for the N-th pair whose flow ran since the game started, `B.rgba16f`, the forward and backward flow, the mask and five consecutive native frames `pair0_A` to `pair4_A` (`B` equals `pair1_A`), and the synthetic frames of those pairs (`synthNN_t….rgba16f`, up to two per pair), so one start gives three triplets (A, middle frame, B) for `interp_eval`; A and B are then two native frames apart, twice the gap the layer sees in use, so offline errors are likely an overestimate. The game does not wait inside the window: host-visible buffers for the whole capture (the native frames, `B`, the flow, the mask and the ten synthetic frames) are allocated once when the swapchain is created, and each pair only records a GPU copy into them inside the command buffer it submits anyway (no extra submit, no fence wait, no CPU copy). After the window one wait covers all the copies and the files are written together from the mapped memory: that is one stall at the end (it blocks the game and the presenter), and the game must keep running until the log line `dump: capture complete, N of N files written in X ms` (the line `dump: window over at HH:MM:SS.mmm, writing N files` marks where the end-of-window stall starts) (`capture FAILED` if a file could not be written; if the game ends or the swapchain is recreated first, the queued images are dropped and the log says how many). `VKIFRAME_DUMP_FRAMES=M` (5 to 60, default 5) keeps M consecutive native frames `pair0_A` to `pair<M-1>_A` and writes `times.txt` (`pair<k>_A` and the steady-clock time in ns when its flow pass ran). The buffers are held from the game start, about 7.4 MB per frame at 1280x720: M=30 is about 300 to 330 MB with B, the flow, the mask and the synthetic frames, M=60 about 450 MB for the A frames alone, taken from the memory the GPU shares with the system. If that much cannot be allocated the layer tries half as many frames, down to 5, and logs each step; if even 5 do not fit the capture is off and the log says so. The copy itself costs GPU time inside the pair; how much is not measured. If the game recreates its swapchain the pool is allocated again and a second capture can run and overwrite the files. It is read at game start only (a launch option, not live). **Disk and memory:** a capture is 7.4 MB per native frame at 1280x720 (about 8.3 MB per frame at 1280x800), plus about 74 MB of synthetic frames, B, the flow and the mask: `VKIFRAME_DUMP_FRAMES=30` writes about 300 MB, `60` about 530 MB, and the same amount of memory is held from the game start; free the space afterwards. Use a folder under the home directory on Proton; its parent must exist (the layer creates only the last level). The dumps are screenshots of your game: the folder is created 0700 and the files 0600, so keep them private. |
| `VKIFRAME_DEBUG_FLOW`, `VKIFRAME_TICKMARK` | `0` | Debug views: motion field instead of frames; frame-position marks. |

More settings are documented next to their definitions in `include/vk_iframes_layer.hpp`: `VKIFRAME_FLOW_BWD_SKIP`, `VKIFRAME_FLOW_SUBPIXEL`, `VKIFRAME_INTERP_CAND`, `VKIFRAME_CAP_MODE` (`late` the default, `early`, or `hybrid`, a test setting for the base-rate cap's timing: `early` waits before the present and gives exact intervals but adds up to one slot of latency, `hybrid` is a short early hold plus a late wait; `VKIFRAME_CAP_EARLY=1` is the same as `early` when `VKIFRAME_CAP_MODE` is not set (`VKIFRAME_CAP_MODE` wins when both are set); read at game start), `VKIFRAME_STATS_FRAMES=1` (with `STATS` on, also writes every frame time of each second to `stats-frames.jsonl`, 0600, same folder and size cap; adds the standard deviation `ft_std_ms` and `content_delay_ms` to the stats rows either way), `VKIFRAME_COST_WARN`, `VKIFRAME_ACQUIRE_WAIT_US` (how long the presenter waits per try for a free swapchain image, default 1000), `VKIFRAME_PACE_LEAD_US` (how long before a tick the presenter calls present, default 2000), `VKIFRAME_PACE_SLEEP`, `VKIFRAME_ABSOLUTE` (absolute target times, untested), and the diagnostics `VKIFRAME_DEBUG_RING`, `VKIFRAME_TIMING_TEST` and `VKIFRAME_HOLD` / `VKIFRAME_SYNTH` (switch holding or synthetic frames off).

---

