# Live config protocol (plugin -> layer)

The Decky plugin changes layer settings while a game runs. The layer has to read them. Today it reads `FRAMEGEN_*` once at startup with `getenv`.

## File
`$XDG_CONFIG_HOME/vkiframes/live.conf` (default `~/.config/vkiframes/live.conf`). Written atomically by the plugin (temp file, then rename).

```
# written by the vkIFrames Decky plugin; do not edit while a game runs
REV=12345
FRAMEGEN_ENABLED=1
FRAMEGEN_TARGET_FPS=60
...
```

* One `KEY=VALUE` per line. `#` starts a comment. Unknown keys are ignored. Missing keys keep the startup (environment) value.
* `REV` increases on every write.

## Layer side (to implement)
1. Once per second from the present path (or a tiny thread), `stat()` the file. If the mtime or size changed, re-read it. Never block the present path on I/O for more than that one `stat()`.
2. Environment variables are the startup defaults. A key in `live.conf` overrides the environment from then on. Removing the file reverts to the environment values.
3. Apply on the next present for: `ENABLED`, `TARGET_FPS`, `MAX_SYNTHETIC`, `BASE_FPS_CAP`, `MIN_GAIN`, `PHASE_LOCK`, `PHASE_OFFSET`, `WARP_BIDIR` (use the next-swapchain/next-pair rule where a buffer must be rebuilt).
4. `ENABLED=0` must behave like bypass: native frames pass straight through, no added latency.
5. Clamp bad values to the same ranges as `settings_schema.json`; never crash on a malformed line.
6. Optional status (layer -> plugin): write `~/.config/vkiframes/status.json` about once a second, e.g. `{"native_fps":42.1,"output_fps":60.0,"k":1,"mode":"interp"}`. The plugin shows it as is.
7. Benchmark (plugin -> layer): a `BENCH=<number>` line asks for one benchmark run when the number is new (not the one in the file when the game started). The plugin keeps the number in every later write so the layer does not see it change; the run stops if any other setting changes. `status.json` carries `"bench"` (warmup, settle, off, on, done, aborted, or empty) and the layer writes `bench.json` (numbers only) next to it. `STATS=1` writes `stats.jsonl`, one record a second.

## Keeping the plugin in step
`settings_schema.json` lists every setting the plugin shows (key, label, type, default, min, max, step). When a `FRAMEGEN_*` setting is added, renamed, removed, or its default or range changes, update `settings_schema.json` in the same change. The plugin needs no other edit.
