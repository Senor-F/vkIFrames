# Project index (vkIFrames)

Format: `path | what it does (12 words at most) | search words`. Find files: `rg -i <word> INDEX.md index/`.
Update this file in the same commit that adds, moves or deletes a file.

## Code: layer (src/, include/)
src/layer_dispatch.cpp | loader negotiation, GetProcAddr routing, dispatch-table registries | negotiate, GetInstanceProcAddr, GetDeviceProcAddr, dispatch table, HOOK, FrameGen_Negotiate
src/layer_main.cpp | all hooks: instance/device/swapchain create, present hook, presenter thread, hold-one | Hook_QueuePresentKHR, Hook_CreateSwapchainKHR, PresenterMain, StopPresenter, hold, cap mode, base fps cap, acquire, fence
include/vk_iframes_dispatch.hpp | dispatch tables, function lists, instance/device/queue/swapchain registries | InstanceDispatch, DeviceDispatch, FRAMEGEN_DEVICE_FUNCS, RegisterDevice, QueueMutex, GetKey
include/vk_iframes_config.hpp | Config struct, VKIFRAME_* env parsing, machine defaults, GetConfig | Config, EnvVar, EnvFlag, ParseFiniteNumber, MachineDefaultsFor, GetConfig, FG_LOG, FRAMEGEN_EXPORT
include/vk_iframes_layer.hpp | shared declarations: dispatch tables, SwapchainState, hook list, log macros | SwapchainState, DeviceDispatch, FG_LOG, FRAMEGEN_EXPORT, CAP_MODE, gpu_mu
src/live_config.cpp | settings parser: VKIFRAME_* env, live.conf polling, clamping, Apply | live.conf, LiveSettings, parse, clamp, REV, hostile env, FRAMEGEN_ fallback, default
src/live_config_io.cpp | live config files and logs: live.log rotation, status.json, stats.jsonl | live.log, ReadOwnFile, ConfDir, status.json, stats.jsonl, rotate
src/live_config_internal.hpp | internal declarations shared by the two live_config sources | liveconf, ConfDir, LogLine, ReadOwnFile
include/live_config.hpp | LiveSettings struct and live-config API | LiveSettings, live.conf, VKIFRAME_LIVE_CONF, status.json
include/timing_engine.hpp | TimingEngine: EMA frame-time filter, smoothed arrival times | TimingEngine, EMA, frame time, alpha, jitter
include/output_clock.hpp | PairScheduler: bypass gate, output clock, per-tick t, jitter buffer | PairScheduler, pacer, bypass, kmax, tick, content delay, target fps, ratio
src/present_timing.cpp | VK_EXT_present_timing: time domains, past presentation timings, refresh duration | present_timing, time domain, vblank, refresh rate, PastPresentationTiming
include/present_timing.hpp | present-timing helpers declarations | present_timing, timestamp, time domain
src/gpu_frames.cpp | GpuState: init, capture, held present, synthetic frame passes, command buffers | GpuInit, GpuCapture, GpuSynthesize, barrier, semaphore, extrapolation, interpolate
src/gpu_setup.cpp | ring slot and blend pass creation, GpuDestroy teardown, dump pool recorder | CreateSlot, CreateBlendPass, GpuDestroy, DumpInit, DumpRecord, dump, teardown
include/gpu_frames.hpp | GpuState struct: ring images, pipelines, per-slot state | GpuState, ring, slot, pipeline, descriptor, hidden image
include/gpu_internal.hpp | helpers shared by gpu_frames and optical_flow: barriers, memory, images | barrier, Synchronization2, memory type, image helper
src/gpu_images.cpp | optical-flow images and memory-type lookup (mock-linkable) | memory type, flow images, TRANSFER_SRC, FindMemoryType
src/optical_flow.cpp | GPU optical flow: pyramid, coarse search, refinement, occlusion, pipelines | optical flow, GpuFlow, pyramid, flow_coarse, SPIR-V, VKIFRAME_OPTICAL_FLOW
include/flow_ref.hpp | CPU reference of flow passes and interpolation; mirrors shaders | flowref, InterpPixel, FillConsistRef, reference, Down
src/stats.cpp | measurement logic: stats.jsonl, status line, bench schedule, bench.json | VKIFRAME_STATS, stats.jsonl, bench.json, bench.req, percentile, benchmark, status
include/stats.hpp | stats and benchmark API, status.json fields | stats, bench, percentile, status.json, cap_wait_ms
src/dump_pool.cpp | VKIFRAME_DUMP: host-visible buffer pool, allocated once at start | VKIFRAME_DUMP, dump pool, capture, host visible
include/dump_pool.hpp | dump buffer pool declarations | dump pool, VKIFRAME_DUMP
src/dump_queue.cpp | VKIFRAME_DUMP: writes capture files together after the window | VKIFRAME_DUMP, dump queue, times.txt, ppm, FAILED
include/dump_queue.hpp | dump file queue declarations | dump queue, VKIFRAME_DUMP
scripts/layer.map | linker version script: exports exactly the three entry points | layer.map, exports, symbols, version script

