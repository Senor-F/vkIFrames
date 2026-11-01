# vkIFrames

**Dynamic, rate-matching frame interpolation for Vulkan on Linux and SteamOS.**

vkIFrames is an independent Vulkan layer that sits between a game and the driver and generates synthetic in-between frames, so a game rendering at a low or unsteady frame rate is displayed at the **Target FPS** you choose (for example 90 Hz on a Steam Deck OLED).

Unlike fixed 2x/3x frame generation, the number of generated frames changes from moment to moment with the game's real frame rate. When the game already reaches the target, the layer steps aside and passes native frames straight through with no added latency.

> **Alpha 0.1.0-alpha.1. Expect rough edges.** vkIFrames has been tested by one developer on one Steam Deck OLED and one Radeon RX 9070 desktop, with a few Proton games. It can break a game. 45 → 90 fps ran locked in the best case (90.0 fps shown, no drops); image quality around thin or fast-moving structures is still being improved. See [STATUS.md](STATUS.md) for measurements and open items.

## If something goes wrong (read this first)

1. Remove `VKIFRAME_ENABLE=1` from the game's launch options. Without it the layer is not loaded.
2. Or set `VKIFRAME_DISABLE=1`.
3. Or run `scripts/uninstall.sh`. It removes the manifest (`~/.local/share/vulkan/implicit_layer.d/VkLayer_frame_gen.json`) and the installed library (`~/.local/share/vkiframes/`). Your settings and logs in `~/.config/vkiframes` stay unless you pass `--purge`.

