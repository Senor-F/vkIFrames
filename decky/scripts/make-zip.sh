#!/usr/bin/env bash
# Builds the Decky plugin zip for Decky Loader's "install plugin from ZIP": <out>/vkIFrames-decky-<version>.zip
#   decky/scripts/make-zip.sh --lib PATH/libvkIFrames.so --version 0.1.0-alpha.1 --out DIR [--epoch UNIX_SECONDS]
# The zip holds one top-level folder, vkIFrames/ (the loader unpacks it as it is and finds the plugin by the "name" in plugin.json), with plugin.json,
# package.json (version set), main.py, settings_schema.json, dist/index.js (built here with pnpm, no source map), bin/libvkIFrames.so, LICENSE (the plugin's),
# LICENSE-layer and NOTICE (the layer's), README.md and THIRD_PARTY_LICENSES.txt (generated from the packages that are bundled into dist/index.js).
# The zip is deterministic: sorted entries, fixed times (--epoch, the commit time), fixed modes. Exit 0 = zip written and self-checked.
set -eu
ME=$(basename "$0")
fail() { echo "$ME: $*" >&2; exit 1; }
HERE="$(cd "$(dirname "$(readlink -f "$0")")/.." && pwd)"      # decky/
REPO="$(cd "$HERE/.." && pwd)"
LIB=""; VERSION=""; OUT=""; EPOCH=0; REPO_URL=""
while [ $# -gt 0 ]; do
    case "$1" in
        --lib) LIB=${2:?}; shift 2 ;;
        --version) VERSION=${2:?}; shift 2 ;;
        --out) OUT=${2:?}; shift 2 ;;
        --epoch) EPOCH=${2:?}; shift 2 ;;
        --repo-url) REPO_URL=${2:?}; shift 2 ;;
        *) fail "unknown argument: $1" ;;
    esac
done
[ -f "$LIB" ] || fail "--lib: library not found"
[ -n "$VERSION" ] || fail "--version is required"
[ -n "$OUT" ] || fail "--out is required"
case "$VERSION" in *-dirty*|*unknown*) fail "refusing the version string \"$VERSION\"";; esac
[ "$(head -c4 "$LIB" | od -An -tx1 | tr -d ' \n')" = "7f454c46" ] || fail "--lib is not an ELF file"
command -v node >/dev/null || fail "node not found"
command -v pnpm >/dev/null || fail "pnpm not found"
command -v python3 >/dev/null || fail "python3 not found"
mkdir -p "$OUT"
# the public repository URL (for the LGPL source paragraph): --repo-url, else the git remote "origin" without credentials and ".git"
if [ -z "$REPO_URL" ] && command -v git >/dev/null; then
    REPO_URL=$(git -C "$REPO" remote get-url origin 2>/dev/null || true)
    case "$REPO_URL" in
        git@github.com:*) REPO_URL="https://github.com/${REPO_URL#git@github.com:}" ;;
        https://*@*) REPO_URL="https://${REPO_URL#https://*@}" ;;
    esac
    REPO_URL=${REPO_URL%.git}
fi
case "$REPO_URL" in https://*) ;; *) fail "no repository URL (pass --repo-url https://...)";; esac

# 1. build dist/index.js
cd "$HERE"
[ -d node_modules ] || pnpm install --frozen-lockfile >/dev/null
pnpm build >/dev/null
[ -f dist/index.js ] || fail "pnpm build did not produce dist/index.js"