## Shaders (shaders/*.comp, GLSL compute; compiled to SPIR-V and embedded by CMake)
shaders/blend.comp | plain temporal cross-fade of two frames (default synthetic frame) | blend, cross-fade, synthetic frame
shaders/flow_luma.comp | luma and level-1 pyramid of a ring image | flow_luma, luma, pyramid
shaders/flow_down.comp | 2x2 box downsample for pyramid levels 2..4 | flow_down, downsample, pyramid
shaders/flow_coarse.comp | exhaustive block matching on the coarsest level | flow_coarse, block matching, RADIUS, coarse
shaders/flow_refine.comp | per-level flow refinement from the coarser level | flow_refine, refine, candidates
shaders/flow_occlusion.comp | forward/backward consistency check, occlusion mask | flow_occlusion, occlusion, mask
shaders/flow_interp.comp | backward-gather interpolation at time t, one- and two-sided fill, extrapolation | flow_interp, apply_fill, ONESIDED_FILL, extrap, backward gather
shaders/flow_vis.comp | debug picture of the motion field | flow_vis, VKIFRAME_DEBUG_FLOW, debug view

## Build, packaging, install (CMakeLists.txt, cmake/, manifest/, scripts/)
CMakeLists.txt | build: library, shader embedding, tests, sanitizer and static-runtime options | cmake, FRAMEGEN_SANITIZER, FRAMEGEN_STATIC_RUNTIME, glslang, ctest, add_test
cmake/GenSchemaTable.cmake | generates C++ settings table from decky/settings_schema.json | schema table, GenSchemaTable, settings_schema
manifest/VkLayer_frame_gen.json.in | implicit layer manifest template | manifest, implicit layer, json, VK_LAYER, enable_environment
manifest/live.conf.template | commented live.conf installed for the user | live.conf, template, settings header
scripts/install.sh | installs the layer for the current user, no root | install, implicit_layer.d, ~/.local/share
scripts/uninstall.sh | removes manifest and library copy | uninstall, remove
scripts/package.sh | builds release tar from a clean checkout, SHA256SUMS | package, release, tar.gz, SHA256SUMS, reproducible
scripts/repro_check.sh | builds twice, compares libraries byte for byte | reproducible build, repro, byte identical
scripts/check_so.sh | release checks on the library: exports, glibc floor, RELRO | check_so, glibc, RELRO, imports, exports
scripts/check_env.sh | reports whether this machine can build and test | check_env, toolchain, environment report
scripts/gamemode_launch.sh | Steam launch wrapper for Game Mode tests | gamemode, launch, wrapper, gamescope
scripts/vkiframes-bench.sh | runs the layer-off/on benchmark in the running game | bench, benchmark, vkiframes-bench, bench.req

