# vkIFrames

**Dynamic, rate-matching frame interpolation for Vulkan on Linux and SteamOS.**

vkIFrames is an independent Vulkan layer that sits between a game and the driver and generates synthetic in-between frames, so a game rendering at a low or unsteady frame rate is displayed at the **Target FPS** you choose (for example 90 Hz on a Steam Deck OLED).

Unlike fixed 2x/3x frame generation, the number of generated frames changes from moment to moment with the game's real frame rate. When the game already reaches the target, the layer steps aside and passes native frames straight through with no added latency.

**Prior art and credits.** vkIFrames is an independent implementation: the pacing engine, the optical flow and the interpolation are written for this project. Its fallback behaviour, showing a single real frame instead of a cross-fade where an interpolation cannot be trusted, is inspired by [afmf-linux](https://github.com/serialexperimentslainnnn/afmf-linux) (MIT licence), which does the same in its interpolation shader.

> **Status: working, experimental.** Tested on a Steam Deck OLED (Game Mode, Proton) and on a Radeon RX 9070 desktop. 45 → 90 fps runs locked (90.0 fps shown, no drops). Motion-based interpolation and an opt-in extrapolation mode exist; image quality around thin or fast-moving structures is still being improved. See [STATUS.md](STATUS.md) for measurements and open items.

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
- **Forced FIFO.** Presentation timing needs the `FIFO` present mode.
- **Artifacts.** Flow estimated from final images (no engine motion vectors) shows artifacts around fast motion, thin geometry such as tree trunks and lines, transparency and disocclusions. The fallbacks reduce them but cannot remove them.
- **GPU cost.** On the Deck at 720p: flow about 5 ms per pair and interpolation about 0.8 ms per synthetic frame. Percentage GPU readings are unreliable on the Deck; the per-stage times in the log are the reference.

---

## Quick start

### 1. Build

Build inside a container with a compiler (on SteamOS: a Distrobox). The repository's Vulkan headers must be recent enough to include `VK_EXT_present_timing`.

```bash
git clone https://github.com/Senor-F/vkIFrames.git && cd vkIFrames
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j"$(nproc)"
ctest --test-dir build          # mock-driver tests, no GPU needed
```

This produces `build/libvkIFrames.so` and `build/VkLayer_frame_gen.json`. Requirements: Linux x86_64 (64-bit games), a Vulkan 1.3 driver, CMake ≥ 3.21, a C++20 compiler, Vulkan-Headers and `glslangValidator` (compiles the shaders). A GPU with a separate compute queue is not required.

### 2. Install on the machine that runs the game (no root, no launch script)

Copy `build/libvkIFrames.so` and the `scripts/` folder to the target machine (for a Steam Deck only these two are needed), then:

```bash
chmod +x scripts/*.sh
scripts/install.sh          # writes ~/.local/share/vulkan/implicit_layer.d/VkLayer_frame_gen.json
scripts/uninstall.sh        # removes it again
```

### 3. Turn it on per game

In Steam, set the game's launch options, for example for a Deck OLED at 90 Hz with the game capped to 45:

```text
VKIFRAME_ENABLE=1 VKIFRAME_TARGET_FPS=90 VKIFRAME_BASE_FPS_CAP=45 VKIFRAME_INTERPOLATE=1 %command%
```

- `VKIFRAME_ENABLE=1` loads the layer, `VKIFRAME_DISABLE=1` forces it off.
- The layer's log (rates, content delay, GPU times, settings changes) goes to `~/.config/vkiframes/live.log` by default; `VKIFRAME_LOG_FILE` writes it to another file instead. The file is rotated to `live.log.1` above 8 MB.
- Without `VKIFRAME_INTERPOLATE=1` synthetic frames are plain cross-fades.
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
| `VKIFRAME_LOG_FILE` | unset (`~/.config/vkiframes/live.log`) | Send the layer's log to this file instead of live.log. Setting it also redirects the game's own stderr there; without it, only the layer's own lines are written (to live.log) and the game's stderr is untouched. |
| `VKIFRAME_TARGET_FPS` | `60` (`90` on the Steam Deck OLED) | Output frame rate the layer aims for exactly. At or above it the layer bypasses. |
| `VKIFRAME_BASE_FPS_CAP` | `45` (`0` = off) | The layer limits the game's present rate itself (Game Mode, testing). The default 45 suits a 90 Hz screen (exactly x2); for another target pick about half of it, or a third for x3 (30 for 90 Hz), and `0` leaves the game uncapped, then a game that already runs near the target gets few or no generated frames. It waits after each present, so the game samples input as late as it can; `VKIFRAME_CAP_EARLY=1` waits before the present instead (exact intervals, but every frame is older when shown). |
| `VKIFRAME_MAX_SYNTHETIC` | `3` | Most synthetic frames per native frame (1–4; up to 10 with `VKIFRAME_EXPERIMENTAL=1`). |
| `VKIFRAME_EXPERIMENTAL` | `0` | `1` raises the `MAX_SYNTHETIC` limit from 4 to 10 and sizes the swapchain for it (more images, more memory, untested above 4). Read once when the game starts, from live.conf or the environment. The Decky plugin also uses it to unlock its "More settings". |
| `VKIFRAME_MIN_GAIN` | `1.01` | Interpolate only when the game's frame time exceeds the target frame time by this factor. |
| `VKIFRAME_INTERPOLATE` | `0` (old name `VKIFRAME_WARP`) | `1`: build synthetic frames by interpolating along the optical flow (implies `VKIFRAME_FLOW`). |
| `VKIFRAME_EXTRAPOLATE` | `0` | `1`: low-latency mode, no frame held back, slots after each native frame show predicted frames (implies `VKIFRAME_INTERPOLATE`). |
| `VKIFRAME_UI_MASK_THRESHOLD` | `0.08` (`0` = off) | Static-UI mask: pixels whose 3x3 mean of \|A−B\| (luma) stays below this cross-fade instead of interpolating. `0.03` suits film-grain content. |
| `VKIFRAME_INTERP_BIDIR` | `1` (old name `VKIFRAME_WARP_BIDIR`) | `1`: two-sided interpolation (A forward, B backward); costs about 0.37 ms more per synthetic frame. |
| `VKIFRAME_FLOW_LEVEL` | `2` | Resolution of the flow field: 1 = half (most expensive), 2 = quarter (default), 3 = eighth. |
| `VKIFRAME_PHASE_LOCK` / `VKIFRAME_PHASE_OFFSET` | `1` / `0.5` | Keep the output ticks at fixed positions between native frames when the ratio is near an integer. |
| `VKIFRAME_SYNTH_MARGIN` | `0.15` | Keep *t* at least this far from 0 and 1, so no native frame is shown as it is. |
| `VKIFRAME_INTERP_GUIDE`, `VKIFRAME_CONF_LO`, `VKIFRAME_CONF_HI` | `0.08`, `0.06`, `0.14` | Colour guidance of the vector weighting and the match-cost range where the interpolation fades to the cross-fade. |
| `VKIFRAME_PAN_LO`, `VKIFRAME_PAN_HI` | `0.15`, `0.35` | Share of unmatched flow where the whole frame starts / finishes cross-fading (fast pans). |
| `VKIFRAME_PRESENTER` | `1` | `0`: present the ticks from the game thread instead of the pacing thread. |
| `VKIFRAME_DUMP`, `VKIFRAME_DUMP_DIR` | off | Write frames, flow and mask of one pair to a folder (debugging; use a folder under the home directory on Proton). The dumps are screenshots of your game: the folder is created 0700 and the files 0600, so keep them private. |
| `VKIFRAME_DEBUG_FLOW`, `VKIFRAME_TICKMARK` | `0` | Debug views: motion field instead of frames; frame-position marks. |

More settings (`VKIFRAME_FLOW_BWD_SKIP`, `VKIFRAME_FLOW_SUBPIXEL`, `VKIFRAME_INTERP_CAND`, `VKIFRAME_PACE_*`, …) are documented next to their definitions in `include/vk_iframes_layer.hpp`.

---

## Live settings (Decky plugin)

The layer re-reads `~/.config/vkiframes/live.conf` about once a second (one `stat()` on the present path), so settings can change while a game runs. The Decky plugin on the `decky-plugin` branch writes that file; it can also be edited by hand (`KEY=VALUE` per line, `#` comments, `VKIFRAME_` or `FRAMEGEN_` prefix optional). The environment gives the startup values, a key in the file overrides them, and removing the file goes back to the environment.

| Key | Range | When it applies |
|---|---|---|
| `ENABLED` | 0 / 1 | next present (0 = plain bypass: no interpolation, no base-fps cap) |
| `TARGET_FPS` | 30..144 | next present |
| `MAX_SYNTHETIC` | 1..4 | next present |
| `BASE_FPS_CAP` | 0..90 | next present |
| `MIN_GAIN` | 1.01..1.5 | next present |
| `PHASE_LOCK`, `PHASE_OFFSET` | 0 / 1, 0..0.999 | next present |
| `INTERP_BIDIR` | 0 / 1 | next pair |
| `TARGET_FPS` limit | | the live target is used up to the display's refresh rate when that is fixed; a variable-refresh display (refreshInterval = UINT64_MAX in `VK_EXT_present_timing`) or an unknown rate is not limited. The log says so once (`display: refresh ...`, `TARGET_FPS ... is above the display's refresh rate`). |
| `STATS` | 0 / 1 | next second: one measurement record a second in `stats.jsonl` (see Measuring) |
| `BENCH` | a number | a number that is new (not in the file when the game started) asks for one benchmark run |
| `EXTRAPOLATE`, `UI_MASK_THRESHOLD`, `FLOW_LEVEL`, `INTERPOLATE`, `EXPERIMENTAL` | as in the table above | read once when the game starts |

Bad values are clamped or ignored, a malformed line never stops the layer. Every change is written, with the time, the old and the new value, to `~/.config/vkiframes/live.log`, the same file as the layer's own log (the folder is 0700 and the files 0600; without `HOME` or `XDG_CONFIG_HOME` the layer has no live settings and writes no files) (one log to read). `status.json` next to it is rewritten once a second with the native and shown fps, the number of generated frames per native frame the mode and the swapchain's refresh rate (`refresh_hz`, 0 when unknown). The Decky plugin uses it to set the top of its Target FPS slider to the higher of 120 and that rate; `TARGET_FPS` in live.conf accepts 30..1000. `VKIFRAME_LIVE_CONF` and `VKIFRAME_LIVE_LOG` change the paths.

## Cost warning

Once per swapchain the layer logs `WARNING: the layer's GPU work is N % of the game's frame time ...` when its own GPU time per second (flow time x pairs per second + interpolation time x generated frames per second) is over `VKIFRAME_COST_WARN` (default `1.0`, that is 100 %: the layer alone would fill the GPU) for three seconds in a row. The ratio is the same for any frame rate, because the layer's work per game frame is compared with the game's frame time. Lower `MAX_SYNTHETIC` or `FLOW_LEVEL`, or the target, when it appears.

## Measuring (stats and benchmark)

`VKIFRAME_STATS=1` (or `STATS=1` in live.conf) writes one JSON line a second to `~/.config/vkiframes/stats.jsonl` (0600, in the 0700 config folder, one old file kept past 8 MB): game frame time (mean, 95th and 99th percentile, max, from present-to-present intervals), CPU time of the layer per frame, the game thread's waits per frame, GPU time of the flow (per pair) and of the interpolation (per generated frame) with the derived GPU share, pacing (lateness, cancelled ticks, dropped frames), the refresh rate and the rates. Percentiles are nearest-rank: sort the second's intervals, take element ceil(p x n). Off by default; when off, nothing is written and nothing new is timed (one atomic load per present; the GPU time sums that feed the cost warning below are two atomic adds per pair). No game name, process name, path, host name or IP is recorded.

A benchmark run alternates the layer off and on in the running game: 30 s warm-up (layer on), then 3 x (60 s off, 60 s on), about 7 minutes. Run it with `scripts/vkiframes-bench.sh` (it writes `BENCH=<number>` to live.conf and prints `bench.json` when done) or the Decky plugin's Benchmark button. The first 2 s after every switch are labelled `settle` and left out. The base-fps cap stays in force during the "off" seconds so both states pace the game the same way. The run stops, and puts ENABLED back, if a setting changes or the game closes its swapchain; `bench.json` then says `aborted`. `bench.json` holds, for each state, the frame-time mean (frame-weighted), the mean of the per-second 95th and 99th percentiles, the CPU and GPU numbers, and the derived layer cost on the game (frame time on minus off), the GPU share of the layer, how much of it is the flow, and `async_gain_upper_bound_ms` (the most a second queue could hide, equal to the layer cost). `gen_per_native` in a record is k, the frames shown beyond the native one (2.0 for 30 → 90); the interpolation pass draws every shown frame, the held native frame included, so `generated_per_s` is about the shown fps and `interp_ms` is per pass. A 60 s segment has about 59 one-second records (windows are a little over a second): 2 settle, about 57 counted, and one at the switch that belongs to neither. Each number can be recomputed from the per-second records in `stats.jsonl` (they carry a `bench` label).

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
├── AUDIT.md                     # Auditor findings
├── manifest/VkLayer_frame_gen.json.in
├── include/
│   ├── vk_iframes_layer.hpp     # config (VKIFRAME_* settings), dispatch tables, shared declarations
│   ├── output_clock.hpp         # gate, fixed output clock, per-pair plan (interpolation and extrapolation)
│   ├── timing_engine.hpp        # frame-time filter
│   ├── present_timing.hpp       # VK_EXT_present_timing helpers
│   ├── gpu_frames.hpp           # ring buffer, hold, flow-engine state
│   └── flow_ref.hpp             # CPU reference of the flow and the interpolation
├── src/
│   ├── layer_main.cpp           # hooked entry points, presenter thread
│   ├── layer_dispatch.cpp       # registries, proc-addr tables, loader negotiation
│   ├── gpu_frames.cpp           # capture, synthesize, present of synthetic frames
│   ├── optical_flow.cpp         # flow passes and GPU timers
│   └── present_timing.cpp
├── shaders/                     # flow_luma/down/coarse/refine/occlusion, flow_interp (interpolation, pan fallback, UI mask, extrapolation), flow_vis, blend
├── scripts/                     # install.sh, uninstall.sh, gamemode_launch.sh, check_env.sh
├── decky/                       # Decky Loader plugin (live settings from Game Mode), see decky/README.md
├── tests/                       # mock-loader and mock-driver tests, scheduler replay, flow reference
└── tools/                       # clock_sim (output-clock simulator), interp_eval (offline scoring)
```

## Target machines

Developed for a **Steam Deck OLED** (base machine, 90 Hz, Game Mode) and a SteamOS desktop (Ryzen 5 5600X, Radeon RX 9070). SteamOS specifics (Mesa and gamescope support for `VK_EXT_present_timing`, building against the Steam Linux Runtime, the game-frame-limit problem in Game Mode, Deck performance budget) are in [TARGETS.md](TARGETS.md).

## Roadmap

Stages 1–5 (layer, present timing, hold and ring buffer, optical flow, interpolation) are implemented; Stage 6 (integration, packaging, resizing) is in progress. Known open items: smudging around trees and thin lines, slow phase-lock recovery after stalls, hiding `VK_EXT_present_timing` from games that use it themselves. See [ROADMAP.md](ROADMAP.md) and [STATUS.md](STATUS.md).

---

## Support

vkIFrames is free to use. If you like the work, you can say thanks on [Ko-fi](https://ko-fi.com/senorfm). Tips do not buy features or priority support.

## License

vkIFrames is licensed under the [Apache License, Version 2.0](LICENSE). The copyright holder is "vkIFrames contributors" (see [NOTICE](NOTICE)). The Decky plugin in `decky/` is based on the Decky plugin template and stays under its BSD 3-Clause license ([decky/LICENSE](decky/LICENSE)); both licenses allow use, changes and redistribution, with notices kept.
