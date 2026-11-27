[index](../README.md) | [next](02-how-it-works.md)

# vkIFrames

**Dynamic, rate-matching frame interpolation for Vulkan on Linux and SteamOS.**

vkIFrames is an independent Vulkan layer that sits between a game and the driver and generates synthetic in-between frames, so a game rendering at a low or unsteady frame rate is displayed at the **Target FPS** you choose (for example 90 Hz on a Steam Deck OLED).

Unlike fixed 2x/3x frame generation, the number of generated frames changes from moment to moment with the game's real frame rate. When the game already reaches the target, the layer steps aside and passes native frames straight through with no added latency.

> **Alpha 0.1.0-alpha.2. Expect rough edges.** vkIFrames has been tested by one developer on one Steam Deck OLED with a few Proton games, and on a Radeon RX 9070 desktop with a test application only. It can break a game. 45 → 90 fps ran locked in the best case (90.0 fps shown, no drops); image quality around thin or fast-moving structures is still being improved. See [STATUS.md](../STATUS.md) for measurements and open items.

## If something goes wrong (read this first)

1. Remove `VKIFRAME_ENABLE=1` from the game's launch options. Without it the layer is not loaded.
2. Or set `VKIFRAME_DISABLE=1`.
3. Or run `scripts/uninstall.sh`. It removes the manifest (`~/.local/share/vulkan/implicit_layer.d/VkLayer_frame_gen.json`) and the installed library (`~/.local/share/vkiframes/`). Your settings and logs in `~/.config/vkiframes` stay unless you pass `--purge`.

