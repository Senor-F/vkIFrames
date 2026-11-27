[index](../README.md) | [previous](02-how-it-works.md)

## Live settings (Decky plugin)

The layer re-reads `live.conf` (`$XDG_CONFIG_HOME/vkiframes/live.conf`, by default `~/.config/vkiframes/live.conf`; `VKIFRAME_LIVE_CONF` and `VKIFRAME_LIVE_LOG` change the paths) about once a second (one `stat()` on the present path), so settings can change while a game runs. The Decky plugin in `decky/` writes that file; it can also be edited by hand (`KEY=VALUE` per line, `#` comments, `VKIFRAME_` or `FRAMEGEN_` prefix optional). The environment gives the startup values, a key in the file overrides them, and removing the file goes back to the environment.

| Key | Range | When it applies |
|---|---|---|
| `ENABLED` | 0 / 1 (default 1) | next present (0 = plain bypass: no interpolation, no base-fps cap) |
| `TARGET_FPS` | 30..600 | next present (the output clock re-anchors when the target changes while running) |
| `MAX_SYNTHETIC` | 1..4 (up to 10 with `EXPERIMENTAL`) | next present, up to what the running game's swapchain was built for (`synth_capacity` in `status.json`); more needs a game restart |
| `BASE_FPS_CAP` | 0..240 | next present |
| `MIN_GAIN` | 1.01..1.5 | next present |
| `PHASE_LOCK`, `PHASE_OFFSET` | 0 / 1, 0..0.999 | next present |
| `INTERP_BIDIR` | 0 / 1 | next pair |
| `CMD_BUFFERS` | 4..32 | next present (entries are used from then on) |
| `SYNTH_MARGIN` | 0..0.45 | next pair |
| `TARGET_FPS` limit | | *(unit-tested only; not yet verified on the Deck, which reports `refresh_hz` 0)* the live target is used up to the display's refresh rate when that is fixed; a variable-refresh display (refreshInterval = UINT64_MAX in `VK_EXT_present_timing`) or an unknown rate is not limited. The log says so once (`display: refresh ...`, `TARGET_FPS ... is above the display's refresh rate`). |
| `STATS` | 0 / 1 | next second: one measurement record a second in `stats.jsonl` (see Measuring) |
| `bench.req` (a separate file, not a `live.conf` key) | a number | next second: a number that is new (not in the file when the game started) asks for one benchmark run; `0` stops a running one. It is not a setting: writing it changes no live value. A `BENCH=` line in `live.conf` (an older plugin or script) is ignored and logged once |
| `EXTRAPOLATE`, `UI_MASK_THRESHOLD`, `FLOW_LEVEL`, `OPTICAL_FLOW` (old name `INTERPOLATE`), `EXPERIMENTAL`, `ONESIDED_FILL` | as in the table above | read once when the game starts |

Bad values are clamped or ignored, a malformed line never stops the layer. A number is digits, a sign, a point and an exponent, nothing else: `nan`, `inf`, hex (`0x10`), an empty value or text after the number is not a number, and the setting keeps its previous (or its default) value; the same rule holds for every `VKIFRAME_*` environment variable and for the start-of-game keys read from `live.conf` (the float, whole-number and on/off settings: a finite number is clamped to the setting's range, anything else, such as `nan`, `5x` or `0x4`, is ignored and logged and the default stays; on/off settings also take `true`/`false`, `on`/`off`, `yes`/`no`), and for the `MAX_SYNTHETIC` line that sizes the swapchain at game start. `BASE_FPS_CAP` below 1 means off. `live.conf` is read up to 64 KiB (only complete lines; a line cut by the limit is dropped), a line with a NUL byte is ignored, control characters are replaced in `live.log`, and a file of junk logs at most 10 notes per read. A `live.conf` that is a FIFO or another special file is refused without blocking the game. Every change is written, with the time, the old and the new value, to `~/.config/vkiframes/live.log`, the same file as the layer's own log (the folder is 0700 and the files 0600; without `HOME` or `XDG_CONFIG_HOME` the layer has no live settings and writes no files) (one log to read). `status.json` next to it is rewritten once a second with the native and shown fps, the number of generated frames per native frame the mode and the swapchain's refresh rate (`refresh_hz`, 0 when unknown). The Decky plugin uses it to set the top of its Target FPS slider to the higher of 120 and that rate; `TARGET_FPS` in live.conf accepts 30..1000. `VKIFRAME_LIVE_CONF` and `VKIFRAME_LIVE_LOG` change the paths.

## Cost warning

Once per swapchain the layer logs `WARNING: the layer's GPU work is N % of the game's frame time ...` when its own GPU time per second (flow time x pairs per second + interpolation time x generated frames per second) is over `VKIFRAME_COST_WARN` (default `1.0`, that is 100 %: the layer alone would fill the GPU) for three seconds in a row. The ratio is the same for any frame rate, because the layer's work per game frame is compared with the game's frame time. Lower `MAX_SYNTHETIC` or `FLOW_LEVEL`, or the target, when it appears.

## Measuring (stats and benchmark)

`VKIFRAME_STATS=1` (or `STATS=1` in live.conf) writes one JSON line a second to `~/.config/vkiframes/stats.jsonl` (0600, in the 0700 config folder, one old file kept past 8 MB): game frame time (mean, 95th and 99th percentile, max, from present-to-present intervals), CPU time of the layer per frame, the game thread's waits per frame, GPU time of the flow (per pair) and of the interpolation (per generated frame) with the derived GPU share, pacing (lateness, cancelled ticks, dropped frames), the refresh rate and the rates. Percentiles are nearest-rank: sort the second's intervals, take element ceil(p x n). Off by default; when off, no `stats.jsonl` record is written and no per-present interval is sampled. `STATS` gates only those two: the per-second line in the log (`live.log`/the layer log) and `status.json` are written either way, and the GPU timestamps around the flow and interpolation passes (and, from alpha.3, the blend pass when optical flow is off) are always on, because the cost warning below uses them (two atomic adds per pair for the sums, plus one atomic load per present to ask whether stats are on). No game name, process name, path, host name or IP is recorded.

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
│   ├── layer_main.cpp           # hook wrappers, queue lock, submit, acquire hooks
│   ├── layer_internal.hpp       # declarations shared by the layer_*.cpp files
│   ├── layer_instance.cpp       # instance and device create/destroy hooks, queue lookup
│   ├── layer_swapchain.cpp      # swapchain create/destroy hooks, extra images
│   ├── layer_present.cpp        # present hook and the waits that release a held frame
│   ├── layer_presenter.cpp      # presenter thread: queued present jobs, start, drain, stop
│   ├── layer_present_steps.cpp  # rate gate plan, rate log, real/held/synthetic presents
│   ├── layer_dispatch.cpp       # registries, proc-addr tables, loader negotiation
│   ├── gpu_frames.cpp           # capture, synthesize, present of synthetic frames
│   ├── gpu_images.cpp           # images and buffers of the layer
│   ├── dump_pool.cpp, dump_queue.cpp  # debug capture (VKIFRAME_DUMP)
│   ├── optical_flow.cpp         # flow passes and GPU timers
│   ├── present_timing.cpp
│   ├── live_config.cpp, live_config_io.cpp  # live.conf, live.log, status.json
│   └── stats.cpp                # stats.jsonl, bench.json
├── shaders/                     # flow_luma/down/coarse/refine/occlusion, flow_interp (interpolation, pan fallback, UI mask, extrapolation), flow_vis, blend
├── scripts/                     # install.sh, uninstall.sh, gamemode_launch.sh, check_env.sh, vkiframes-bench.sh, package.sh (release archives), check_so.sh, repro_check.sh, layer.map
├── decky/                       # Decky Loader plugin (live settings from Game Mode), its scripts and tests, see decky/README.md
├── tests/                       # mock-loader and mock-driver tests, scheduler replay, flow reference
└── tools/                       # clock_sim (output-clock simulator), interp_eval (offline scoring)
```

