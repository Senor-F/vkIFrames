#!/usr/bin/env bash
# Installs vkIFrames as an implicit Vulkan layer for the current user: no root, no build tree, no launch script.
#   scripts/install.sh                 copies libvkIFrames.so to ~/.local/share/vkiframes/ and writes the layer manifest
#   scripts/install.sh --use PATH      developers: points the manifest at PATH in place (for example build/libvkIFrames.so), copies nothing
# Run it from the extracted release folder or from a repository checkout. It is safe to run again (also after an upgrade).
# Afterwards a game uses the layer when its launch options start with VKIFRAME_ENABLE=1, e.g.
#   VKIFRAME_ENABLE=1 %command%
# Remove everything again with scripts/uninstall.sh.
# Exit codes: 0 done, 1 a check failed (nothing is changed), 2 not run as a normal user or no home folder.
set -eu

ME=$(basename "$0")
fail() { echo "$ME: $*" >&2; exit 1; }
ROOT="$(cd "$(dirname "$(readlink -f "$0")")/.." && pwd)"
USE=""
case "${1:-}" in
    "") ;;
    --use) [ -n "${2:-}" ] || fail "--use needs a library path"; USE=$2 ;;
    -h|--help) sed -n '2,10p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
    *) fail "unknown argument: $1 (try --help)" ;;
esac

[ "$(id -u)" -ne 0 ] || { echo "$ME: do not run this as root: the files belong in your own home folder" >&2; exit 2; }
if [ -z "${HOME:-}" ] || [ "${HOME#/}" = "$HOME" ]; then echo "$ME: HOME is not set to an absolute path" >&2; exit 2; fi

DATA_HOME="${XDG_DATA_HOME:-$HOME/.local/share}"
[ "${DATA_HOME#/}" != "$DATA_HOME" ] || fail "XDG_DATA_HOME is not an absolute path"
[ -z "${XDG_CONFIG_HOME:-}" ] || [ "${XDG_CONFIG_HOME#/}" != "$XDG_CONFIG_HOME" ] || fail "XDG_CONFIG_HOME is not an absolute path"
LIB_DIR="$DATA_HOME/vkiframes"
LAYER_DIR="$DATA_HOME/vulkan/implicit_layer.d"
MANIFEST="$LAYER_DIR/VkLayer_frame_gen.json"
LIB_DST="$LIB_DIR/libvkIFrames.so"

# --- the symbolic-link rule (the same text in install.sh and uninstall.sh; tests/test_install.sh compares them) -----------------------------------------
# The scripts write into and remove from three folders: the library folder, $DATA_HOME/vulkan and $DATA_HOME/vulkan/implicit_layer.d. None of them may be
# a symbolic link (a file path through a link would reach a folder that was never meant: rm -f and mv follow links in the folders above the file). $DATA_HOME
# itself may be a link: XDG_DATA_HOME on another disk is the user's own choice. first_link prints the first of the given folders that is a link and returns 0.
first_link() { local d; for d in "$@"; do if [ -L "$d" ]; then printf '%s' "$d"; return 0; fi; done; return 1; }
# --- end of the rule ---------------------------------------------------------------------------------------------------------------------------------
if bad=$(first_link "$LIB_DIR" "$DATA_HOME/vulkan" "$LAYER_DIR"); then fail "$bad is a symbolic link; refusing to write through it (nothing is changed)"; fi

# --- find the library and the manifest template -------------------------------------------------------------------------------------------
if [ -n "$USE" ]; then SRC=$USE
elif [ -f "$ROOT/libvkIFrames.so" ]; then SRC=$ROOT/libvkIFrames.so             # release folder
else SRC=$ROOT/build/libvkIFrames.so; fi                                          # repository checkout
[ -f "$SRC" ] || fail "library not found: $SRC (build it first, or pass --use PATH)"
SRC=$(readlink -f "$SRC")
[ "$(head -c4 "$SRC" | od -An -tx1 | tr -d ' \n')" = "7f454c46" ] || fail "not an ELF shared library: $SRC"
TEMPLATE="$ROOT/manifest/VkLayer_frame_gen.json.in"
[ -f "$TEMPLATE" ] || fail "manifest template not found: $TEMPLATE"

# The target path goes into a JSON string: refuse control characters, escape backslash and double quote.
json_escape() { local s=$1; s=${s//\\/\\\\}; printf '%s' "${s//\"/\\\"}"; }
have_ctrl() { case "$1" in *[[:cntrl:]]*) return 0;; esac; return 1; }

# A folder we write into must be a real folder owned by this user (not a symlink, not somebody else's).
own_dir() {
    local d=$1
    [ ! -L "$d" ] || fail "$d is a symbolic link; refusing to write there"
    [ -d "$d" ] || return 1
    [ "$(stat -c %u "$d")" = "$(id -u)" ] || fail "$d belongs to another user"
}

if [ -n "$USE" ]; then TARGET=$SRC; else TARGET=$LIB_DST; fi
have_ctrl "$TARGET" && fail "the library path contains control characters"

# --- the library: copied beside the data, replaced by rename so a running game that has the old file mapped keeps a valid one -----------------
if [ -z "$USE" ]; then
    # shellcheck disable=SC2174  # -m applies to the deepest folder only, which is intended: ~/.local/share and its parents keep their own modes
    mkdir -p -m 0700 "$LIB_DIR"                         # only a folder we create is 0700; an existing one is checked, not changed
    own_dir "$LIB_DIR" || fail "cannot use $LIB_DIR"
    if [ -f "$LIB_DST" ] && [ ! -L "$LIB_DST" ] && cmp -s "$SRC" "$LIB_DST"; then
        chmod 0755 "$LIB_DST"
        echo "library already installed: $LIB_DST"
    else
        TMP=$(mktemp "$LIB_DIR/.libvkIFrames.so.XXXXXX")
        trap 'rm -f "$TMP"' EXIT
        cp -- "$SRC" "$TMP"
        chmod 0755 "$TMP"
        mv -f -- "$TMP" "$LIB_DST"                      # rename: never overwrites the old file in place
        trap - EXIT
        echo "library installed: $LIB_DST"
    fi
fi

# --- the manifest, from the template ----------------------------------------------------------------------------------------------------------
mkdir -p "$LAYER_DIR"
own_dir "$LAYER_DIR" || fail "cannot use $LAYER_DIR"
TMPM=$(mktemp "$LAYER_DIR/.VkLayer_frame_gen.json.XXXXXX")
trap 'rm -f "$TMPM"' EXIT
ESC=$(json_escape "$TARGET")
# The placeholder is replaced by splitting the line around it, not with ${line//pattern/replacement}: since bash 5.2 an unquoted "&" in a replacement
# stands for the matched text, which breaks a library path that contains one.
PH='@FRAMEGEN_LIBRARY_PATH@'
while IFS= read -r line || [ -n "$line" ]; do
    out=""
    while [[ $line == *"$PH"* ]]; do out+="${line%%"$PH"*}$ESC"; line=${line#*"$PH"}; done
    printf '%s\n' "$out$line"
done < "$TEMPLATE" > "$TMPM"
chmod 0644 "$TMPM"                                       # the Vulkan loader reads it
mv -f -- "$TMPM" "$MANIFEST"
trap - EXIT
echo "manifest written:   $MANIFEST -> $TARGET"
echo "Use it per game with the launch option:  VKIFRAME_ENABLE=1 %command%"
