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
cp "$REPO/scripts/install.sh" "$REPO/scripts/uninstall.sh" "$REL/scripts/"; cp "$REPO/manifest/VkLayer_frame_gen.json.in" "$REPO/manifest/live.conf.template" "$REL/manifest/"
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
# shellcheck disable=SC2034  # used inside the eval strings of ok()
ino1=$(stat -c %i "$D/vkiframes/libvkIFrames.so")
# shellcheck disable=SC2034
sum1=$(sha256sum "$D/vulkan/implicit_layer.d/VkLayer_frame_gen.json")
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
# shellcheck disable=SC2034  # used inside the eval strings of ok()
out=$("$REL/scripts/uninstall.sh")
# shellcheck disable=SC2034
rc=$?
ok "uninstall exits 0" '[ $rc -eq 0 ]'
ok "manifest and library gone, the folder too" '[ ! -e "$D/vulkan/implicit_layer.d/VkLayer_frame_gen.json" ] && [ ! -e "$D/vkiframes" ]'
ok "settings kept and listed" '[ -f "$HOME/.config/vkiframes/live.conf" ] && echo "$out" | grep -F "$HOME/.config/vkiframes" | grep -q "remove with --purge"'
ok "uninstall twice exits 0" '"$REL/scripts/uninstall.sh" >/dev/null'
ok "--purge without an answer keeps the settings" 'echo n | "$REL/scripts/uninstall.sh" --purge >/dev/null; [ -f "$HOME/.config/vkiframes/live.conf" ]'
ok "--purge --yes removes them" '"$REL/scripts/uninstall.sh" --purge --yes >/dev/null; [ ! -e "$HOME/.config/vkiframes" ]'
ok "uninstall of the XDG install" 'XDG_DATA_HOME="$X" "$REL/scripts/uninstall.sh" >/dev/null; [ ! -e "$X/vkiframes" ] && [ ! -e "$X/vulkan/implicit_layer.d/VkLayer_frame_gen.json" ]'
ok "unknown argument to uninstall exits 1" '"$REL/scripts/uninstall.sh" --bogus >/dev/null 2>&1; [ $? -eq 1 ]'

# 7b. characters that are special to the shell, to JSON or to bash 5.2's "&" in a replacement: a HOME and a --use path containing &, a backslash and a double quote
WEIRD="$T/we&ird \\ \"q\" home"; mkdir -p "$WEIRD"
ok "install into a HOME with & backslash and quote exits 0" 'HOME="$WEIRD" "$REL/scripts/install.sh" >/dev/null'
ok "the manifest is valid JSON with exactly that path" 'python3 -I -c "import json,sys; m=json.load(open(sys.argv[1])); assert m[\"layer\"][\"library_path\"]==sys.argv[2], m[\"layer\"][\"library_path\"]" "$WEIRD/.local/share/vulkan/implicit_layer.d/VkLayer_frame_gen.json" "$WEIRD/.local/share/vkiframes/libvkIFrames.so"'
WL="$T/a&b \\ \"x\"/lib.so"; mkdir -p "$(dirname "$WL")"; cp "$T/libB.so" "$WL"
ok "--use with & backslash and quote exits 0" 'HOME="$H2" "$REL/scripts/install.sh" --use "$WL" >/dev/null'
ok "... and the manifest has exactly that path" 'python3 -I -c "import json,sys; m=json.load(open(sys.argv[1])); assert m[\"layer\"][\"library_path\"]==sys.argv[2], m[\"layer\"][\"library_path\"]" "$H2/.local/share/vulkan/implicit_layer.d/VkLayer_frame_gen.json" "$WL"'
ok "uninstall from that HOME" 'HOME="$WEIRD" "$REL/scripts/uninstall.sh" >/dev/null && [ ! -e "$WEIRD/.local/share/vkiframes" ]'