Files the layer writes: `~/.config/vkiframes/live.log` (and `live.log.1`), `status.json`, and with `STATS` set or a benchmark requested (`bench.req`, below) also `stats.jsonl` (and `.1`) and `bench.json`. With `VKIFRAME_DUMP` it also writes the capture folder (`VKIFRAME_DUMP_DIR`, default `/tmp/vkIFrames_dump`; under Proton that is the game container's `/tmp`), and with `VKIFRAME_LOG_FILE` the log goes to the file you name. Nothing else, and it opens no network port.

## Where it helps, and where it does not

vkIFrames adds GPU work. By default every shown frame is a plain cross-fade of two game frames (it ghosts on movement). With optical flow on (opt-in, `VKIFRAME_OPTICAL_FLOW=1` or the panel's "Optical flow" toggle) the layer also estimates the motion of every base frame and builds the in-between frames along it instead of doubling moving objects (in an offline check on 25 captured triplets it had the lower error in 16), and costs a lot more GPU (about 5 ms per game frame in the scene we measured: Tomb Raider (2013) on a Steam Deck at 720p, one run). That work is the price of the extra frames, so the layer fits some games much better than others.

**Good fit**
- **CPU-bound games.** The game is limited by its CPU side and the GPU has idle time, which can absorb the layer's work. In one Deck run with a 30 fps cap the game's frame time did not change with the layer on (33.40 ms off, 33.38 ms on; benchmark 1 in STATUS.md). The GPU still does the extra work; only the frame time was unaffected.
- **Games with a forced frame-rate cap** (an in-engine 30 or 45 fps lock, a locked console port, or a limiter you set yourself): the game cannot go faster, so the extra frames are new. Set the cap to the base rate you want (see "Cap your game" under "Trade-offs to know about") and the Target FPS to the display rate, for example 45 to 90. The layer's own cap already defaults to 45; with an in-game limiter as well, the lower of the two wins.
- **A base rate of 30 fps or more** is the advice, not a limit in the code: below it the added latency is more noticeable (one to one and a half base frames, 33 to 50 ms at 30 fps and 50 to 75 ms at 20 fps; the layer reports it as content delay) and the layer may drop frames at the `VKIFRAME_MAX_SYNTHETIC` limit (see STATUS.md).

**Poor fit**
- **GPU-bound games.** The layer takes GPU time from the game, so the game's own frame rate falls: in one Deck scene (one run of six off/on pairs) 55.6 fps native calculates to about 43 fps (from the frame times) while 90 fps are shown.
- **Games that already reach the target** without the layer. When the game's own frame rate is at or above Target FPS (within about 1 %, `VKIFRAME_MIN_GAIN`) the layer passes frames through, so there is nothing to gain. Note that the layer's own base-rate cap is on by default (`VKIFRAME_BASE_FPS_CAP`, 45): while the layer is enabled the game is held at 45 fps, so a game that could run 90 natively does not reach the target with the layer on. Set `VKIFRAME_BASE_FPS_CAP=0` to leave the game uncapped, or switch the layer off for such a game.
- **Light scenes** (the game needs little GPU time per frame). With optical flow on, going from 45 to 90 fps costs more GPU than rendering 90 natively when one game frame costs less than the layer's work per base frame: one optical-flow pass plus two interpolation passes. Measured pass times on a Deck were flow 3 to 5.5 ms per pair (it has varied from run to run) and about 0.9 ms per interpolated frame, so roughly 5 to 7 ms (calculated for 45 to 90, not a guarantee; the flow time depends on the GPU clock: the same scene measured 3.1 to 3.3 ms per pair with the clock fixed at 1600 MHz and 4.8 to 4.9 ms with the automatic clock; the flow time includes any wait behind the game's earlier GPU work, and the capture and final copies are not timed, so this is not an exact total). In one scene of Cuphead (the forest level) the Game Mode overlay read 23 to 27 % GPU at 480 to 540 MHz with the layer off at 90 fps native, and 48 to 53 % at 740 to 830 MHz with the layer and optical flow on at 45 to 90: the busy percentage roughly doubled and the clock rose by about half (one scene, overlay readings, not a controlled measurement). The picture rate is the same and the GPU works harder. Measured on the same scene on battery (Tester, three sessions): with the layer and optical flow on the battery drew about 1.0 to 1.4 W more than with the layer off (about 10.2 to 10.6 W against 9.2 W), the chip about 0.5 to 0.7 W more, the temperature 2.6 to 4.5 °C more; with the default cross-fade (optical flow off) it drew about 0.7 W less than with the layer off, mostly because the 45 fps cap halves the game's own frames. One scene, one session each, not repeated. Part of this is the default cap: that scene could run 90 fps natively, and the layer held it at 45 and generated the other 45.
- **Latency-sensitive play** (fast shooters, rhythm games): interpolation adds one to one and a half base frames of latency (about 33 to 50 ms at a 30 fps base, 22 to 33 ms at 45 fps).

If you are unsure, try it on one game and compare with and without the layer. The log line "native N fps -> output M fps (planned) | shown K fps" shows what the layer did (`VKIFRAME_STATS=1` adds a per-second file). In Desktop Mode the Steam overlay's counter can show the game's own rate instead of the shown rate (see "Tested").

### Cross-fade or optical flow?

Optical flow is off by default. Which setting looks and feels better depends on the game and, to some extent, on the person; this is what we saw (one tester's eyes, not a measurement):
- **Cross-fade (default):** the image stays clean, with no warping of trees or thin lines. In Batman: Arkham Asylum and Tomb Raider (2013) it looked like the target frame rate. In Cuphead (forest level) it still looked like the base rate (45 fps) on a 90 fps target, although the logs showed the same 45 to 90 as with optical flow on.
- **Optical flow on:** in Cuphead the motion felt like the target rate, with warping and distortion on trees and thin lines; it costs about 5 ms of GPU time per game frame in the scene we measured.
Try the default first; if motion still feels like the base rate, turn on "Optical flow" in the Decky panel or set `VKIFRAME_OPTICAL_FLOW=1`, and see whether the artefacts are acceptable to you.

## Install the alpha

- **Layer:** download `vkIFrames-0.1.0-alpha.2-linux-x86_64.tar.gz` and `SHA256SUMS` from the release, check with `sha256sum -c SHA256SUMS` and verify the signature (see "Check the download" below), unpack, and run `scripts/install.sh` (no root; it refuses to run as root and is safe to run twice).
- **Check the download.** `SHA256SUMS` is signed with an SSH key (`SHA256SUMS.sig` in the release). Put this line in a file `allowed_signers`: `vkiframes-release ssh-ed25519 AAAAC3NzaC1lZDI1NTE5AAAAIOMdQXwSmxKkWwjWSm+fZF+PneWMTOtBXw72g4W1jSTg`, then run `ssh-keygen -Y verify -f allowed_signers -I vkiframes-release -n vkiframes -s SHA256SUMS.sig < SHA256SUMS`. It must print `Good "vkiframes" signature`. Then `sha256sum -c SHA256SUMS` proves the archives are the signed ones. The key comes from the same GitHub account as the release, so this protects against a changed or replaced download, not against a compromised account; compare the fingerprint (`ssh-keygen -lf` on that line) with one you got elsewhere if that matters to you. The fingerprint is `SHA256:fhj+KWHGKFDU9rDeiGQ9aI/NeaXiW/rvCsJLI5YOT8o`.
- **Steam Deck panel (optional):** install `vkIFrames-decky-0.1.0-alpha.2.zip` with Decky Loader's "install from ZIP". The panel changes settings while a game runs. It is the only live control for now: there are no in-game keys or menu yet.
- **Turn it on per game:** Steam launch options `VKIFRAME_ENABLE=1 %command%`. The layer then runs with its default settings. The plugin never sets the launch option for you. For a 32-bit game (Proton runs it in a 64-bit process) use `PROTON_USE_WOW64=1 VKIFRAME_ENABLE=1 %command%`. If the layer loads but shows no extra frames (seen with Batman: Arkham Asylum on Proton - Experimental), try GE-Proton, or add the DXVK setting from "Known issues" to the launch options: `DXVK_CONFIG="dxvk.enablePresentTiming = False" PROTON_USE_WOW64=1 VKIFRAME_ENABLE=1 %command%` (tested on one game). The same without `PROTON_USE_WOW64=1` (`DXVK_CONFIG="dxvk.enablePresentTiming = False" VKIFRAME_ENABLE=1 %command%`) is not tested by itself. The Decky panel has a copy button for each of these.
- **Uninstall:** `scripts/uninstall.sh` and the panel's "Uninstall layer…" button remove the same two files (the manifest and `~/.local/share/vkiframes/libvkIFrames.so`), so either one is enough, in any order. Removing the plugin in Decky Loader removes them too (as far as we know; not yet tested on a Deck). Neither the script nor the panel removes the other's part: `uninstall.sh` leaves the plugin installed, and removing the plugin leaves `~/.config/vkiframes`. Only `scripts/uninstall.sh --purge` (or deleting that folder) removes your settings and logs. The Decky zip has no `uninstall.sh`; if you removed the plugin first and something is left, use the one from the layer archive. Installing the plugin's layer and running `install.sh` both write the same two files, so the one run last wins. Both scripts refuse to write or remove through a symbolic link at `~/.local/share/vkiframes`, `~/.local/share/vulkan` or `~/.local/share/vulkan/implicit_layer.d` (`install.sh` changes nothing and exits 1; `uninstall.sh` skips what is behind the link, says so and exits 1); `~/.local/share` itself (or `XDG_DATA_HOME`) may be a link.
- **Requirements:** Linux x86-64, 64-bit games only, a Vulkan 1.3 driver, glibc 2.34 or newer (SteamOS 3.x, Ubuntu 22.04+, Debian 12+).

## Known issues and limits (alpha)

- **Cost.** In one saved-game scene of Tomb Raider (2013) on a Steam Deck at 1280x720 the layer cost about 5.1 ms per game frame (one scene, one run of six off/on pairs). A GPU-bound game loses base frame rate: 55.6 fps native calculates to about 43 fps (from the frame times) while 90 fps are shown. Other games and scenes measured 0 to 6 ms so far (0 when the game has GPU headroom). These numbers are for optical flow on. Optical flow is off by default: the default cross-fade runs no flow passes and ghosts on movement. Its cost was not measured in a controlled on/off test; one estimate from the frame times of a matrix is about 0.6 to 1.2 ms more per game frame in a GPU-bound scene (Tomb Raider (2013), see the second matrix table). In alpha.2 the layer's own timing (`flow_ms`, `interp_ms`, `gpu_share`) did not see the blend, so those read 0 and the cost warning could not fire, which did not mean the cross-fade was free; from alpha.3 it times the blend pass (a lower bound: the copies are not timed). In one Cuphead scene the battery drew about 0.7 W less than with the layer off (the Tester's reading, one session), mostly because the 45 fps cap halves the game's own frames.
- **Picture quality.** With the default cross-fade, anything that moves ghosts. With optical flow on, thin lines and tree trunks distort, and occlusions can ghost. In an offline check on 25 captured triplets the interpolation had the lower error than the plain cross-fade in 16; it can be slightly worse on near-static content. Not measured against any quality bar.
- **The Decky panel rewrites the whole `live.conf`** at every change and at loader start, so edits made by hand to that file are overwritten.
- **Input latency** is one to one and a half base frames while interpolating (the log shows it as content delay), on top of the game's own latency.
- **Frame pacing.** The layer forces FIFO presentation, which may not suit every variable-refresh setup. Present-timing support differs between gamescope and Mesa versions.
- **Proton - Experimental (observed).** When the game's present chain carries `VK_EXT_present_timing` or `VK_KHR_present_id2` structures, the layer cannot replay them for a frame shown later, so it stays in pass-through on that swapchain: the shown rate equals the native rate and no synthetic frames are made (the log says `hold: off, the application's present chain uses present id 2 / present timing ...`). Seen in Batman: Arkham Asylum GOTY Edition (a DX9-era title through DXVK) and in Tomb Raider (2013) on Proton - Experimental (experimental-11.0-20261027b, DXVK 3.1.1); Batman on GE-Proton11-7 (DXVK 3.1) carries the older present id and works (tested), as do Tomb Raider (2013) and Cuphead on GE-Proton11-7. If the layer loads but shows no extra frames, try GE-Proton in the game's Compatibility setting. Another workaround seen on those two games (one run each, the Tester): the Steam launch option `DXVK_CONFIG="dxvk.enablePresentTiming = False" PROTON_USE_WOW64=1 VKIFRAME_ENABLE=1 %command%` (the line that was used for Batman; Tomb Raider (2013) was run with `dxvk.enablePresentTiming=False`, without the spaces, and worked too; Steam can mangle the quote marks) made the present chain the older one, and the layer then held and generated frames (about 90 shown, 0 dropped). It is not a fix: the option name appears in the DXVK 3.1.1 libraries of that Proton build (checked by the Auditor in the same Proton version on another machine) but is not checked against DXVK's documentation, its meaning is inferred from the effect on the present chain, it may change between Proton builds, and disabling present timing in DXVK may change the game's own frame pacing (not looked at). Remove it from the launch option if the game misbehaves. The cause is inferred from the present chain (the newer DXVK in Proton - Experimental appears to use present timing and present id 2), not from the DXVK sources.
- **Low memory.** A failing allocation inside the layer no longer ends the game by itself: the call fails with `VK_ERROR_OUT_OF_HOST_MEMORY` or falls back to the game's own call (a swapchain being destroyed at that moment may skip the layer's cleanup). A game that does not handle that error can still stop. Tested with an injected allocation failure on mocks only.
- **Debug capture.** `VKIFRAME_DUMP` writes frames of the game to a folder (it needs optical flow on: with it off nothing is captured); it holds about (frames + 11) x width x height x 8 bytes of memory (about 300 MB at 30 frames at 1280x720, about 1.2 GB at 60 frames at 1920x1080) and writes the files in one stall of about 0.3 s at the end. Off by default.
- **Not tested:** NVIDIA, Intel, native 32-bit games, native Linux Vulkan games, other distributions than SteamOS.

## Tested

| Where | What | Result |
|---|---|---|
| Steam Deck OLED, SteamOS 3.9.5, AMD (RADV) | Tomb Raider (2013), 18 to 60 fps native, 90 fps shown | works, see cost above |
| same Deck | Cuphead (2D run-and-gun, 45 fps cap, 90 fps target; GE-Proton11-7) | works |
| same Deck | Batman: Arkham Asylum GOTY Edition (DX9-era, DXVK) | works on GE-Proton11-7 (the Tester's log: 29 to 30 fps native, 89 to 91 shown); on Proton - Experimental the layer stays in pass-through, see Known issues |
| Radeon RX 9070 desktop (RADV), SteamOS | a test application only; no game run with the layer | works for the test application |

Game runs for the table started from Steam in Game Mode with Proton. Desktop Mode was checked once, with Cuphead: the layer reported 90 fps shown and KWin's "Show FPS" effect also counted 90, while the Steam overlay's counter showed 45, the game's own rate. If the Steam overlay shows the base rate in Desktop Mode, check with KWin's "Show FPS" effect before concluding that the layer does nothing. The layer reads the refresh rate through `VK_EXT_present_timing`, and uses that extension only when the game does not use present timing or present id itself and the driver offers it. In every Steam Deck run so far (Cuphead, Tomb Raider (2013), Batman: Arkham Asylum, through Proton and DXVK) the log says `present timing: skipped (the application uses present timing / present id itself)`, so `refresh_hz` shows 0 in Game Mode and Desktop Mode alike, and the refresh-rate limit on `TARGET_FPS` does not apply; set `TARGET_FPS` at or below your display's refresh rate yourself. Whether the Deck's driver offers the extension was not checked. On a machine where the driver does not offer the extension at all (our Radeon RX 9070 desktop), the log says `device extensions not offered by this driver` and the result is the same.

### Frame-rate matrix (Batman: Arkham Asylum GOTY Edition, plain cross-fade)

A test for the curious, read it as "base frame-rate cap > Target FPS". Setup: Steam Deck OLED, Game Mode, Proton - Experimental (experimental-11.0-20261027b, DXVK 3.1.1) with the DXVK launch option from "Known issues", optical flow off (the default cross-fade), one game start, only `VKIFRAME_BASE_FPS_CAP` and `TARGET_FPS` changed in `live.conf` between rows, standing still in the same scene, 10 s settle and 90 s recorded per row (the Tester's logs; the Auditor's recompute from the raw files agrees to within 0.01 fps). The matrix was run with an earlier build than this release (commit 4128abb: it did not yet have the `OPTICAL_FLOW` key or the extra panel buttons, and the optical flow was switched off with `INTERPOLATE=0`). The release behaves the same for these settings by design: between that build and the release the source changed only the default of the optical flow, the setting names, a log word, a field in `bench.json` and a note for `VKIFRAME_DUMP`, nothing in the scheduler or the present path.

| Cap > target | Mode | Native fps | Shown fps | Synthetic frames per game frame (k) | Dropped |
|---|---|---|---|---|---|
| 30 > 30 | pass-through | 30.00 | 30.00 | 0 | 0 |
| 30 > 60 | generating | 30.00 | 60.00 | 1.00 | 0 |
| 30 > 90 | generating | 30.00 | 90.00 | 2.00 | 0 |
| 30 > 120 | generating | 30.00 | 120.00 | 3.00 | 0 |
| 60 > 30 | pass-through | 59.99 | 59.99 (shown 60, not 30) | 0 | 0 |
| 60 > 60 | pass-through (1 s generating) | 59.99 | 60.00 | 0 | 0 |
| 60 > 90 | generating | 60.00 | 89.99 | 0.50 | 0 |
| 60 > 120 | generating | 59.99 | 120.00 | 1.00 | 0 |
| 90 > 30 | pass-through | 61.97 | 61.97 | 0 | 0 |
| 90 > 60 | pass-through | 61.94 | 61.94 | 0 | 0 |
| 90 > 90 | generating | 61.96 | 90.00 | 0.45 | 0 |
| 90 > 120 | generating | 61.98 | 119.99 | 0.94 | 0 |

How to read it:
- The game itself ran at about 62 fps, so with a cap of 90 the cap did not bind: the 90 rows are really 62 to the target.
- When the target is at or below the game's rate the layer passes frames through: a target below the game's rate does not slow the game down (60 > 30 still shows 60).
- With whole ratios (x2, x3, x4) k is exactly 1, 2 and 3. With the other ratios (1.5, 1.45, 1.94) k is a fraction and the generated frames are spaced unevenly by design; the per-second records do not show the spacing.
- "Shown" at a target of 120 is the layer's plan: the Deck's panel is 90 Hz and what the screen displayed was not measured. The layer does not know the refresh rate on the Deck (see above), so it does not clamp the target.
- The cost counters (`flow_ms`, `interp_ms`, `gpu_share`) read 0 for the cross-fade in the alpha.2 build these rows were taken with, because that build did not time it; an estimate of its cost from the frame times is under the second table. From alpha.3 the layer times the blend pass: `interp_ms` and `gpu_share` then show the blend (a lower bound, the copies are not timed), `flow_ms` stays 0. Nothing was measured about the picture quality or the latency of these rows.

### Frame-rate matrix 2 (Tomb Raider (2013), plain cross-fade)

The same twelve rows on a second game, on the alpha.2 release candidate (commit c8e5c30; the settings, the pacing and the present path are the same as in the final alpha.2 build). Steam Deck OLED, Game Mode, Proton - Experimental (experimental-11.0-20261027b, DXVK 3.1.1) with `DXVK_CONFIG="dxvk.enablePresentTiming=False"` (without it the present chain is unsupported on this Proton and nothing is generated, as with Batman), the optical flow off, the same saved-game scene with the player standing still, 10 s settle and 90 s recorded per row (the Tester's logs; the Auditor's recompute agrees to within 0.01 fps). The game itself reaches about 56 fps here, so the 60 and 90 caps do not bind.

| Cap > target | Mode | Native fps | Shown fps | k | Frame time (ms) | Dropped |
|---|---|---|---|---|---|---|
| 30 > 30 | pass-through | 30.00 | 30.00 | 0 | 33.33 | 0 |
| 30 > 60 | generating | 30.00 | 60.00 | 1.00 | 33.33 | 0 |
| 30 > 90 | generating | 30.00 | 90.00 | 2.00 | 33.33 | 0 |
| 30 > 120 | generating | 30.00 | 120.00 | 3.00 | 33.33 | 0 |
| 60 > 30 | pass-through | 55.77 | 55.77 | 0 | 17.93 | 0 |
| 60 > 60 | generating | 53.74 | 60.00 | 0.12 | 18.61 | 0 |
| 60 > 90 | generating | 53.12 | 89.99 | 0.69 | 18.83 | 0 |
| 60 > 120 | generating | 52.41 | 119.99 | 1.29 | 19.08 | 0 |
| 90 > 30 | pass-through | 55.88 | 55.88 | 0 | 17.90 | 0 |
| 90 > 60 | generating | 54.07 | 60.00 | 0.11 | 18.50 | 0 |
| 90 > 90 | generating | 53.36 | 90.00 | 0.69 | 18.74 | 0 |
| 90 > 120 | generating | 52.66 | 119.99 | 1.28 | 18.99 | 0 |

How to read it, besides the notes under the first table:
- The game is GPU-bound at about 56 fps, and its native rate falls when the layer generates: 17.9 ms per frame in pass-through, 18.5 to 19.1 ms while the layer generated (0.1 to 1.3 generated frames per game frame). That is about 0.6 to 1.2 ms more per game frame, growing with the number of generated frames. This is derived from different rows of one session (a second sequence of the same rows agreed within 0.1 ms), not from a controlled on/off test, and it includes frame holding and the capture copies, not only the blend. It is one scene of one game; a CPU-bound game would show nothing (see the Cost note above).
- A target of 60 with a cap of 60 or 90 generates a little (k 0.11 to 0.12) because the game's rate falls below 60 once the layer runs.

## Reporting a problem

Open an issue in this repository and attach `~/.config/vkiframes/live.log` (and `status.json`). They contain the process id, the local times you played and the setting values; in normal use they do not contain your user name, home folder, host name or the game's name. Say which game, which mode (Game Mode or Desktop Mode) and what you set. Do not post anything else personal.

**Security problems:** do not open a public issue. Use the private report on this repository's Security tab ("Report a vulnerability").

*New issues and pull requests are limited to the maintainer until 2026-11-08; the limit is lifted after that date.*

---