## Decky plugin (decky/)
decky/main.py | plugin backend: install/remove layer, read and write live.conf, status | Decky, backend, live.conf, install, status text, launch options
decky/src/index.tsx | quick-access panel UI (React) | panel, tsx, slider, toggle, QuickAccess
decky/src/types.d.ts | TypeScript type declarations for the panel | types
decky/settings_schema.json | settings the panel shows: key, default, range, step | settings_schema, default, range, step, VKIFRAME_
decky/LIVECONF.md | live.conf protocol, plugin to layer | live config, REV, protocol, live.conf, hotkeys, vkiframes-ctl
decky/README.md | plugin readme: build, install, dev loop | decky readme, pnpm, deploy
decky/scripts/make-zip.sh | builds the Decky plugin zip, reproducible | make-zip, Decky zip, plugin zip, THIRD_PARTY_LICENSES
decky/scripts/deploy.sh | dev loop: copy build to the Deck and reload | deploy, dev loop, DECK
decky/scripts/link-build.sh | dev helper: manifest pointing at a local build | link-build, local build, manifest
decky/package.json, decky/plugin.json, decky/rollup.config.js, decky/tsconfig.json | plugin build config, metadata, bundling | package.json, plugin.json, rollup, tsconfig, version
decky/decky.pyi | Decky Loader API stub (vendored, read-only) | decky module, DECKY_PLUGIN_SETTINGS_DIR, stub
decky/pnpm-lock.yaml | pnpm lock file (read-only, do not edit) | lock, pnpm, dependencies
decky/LICENSE | plugin licence text (read-only) | licence, license
decky/bin/.gitkeep, decky/.gitignore | keeps bin/ folder; plugin ignore list | gitignore

## Tests: layer (tests/, ctest; mock drivers, no GPU needed)
tests/test_chain.cpp, tests/test_instance_ext.cpp | loader negotiation, chaining, CreateInstance extensions | mock loader, negotiation, chain, proc-addr
tests/test_timing.cpp | present timing: extension and feature injection, forced FIFO | present_timing, forced FIFO, swapchain flags
tests/test_hold.cpp, tests/test_pacer.cpp | hold-one sequencing; presenter thread pacing, locking, polled acquire | hold, presenter, pacing, acquire, queue lock
tests/test_scheduler.cpp, tests/data/ | PairScheduler replay of clock_sim traces | scheduler, PairScheduler, ref_ traces, export_ref
tests/test_synth.cpp, tests/test_extrap.cpp, tests/test_capacity.cpp, tests/test_cmdreuse.cpp | synthetic frames, extrapolation, capacity, command reuse | synthetic, blend, extrapolation, hidden acquire
tests/test_blendtime.cpp, tests/test_passthru.cpp | blend pass timing; status line on passthrough swapchains | blend time, timestamps, passthrough, status.json
tests/test_flow_ref.cpp, tests/test_flow_images.cpp, tests/test_fill_ref.cpp, tests/test_fill_gate.cpp | flow reference, flow images, one-sided fill and its gate | flow_ref, fill, ONESIDED_FILL, EXPERIMENTAL
tests/test_live.cpp, tests/test_live_template.py, tests/test_template_parse.cpp, tests/test_capacity_live.cpp | live.conf parsing, clamping, template, live capacity | live.conf, parse, clamp, template, REV
tests/test_defaults.cpp, tests/test_flowkey.cpp, tests/test_hostile_env.cpp | defaults versus schema, renamed keys, bad env values | defaults, schema, FRAMEGEN_, nan, hostile, VKIFRAME_OPTICAL_FLOW
tests/test_stats.cpp, tests/test_capstats.cpp, tests/test_capmodes.cpp, tests/test_logcap.cpp | stats, bench.json, cap modes, log size caps | stats, bench, cap mode, late, early, hybrid, rotate
tests/test_dump.cpp, tests/test_dump_pool.cpp | VKIFRAME_DUMP queue and buffer pool failures | dump, capture, pool, allocation failure
tests/test_destroy_race.cpp, tests/test_detach.cpp, tests/test_oom.cpp | shutdown races, stuck presenter, out-of-memory | race, detach, destroy swapchain, oom, bad_alloc
tests/test_hostlib.cpp, tests/hoststd.cpp, tests/clean_env.hpp, tests/tsan.supp | own C++ runtime check, env cleaning, TSan suppressions | hoststd, static runtime, clean_env, tsan
tests/test_package.sh, tests/test_decky_zip.sh, tests/test_install.sh, tests/test_check_so.sh, tests/test_bench_script.sh | shell tests of scripts and archives | package, zip, install, check_so, bench script, test_*.sh
tests/data/ref_*.txt | test reference data (clock-sim traces), read-only; see index/data.md | ref_steady, ref_jit, ref_ramp, ref_wobble, trace, reference
decky/tests/test_*.py | Decky backend tests: clamp, launch options, status text, symlinks | decky backend, launch options, status text, symlink, script agreement

