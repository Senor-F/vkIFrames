#!/usr/bin/env bash
# Builds the release artifacts from a clean checkout of one commit:
#   vkIFrames-<version>-linux-x86_64.tar.gz     libvkIFrames.so (stripped), manifest template, install/uninstall/check_env scripts, LICENSE, NOTICE, README.md
#   vkIFrames-decky-<version>.zip               the Decky plugin with the same library in bin/
#   SHA256SUMS                                  the two archives (check with: sha256sum -c SHA256SUMS in the output folder)
#   SHA256SUMS-contents                         every file inside both archives
#
#   scripts/package.sh --builder 'distrobox enter vkbuild --' [--commit REV] [--out DIR] [--vulkan-headers DIR] [--repo-url URL] [--fast]
#
# The library is built in the pinned toolchain only (Ubuntu 22.04: glibc 2.35, GCC 11, CMake 3.22), because the glibc symbol versions come from the
# machine that links it: a newer distribution would raise the floor above GLIBC_2.34 and the library would not load on SteamOS 3.x or Ubuntu 22.04.
# --builder is the command that runs a command inside that environment (distrobox, podman exec, ...); without it the host is used and must be that environment.
# Every build is configured fresh with the version, commit and commit time passed explicitly, so the same commit gives the same bytes: the library is built
# twice from two checkouts in two folders and the script stops if they differ (--fast skips the second build).
# Refuses: a version with "-dirty" or "unknown", a Decky package.json version that differs from the project's, a library that fails scripts/check_so.sh
# after strip, scripts that fail shellcheck (shellcheck is required), an archive with an unexpected member.
# --lib PATH (tests only): use that file as the stripped library, skip the build, strip and library checks; the output is marked NOT FOR RELEASE.
set -eu
ME=$(basename "$0")
fail() { echo "$ME: $*" >&2; exit 1; }
ROOT="$(cd "$(dirname "$(readlink -f "$0")")/.." && pwd)"
REV=HEAD; OUT=""; BUILDER=""; VKH="${VULKAN_INCLUDE_DIR:-}"; REPO_URL=""; FAST=0; TESTLIB=""
while [ $# -gt 0 ]; do
    case "$1" in
        --commit) REV=${2:?}; shift 2 ;;
        --out) OUT=${2:?}; shift 2 ;;
        --builder) BUILDER=${2:?}; shift 2 ;;
        --vulkan-headers) VKH=${2:?}; shift 2 ;;
        --repo-url) REPO_URL=${2:?}; shift 2 ;;
        --fast) FAST=1; shift ;;
        --lib) TESTLIB=${2:?}; shift 2 ;;
        -h|--help) sed -n '2,19p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
        *) fail "unknown argument: $1 (try --help)" ;;
    esac
done
for t in git tar gzip sha256sum python3; do command -v "$t" >/dev/null || fail "$t not found"; done
SHA=$(git -C "$ROOT" rev-parse --verify "$REV^{commit}") || fail "unknown commit: $REV"
SHORT=$(git -C "$ROOT" rev-parse --short=10 "$SHA")
CT=$(git -C "$ROOT" log -1 --format=%ct "$SHA")
VER=$(git -C "$ROOT" show "$SHA:CMakeLists.txt" | sed -n 's/.*set(FRAMEGEN_VERSION "\([^"]*\)".*/\1/p' | head -1)
[ -n "$VER" ] || fail "no FRAMEGEN_VERSION in CMakeLists.txt at $SHORT"
case "$VER" in *-dirty*|*unknown*) fail "refusing the version string \"$VER\"";; esac
DV=$(git -C "$ROOT" show "$SHA:decky/package.json" | python3 -I -c 'import json,sys; print(json.load(sys.stdin)["version"])')
[ "$DV" = "$VER" ] || fail "decky/package.json version ($DV) differs from the project's ($VER)"
if [ -z "$REPO_URL" ]; then
    REPO_URL=$(git -C "$ROOT" remote get-url origin 2>/dev/null || true)
    case "$REPO_URL" in git@github.com:*) REPO_URL="https://github.com/${REPO_URL#git@github.com:}";; https://*@*) REPO_URL="https://${REPO_URL#https://*@}";; esac
    REPO_URL=${REPO_URL%.git}
