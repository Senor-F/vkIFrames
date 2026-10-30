# vkIFrames: Target Machines and Environment

The layer is developed for two SteamOS machines. This file records what is known about them, what is still unverified, and what that means for the build and the test plan. Items marked **(verify)** have not been confirmed on the actual hardware.

## 1. Machines

| | Steam Deck (base machine) | Custom desktop |
| :--- | :--- | :--- |
| CPU / GPU | AMD APU, RDNA 2, 8 CUs, shared memory *(from memory, verify)* | Ryzen 5 5600X + Radeon RX 9070 (RDNA 4) |
| Display | 1280×800; LCD 60 Hz, OLED up to 90 Hz *(from memory, verify)* | not yet specified (monitor resolution and refresh rate needed) |
| OS | SteamOS 3.x, immutable root, Game Mode (gamescope) and Desktop Mode (KDE) | SteamOS |
| Driver | Mesa RADV | Mesa RADV |
| Role | The design target: every performance number is judged here | Development and high-resolution testing |

The Deck is the harder case: a small GPU shared with the game, 800p, and an immutable OS with a container for games. If it works there, it will work on the 9070. (The 9070 box runs the same SteamOS build as the Deck, with a different CPU and GPU, so the Deck is expected to behave like it; confirm by running the scripts on the Deck.)

## 2. What the research found (October 2026)