## Target machines

Developed for a **Steam Deck OLED** (base machine, 90 Hz, Game Mode) and a SteamOS desktop (Ryzen 5 5600X, Radeon RX 9070). SteamOS specifics (Mesa and gamescope support for `VK_EXT_present_timing`, building against the Steam Linux Runtime, the game-frame-limit problem in Game Mode, Deck performance budget) are in [TARGETS.md](../TARGETS.md).

## Roadmap

Stages 1–5 (layer, present timing, hold and ring buffer, optical flow, interpolation) are implemented; Stage 6 is partly done (install, packaging, settings), while swapchain resizing and the compositor test matrix are untested. Known open items: smudging around trees and thin lines, slow phase-lock recovery after stalls, hiding `VK_EXT_present_timing` from games that use it themselves. Planned next: lower GPU load, an in-game menu with hotkeys, and a study of the interpolation shader. See [ROADMAP.md](../ROADMAP.md) and [STATUS.md](../STATUS.md).

---

## Support

vkIFrames is free to use. If you like the work, you can say thanks on [Ko-fi](https://ko-fi.com/senorfm). Tips do not buy features or priority support.

## License

vkIFrames is licensed under the [Apache License, Version 2.0](../LICENSE). The copyright holder is "vkIFrames contributors" (see [NOTICE](../NOTICE)). The Decky plugin in `decky/` is based on the Decky plugin template and stays under its BSD 3-Clause license ([decky/LICENSE](../decky/LICENSE)); both licenses allow use, changes and redistribution, with notices kept.