fi
case "$REPO_URL" in https://*) ;; *) fail "no repository URL (pass --repo-url https://...)";; esac
[ -n "$OUT" ] || OUT="$ROOT/dist-release/$VER"
mkdir -p "$OUT"; OUT=$(cd "$OUT" && pwd)
TMP=$(mktemp -d "${TMPDIR:-/tmp}/vkiframes-package.XXXXXX")
cleanup() { git -C "$ROOT" worktree remove --force "$TMP/src" 2>/dev/null || true; git -C "$ROOT" worktree remove --force "$TMP/src2" 2>/dev/null || true; rm -rf "$TMP"; }
trap cleanup EXIT
git -C "$ROOT" worktree add --detach -q "$TMP/src" "$SHA"
SRC="$TMP/src"
echo "$ME: version $VER, commit $SHORT, commit time $CT (unix), output $OUT"

run_in() { if [ -n "$BUILDER" ]; then $BUILDER "$@"; else "$@"; fi; }          # $BUILDER is a command prefix, split on purpose

# --- shellcheck over every shipped script (required) --------------------------------------------------------------------------------------------
if run_in sh -c 'command -v shellcheck >/dev/null 2>&1'; then
    run_in shellcheck "$SRC"/scripts/install.sh "$SRC"/scripts/uninstall.sh "$SRC"/scripts/check_env.sh "$SRC"/scripts/check_so.sh "$SRC"/scripts/package.sh "$SRC"/decky/scripts/make-zip.sh \
        || fail "shellcheck found problems in the shipped scripts"
    echo "$ME: shellcheck clean"
else
    [ -n "$TESTLIB" ] || fail "shellcheck is required for a release build (install it, or use --lib for a test run)"
    echo "$ME: NOTE: shellcheck skipped (shellcheck not installed)"
fi

# --- the library -----------------------------------------------------------------------------------------------------------------------------------
LIB="$TMP/libvkIFrames.so"
if [ -n "$TESTLIB" ]; then
    [ -f "$TESTLIB" ] || fail "--lib: not found"
    cp "$TESTLIB" "$LIB"; echo "$ME: TEST MODE: library taken from $TESTLIB, NOT FOR RELEASE"
else
    # shellcheck disable=SC2016
    run_in sh -c 'echo "toolchain: $(ldd --version | head -1) | $(g++ --version | head -1) | $(cmake --version | head -1)"; ldd --version | head -1 | grep -q " 2\.35" && g++ --version | head -1 | grep -q " 11\." && cmake --version | head -1 | grep -q " 3\.22"' \
        || fail "not the pinned toolchain (glibc 2.35, GCC 11, CMake 3.22): the glibc floor of the library would be wrong. Use --builder with an Ubuntu 22.04 container"
    [ -n "$VKH" ] || fail "no Vulkan headers: pass --vulkan-headers DIR (the folder that contains vulkan/vk_layer.h) or set VULKAN_INCLUDE_DIR"
    case "$VKH" in *"'"*) fail "the Vulkan headers path contains a single quote; use another folder";; esac
    VKV=$(grep -h "define VK_HEADER_VERSION " "$VKH/vulkan/vulkan_core.h" | head -1 || true)
    echo "$ME: Vulkan headers: ${VKV:-unknown} ($VKH)"
    build() {   # build <source folder> <build folder> <output file>
        run_in sh -c "cmake -S '$1' -B '$2' -DCMAKE_BUILD_TYPE=Release -DFRAMEGEN_BUILD_TESTS=OFF -DFRAMEGEN_VERSION='$VER' -DFRAMEGEN_COMMIT='$SHORT' -DFRAMEGEN_COMMIT_TIME='$CT' -DFRAMEGEN_VULKAN_INCLUDE_DIR='$VKH' >/dev/null \
            && cmake --build '$2' -j\"\$(nproc)\" >/dev/null && cp '$2/libvkIFrames.so' '$3' && strip --strip-unneeded '$3'" || fail "build failed"
    }
    build "$SRC" "$TMP/build" "$LIB"
    if [ "$FAST" -eq 0 ]; then
        git -C "$ROOT" worktree add --detach -q "$TMP/src2" "$SHA"
        sleep 2                                              # a different wall-clock time and folder for the second build
        build "$TMP/src2" "$TMP/build2" "$TMP/lib2.so"
        cmp -s "$LIB" "$TMP/lib2.so" || fail "two builds of $SHORT differ: the build is not reproducible"
        echo "$ME: two builds of $SHORT are byte-identical"
    else echo "$ME: --fast: the reproducibility build was skipped"; fi
    run_in sh "$SRC/scripts/check_so.sh" "$LIB" || fail "scripts/check_so.sh failed on the stripped library"
