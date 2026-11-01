#!/usr/bin/env bash
# Removes what scripts/install.sh (or the Decky plugin) installed: the layer manifest and the library copy in ~/.local/share/vkiframes.
#   scripts/uninstall.sh              keeps your settings and logs in ~/.config/vkiframes and says so
#   scripts/uninstall.sh --purge      also deletes ~/.config/vkiframes (asks first; --yes skips the question)
# Exit codes: 0 done (also when there was nothing to remove), 1 a file could not be removed, 2 not run as a normal user or no home folder.
set -eu

ME=$(basename "$0")
PURGE=0; YES=0
for a in "$@"; do
    case "$a" in
        --purge) PURGE=1 ;;
        --yes) YES=1 ;;
        -h|--help) sed -n '2,6p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
        *) echo "$ME: unknown argument: $a (try --help)" >&2; exit 1 ;;
    esac
done
[ "$(id -u)" -ne 0 ] || { echo "$ME: do not run this as root: the files are in your own home folder" >&2; exit 2; }
[ -n "${HOME:-}" ] && [ "${HOME#/}" != "$HOME" ] || { echo "$ME: HOME is not set to an absolute path" >&2; exit 2; }

DATA_HOME="${XDG_DATA_HOME:-$HOME/.local/share}"
CONF_DIR="${XDG_CONFIG_HOME:-$HOME/.config}/vkiframes"
MANIFEST="$DATA_HOME/vulkan/implicit_layer.d/VkLayer_frame_gen.json"
LIB_DIR="$DATA_HOME/vkiframes"
rc=0
rm_file() {   # a regular file or a symlink at that path (unlink never follows)
    if [ -e "$1" ] || [ -L "$1" ]; then
        if rm -f -- "$1"; then echo "removed $1"; else echo "$ME: could not remove $1" >&2; rc=1; fi
    else echo "not present: $1"; fi
}

rm_file "$MANIFEST"
rm_file "$LIB_DIR/libvkIFrames.so"
rm -f -- "$LIB_DIR"/.libvkIFrames.so.* 2>/dev/null || true       # a temporary copy left by an interrupted install
[ -d "$LIB_DIR" ] && [ ! -L "$LIB_DIR" ] && rmdir "$LIB_DIR" 2>/dev/null && echo "removed $LIB_DIR" || true

echo "left in place:"
left=0
if [ -d "$LIB_DIR" ]; then echo "  $LIB_DIR (not empty: $(ls -A "$LIB_DIR" | wc -l) item(s))"; left=1; fi
if [ -d "$CONF_DIR" ]; then echo "  $CONF_DIR (your settings, logs and measurements; remove with --purge)"; left=1; fi
[ "$left" -eq 1 ] || echo "  nothing"

if [ "$PURGE" -eq 1 ]; then
    if [ -d "$CONF_DIR" ] && [ ! -L "$CONF_DIR" ]; then
        ok=$YES
        if [ "$ok" -ne 1 ]; then
            printf 'Delete %s and everything in it? [y/N] ' "$CONF_DIR"
            read -r ans || ans=n
            case "$ans" in y|Y|yes|YES) ok=1 ;; esac
        fi
        if [ "$ok" -eq 1 ]; then rm -rf -- "$CONF_DIR" && echo "removed $CONF_DIR" || { echo "$ME: could not remove $CONF_DIR" >&2; rc=1; }
        else echo "kept $CONF_DIR"; fi
    else echo "not present: $CONF_DIR"; fi
fi
exit $rc