Files the layer writes: `~/.config/vkiframes/live.log` (and `live.log.1`), `status.json`, and with `STATS` set or a benchmark requested (`bench.req`, below) also `stats.jsonl` (and `.1`) and `bench.json`. With `VKIFRAME_DUMP` it also writes the capture folder (`VKIFRAME_DUMP_DIR`, default `/tmp/vkIFrames_dump`; under Proton that is the game container's `/tmp`), and with `VKIFRAME_LOG_FILE` the log goes to the file you name. Nothing else, and it opens no network port.

## Install the alpha

- **Layer:** download `vkIFrames-0.1.0-alpha.1-linux-x86_64.tar.gz` and `SHA256SUMS` from the release, check with `sha256sum -c SHA256SUMS` and verify the signature (see "Check the download" below), unpack, and run `scripts/install.sh` (no root; it refuses to run as root and is safe to run twice).
- **Check the download.** `SHA256SUMS` is signed with an SSH key (`SHA256SUMS.sig` in the release). Put this line in a file `allowed_signers`: `vkiframes-release ssh-ed25519 AAAAC3NzaC1lZDI1NTE5AAAAIOMdQXwSmxKkWwjWSm+fZF+PneWMTOtBXw72g4W1jSTg`, then run `ssh-keygen -Y verify -f allowed_signers -I vkiframes-release -n vkiframes -s SHA256SUMS.sig < SHA256SUMS`. It must print `Good "vkiframes" signature`. Then `sha256sum -c SHA256SUMS` proves the archives are the signed ones. The key comes from the same GitHub account as the release, so this protects against a changed or replaced download, not against a compromised account; compare the fingerprint (`ssh-keygen -lf` on that line) with one you got elsewhere if that matters to you. The fingerprint is `SHA256:fhj+KWHGKFDU9rDeiGQ9aI/NeaXiW/rvCsJLI5YOT8o`.
- **Steam Deck panel (optional):** install `vkIFrames-decky-0.1.0-alpha.1.zip` with Decky Loader's "install from ZIP". The panel changes settings while a game runs. It is the only live control for now: there are no in-game keys or menu yet.
- **Turn it on per game:** Steam launch options `VKIFRAME_ENABLE=1 %command%`. The layer then runs with its default settings. The plugin never sets the launch option for you.
- **Uninstall:** `scripts/uninstall.sh` and the panel's "Uninstall layer…" button remove the same two files (the manifest and `~/.local/share/vkiframes/libvkIFrames.so`), so either one is enough, in any order. Removing the plugin in Decky Loader removes them too (as far as we know; not yet tested on a Deck). Neither the script nor the panel removes the other's part: `uninstall.sh` leaves the plugin installed, and removing the plugin leaves `~/.config/vkiframes`. Only `scripts/uninstall.sh --purge` (or deleting that folder) removes your settings and logs. The Decky zip has no `uninstall.sh`; if you removed the plugin first and something is left, use the one from the layer archive. Installing the plugin's layer and running `install.sh` both write the same two files, so the one run last wins.
- **Requirements:** Linux x86-64, 64-bit games only, a Vulkan 1.3 driver, glibc 2.34 or newer (SteamOS 3.x, Ubuntu 22.04+, Debian 12+).

## Known issues and limits (alpha)

- **Cost.** In one saved-game scene on a Steam Deck at 1280x720 the layer cost about 5.1 ms per game frame (one scene, one run of six off/on pairs). A GPU-bound game loses base frame rate: 55.6 fps native calculates to about 43 fps (from the frame times) while 90 fps are shown. Other games and scenes measured 0 to 6 ms so far (0 when the game has GPU headroom). `VKIFRAME_INTERPOLATE` is on by default; set it to 0 for the much cheaper cross-fade.
- **Picture quality.** Thin lines and tree trunks distort, and occlusions can ghost. In an offline check on 25 captured triplets the interpolation had the lower error than the plain cross-fade in 16; it can be slightly worse on near-static content. Not measured against any quality bar.
- **The Decky panel rewrites the whole `live.conf`** at every change and at loader start, so edits made by hand to that file are overwritten.
- **Input latency** is about one base frame while interpolating.
- **Frame pacing.** The layer forces FIFO presentation, which may not suit every variable-refresh setup. Present-timing support differs between gamescope and Mesa versions.
- **Crash on low memory.** An out-of-memory inside the layer ends the game (uncaught `bad_alloc`).
- **Debug capture.** `VKIFRAME_DUMP` writes frames of the game to a folder; it holds about 300 MB of memory at 30 frames and writes the files in one stall of about 0.3 s at the end. Off by default.
- **Not tested:** NVIDIA, Intel, native 32-bit games, native Linux Vulkan games, other distributions than SteamOS.

## Tested

| Where | What | Result |
|---|---|---|
| Steam Deck OLED, SteamOS 3.9.5, AMD (RADV) | Tomb Raider (2013), 18 to 60 fps native, 90 fps shown | works, see cost above |
| same Deck | a 2D side-scrolling shooter (45 fps cap, 90 fps target) | works |
| Radeon RX 9070 desktop (RADV), SteamOS | a test application only; no game run with the layer | works for the test application |

All runs started from Steam in Game Mode with Proton; Desktop Mode is not verified.

## Reporting a problem

Open an issue in this repository and attach `~/.config/vkiframes/live.log` (and `status.json`). They contain the process id, the local times you played and the setting values; in normal use they do not contain your user name, home folder, host name or the game's name. Say which game, which mode (Game Mode or Desktop Mode) and what you set. Do not post anything else personal.

**Security problems:** do not open a public issue. Use the private report on this repository's Security tab ("Report a vulnerability").

*New issues and pull requests are limited to the maintainer until 2026-11-08; the limit is lifted after that date.*

---

## How it works

For each native frame the game presents, the layer:

1. **Measures** the game's frame time at `vkQueuePresentKHR` and smooths it, so brief stutters do not make the output rate jump around.
2. **Runs a fixed output clock at the Target FPS.** Every tick presents exactly one frame, either a native frame or a synthetic frame at a fractional time *t* between two native frames, so the output hits the target even when it is not a multiple of the game's rate (40 → 60, 45 → 90). The number of synthetic frames per pair is capped (`VKIFRAME_MAX_SYNTHETIC`). At or above the target the layer bypasses.
3. **Holds one frame** (interpolation mode). To insert frames *between* two native frames, the layer delays the game's own presentation by one frame and presents it on the next one, followed by the synthetic frames. With a near-integer ratio a phase lock keeps the ticks at fixed positions between native frames.
4. **Estimates motion** between the two newest native frames with a hierarchical optical flow (4-level pyramid, forward and backward, sub-pixel refinement) on the GPU, plus a forward/backward check that marks occluded regions.
5. **Interpolates** the frames to each tick's time *t* with a colour-guided vector field. Where the match is poor, a region is occluded or motion is too fast, it fades toward a plain cross-fade; when most of the frame cannot be matched (a fast camera pan) the whole frame cross-fades.
6. **Optionally protects static UI** (`VKIFRAME_UI_MASK_THRESHOLD`): pixels that do not change between two frames cross-fade instead of being dragged along by nearby motion.
7. **Paces** every frame with its own presenter thread against the fixed output clock, and uses `VK_EXT_present_timing` when the driver and compositor offer it.

The math and module breakdown are in [DESIGN.md](DESIGN.md).

### Extrapolation mode (lower latency)

Interpolation needs the *next* native frame before it can show anything in between, so it adds about one native frame of delay (68 ms at 15 fps, about 24 ms at 45 fps). With `VKIFRAME_EXTRAPOLATE=1` nothing is held back: the newest native frame is shown on the next output slot and the slots until the following frame show that frame moved forward along its own backward flow. Added delay drops to under one output frame (about 11 ms at 90 Hz). The price is prediction errors: wrong guesses are visible when a real frame replaces them, and appearing or reversing motion cannot be predicted. Opt-in and experimental. Offline, on captured start / stop / reversal frames the prediction rarely overshoots (the fallbacks turn it back into repeating the newest frame), but then it also gains nothing; the gain is on steady motion (error 33–44 % lower). The log line `extrapolation: predicted frames not shown …` counts predicted frames that were cancelled before their slot and how late the native frame went out.

### Trade-offs to know about

- **Added latency.** Interpolation adds about one base frame time of input latency. It is inherent: use it only when the base rate is 30 fps or more, or try the extrapolation mode.
- **Cap your game.** The game should be limited to its intended base rate (in-game limiter, or `VKIFRAME_BASE_FPS_CAP` in Game Mode where Steam's limiter acts below the layer).
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
- The layer's log (rates, content delay, GPU times, settings changes) goes to `~/.config/vkiframes/live.log` by default; `VKIFRAME_LOG_FILE` writes it to another file instead. `live.log` and `stats.jsonl` are each capped at 8 MiB: past that the file becomes `.1` (one old file is kept, the previous one goes), checked when the layer starts and about once a second while it runs, so the two files and their old copies together stay under about 32 MiB. If `live.log` cannot be opened (no writable config folder) the layer's per-second lines are cut to one a minute, so the game's own stderr (and Steam's log) does not fill up. The Decky plugin caps its lines in the same file the same way. `VKIFRAME_LOG_FILE` (below) has no cap: use it for short runs only.
- With `VKIFRAME_INTERPOLATE=0` synthetic frames are plain cross-fades (no optical flow, much cheaper, more ghosting).
- The earlier `FRAMEGEN_` spelling of all settings still works; `VKIFRAME_` wins if both are set.

For a quick test of the base frame rate use `VKIFRAME_BASE_FPS_CAP`, an in-game limiter, or `DXVK_FRAME_RATE=30` on Proton.

During development the layer can also be loaded explicitly: `VK_LAYER_PATH=$PWD/build VK_LOADER_LAYERS_ENABLE=VK_LAYER_CUSTOM_frame_gen VKIFRAME_TARGET_FPS=60 vkcube`, or through `scripts/gamemode_launch.sh %command%`, which also writes `build/gamemode_<time>.log`.

### Reading the log

Once per second the layer logs lines such as:

```text
native 45.0 fps -> output 90.0 fps (planned) | shown 90.0 fps | interpolating | pairs 45 (synthetic 90) bypass 0 carrier-only 0 dropped 0 | content delay 25.4 ms
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
| `VKIFRAME_TARGET_FPS` | `60` (`90` on the Steam Deck OLED) | Output frame rate the layer aims for exactly. At or above it the layer bypasses. |
| `VKIFRAME_BASE_FPS_CAP` | `45` (`0` = off) | The layer limits the game's present rate itself (Game Mode, testing). The default 45 suits a 90 Hz screen (exactly x2); for another target pick about half of it, or a third for x3 (30 for 90 Hz), and `0` leaves the game uncapped, then a game that already runs near the target gets few or no generated frames. It waits after each present, so the game samples input as late as it can; `VKIFRAME_CAP_EARLY=1` waits before the present instead (exact intervals, but every frame is older when shown). |
| `VKIFRAME_MAX_SYNTHETIC` | `3` | Most synthetic frames per native frame (1–4; up to 10 with `VKIFRAME_EXPERIMENTAL=1`). |
| `VKIFRAME_CMD_BUFFERS` | `8` | Command buffer entries per swapchain (4..32). A GPU-bound game makes the game thread wait on the fence of the oldest entry (`wait_fence_ms` in the stats); more entries let the layer queue further ahead of the GPU before that happens, at the price of more latency in the layer's own work and a little memory. Live: change it in live.conf or the Decky panel while the game runs. |
| `VKIFRAME_EXPERIMENTAL` | `0` | `1` raises the `MAX_SYNTHETIC` limit from 4 to 10 (untested above 4). It adds no swapchain images by itself: the swapchain is built for `MAX_SYNTHETIC` as set at game start (live.conf over the environment), at least 4, so values above 4 use more images and memory only when you set them and restart the game. Read once when the game starts, from live.conf or the environment. The Decky plugin also uses it to unlock its "More settings". |
| `VKIFRAME_MIN_GAIN` | `1.01` | Interpolate only when the game's frame time exceeds the target frame time by this factor. |
| `VKIFRAME_INTERPOLATE` | `1` (old name `VKIFRAME_WARP`) | `1`: build synthetic frames by interpolating along the optical flow (implies `VKIFRAME_FLOW`; costs about 5 ms of GPU time per native frame at 720p on the Steam Deck). `0`: plain cross-fades, no optical flow. |
| `VKIFRAME_EXTRAPOLATE` | `0` | `1`: low-latency mode, no frame held back, slots after each native frame show predicted frames (implies `VKIFRAME_INTERPOLATE`). |
| `VKIFRAME_UI_MASK_THRESHOLD` | `0.08` (`0` = off) | Static-UI mask: pixels whose 3x3 mean of \|A−B\| (luma) stays below this cross-fade instead of interpolating. `0.03` suits film-grain content. |
| `VKIFRAME_ONESIDED_FILL` | `0` | **Experimental, needs `VKIFRAME_EXPERIMENTAL=1`, read at game start.** `1`: behind a moving object, a pixel that the forward/backward check calls occluded is taken from the one frame that shows the background, along the best consistent vector of that frame's flow, instead of from the blend of both. Measured offline (five scenes from the Steam Deck, 1280x720, CPU reference flow, 25 triplets, the best of four variants): worse than the default in 21 of 25 triplets on hard pixels (mean +6.0 %, all pixels +3.0 %), better in 4 (-1.0 to -2.7 %); worse in 90 of 100 comparisons over all four variants on all pixels and 93 of 100 on hard pixels. Provided for testing, not as an improvement. Without `VKIFRAME_EXPERIMENTAL=1` it is ignored and the log says so. |
| `VKIFRAME_INTERP_BIDIR` | `1` (old name `VKIFRAME_WARP_BIDIR`) | `1`: two-sided interpolation (A forward, B backward); costs about 0.37 ms more per synthetic frame. |
| `VKIFRAME_FLOW_LEVEL` | `2` | Resolution of the flow field: 1 = half (most expensive), 2 = quarter (default), 3 = eighth. |
| `VKIFRAME_PHASE_LOCK` / `VKIFRAME_PHASE_OFFSET` | `1` / `0.5` | Keep the output ticks at fixed positions between native frames when the ratio is near an integer. With the lock on the content delay is not constant: at 30 native and 60 target it ranged from 33.6 to 51.7 ms (median 41.0 ms) in one log of an earlier build with `PHASE_OFFSET` 0. In an offline simulation of the scheduler with offset 0 it ranged from 33.6 to 50.2 ms with the lock on and from 33.6 to 34.5 ms with it off; at the default offset 0.5 it ranged from 37.8 to 47.3 ms with small jitter and from 34.5 to 51.2 ms with ±1 ms jitter. The lock trades a steady pass count for a varying delay. The effect on how it feels is not established. |
| `VKIFRAME_SYNTH_MARGIN` | `0.15` | Keep *t* at least this far from 0 and 1, so no native frame is shown as it is. Live: change it in live.conf or the Decky panel while the game runs (from the next pair); bench.json echoes the value. With 0 a tick can sit on the native frame (a sharper frame now and then); it skips the interpolation pass only when the phase lock holds and `VKIFRAME_PHASE_OFFSET` is 0, otherwise only about 4 % of ticks qualify (t < 0.02 after a native frame, or within 0.02 of the next one, which is then shown as the next pair's t = 0). |
| `VKIFRAME_INTERP_GUIDE`, `VKIFRAME_CONF_LO`, `VKIFRAME_CONF_HI` | `0.08`, `0.06`, `0.14` | Colour guidance of the vector weighting and the match-cost range where the interpolation fades to the cross-fade. |
| `VKIFRAME_PAN_LO`, `VKIFRAME_PAN_HI` | `0.15`, `0.35` | Share of unmatched flow where the whole frame starts / finishes cross-fading (fast pans). |
| `VKIFRAME_PRESENTER` | `1` | `0`: present the ticks from the game thread instead of the pacing thread. |
| `VKIFRAME_DUMP`, `VKIFRAME_DUMP_DIR` | off | `VKIFRAME_DUMP=N` writes, for the N-th pair whose flow ran since the game started, `B.rgba16f`, the forward and backward flow, the mask and five consecutive native frames `pair0_A` to `pair4_A` (`B` equals `pair1_A`), and the synthetic frames of those pairs (`synthNN_t….rgba16f`, up to two per pair), so one start gives three triplets (A, middle frame, B) for `interp_eval`; A and B are then two native frames apart, twice the gap the layer sees in use, so offline errors are likely an overestimate. The game does not wait inside the window: host-visible buffers for the whole capture (the native frames, `B`, the flow, the mask and the ten synthetic frames) are allocated once when the swapchain is created, and each pair only records a GPU copy into them inside the command buffer it submits anyway (no extra submit, no fence wait, no CPU copy). After the window one wait covers all the copies and the files are written together from the mapped memory: that is one stall at the end (it blocks the game and the presenter), and the game must keep running until the log line `dump: capture complete, N of N files written in X ms` (the line `dump: window over at HH:MM:SS.mmm, writing N files` marks where the end-of-window stall starts) (`capture FAILED` if a file could not be written; if the game ends or the swapchain is recreated first, the queued images are dropped and the log says how many). `VKIFRAME_DUMP_FRAMES=M` (5 to 60, default 5) keeps M consecutive native frames `pair0_A` to `pair<M-1>_A` and writes `times.txt` (`pair<k>_A` and the steady-clock time in ns when its flow pass ran). The buffers are held from the game start, about 7.4 MB per frame at 1280x720: M=30 is about 300 to 330 MB with B, the flow, the mask and the synthetic frames, M=60 about 450 MB for the A frames alone, taken from the memory the GPU shares with the system. If that much cannot be allocated the layer tries half as many frames, down to 5, and logs each step; if even 5 do not fit the capture is off and the log says so. The copy itself costs GPU time inside the pair; how much is not measured. If the game recreates its swapchain the pool is allocated again and a second capture can run and overwrite the files. It is read at game start only (a launch option, not live). **Disk and memory:** a capture is 7.4 MB per native frame at 1280x720 (about 8.3 MB per frame at 1280x800), plus about 74 MB of synthetic frames, B, the flow and the mask: `VKIFRAME_DUMP_FRAMES=30` writes about 300 MB, `60` about 530 MB, and the same amount of memory is held from the game start; free the space afterwards. Use a folder under the home directory on Proton; its parent must exist (the layer creates only the last level). The dumps are screenshots of your game: the folder is created 0700 and the files 0600, so keep them private. |
| `VKIFRAME_DEBUG_FLOW`, `VKIFRAME_TICKMARK` | `0` | Debug views: motion field instead of frames; frame-position marks. |

More settings are documented next to their definitions in `include/vk_iframes_layer.hpp`: `VKIFRAME_FLOW_BWD_SKIP`, `VKIFRAME_FLOW_SUBPIXEL`, `VKIFRAME_INTERP_CAND`, `VKIFRAME_CAP_EARLY`, `VKIFRAME_COST_WARN`, `VKIFRAME_ACQUIRE_WAIT_US` (how long the presenter waits per try for a free swapchain image, default 1000), `VKIFRAME_PACE_LEAD_US` (how long before a tick the presenter calls present, default 2000), `VKIFRAME_PACE_SLEEP`, `VKIFRAME_ABSOLUTE` (absolute target times, untested), and the diagnostics `VKIFRAME_DEBUG_RING`, `VKIFRAME_TIMING_TEST` and `VKIFRAME_HOLD` / `VKIFRAME_SYNTH` (switch holding or synthetic frames off).

---

## Live settings (Decky plugin)

The layer re-reads `live.conf` (`$XDG_CONFIG_HOME/vkiframes/live.conf`, by default `~/.config/vkiframes/live.conf`; `VKIFRAME_LIVE_CONF` and `VKIFRAME_LIVE_LOG` change the paths) about once a second (one `stat()` on the present path), so settings can change while a game runs. The Decky plugin in `decky/` writes that file; it can also be edited by hand (`KEY=VALUE` per line, `#` comments, `VKIFRAME_` or `FRAMEGEN_` prefix optional). The environment gives the startup values, a key in the file overrides them, and removing the file goes back to the environment.

| Key | Range | When it applies |
|---|---|---|
| `ENABLED` | 0 / 1 (default 1) | next present (0 = plain bypass: no interpolation, no base-fps cap) |
| `TARGET_FPS` | 30..1000 | next present (the output clock re-anchors when the target changes while running) |
| `MAX_SYNTHETIC` | 1..4 (up to 10 with `EXPERIMENTAL`) | next present, up to what the running game's swapchain was built for (`synth_capacity` in `status.json`); more needs a game restart |
| `BASE_FPS_CAP` | 0..90 | next present |
| `MIN_GAIN` | 1.01..1.5 | next present |
| `PHASE_LOCK`, `PHASE_OFFSET` | 0 / 1, 0..0.999 | next present |
| `INTERP_BIDIR` | 0 / 1 | next pair |
| `CMD_BUFFERS` | 4..32 | next present (entries are used from then on) |
| `SYNTH_MARGIN` | 0..0.45 | next pair |
| `TARGET_FPS` limit | | *(unit-tested only; not yet verified on the Deck, which reports `refresh_hz` 0)* the live target is used up to the display's refresh rate when that is fixed; a variable-refresh display (refreshInterval = UINT64_MAX in `VK_EXT_present_timing`) or an unknown rate is not limited. The log says so once (`display: refresh ...`, `TARGET_FPS ... is above the display's refresh rate`). |
| `STATS` | 0 / 1 | next second: one measurement record a second in `stats.jsonl` (see Measuring) |
| `bench.req` (a separate file, not a `live.conf` key) | a number | next second: a number that is new (not in the file when the game started) asks for one benchmark run; `0` stops a running one. It is not a setting: writing it changes no live value. A `BENCH=` line in `live.conf` (an older plugin or script) is ignored and logged once |
| `EXTRAPOLATE`, `UI_MASK_THRESHOLD`, `FLOW_LEVEL`, `INTERPOLATE`, `EXPERIMENTAL`, `ONESIDED_FILL` | as in the table above | read once when the game starts |

Bad values are clamped or ignored, a malformed line never stops the layer. Every change is written, with the time, the old and the new value, to `~/.config/vkiframes/live.log`, the same file as the layer's own log (the folder is 0700 and the files 0600; without `HOME` or `XDG_CONFIG_HOME` the layer has no live settings and writes no files) (one log to read). `status.json` next to it is rewritten once a second with the native and shown fps, the number of generated frames per native frame the mode and the swapchain's refresh rate (`refresh_hz`, 0 when unknown). The Decky plugin uses it to set the top of its Target FPS slider to the higher of 120 and that rate; `TARGET_FPS` in live.conf accepts 30..1000. `VKIFRAME_LIVE_CONF` and `VKIFRAME_LIVE_LOG` change the paths.

## Cost warning

Once per swapchain the layer logs `WARNING: the layer's GPU work is N % of the game's frame time ...` when its own GPU time per second (flow time x pairs per second + interpolation time x generated frames per second) is over `VKIFRAME_COST_WARN` (default `1.0`, that is 100 %: the layer alone would fill the GPU) for three seconds in a row. The ratio is the same for any frame rate, because the layer's work per game frame is compared with the game's frame time. Lower `MAX_SYNTHETIC` or `FLOW_LEVEL`, or the target, when it appears.

## Measuring (stats and benchmark)

`VKIFRAME_STATS=1` (or `STATS=1` in live.conf) writes one JSON line a second to `~/.config/vkiframes/stats.jsonl` (0600, in the 0700 config folder, one old file kept past 8 MB): game frame time (mean, 95th and 99th percentile, max, from present-to-present intervals), CPU time of the layer per frame, the game thread's waits per frame, GPU time of the flow (per pair) and of the interpolation (per generated frame) with the derived GPU share, pacing (lateness, cancelled ticks, dropped frames), the refresh rate and the rates. Percentiles are nearest-rank: sort the second's intervals, take element ceil(p x n). Off by default; when off, no `stats.jsonl` record is written and no per-present interval is sampled. `STATS` gates only those two: the per-second line in the log (`live.log`/the layer log) and `status.json` are written either way, and the GPU timestamps around the flow and interpolation passes are always on, because the cost warning below uses them (two atomic adds per pair for the sums, plus one atomic load per present to ask whether stats are on). No game name, process name, path, host name or IP is recorded.

A benchmark run alternates the layer off and on in the running game: 30 s warm-up (layer on), then 6 pairs of 60 s segments (off then on; every second pair on then off, so a steady drift cancels), about 13 minutes. Run it with `scripts/vkiframes-bench.sh` (it writes the request number to `~/.config/vkiframes/bench.req`, one line, by temporary file and rename, and prints `bench.json` when done; `--no-wait` returns at once, `--cancel` writes `0`) or the Decky plugin's Benchmark button. The first 2 s after every switch are labelled `settle` and left out. The base-fps cap stays in force during the "off" seconds so both states pace the game the same way. The run stops, and puts ENABLED back, if a setting changes, if `bench.req` is set to `0`, or if the game closes its swapchain. Writes that change no value (the plugin rewriting `live.conf` at a loader restart, a new `REV`) and a new number in `bench.req` do not stop it; `bench.json` then says `aborted`. `bench.json` holds, for each state, the frame-time mean (frame-weighted), the mean of the per-second 95th and 99th percentiles, the CPU and GPU numbers, and the derived layer cost on the game (frame time on minus off), the GPU share of the layer, how much of it is the flow, `layer_cost_pairs_ms` (the cost of each off/on pair), `layer_cost_pairs_mean_ms`, `layer_cost_se_ms` (standard error of that mean) and `layer_cost_within_noise` (true when the mean is within the 95 % Student t interval of zero for that many pairs: the run cannot tell the cost from drift), and `async_gain_upper_bound_ms` (equal to the layer cost; NOT an achievable gain: in a GPU-bound game the cost is the layer's own GPU time, which a second queue does not remove). The settings block of `bench.json` also records `cmd_buffers` and `flow_level`, so a file says which settings it was run with. `gen_per_native` in a record is k, the frames shown beyond the native one (2.0 for 30 → 90); the interpolation pass draws every shown frame (by default all of them are synthetic: `VKIFRAME_SYNTH_MARGIN` 0.15 keeps t away from 0 and 1; with `VKIFRAME_SYNTH_MARGIN=0` a tick at t < 0.02 is the native frame and costs no pass, which needs the phase lock to hold and `VKIFRAME_PHASE_OFFSET=0`), so `generated_per_s` is about the shown fps and `interp_ms` is per pass. A 60 s segment has about 59 one-second records (windows are a little over a second): 2 settle, about 57 counted, and one at the switch that belongs to neither. Each number can be recomputed from the per-second records in `stats.jsonl` (they carry a `bench` label).

## Evaluating changes offline

`tools/interp_eval` scores interpolation and extrapolation methods against a real middle frame, without a GPU, on frames captured with `VKIFRAME_DUMP`:

```bash
g++ -O2 -std=c++17 -Iinclude tools/interp_eval/interp_eval.cpp -o interp_eval
./interp_eval A.rgba16f TRUTH.rgba16f B.rgba16f 1280 720 [out_prefix] [field_level]
```

It prints mean absolute error and PSNR for the cross-fade, the interpolation variants, the pan fallback, the UI mask (`HUD=x0,y0,x1,y1` adds a HUD-region column) and extrapolation (`EXTRAP_S=<intervals ahead>`). The C++ flow reference in `include/flow_ref.hpp` is tested against known motion and mirrors the GPU shaders.

---

## Project layout

```text
vkIFrames/
├── CMakeLists.txt
├── README.md, DESIGN.md, ROADMAP.md, STATUS.md, TARGETS.md   # overview, architecture, plan, measurements, target machines
├── STAGE1.md … STAGE6.md        # per-stage implementation notes
├── ASYNC.md                     # plan and measurements for the asynchronous layer
├── AUDIT.md                     # review log (development record)
├── CLAUDE.md                    # working rules of the development sessions (development record)
├── LICENSE, NOTICE
├── manifest/VkLayer_frame_gen.json.in
├── include/
│   ├── vk_iframes_layer.hpp     # config (VKIFRAME_* settings), dispatch tables, shared declarations
│   ├── output_clock.hpp         # gate, fixed output clock, per-pair plan (interpolation and extrapolation)
│   ├── timing_engine.hpp        # frame-time filter
│   ├── present_timing.hpp       # VK_EXT_present_timing helpers
│   ├── gpu_frames.hpp           # ring buffer, hold, flow-engine state
│   ├── gpu_internal.hpp         # helpers shared by gpu_frames.cpp and optical_flow.cpp
│   ├── dump_pool.hpp, dump_queue.hpp  # debug capture buffers and the write queue
│   ├── live_config.hpp          # settings that change while a game runs (live.conf)
│   ├── stats.hpp                # measurement records and the benchmark
│   └── flow_ref.hpp             # CPU reference of the flow and the interpolation
├── src/
│   ├── layer_main.cpp           # hooked entry points, presenter thread
│   ├── layer_dispatch.cpp       # registries, proc-addr tables, loader negotiation
│   ├── gpu_frames.cpp           # capture, synthesize, present of synthetic frames
│   ├── gpu_images.cpp           # images and buffers of the layer
│   ├── dump_pool.cpp, dump_queue.cpp  # debug capture (VKIFRAME_DUMP)
│   ├── optical_flow.cpp         # flow passes and GPU timers
│   ├── present_timing.cpp
│   ├── live_config.cpp          # live.conf, live.log, status.json
│   └── stats.cpp                # stats.jsonl, bench.json
├── shaders/                     # flow_luma/down/coarse/refine/occlusion, flow_interp (interpolation, pan fallback, UI mask, extrapolation), flow_vis, blend
├── scripts/                     # install.sh, uninstall.sh, gamemode_launch.sh, check_env.sh, vkiframes-bench.sh, package.sh (release archives), check_so.sh, repro_check.sh, layer.map
├── decky/                       # Decky Loader plugin (live settings from Game Mode), its scripts and tests, see decky/README.md
├── tests/                       # mock-loader and mock-driver tests, scheduler replay, flow reference
└── tools/                       # clock_sim (output-clock simulator), interp_eval (offline scoring)
```

## Target machines

Developed for a **Steam Deck OLED** (base machine, 90 Hz, Game Mode) and a SteamOS desktop (Ryzen 5 5600X, Radeon RX 9070). SteamOS specifics (Mesa and gamescope support for `VK_EXT_present_timing`, building against the Steam Linux Runtime, the game-frame-limit problem in Game Mode, Deck performance budget) are in [TARGETS.md](TARGETS.md).

## Roadmap

Stages 1–5 (layer, present timing, hold and ring buffer, optical flow, interpolation) are implemented; Stage 6 is partly done (install, packaging, settings), while swapchain resizing and the compositor test matrix are untested. Known open items: smudging around trees and thin lines, slow phase-lock recovery after stalls, hiding `VK_EXT_present_timing` from games that use it themselves. Planned next: lower GPU load, an in-game menu with hotkeys, and a study of the interpolation shader. See [ROADMAP.md](ROADMAP.md) and [STATUS.md](STATUS.md).

---

## Support

vkIFrames is free to use. If you like the work, you can say thanks on [Ko-fi](https://ko-fi.com/senorfm). Tips do not buy features or priority support.

## License

vkIFrames is licensed under the [Apache License, Version 2.0](LICENSE). The copyright holder is "vkIFrames contributors" (see [NOTICE](NOTICE)). The Decky plugin in `decky/` is based on the Decky plugin template and stays under its BSD 3-Clause license ([decky/LICENSE](decky/LICENSE)); both licenses allow use, changes and redistribution, with notices kept.