# 7c. relative XDG folders are refused before anything is written or removed (both scripts)
mkdir -p "$T/cwd/rel/vkiframes"; echo keep > "$T/cwd/rel/vkiframes/live.conf"
ok "install refuses a relative XDG_DATA_HOME (exit 1)" '(cd "$T/cwd" && XDG_DATA_HOME=rel "$REL/scripts/install.sh" >/dev/null 2>&1; [ $? -eq 1 ])'
ok "install refuses a relative XDG_CONFIG_HOME (exit 1)" '(cd "$T/cwd" && XDG_CONFIG_HOME=rel "$REL/scripts/install.sh" >/dev/null 2>&1; [ $? -eq 1 ])'
ok "uninstall --purge --yes refuses a relative XDG_CONFIG_HOME (exit 1) and removes nothing" '(cd "$T/cwd" && XDG_CONFIG_HOME=rel "$REL/scripts/uninstall.sh" --purge --yes >/dev/null 2>&1; [ $? -eq 1 ]) && [ -f "$T/cwd/rel/vkiframes/live.conf" ]'
ok "uninstall refuses a relative XDG_DATA_HOME (exit 1)" '(cd "$T/cwd" && XDG_DATA_HOME=rel "$REL/scripts/uninstall.sh" >/dev/null 2>&1; [ $? -eq 1 ])'
ok "nothing was created under the relative folder" '[ ! -e "$T/cwd/rel/vulkan" ] && [ ! -e "$T/cwd/rel/vkiframes/libvkIFrames.so" ]'

# 7d. a symbolic link in the path: install refuses and changes nothing, uninstall removes nothing through it, and both scripts use the same rule text
rule_text() { sed -n '/^# --- the symbolic-link rule/,/^# --- end of the rule/p' "$1"; }
ok "install.sh and uninstall.sh carry the same link rule" '[ -n "$(rule_text "$REPO/scripts/install.sh")" ] && [ "$(rule_text "$REPO/scripts/install.sh")" = "$(rule_text "$REPO/scripts/uninstall.sh")" ]'
sums() { find "$1" -type f -print0 | sort -z | xargs -0 sha256sum; }
for LINK in .local/share/vkiframes .local/share/vulkan .local/share/vulkan/implicit_layer.d; do
    N=$(echo "$LINK" | tr / _); HL="$T/hl$N"; TG="$T/tgt$N"
    mkdir -p "$HL/.local/share" "$TG/implicit_layer.d"                       # whichever folder the link points at holds files with the layer's names
    for f in libvkIFrames.so .libvkIFrames.so.stale VkLayer_frame_gen.json implicit_layer.d/VkLayer_frame_gen.json; do echo keep > "$TG/$f"; done
    case "$LINK" in
        .local/share/vkiframes) ln -s "$TG" "$HL/.local/share/vkiframes" ;;
        .local/share/vulkan) ln -s "$TG" "$HL/.local/share/vulkan" ;;
        *) mkdir -p "$HL/.local/share/vulkan"; ln -s "$TG" "$HL/.local/share/vulkan/implicit_layer.d" ;;
    esac
    # shellcheck disable=SC2034  # used inside the eval strings of ok()
    before=$(sums "$TG")
    ok "install refuses a link at $LINK (exit 1)" 'HOME="$HL" "$REL/scripts/install.sh" >/dev/null 2>&1; [ $? -eq 1 ]'
    ok "... and nothing behind it changed" '[ "$(sums "$TG")" = "$before" ]'
    ok "uninstall with a link at $LINK exits 1" 'HOME="$HL" "$REL/scripts/uninstall.sh" >/dev/null 2>&1; [ $? -eq 1 ]'
    ok "... and removed nothing behind it" '[ "$(sums "$TG")" = "$before" ]'
done
# a link at XDG_DATA_HOME itself is the user's choice and works
XL="$T/xdgreal"; mkdir -p "$XL"; ln -s "$XL" "$T/xdglink"
ok "XDG_DATA_HOME itself may be a link (install, uninstall)" 'XDG_DATA_HOME="$T/xdglink" "$REL/scripts/install.sh" >/dev/null && [ -f "$XL/vkiframes/libvkIFrames.so" ] && XDG_DATA_HOME="$T/xdglink" "$REL/scripts/uninstall.sh" >/dev/null && [ ! -e "$XL/vkiframes" ]'