fi

# --- the layer archive ------------------------------------------------------------------------------------------------------------------------------
ARCH="vkIFrames-$VER-linux-x86_64"
ST="$TMP/stage/$ARCH"; mkdir -p "$ST/scripts" "$ST/manifest"
cp "$LIB" "$ST/libvkIFrames.so"
cp "$SRC/manifest/VkLayer_frame_gen.json.in" "$ST/manifest/"
cp "$SRC/scripts/install.sh" "$SRC/scripts/uninstall.sh" "$SRC/scripts/check_env.sh" "$ST/scripts/"
cp "$SRC/LICENSE" "$ST/"
{   # NOTICE for this archive: the repository's NOTICE refers to decky/LICENSE, which is not in it
    cat <<TXT
This archive holds the layer library only (libvkIFrames.so, Apache-2.0, see LICENSE). The Decky plugin is a separate archive with its own BSD 3-Clause licence and
the third-party notices. The project's NOTICE follows (its paths refer to the source repository).

TXT
    cat "$SRC/NOTICE"
} > "$ST/NOTICE"
cat > "$ST/README.md" <<TXT
# vkIFrames $VER

A Vulkan layer that adds in-between frames to games (frame interpolation) on Linux and the Steam Deck. Source and full documentation: $REPO_URL

This is an alpha release; known issues are listed in the README in the repository.

