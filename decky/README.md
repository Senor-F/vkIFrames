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

**Install from the release zip:** in Decky Loader's developer settings choose "install plugin from ZIP" and pick `vkIFrames-decky-<version>.zip`, then open the plugin's panel and tap Install (or Reinstall): it copies the bundled `bin/libvkIFrames.so` to `~/.local/share/vkiframes/` and writes the layer manifest to `~/.local/share/vulkan/implicit_layer.d/`, the same two files `scripts/install.sh` writes (the one run last wins). "Uninstall layer…" removes those two files; removing the plugin in Decky Loader should remove them too (not yet tested on a Deck). `~/.config/vkiframes` (settings, logs) stays unless you delete it or run `scripts/uninstall.sh --purge` from the layer archive.

Release zip: `decky/scripts/make-zip.sh --lib <libvkIFrames.so> --version <version> --out <dir> [--epoch <unix seconds>]` builds `vkIFrames-decky-<version>.zip` for Decky Loader's own "install plugin from ZIP": one `vkIFrames/` folder with the plugin, `bin/libvkIFrames.so`, the licences and a generated list of the bundled npm packages. It is reproducible (same inputs, same bytes) and checks its own output. The panel's "Install" copies `bin/libvkIFrames.so` to `~/.local/share/vkiframes/` and writes the layer manifest; the plugin runs as the user (`"flags": []`), not as root.

Status: backend tested; the panel builds and runs on a Deck. Layer side: the parent repo (live.conf, live.log, status.json). `settings_schema.json` carries a short description, a finer step and grey-out rules per setting.