## Tools
INDEX.md | this index: path, purpose, search words, by area | index, find files, rg INDEX.md
tools/check_docs.py | checks doc sizes, links and index coverage; runs at configure | check_docs, 150 lines, broken link, index coverage, VKIFRAMES_CHECK_DOCS
tools/code_size.py | measures file, function and change size against the limits | code_size, 500 lines, 40 lines, function length, over limit
tools/make_data_map.py | writes index/data.md for read-only data files | make_data_map, data map, index/data.md
tools/clock_sim/clock_sim.py | models the output clock: EMA, bypass gate, jitter buffer | clock_sim, output clock, simulation
tools/clock_sim/export_ref.py | writes reference traces for test_scheduler into tests/data | export_ref, ref traces, tests/data
tools/clock_sim/frame_sources.py | synthetic game frame-time patterns for the simulator | frame_sources, jitter, steady, wobble, ramp
tools/interp_eval/interp_eval.cpp | offline scoring of interpolation methods on a real middle frame | interp_eval, PSNR, offline, FillConsistRef, VKIFRAME_DUMP

## Docs (indexes of parts; text lives in the folders)
README.md | user README: help, install, limits, config, how it works, build | readme, install, known issues, launch options, config, sanitizer builds, project layout
readme/*.md | README parts: troubleshooting, how it works, quick start, live settings | If something goes wrong, Tested, matrix, Configuration, Project layout, Evaluating changes offline
DESIGN.md | architecture blueprint: rate matching, output clock, hold-one, flow, interpolation | design, architecture, ratio, output clock, t, held frame
STATUS.md | implementation status and measurements | status, measurements, resume point, alpha
TARGETS.md | target machines (Deck, desktop), environment, gamescope notes | Deck, gamescope, Mesa, target, present_timing support
ASYNC.md | async layer design note and early measurements | async, compute queue, VKIFRAME_ASYNC, command pools
ROADMAP.md, roadmap/ | stage roadmap and plans after the alpha | roadmap, plan, GPU load, upscaler, D3D hint
STAGE1.md, stage1/ | stage 1 spec: skeleton, dispatch tables, hooks, manifest | stage 1, skeleton, dispatch, manifest, hooks
STAGE2.md, stage2/ | stage 2 spec: timing engine, VK_EXT_present_timing | stage 2, timing, EMA, time domain
STAGE3.md, stage3/ | stage 3 spec: hold-one, ring buffer, output clock, present fences | stage 3, hold-one, ring, present_id, present fences, vkWaitForPresentKHR
STAGE4.md, stage4/ | stage 4 spec: optical flow shaders and engine, async queue plan | stage 4, optical flow, pyramid, async queue plan
STAGE5.md | stage 5: interpolation shader, status and verification | stage 5, interpolation, backward gather, UI mask
STAGE6.md, stage6/ | stage 6: resize, config, packaging, performance budget | stage 6, swapchain recreation, packaging, performance budget, checklist
AUDIT.md, audit/ | Auditor notes: priority list, review log (running log) | audit, review log, sanitizer, residuals, priorities
CLAUDE.md | rules for every session: roles, commits, anonymity, design decisions, project rules | rules, roles, Co-Authored-By, anonymity, TZ

## Data
index/data.md | data map: size, purpose, section start lines of read-only data | data map, ref traces, pnpm-lock, offset, make_data_map
tests/data/ref_*.txt | clock-sim reference traces, read-only; see [index/data.md](index/data.md) | ref_steady, ref_jit, ref_hitch, trace

## Third-party and generated
LICENSE | Apache 2.0 licence text (read-only) | license, Apache
NOTICE | project notice file (read-only) | notice, attribution, copyright
decky/pnpm-lock.yaml, decky/LICENSE, decky/decky.pyi | read-only vendored or lock files (see Decky plugin) | lock, vendored
build/, generated/, *.spv | build output, git-ignored; generated: edit CMakeLists.txt and shaders instead | build, generated, spv, SPIR-V
.gitattributes, .gitignore | line-ending rules and ignore list | eol, gitignore