# 7e. the settings file for people without the Decky panel: install.sh creates ~/.config/vkiframes/live.conf from manifest/live.conf.template when there is none (a
# comment-only file), never touches an existing one, and whatever goes wrong there is a warning that does not change the exit status or the layer install.
# shellcheck disable=SC2034  # used inside the eval strings of ok()
TPL="$REPO/manifest/live.conf.template"
live_installed() { [ -f "$1/.local/share/vkiframes/libvkIFrames.so" ] && [ -f "$1/.local/share/vulkan/implicit_layer.d/VkLayer_frame_gen.json" ]; }
active_lines() { grep -c -v -e '^[[:space:]]*#' -e '^[[:space:]]*$' "$1" || true; }
H9="$T/home9"; mkdir -p "$H9"
ok "first install creates live.conf (exit 0, says so)" 'out=$(HOME="$H9" "$REL/scripts/install.sh" 2>&1); [ $? -eq 0 ] && echo "$out" | grep -q "settings file created"'
ok "it is the template, mode 600, in a 700 folder, no temporary file left" 'cmp -s "$TPL" "$H9/.config/vkiframes/live.conf" && [ "$(mode "$H9/.config/vkiframes/live.conf")" = 600 ] && [ "$(mode "$H9/.config/vkiframes")" = 700 ] && [ "$(ls -A "$H9/.config/vkiframes")" = live.conf ]'
ok "every line is a comment or blank: no active setting" '[ "$(active_lines "$H9/.config/vkiframes/live.conf")" = 0 ]'
# shellcheck disable=SC2034
ino9=$(stat -c %i "$H9/.config/vkiframes/live.conf")
# shellcheck disable=SC2034
mt9=$(stat -c %Y "$H9/.config/vkiframes/live.conf")
ok "a second install leaves it alone (same inode, same time, says so)" 'out=$(HOME="$H9" "$REL/scripts/install.sh" 2>&1); [ $? -eq 0 ] && echo "$out" | grep -q "left as it is" && [ "$(stat -c %i "$H9/.config/vkiframes/live.conf")" = "$ino9" ] && [ "$(stat -c %Y "$H9/.config/vkiframes/live.conf")" = "$mt9" ] && [ "$(ls -A "$H9/.config/vkiframes")" = live.conf ]'
ok "uninstall keeps it; --purge --yes removes the folder" 'HOME="$H9" "$REL/scripts/uninstall.sh" >/dev/null && [ -f "$H9/.config/vkiframes/live.conf" ] && HOME="$H9" "$REL/scripts/uninstall.sh" --purge --yes >/dev/null && [ ! -e "$H9/.config/vkiframes" ]'
# an existing file is never touched: one with settings (the Decky panel's), an empty one, one without a trailing newline
for kind in settings empty partial; do
    HK="$T/home9-$kind"; mkdir -p "$HK/.config/vkiframes"
    case $kind in settings) printf '# written by the vkIFrames Decky plugin\nREV=5\nVKIFRAME_TARGET_FPS=75\n' > "$HK/.config/vkiframes/live.conf" ;; empty) : > "$HK/.config/vkiframes/live.conf" ;; partial) printf 'TARGET_FPS=60' > "$HK/.config/vkiframes/live.conf" ;; esac
    chmod 644 "$HK/.config/vkiframes/live.conf"; touch -d '2020-01-01 00:00:00' "$HK/.config/vkiframes/live.conf"
    # shellcheck disable=SC2034
    sumk=$(sha256sum "$HK/.config/vkiframes/live.conf")
    ok "an existing live.conf ($kind) is untouched: content, mode and time, nothing else created" 'HOME="$HK" "$REL/scripts/install.sh" >/dev/null && [ "$(sha256sum "$HK/.config/vkiframes/live.conf")" = "$sumk" ] && [ "$(mode "$HK/.config/vkiframes/live.conf")" = 644 ] && [ "$(stat -c %Y "$HK/.config/vkiframes/live.conf")" = "$(date -d "2020-01-01 00:00:00" +%s)" ] && [ "$(ls -A "$HK/.config/vkiframes")" = live.conf ] && live_installed "$HK"'
done
# a symbolic link anywhere on the way, or at the file: warning, exit 0, nothing written through it, the layer still installed
for spot in folder ancestor file; do
    HL="$T/home9-link-$spot"; mkdir -p "$HL" "$T/elsewhere-$spot"
    case $spot in
        folder)   mkdir -p "$HL/.config"; ln -s "$T/elsewhere-$spot" "$HL/.config/vkiframes" ;;
        ancestor) ln -s "$T/elsewhere-$spot" "$HL/.config" ;;
        file)     mkdir -p "$HL/.config/vkiframes"; ln -s "$T/elsewhere-$spot/target" "$HL/.config/vkiframes/live.conf" ;;
    esac
    ok "a symbolic link at the $spot: exit 0, a note, nothing written through it, the layer installed" 'out=$(HOME="$HL" "$REL/scripts/install.sh" 2>&1); [ $? -eq 0 ] && live_installed "$HL" && [ -z "$(ls -A "$T/elsewhere-$spot")" ] && echo "$out" | grep -Eq "symbolic link|left as it is"'