## What is in this folder
* \`libvkIFrames.so\`: the layer (x86-64, needs glibc 2.34 or newer; a 64-bit game process).
* \`scripts/install.sh\`, \`scripts/uninstall.sh\`, \`scripts/check_env.sh\`: install, remove, and a check of the environment.
* \`manifest/\`: the template of the Vulkan layer manifest that install.sh fills in.
* \`LICENSE\` (Apache-2.0) and \`NOTICE\`. The Decky Loader panel is a separate archive (\`vkIFrames-decky-$VER.zip\`).

## Install (no root)
Run \`scripts/install.sh\` as your normal user. It copies the library to \`~/.local/share/vkiframes/\` and writes the manifest \`~/.local/share/vulkan/implicit_layer.d/VkLayer_frame_gen.json\`. It is safe to run again after an upgrade.

## Use
Add this to a game's launch options (Steam: Properties, General): \`VKIFRAME_ENABLE=1 %command%\`. Without it the layer does nothing. \`VKIFRAME_DISABLE=1\` switches it off for one launch.
Settings and logs live in \`~/.config/vkiframes/\` (live.conf, live.log, status.json).
32-bit games: \`PROTON_USE_WOW64=1 VKIFRAME_ENABLE=1 %command%\`.
If the layer loads but shows no extra frames (seen with Batman: Arkham Asylum on Proton - Experimental), try GE-Proton, or \`DXVK_CONFIG="dxvk.enablePresentTiming = False" PROTON_USE_WOW64=1 VKIFRAME_ENABLE=1 %command%\` (tested on one game, a short run; it may change the game's frame pacing; remove it if the game misbehaves).

## Remove
\`scripts/uninstall.sh\` removes the library and the manifest; \`scripts/uninstall.sh --purge\` also deletes \`~/.config/vkiframes\` (it asks first).

## Check the download
\`sha256sum -c SHA256SUMS\` in the folder that holds the archives and \`SHA256SUMS\`. The release also carries SHA256SUMS.sig, an SSH signature of the checksum file; the README shows how to verify it.
TXT
find "$ST" -type d -exec chmod 0755 {} +
find "$ST" -type f -exec chmod 0644 {} +
chmod 0755 "$ST/libvkIFrames.so" "$ST"/scripts/*.sh
TAR="$OUT/$ARCH.tar.gz"
( cd "$TMP/stage" && tar --sort=name --owner=0 --group=0 --numeric-owner --mtime="@$CT" --format=gnu -cf - "$ARCH" ) | gzip -n -9 > "$TAR.tmp"
mv -f "$TAR.tmp" "$TAR"

# --- the Decky zip ----------------------------------------------------------------------------------------------------------------------------------
"$SRC/decky/scripts/make-zip.sh" --lib "$LIB" --version "$VER" --out "$OUT" --epoch "$CT" --repo-url "$REPO_URL" >/dev/null || fail "the Decky zip could not be built"
ZIP="$OUT/vkIFrames-decky-$VER.zip"

# --- checks on what was written ------------------------------------------------------------------------------------------------------------------
python3 -I - "$TAR" "$ARCH" <<'PY' || fail "the tar has an unexpected member list"
import sys, tarfile
t = tarfile.open(sys.argv[1]); a = sys.argv[2]
rd = t.extractfile(a + "/README.md").read().decode()
assert "install.sh" in rd and "VKIFRAME_ENABLE=1" in rd and "cmake -S" not in rd and "Quick start" not in rd, "README.md is not the user README"
assert "layer library only" in t.extractfile(a + "/NOTICE").read().decode(), "NOTICE has no archive header"
want = sorted([a, a + "/LICENSE", a + "/NOTICE", a + "/README.md", a + "/libvkIFrames.so", a + "/manifest", a + "/manifest/VkLayer_frame_gen.json.in", a + "/scripts",
               a + "/scripts/check_env.sh", a + "/scripts/install.sh", a + "/scripts/uninstall.sh"])
got = [m.name for m in t.getmembers()]
assert got == want, "members: %s" % got
for m in t.getmembers():
    assert m.uid == 0 and m.gid == 0 and m.uname == "" and m.gname == "", (m.name, m.uid, m.gid)
    assert m.mode == (0o755 if m.isdir() or m.name.endswith((".so", ".sh")) else 0o644), (m.name, oct(m.mode))
PY
# the extracted archive installs and uninstalls (no root, a throw-away HOME)
SM="$TMP/smoke"; mkdir -p "$SM/home" "$SM/x"
tar -xzf "$TAR" -C "$SM/x"
( cd "$SM" && HOME="$SM/home" XDG_DATA_HOME="" "$SM/x/$ARCH/scripts/install.sh" >/dev/null && HOME="$SM/home" "$SM/x/$ARCH/scripts/install.sh" >/dev/null \
    && cmp -s "$SM/x/$ARCH/libvkIFrames.so" "$SM/home/.local/share/vkiframes/libvkIFrames.so" && HOME="$SM/home" "$SM/x/$ARCH/scripts/uninstall.sh" >/dev/null \
    && [ ! -e "$SM/home/.local/share/vkiframes/libvkIFrames.so" ] ) || fail "the extracted archive does not install and uninstall cleanly"
echo "$ME: the extracted archive installs twice and uninstalls cleanly"

# --- checksums --------------------------------------------------------------------------------------------------------------------------------------
( cd "$OUT" && sha256sum "$(basename "$TAR")" "$(basename "$ZIP")" > SHA256SUMS )
python3 -I - "$TAR" "$ZIP" > "$OUT/SHA256SUMS-contents" <<'PY'
import hashlib, os, sys, tarfile, zipfile
t = tarfile.open(sys.argv[1])
for m in t.getmembers():
    if m.isfile(): print("%s  %s:%s" % (hashlib.sha256(t.extractfile(m).read()).hexdigest(), os.path.basename(sys.argv[1]), m.name))
z = zipfile.ZipFile(sys.argv[2])
for n in z.namelist(): print("%s  %s:%s" % (hashlib.sha256(z.read(n)).hexdigest(), os.path.basename(sys.argv[2]), n))
PY
( cd "$OUT" && sha256sum -c SHA256SUMS >/dev/null ) || fail "SHA256SUMS does not verify"
[ -z "$TESTLIB" ] || echo "NOT FOR RELEASE (--lib test run)" > "$OUT/NOT-FOR-RELEASE"
echo "$ME: done:"; cat "$OUT/SHA256SUMS"
