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
if [ -z "${HOME:-}" ] || [ "${HOME#/}" = "$HOME" ]; then echo "$ME: HOME is not set to an absolute path" >&2; exit 2; fi

DATA_HOME="${XDG_DATA_HOME:-$HOME/.local/share}"
CONF_HOME="${XDG_CONFIG_HOME:-$HOME/.config}"
# the same check as install.sh, before anything is removed: a relative folder would be taken relative to wherever the script was started
[ "${DATA_HOME#/}" != "$DATA_HOME" ] || { echo "$ME: XDG_DATA_HOME is not an absolute path" >&2; exit 1; }
[ "${CONF_HOME#/}" != "$CONF_HOME" ] || { echo "$ME: XDG_CONFIG_HOME is not an absolute path" >&2; exit 1; }
CONF_DIR="$CONF_HOME/vkiframes"
LAYER_DIR="$DATA_HOME/vulkan/implicit_layer.d"
MANIFEST="$LAYER_DIR/VkLayer_frame_gen.json"
LIB_DIR="$DATA_HOME/vkiframes"
rc=0

# --- the symbolic-link rule (the same text in install.sh and uninstall.sh; tests/test_install.sh compares them) -----------------------------------------
# The scripts write into and remove from three folders: the library folder, $DATA_HOME/vulkan and $DATA_HOME/vulkan/implicit_layer.d. None of them may be
# a symbolic link (a file path through a link would reach a folder that was never meant: rm -f and mv follow links in the folders above the file). $DATA_HOME
# itself may be a link: XDG_DATA_HOME on another disk is the user's own choice. first_link prints the first of the given folders that is a link and returns 0.
first_link() { local d; for d in "$@"; do if [ -L "$d" ]; then printf '%s' "$d"; return 0; fi; done; return 1; }
# --- end of the rule ---------------------------------------------------------------------------------------------------------------------------------
rm_file() {   # a regular file or a symlink at that path (unlink never follows)
    if [ -e "$1" ] || [ -L "$1" ]; then
        if rm -f -- "$1"; then echo "removed $1"; else echo "$ME: could not remove $1" >&2; rc=1; fi
    else echo "not present: $1"; fi
}

# a link in the path: nothing is removed through it (the rest is still done), and the exit code says so
if bad=$(first_link "$DATA_HOME/vulkan" "$LAYER_DIR"); then echo "$ME: $bad is a symbolic link; the manifest is not removed through it (remove $MANIFEST yourself if it is yours)" >&2; rc=1
else rm_file "$MANIFEST"; fi
if bad=$(first_link "$LIB_DIR"); then echo "$ME: $bad is a symbolic link; the library is not removed through it (remove its libvkIFrames.so yourself if it is yours)" >&2; rc=1
else
    rm_file "$LIB_DIR/libvkIFrames.so"
    rm -f -- "$LIB_DIR"/.libvkIFrames.so.* 2>/dev/null || true       # a temporary copy left by an interrupted install
    if [ -d "$LIB_DIR" ] && rmdir "$LIB_DIR" 2>/dev/null; then echo "removed $LIB_DIR"; fi
fi

echo "left in place:"
left=0
if [ -d "$LIB_DIR" ]; then
    n=0; for f in "$LIB_DIR"/* "$LIB_DIR"/.[!.]*; do if [ -e "$f" ] || [ -L "$f" ]; then n=$((n + 1)); fi; done
    echo "  $LIB_DIR (not empty: $n item(s))"; left=1
fi
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
        if [ "$ok" -eq 1 ]; then
            if rm -rf -- "$CONF_DIR"; then echo "removed $CONF_DIR"; else echo "$ME: could not remove $CONF_DIR" >&2; rc=1; fi
        else echo "kept $CONF_DIR"; fi
    else echo "not present: $CONF_DIR"; fi
fi
exit $rc