# 2. stage
STAGE=$(mktemp -d "${TMPDIR:-/tmp}/vkiframes-decky.XXXXXX"); trap 'rm -rf "$STAGE"' EXIT
P="$STAGE/vkIFrames"; mkdir -p "$P/dist" "$P/bin"
cp plugin.json main.py settings_schema.json LICENSE "$P/"
# the source map (dist/index.js.map) stays out: it names source folders and is 1.4 MB; so does the comment that points at it
sed '/^\/\/# sourceMappingURL=/d' dist/index.js > "$P/dist/index.js"
cp "$LIB" "$P/bin/libvkIFrames.so"
cp "$REPO/LICENSE" "$P/LICENSE-layer"
{   # NOTICE for this archive: the repository's NOTICE refers to LICENSE and decky/LICENSE, which are named differently here
    cat <<'TXT'
In this archive (vkIFrames Decky plugin):
  LICENSE                   the plugin: BSD 3-Clause, with the copyright line of the original Steam Deck Homebrew plugin template
  LICENSE-layer             the Vulkan layer library bin/libvkIFrames.so: Apache License 2.0
  THIRD_PARTY_LICENSES.txt  the npm packages bundled into dist/index.js
  The shaders in the library are compiled from the project's own GLSL source.

The project's NOTICE follows (its paths refer to the source repository: LICENSE = LICENSE-layer here, decky/LICENSE = LICENSE here):

TXT
    cat "$REPO/NOTICE"
} > "$P/NOTICE"
# README for the people who unzip this
cat > "$P/README.md" <<TXT
# vkIFrames Decky plugin $VERSION

A panel in the Steam Deck's quick-access menu for the vkIFrames frame interpolation layer (source: $REPO_URL).

