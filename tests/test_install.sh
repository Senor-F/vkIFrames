#!/usr/bin/env bash
# scripts/install.sh and scripts/uninstall.sh in a throw-away HOME (with a space in its name): files, modes, idempotence, upgrade while the old library is open,
# XDG_DATA_HOME set and unset, --use, refusals, uninstall and --purge. No GPU, no network. usage: test_install.sh <repo root>
set -u
REPO=${1:?usage: test_install.sh <repo root>}
T=$(mktemp -d /tmp/vkiinst.XXXXXX); trap 'rm -rf "$T"' EXIT
fails=0
ok() { if eval "$2"; then :; else echo "FAIL: $1 [$2]"; fails=$((fails+1)); fi; }

# a fake release folder: library, manifest template, scripts
REL="$T/release folder"; mkdir -p "$REL/scripts" "$REL/manifest"
cp "$REPO/scripts/install.sh" "$REPO/scripts/uninstall.sh" "$REL/scripts/"; cp "$REPO/manifest/VkLayer_frame_gen.json.in" "$REL/manifest/"
printf '\177ELFlibrary-A' > "$REL/libvkIFrames.so"
printf '\177ELFlibrary-B-longer' > "$T/libB.so"
export HOME="$T/home dir"; mkdir -p "$HOME"; unset XDG_DATA_HOME XDG_CONFIG_HOME
D="$HOME/.local/share"
mode() { stat -c %a "$1"; }

# 1. first install, XDG_DATA_HOME unset
ok "install exits 0" '"$REL/scripts/install.sh" >/dev/null'
ok "library copied" '[ -f "$D/vkiframes/libvkIFrames.so" ] && cmp -s "$REL/libvkIFrames.so" "$D/vkiframes/libvkIFrames.so"'
ok "modes: folder 700, library 755, manifest 644" '[ "$(mode "$D/vkiframes")" = 700 ] && [ "$(mode "$D/vkiframes/libvkIFrames.so")" = 755 ] && [ "$(mode "$D/vulkan/implicit_layer.d/VkLayer_frame_gen.json")" = 644 ]'
ok "manifest is valid JSON and points at the copy (path with a space)" 'python3 -I -c "import json,sys; m=json.load(open(sys.argv[1])); assert m[\"layer\"][\"library_path\"]==sys.argv[2]; assert m[\"layer\"][\"enable_environment\"]=={\"VKIFRAME_ENABLE\":\"1\"}" "$D/vulkan/implicit_layer.d/VkLayer_frame_gen.json" "$D/vkiframes/libvkIFrames.so"'

# 2. again: nothing changes
ino1=$(stat -c %i "$D/vkiframes/libvkIFrames.so"); sum1=$(sha256sum "$D/vulkan/implicit_layer.d/VkLayer_frame_gen.json")
ok "second install exits 0" '"$REL/scripts/install.sh" >/dev/null'
ok "library file untouched (same inode)" '[ "$(stat -c %i "$D/vkiframes/libvkIFrames.so")" = "$ino1" ]'
ok "manifest identical" '[ "$(sha256sum "$D/vulkan/implicit_layer.d/VkLayer_frame_gen.json")" = "$sum1" ]'
ok "no temporary files left" '[ -z "$(ls -A "$D/vkiframes" | grep -v "^libvkIFrames.so$")" ] && [ -z "$(ls -A "$D/vulkan/implicit_layer.d" | grep -v "^VkLayer_frame_gen.json$")" ]'

# 3. upgrade while a game has the old library open: the old file stays valid, the new one is a new file
cp "$T/libB.so" "$REL/libvkIFrames.so"
exec 3< "$D/vkiframes/libvkIFrames.so"
ok "upgrade exits 0" '"$REL/scripts/install.sh" >/dev/null'
ok "new library installed under a new inode" 'cmp -s "$T/libB.so" "$D/vkiframes/libvkIFrames.so" && [ "$(stat -c %i "$D/vkiframes/libvkIFrames.so")" != "$ino1" ]'
ok "the open old library still reads as the old one" '[ "$(cat <&3)" = "$(printf "\177ELFlibrary-A")" ]'
exec 3<&-

