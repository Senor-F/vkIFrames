# vkIFrames Decky plugin

Quick-access panel on the Steam Deck for the vkIFrames Vulkan layer: install or remove the layer, and change its settings while a game runs.

* `main.py` Python backend: writes `~/.config/vkiframes/live.conf`, installs the layer manifest.
* `src/index.tsx` the panel.
* `settings_schema.json` the list of settings shown. Keep it in step with the layer (see LIVECONF.md).
* `LIVECONF.md` the file protocol the layer must follow.
* `scripts/deploy.sh` copy to Decky's plugin folder and restart the loader (Decky has no hot reload).
* `scripts/link-build.sh` point the layer manifest at a local build so rebuilds go live without copying.
* `tests/test_backend.py` backend tests, no Deck needed (`python3 tests/test_backend.py`).

Build (from this `decky/` folder): `pnpm i && pnpm run build`, then `DECK=deck@<ip> scripts/deploy.sh`. Put the layer library in `bin/libvkIFrames.so` to bundle it.

Status: backend tested; the panel builds and runs on a Deck. Layer side: the parent repo (live.conf, live.log, status.json). `settings_schema.json` carries a short description, a finer step and grey-out rules per setting.