| Fact | Source | Consequence |
| :--- | :--- | :--- |
| `VK_EXT_present_timing` was merged into Mesa 26.1 for RADV, ANV, NVK, PanVK and TURNIP, **Wayland first**. | [Phoronix, Mesa 26.1 merge](https://www.phoronix.com/news/Mesa-Merges-Present-Timing) | Native-Wayland Vulkan apps can have exact pacing on Mesa ≥ 26.1. |
| The X11 / **XWayland** path was only added in Mesa 26.2. | [Phoronix, Mesa 26.2 X11 present timing](https://www.phoronix.com/news/Mesa-26.2-X11-Present-Timing) | Under XWayland (most Proton games in desktop mode) timing needs Mesa ≥ 26.2. |
| SteamOS stable has been reported on **Mesa 26.1.x** (user reports; Valve's notes do not name the version). | [SteamOS issue #2754](https://github.com/ValveSoftware/SteamOS/issues/2754), [SteamOS 3.8.28 notes](https://www.gamingonlinux.com/2026/09/steamos-3-8-28-stable-released-with-big-improvements-for-steam-deck-steam-machine-and-more-hardware-support/) | Check `vulkaninfo` on each machine; do not assume. |
| gamescope support for `VK_EXT_present_timing` is a Valve **draft PR** (status as last fetched: pending on-device testing). | [gamescope PR #2395](https://github.com/ValveSoftware/gamescope/pull/2395) | **In Game Mode, games reach the display through gamescope's own Vulkan WSI layer, so the extension may be absent there.** The vblank-paced fallback (DESIGN §4.6) is not optional on the Deck. |

## 3. Consequences for the design

1. **Both pacing paths must work.** Timed path (desktop Wayland, Mesa ≥ 26.1) and vblank fallback (Game Mode, XWayland on older Mesa). The Stage 3 output clock already uses the same `t` values for both; Stage 3 testing must cover the fallback first, not as an afterthought.
2. **The game's frame limit.** In Game Mode the Steam frame limiter acts at the display side, below this layer. It would cap the layer's *output*, not the game's base rate. Options: cap the base rate in the game or with `DXVK_FRAME_RATE`, or add an optional base-rate limiter inside the layer (proposed `VKIFRAME_BASE_FPS_CAP`; not yet designed). Decide before Stage 3 is tested on the Deck.
3. **Performance budget on the Deck.** The GPU is a fraction of a desktop card and is already busy with the game. The 2.5 ms / 1080p budget in STAGE6 is a desktop number. For the Deck, plan on running optical flow at half resolution by default (STAGE6 §6, lever 1) and expose it as a setting (proposed `VKIFRAME_FLOW_SCALE`). Measure with timestamp queries on the Deck before trusting any figure.
4. **Presets.** Likely useful: Deck 30 → 60, 40 → 80 on OLED, 45 → 90; desktop 60 → 120/144 depending on the monitor. The bypass gate (`VKIFRAME_MIN_GAIN`) already stops a 55 FPS game from being interpolated to 60.

## 4. Building for SteamOS

The root filesystem is read-only, and Steam runs games in a container (the Steam Linux Runtime) with its own, older libraries. A layer built on a current distribution can fail to load inside it. Measured on the Stage 1 library built with GCC 13 (Ubuntu 24.04):

* Dynamic libstdc++ build: needs `GLIBCXX_3.4.29` (newer than the Steam Runtime 3 "sniper" libstdc++ is likely to provide) and glibc ≥ 2.4 symbols only.
* Static libstdc++/libgcc build: removes the C++ library dependency but pulls in glibc symbols up to **2.38**, which is worse for an older runtime.

So neither build is safe from a new toolchain. Recommended: **build inside the Steam Runtime 3 "sniper" SDK container**, so the compiler, libstdc++ and glibc match what games run against, using Distrobox or Podman on SteamOS. The SDK image is published by Valve at `registry.gitlab.steamos.cloud/steamrt/sniper/sdk` **(verify the exact image name and tag in Valve's steam-runtime documentation)**. Then check the result with `objdump -T libvkIFrames.so | grep -o 'GLIBC[X]*_[0-9.]*' | sort -uV | tail` and confirm that the newest versions are not newer than the runtime's.

Fallback if the SDK route is awkward: a small Ubuntu 20.04 / Debian 11 container with a newer CMake and Vulkan-Headers.

Installing (no root needed): put the library and an implicit-layer manifest under `~/.local/share/vulkan/implicit_layer.d/` and enable per game with `VKIFRAME_ENABLE=1` (STAGE6 §5). **(verify)** that the Steam container exposes this directory's layers to games; if not, the manifest must be placed where pressure-vessel imports host layers, or the game launched with `VK_ADD_LAYER_PATH` / `VK_LAYER_PATH` set in the launch options.

## 5. Test plan by environment

Each row needs the corresponding report from the scripts (`scripts/check_env.sh`, `scripts/test_stage1.sh`; later stages add their own).

| # | Environment | What it proves |
| :--- | :--- | :--- |
| 1 | Deck, Desktop Mode, native Wayland `vkcube` | Layer loads; timed pacing path if Mesa ≥ 26.1 |
| 2 | Deck, Desktop Mode, nested gamescope (`GAMESCOPE=1 scripts/test_stage1.sh`) | Layer coexists with gamescope's WSI layer; whether the extension is exposed through it |
| 3 | Deck, Game Mode, a Proton game via launch options | The real use case; fallback pacing; container visibility of the layer |
| 4 | Deck, XWayland / X11 app | Mesa < 26.2 gives the fallback; ≥ 26.2 the timed path |
| 5 | Custom desktop, same four | Same, at higher resolution and with a bigger GPU |
| 6 | Either machine, 25–60 FPS games | Cadence, latency, artifacts (Stage 6) |

## 6. Questions that need the machines

* `vulkaninfo --summary` on each (Mesa version, API version) and whether `VK_EXT_present_timing` and `VK_KHR_present_id2` are listed, in Desktop Mode and (via `scripts/check_env.sh` run from a Game Mode launch option) in Game Mode.
* The monitor on the custom machine: resolution, refresh rate, VRR?
* Does the custom machine boot into Game Mode, a desktop session, or both?

## 7. Measured on the machines

### Custom desktop (reported 2026-10-29, `scripts/check_env.sh`)

| Item | Result |
| :--- | :--- |
| Board / GPU | MSI MAG B550M MORTAR WIFI; Radeon RX 9070 (Navi 48, `RADV GFX1201`) |
| OS | SteamOS (variant `steamdeck`), build 20261023.100, kernel `7.2.7-valve2-2-neptune`, host glibc 2.43, root not read-only |
| Vulkan | API 1.4.350, driver **Mesa 26.2.0-devel**, Wayland session |
| `VK_EXT_present_timing` / `VK_KHR_present_id2` | **both exposed** (timed pacing path is available here) |
| Tools on the host | `vulkaninfo`, `vkcube`, `glslangValidator`, `gamescope`, `distrobox`, `podman`; **no** `cmake`, `g++` or Vulkan headers; no validation layer |
| Implicit layers present | MangoHud (x86_64, x86), `VK_LAYER_FROG_gamescope_wsi` (x86_64, x86), `VK_LAYER_MESA_anti_lag`, `VK_LAYER_MESA_device_select`, RenderDoc, and Steam's own fossilize / overlay layers in `~/.local/share/vulkan/implicit_layer.d` |

Conclusions: the timed path (Stage 2) can be developed and tested on this machine; Steam itself puts its layers in `~/.local/share/vulkan/implicit_layer.d`, so that is the right install location for ours (still to confirm from inside a game); the Mesa anti-lag layer and MangoHud are other layers in the same chain, so layer ordering has to be looked at when we test with them enabled. **Stage 1 result on this machine:** 9 passed, 0 failed, 1 skipped (validation layer not installed). The Steam Deck itself has not been measured yet.

### Stage 2 measurement (custom machine, `vkcube`, Mesa 26.2.0-devel, Wayland session)

| Surface | `presentTimingSupported` | absolute targets | relative targets | present id 2 | stages |
| :--- | :--- | :--- | :--- | :--- | :--- |
| `vkcube --wsi wayland` | yes | **no** | no | yes | 0x5 (queue ops end, first pixel out) |
| `vkcube --wsi xcb` (XWayland) | **no** | no | no | yes | none |

Stage 2 test result: 9 passed, 0 failed, 1 skipped (aimed presents, no absolute targets). On Wayland the layer got 837 presentation results in one run, interval mean 8.334 ms with sd 0.001 ms against a reported refresh of 8.333 ms (a 120 Hz display), no queue-full, time domain `present_stage_local` (no CLOCK_MONOTONIC domain offered), and `VKIFRAME_BASE_FPS_CAP=30` held the base rate at 30.00 FPS.

Stage 3a test result (same machine, Wayland, 60 FPS target, base rate capped by `VKIFRAME_BASE_FPS_CAP`): 7 passed, 0 failed. Caps 30, 40 and 45 FPS gave native = cap and output 60.0 FPS (interpolating, content delay 33.5, 25.1 and 22.3 ms). Caps 55 and 60 stayed in bypass (output = native), and the uncapped 120 FPS display rate stayed in bypass.

Consequence: on this compositor present timing gives feedback only (when frames reached the screen); requested target times cannot be honoured. The layer therefore enables timing for feedback and keeps vblank-driven FIFO pacing, which is the fallback path of DESIGN section 4.6 and what Stage 3 must be tested against first. Whether absolute targets appear needs a compositor with commit-timing support (to be checked on the Deck, Desktop Mode and Game Mode).

### Steam Deck

Not measured yet. Run `bash scripts/check_env.sh` on it in Desktop Mode.


## Steam Deck, Game Mode (measured 2026-10-30, layer from the 9070 build, vkcube as a non-Steam game)

- The layer loads in Game Mode (launch option `scripts/gamemode_launch.sh %command%`); the app runs through XWayland and gamescope's own WSI layer (`--wsi xcb`). Non-Steam native apps are not run inside the Steam Linux Runtime container, so layer visibility inside that container (Proton games) is still untested.
- Surface: present timing = yes (stages 0x5, domain `present_stage_local`), absolute = no, relative = no, present id 2 = yes. Same as desktop Wayland: feedback only, pacing is vblank-driven (presenter thread + FIFO). Refresh 11.11 ms (90 Hz, OLED Deck).
- 45 -> 90 FPS (cap 45, target 90, Steam frame limit disabled): shown 90.0 FPS, present interval 11.111 ms with sd 0.000 ms in most one-second windows, no failures in 5,600 presents. Occasional windows with sd 2-7 ms (about one in ten).
- 40 -> 60 FPS on a 90 Hz screen: shown 60 FPS, but intervals alternate between 1 and 2 refreshes (mean 18.5 ms, sd 5.2 ms): 60 does not divide 90. Use targets that divide the refresh rate (45, 90, 30) on this display. While Steam's frame limiter was active the measured interval mean was 33-39 ms.
