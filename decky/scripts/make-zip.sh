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
LIB=""; VERSION=""; OUT=""; EPOCH=0
while [ $# -gt 0 ]; do
    case "$1" in
        --lib) LIB=${2:?}; shift 2 ;;
        --version) VERSION=${2:?}; shift 2 ;;
        --out) OUT=${2:?}; shift 2 ;;
        --epoch) EPOCH=${2:?}; shift 2 ;;
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

# 1. build dist/index.js
cd "$HERE"
[ -d node_modules ] || pnpm install --frozen-lockfile >/dev/null
pnpm build >/dev/null
[ -f dist/index.js ] || fail "pnpm build did not produce dist/index.js"

# 2. stage
STAGE=$(mktemp -d "${TMPDIR:-/tmp}/vkiframes-decky.XXXXXX"); trap 'rm -rf "$STAGE"' EXIT
P="$STAGE/vkIFrames"; mkdir -p "$P/dist" "$P/bin"
cp plugin.json main.py settings_schema.json LICENSE README.md "$P/"
cp dist/index.js "$P/dist/index.js"                     # the source map (dist/index.js.map) stays out: it names source folders and is 1.4 MB
cp "$LIB" "$P/bin/libvkIFrames.so"
cp "$REPO/LICENSE" "$P/LICENSE-layer"; cp "$REPO/NOTICE" "$P/NOTICE"
python3 -I - "$HERE/package.json" "$P/package.json" "$VERSION" <<'PY'
import json, sys
d = json.load(open(sys.argv[1])); d["version"] = sys.argv[3]
open(sys.argv[2], "w").write(json.dumps(d, indent=2) + "\n")
PY
# 3. third-party notices for what is bundled into dist/index.js (read from the installed packages, so the text always matches the version used)
python3 -I - "$HERE" "$P/THIRD_PARTY_LICENSES.txt" <<'PY'
import glob, json, os, sys
here, out = sys.argv[1], sys.argv[2]
bundled = ["@decky/api", "react-icons", "tslib"]          # what rollup puts into dist/index.js (sources of the map); react and @decky/ui are provided by Decky's UI at run time
parts = ["Third-party software bundled into dist/index.js\n"]
for name in bundled:
    d = os.path.join(here, "node_modules", name)
    pj = json.load(open(os.path.join(d, "package.json")))
    parts.append("=" * 72 + "\n%s %s  (license: %s)\n%s\n" % (name, pj["version"], pj.get("license", "see below"), pj.get("homepage", pj.get("repository", ""))))
    for f in sorted(glob.glob(os.path.join(d, "LICENSE*")) + glob.glob(os.path.join(d, "LICENCE*"))):
        parts.append(open(f, encoding="utf-8", errors="replace").read().rstrip() + "\n")
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
print("zip ok: %s (%d entries, %d bytes)" % (out, len(ents), os.path.getsize(out)))
PY
