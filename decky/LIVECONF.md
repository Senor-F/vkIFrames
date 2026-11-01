# Live config protocol (plugin -> layer)

The Decky plugin changes layer settings while a game runs. The layer implements the protocol in `src/live_config.cpp`: it reads `VKIFRAME_*` (with the `FRAMEGEN_*` fallback) at startup, re-reads the file when its mtime or size changes, and logs every change as `live: CHANGE KEY: old -> new (live.conf)` in `live.log`.

**The plugin is the only writer today, and it is one-way.** It rewrites the whole file from its saved `settings.json` at every loader start and at every panel change, and never reads `live.conf` back. An edit from outside (a script, `vkiframes-ctl`, the planned overlay) is overwritten at the next loader restart or panel change and is not shown in the panel. Do not restart the loader or save in the panel during a benchmark or a capture. The intended fix (the plugin merges the file at start, or the file becomes the source of truth) is an open decision for the owner; it must be settled before the overlay and `vkiframes-ctl` write the same file.

## File
`$XDG_CONFIG_HOME/vkiframes/live.conf` (default `~/.config/vkiframes/live.conf`; the environment variables `VKIFRAME_LIVE_CONF` and `VKIFRAME_LIVE_LOG` override the paths of `live.conf` and `live.log`). Written atomically by the plugin (temp file, then rename).

```
# written by the vkIFrames Decky plugin; do not edit while a game runs
REV=12345
VKIFRAME_ENABLED=1
VKIFRAME_TARGET_FPS=60
...
```

* One `KEY=VALUE` per line. `#` starts a comment. Unknown keys are ignored. Missing keys keep the startup (environment) value.
* `REV` increases on every write.

## Layer side
1. Once per second from the present path (or a tiny thread), `stat()` the file. If the mtime or size changed, re-read it. Never block the present path on I/O for more than that one `stat()`.
2. Environment variables are the startup defaults. A key in `live.conf` overrides the environment from then on. Removing the file reverts to the environment values.
3. Live keys (`src/live_config.cpp`): `ENABLED`, `TARGET_FPS`, `MAX_SYNTHETIC`, `BASE_FPS_CAP`, `MIN_GAIN`, `PHASE_LOCK`, `PHASE_OFFSET`, `INTERP_BIDIR`, `CMD_BUFFERS`, `SYNTH_MARGIN`, `STATS`, `BENCH`. Other keys (`INTERPOLATE`, `FLOW_LEVEL`, `EXPERIMENTAL`, `EXTRAPOLATE`, `UI_MASK_THRESHOLD`) are read at game start. `INTERP_BIDIR` (old alias `WARP_BIDIR`) applies from the next pair; the others apply on the next present; `SYNTH_MARGIN` is 0..0.45. Use the next-swapchain/next-pair rule where a buffer must be rebuilt.
4. `ENABLED=0` must behave like bypass: native frames pass straight through, no added latency.
5. Clamp bad values to the same ranges as `settings_schema.json`; never crash on a malformed line.
6. Optional status (layer -> plugin): write `~/.config/vkiframes/status.json` about once a second, e.g. `{"native_fps":42.1,"output_fps":60.0,"k":1,"mode":"interp","enabled":1,"refresh_hz":60.00,"bench":""}`. `mode` is `interp`, `extrap`, `bypass` or `off`; `refresh_hz` is 0 when the swapchain's refresh rate is unknown. The plugin shows it as is.
7. Benchmark (plugin -> layer): a `BENCH=<number>` line asks for one benchmark run when the number is new (not the one in the file when the game started). The plugin keeps the number in every later write so the layer does not see it change; the run stops if any other setting changes. `status.json` carries `"bench"` (warmup, settle, off, on, done, aborted, or empty) and the layer writes `bench.json` (numbers only) next to it. `STATS=1` writes `stats.jsonl`, one record a second.

## Keeping the plugin in step
`settings_schema.json` lists every setting the plugin shows (key, label, type, default, min, max, step). When a `VKIFRAME_*` setting is added, renamed, removed, or its default or range changes, update `settings_schema.json` in the same change. The plugin needs no other edit.