done
ok "... and the link at the file is still a link and its target was not created" '[ -L "$T/home9-link-file/.config/vkiframes/live.conf" ] && [ ! -e "$T/elsewhere-file/target" ]'
# something else at the path: a folder, a FIFO (must not block), the folder is not ours / not writable
HD="$T/home9-dir"; mkdir -p "$HD/.config/vkiframes/live.conf"
ok "a folder at the path is left alone (exit 0)" 'HOME="$HD" "$REL/scripts/install.sh" >/dev/null 2>&1; [ $? -eq 0 ] && [ -d "$HD/.config/vkiframes/live.conf" ] && live_installed "$HD"'
HF="$T/home9-fifo"; mkdir -p "$HF/.config/vkiframes"; mkfifo "$HF/.config/vkiframes/live.conf"
ok "a FIFO at the path is left alone and does not block" 'timeout 20 env HOME="$HF" "$REL/scripts/install.sh" >/dev/null 2>&1; [ $? -eq 0 ] && [ -p "$HF/.config/vkiframes/live.conf" ] && live_installed "$HF"'
HR="$T/home9-ro"; mkdir -p "$HR/.config/vkiframes"; chmod 500 "$HR/.config/vkiframes"
ok "a config folder we cannot write in: exit 0, a note, the layer installed" 'out=$(HOME="$HR" "$REL/scripts/install.sh" 2>&1); rc=$?; chmod 700 "$HR/.config/vkiframes"; [ $rc -eq 0 ] && live_installed "$HR" && echo "$out" | grep -q "note:" && [ ! -e "$HR/.config/vkiframes/live.conf" ]'
# no template in the release folder: the same
mv "$REL/manifest/live.conf.template" "$T/tpl.keep"; HM="$T/home9-notpl"; mkdir -p "$HM"
ok "no template: exit 0, a note, the layer installed, no file" 'out=$(HOME="$HM" "$REL/scripts/install.sh" 2>&1); [ $? -eq 0 ] && live_installed "$HM" && echo "$out" | grep -q "template is missing" && [ ! -e "$HM/.config/vkiframes/live.conf" ]'
mv "$T/tpl.keep" "$REL/manifest/live.conf.template"
# XDG_CONFIG_HOME: used when set; a link there, quotes and a newline in the path
XC="$T/xdg config \"q\" & \$x"; mkdir -p "$XC"; HX="$T/home9-xdg"; mkdir -p "$HX"
ok "XDG_CONFIG_HOME is used (path with spaces, quotes, & and a dollar sign)" 'HOME="$HX" XDG_CONFIG_HOME="$XC" "$REL/scripts/install.sh" >/dev/null && cmp -s "$TPL" "$XC/vkiframes/live.conf" && [ ! -e "$HX/.config" ]'
XL="$T/xdgcfgdest"; mkdir -p "$XL"; ln -s "$XL" "$T/xdgcfglink"; HY="$T/home9-xdglink"; mkdir -p "$HY"
ok "XDG_CONFIG_HOME that is itself a link outside HOME is the user's choice (the file is created in it)" 'HOME="$HY" XDG_CONFIG_HOME="$T/xdgcfglink" "$REL/scripts/install.sh" >/dev/null && [ -f "$XL/vkiframes/live.conf" ]'
XN="$T/xdg
newline"; mkdir -p "$XN"; HN="$T/home9-nl"; mkdir -p "$HN"
ok "a newline in XDG_CONFIG_HOME: exit 0, a note, no file, the layer installed" 'out=$(HOME="$HN" XDG_CONFIG_HOME="$XN" "$REL/scripts/install.sh" 2>&1); [ $? -eq 0 ] && live_installed "$HN" && [ ! -e "$XN/vkiframes/live.conf" ] && echo "$out" | grep -q "control characters"'
ok "--use also creates it" 'HU="$T/home9-use"; mkdir -p "$HU"; HOME="$HU" "$REL/scripts/install.sh" --use "$T/libB.so" >/dev/null && cmp -s "$TPL" "$HU/.config/vkiframes/live.conf"'

# 8. syntax
ok "bash -n install.sh" 'bash -n "$REPO/scripts/install.sh"'
ok "bash -n uninstall.sh" 'bash -n "$REPO/scripts/uninstall.sh"'
if command -v shellcheck >/dev/null 2>&1; then
    ok "shellcheck clean (the scripts at the default severity, this test at warning level)" 'shellcheck "$REPO/scripts/install.sh" "$REPO/scripts/uninstall.sh" && shellcheck -S warning "$REPO/tests/test_install.sh"'
else
    echo "NOTE: shellcheck skipped (shellcheck not installed)"
fi

if [ $fails -eq 0 ]; then echo "install scripts: ok"; else echo "install scripts: $fails FAILED"; fi
exit $((fails > 0))