* **Install layer** (**Reinstall layer** after a plugin update) copies \`bin/libvkIFrames.so\` to \`~/.local/share/vkiframes/\` and writes the Vulkan layer manifest \`~/.local/share/vulkan/implicit_layer.d/VkLayer_frame_gen.json\`. **Uninstall layer** (it asks for a second tap) deletes both. The plugin runs as your user, not as root.
* The layer only works in a game whose launch options start with \`VKIFRAME_ENABLE=1 \` (e.g. \`VKIFRAME_ENABLE=1 %command%\`). The plugin does not set it for you.
* The panel writes your settings to \`~/.config/vkiframes/live.conf\`; the layer picks changes up while the game runs. The layer's log (\`live.log\`), status and measurements are in the same folder \`~/.config/vkiframes/\`. Removing the plugin or the layer leaves that folder; delete it by hand to remove your settings and logs.
* Licences: \`LICENSE\` (this plugin), \`LICENSE-layer\` and \`NOTICE\` (the layer library), \`THIRD_PARTY_LICENSES.txt\` (packages bundled into the panel).
TXT
python3 -I - "$HERE/package.json" "$P/package.json" "$VERSION" <<'PY'
import json, sys
d = json.load(open(sys.argv[1])); d["version"] = sys.argv[3]
open(sys.argv[2], "w").write(json.dumps(d, indent=2) + "\n")
PY
# 3. third-party notices for what is bundled into dist/index.js (read from the installed packages, so the text always matches the version used)
python3 -I - "$HERE" "$P/THIRD_PARTY_LICENSES.txt" "$REPO_URL" <<'PY'
import glob, json, os, sys
here, out, repo_url = sys.argv[1], sys.argv[2], sys.argv[3]
bundled = ["@decky/api", "react-icons", "tslib"]          # what rollup puts into dist/index.js (sources of the map); react and @decky/ui are provided by Decky's UI at run time
parts = ["Third-party software bundled into dist/index.js\n"]
for name in bundled:
    d = os.path.join(here, "node_modules", name)
    pj = json.load(open(os.path.join(d, "package.json")))
    parts.append("=" * 72 + "\n%s %s  (license: %s)\n%s\n" % (name, pj["version"], pj.get("license", "see below"), pj.get("homepage", pj.get("repository", ""))))
    for f in sorted(glob.glob(os.path.join(d, "LICENSE*")) + glob.glob(os.path.join(d, "LICENCE*"))):
        parts.append(open(f, encoding="utf-8", errors="replace").read().rstrip() + "\n")
    if name == "@decky/api":
        parts.append("\nThis package is bundled (not linked at run time) into dist/index.js. Its source is at https://github.com/SteamDeckHomebrew/loader-api . dist/index.js is built from the plugin source in the decky/ folder of " + repo_url + " with `pnpm install --frozen-lockfile && pnpm build`; the exact versions are in decky/pnpm-lock.yaml there. You may replace or modify @decky/api and rebuild dist/index.js the same way (LGPL-2.1, section 6).\n")
    if name == "react-icons":
        parts.append("\nThe one icon used (FaFilm) is a Font Awesome Free icon: Icons CC BY 4.0, https://fontawesome.com/license/free, Copyright Fonticons, Inc.\n")
open(out, "w").write("\n".join(parts))
PY

# 4. deterministic zip + self-check
python3 -I - "$STAGE" "$OUT/vkIFrames-decky-$VERSION.zip" "$EPOCH" <<'PY'
import os, stat, sys, zipfile
stage, out, epoch = sys.argv[1], sys.argv[2], int(sys.argv[3])
import time
t = time.gmtime(max(epoch, 315532800))[:6]                 # a zip cannot hold a date before 1980
names = []
for root, dirs, files in os.walk(stage):
    dirs.sort()
    for f in sorted(files):
        names.append(os.path.relpath(os.path.join(root, f), stage))
names.sort()
tmp = out + ".tmp"
with zipfile.ZipFile(tmp, "w", zipfile.ZIP_DEFLATED, compresslevel=9) as z:
    for n in names:
        zi = zipfile.ZipInfo(n, t)
        zi.create_system = 3
        zi.compress_type = zipfile.ZIP_DEFLATED
        zi.external_attr = ((stat.S_IFREG | (0o755 if n.endswith(".so") else 0o644)) & 0xFFFF) << 16
        z.writestr(zi, open(os.path.join(stage, n), "rb").read())
os.replace(tmp, out)
# self-check: what Decky Loader needs, and nothing it should not carry
z = zipfile.ZipFile(out)
ents = z.namelist()
need = ["plugin.json", "package.json", "main.py", "dist/index.js", "bin/libvkIFrames.so", "LICENSE", "settings_schema.json", "NOTICE", "LICENSE-layer", "THIRD_PARTY_LICENSES.txt"]
assert all(e.startswith("vkIFrames/") for e in ents), "an entry is outside vkIFrames/"
for n in need: assert "vkIFrames/" + n in ents, "missing " + n
bad = [e for e in ents if e.endswith(".map") or "node_modules" in e or "/src/" in e or "/tests/" in e or ".git" in e or e.endswith(".pyc")]
assert not bad, "should not be in the zip: %s" % bad
for e in ents:
    if e.endswith((".js", ".py", ".json", ".txt", ".md")) or e.endswith(("LICENSE", "NOTICE")):
        data = z.read(e).decode("utf-8", "replace")
        assert "/home/" not in data and "/Users/" not in data, "a home path in " + e
lic = z.read("vkIFrames/LICENSE").decode()
assert "Copyright (c) 2026, vkIFrames contributors" in lic and "Original Copyright (c) 2022-2024, Steam Deck Homebrew" in lic, "LICENSE lacks one of the two copyright lines"
assert "LICENSE-layer" in z.read("vkIFrames/NOTICE").decode(), "NOTICE has no archive header"
tp = z.read("vkIFrames/THIRD_PARTY_LICENSES.txt").decode()
assert "pnpm install --frozen-lockfile" in tp and "section 6" in tp and "Font Awesome" in tp, "THIRD_PARTY_LICENSES.txt lacks the LGPL source paragraph or the attribution"
rd = z.read("vkIFrames/README.md").decode()
assert "src/index.tsx" not in rd and "deploy.sh" not in rd and "VKIFRAME_ENABLE=1" in rd, "README.md is not the user README"
for e in ents:
    if e.endswith(".js"): assert "sourceMappingURL" not in z.read(e).decode("utf-8", "replace"), "a dangling source map reference in " + e
print("zip ok: %s (%d entries, %d bytes)" % (out, len(ents), os.path.getsize(out)))
PY