# 4. XDG_DATA_HOME set: installs there, leaves the other place alone
X="$T/xdg data"; mkdir -p "$X"
ok "install with XDG_DATA_HOME exits 0" 'XDG_DATA_HOME="$X" "$REL/scripts/install.sh" >/dev/null'
ok "files under XDG_DATA_HOME" '[ -f "$X/vkiframes/libvkIFrames.so" ] && [ -f "$X/vulkan/implicit_layer.d/VkLayer_frame_gen.json" ]'
ok "the manifest there points there" 'grep -F "$X/vkiframes/libvkIFrames.so" "$X/vulkan/implicit_layer.d/VkLayer_frame_gen.json" >/dev/null'

# 5. --use PATH: in place, nothing copied
H2="$T/home2"; mkdir -p "$H2"
ok "--use exits 0" 'HOME="$H2" "$REL/scripts/install.sh" --use "$T/libB.so" >/dev/null'
ok "--use copies nothing and points at the file" '[ ! -e "$H2/.local/share/vkiframes" ] && grep -F "$T/libB.so" "$H2/.local/share/vulkan/implicit_layer.d/VkLayer_frame_gen.json" >/dev/null'

# 6. refusals change nothing
H3="$T/home3"; mkdir -p "$H3/.local/share" "$T/elsewhere"; ln -s "$T/elsewhere" "$H3/.local/share/vkiframes"
ok "a symlinked library folder is refused (exit 1)" 'HOME="$H3" "$REL/scripts/install.sh" >/dev/null 2>&1; [ $? -eq 1 ]'
ok "... and nothing was written" '[ -z "$(ls -A "$T/elsewhere")" ] && [ ! -e "$H3/.local/share/vulkan" ]'
printf 'not an elf' > "$T/notelf.so"
ok "a file that is not an ELF library is refused (exit 1)" 'HOME="$H2" "$REL/scripts/install.sh" --use "$T/notelf.so" >/dev/null 2>&1; [ $? -eq 1 ]'
ok "an unknown argument is refused (exit 1)" '"$REL/scripts/install.sh" --bogus >/dev/null 2>&1; [ $? -eq 1 ]'
ok "missing library is refused (exit 1)" 'rm -f "$T/none.so"; "$REL/scripts/install.sh" --use "$T/none.so" >/dev/null 2>&1; [ $? -eq 1 ]'

# 7. uninstall: removes manifest and library, keeps the settings, says so; --purge asks / --yes
mkdir -p "$HOME/.config/vkiframes"; echo "x" > "$HOME/.config/vkiframes/live.conf"
touch "$D/vkiframes/.libvkIFrames.so.stale"                 # an interrupted install
out=$("$REL/scripts/uninstall.sh"); rc=$?
ok "uninstall exits 0" '[ $rc -eq 0 ]'
ok "manifest and library gone, the folder too" '[ ! -e "$D/vulkan/implicit_layer.d/VkLayer_frame_gen.json" ] && [ ! -e "$D/vkiframes" ]'
ok "settings kept and listed" '[ -f "$HOME/.config/vkiframes/live.conf" ] && echo "$out" | grep -F "$HOME/.config/vkiframes" | grep -q "remove with --purge"'
ok "uninstall twice exits 0" '"$REL/scripts/uninstall.sh" >/dev/null'
ok "--purge without an answer keeps the settings" 'echo n | "$REL/scripts/uninstall.sh" --purge >/dev/null; [ -f "$HOME/.config/vkiframes/live.conf" ]'
ok "--purge --yes removes them" '"$REL/scripts/uninstall.sh" --purge --yes >/dev/null; [ ! -e "$HOME/.config/vkiframes" ]'
ok "uninstall of the XDG install" 'XDG_DATA_HOME="$X" "$REL/scripts/uninstall.sh" >/dev/null; [ ! -e "$X/vkiframes" ] && [ ! -e "$X/vulkan/implicit_layer.d/VkLayer_frame_gen.json" ]'
ok "unknown argument to uninstall exits 1" '"$REL/scripts/uninstall.sh" --bogus >/dev/null 2>&1; [ $? -eq 1 ]'

# 8. syntax
ok "bash -n install.sh" 'bash -n "$REPO/scripts/install.sh"'
ok "bash -n uninstall.sh" 'bash -n "$REPO/scripts/uninstall.sh"'
if command -v shellcheck >/dev/null 2>&1; then ok "shellcheck clean" 'shellcheck "$REPO/scripts/install.sh" "$REPO/scripts/uninstall.sh"'; fi

if [ $fails -eq 0 ]; then echo "install scripts: ok"; else echo "install scripts: $fails FAILED"; fi
exit $((fails > 0))
