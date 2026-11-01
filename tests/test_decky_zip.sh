#!/usr/bin/env bash
# The Decky plugin zip: built twice it is byte-identical, it has what Decky Loader needs, the version is the project's, main.py compiles, the library keeps mode 755.
# usage: test_decky_zip.sh <repo root> <version>
set -u
# shellcheck disable=SC2034  # used inside the eval strings of ok()
REPO=${1:?}; VERSION=${2:?}
command -v node >/dev/null && command -v pnpm >/dev/null && pnpm --version >/dev/null 2>&1 || { echo "node/pnpm not usable here: skipped"; exit 0; }
T=$(mktemp -d /tmp/vkizip.XXXXXX); trap 'rm -rf "$T"' EXIT
printf '\177ELFfake library' > "$T/lib.so"
fails=0; ok() { if eval "$2"; then :; else echo "FAIL: $1"; fails=$((fails+1)); fi; }
ok "first build" '"$REPO/decky/scripts/make-zip.sh" --lib "$T/lib.so" --version "$VERSION" --out "$T/a" --epoch 1793538095 >/dev/null'
ok "second build" '"$REPO/decky/scripts/make-zip.sh" --lib "$T/lib.so" --version "$VERSION" --out "$T/b" --epoch 1793538095 >/dev/null'
# shellcheck disable=SC2034
Z="vkIFrames-decky-$VERSION.zip"
ok "byte-identical" 'cmp -s "$T/a/$Z" "$T/b/$Z"'
ok "package.json inside has the project version" '[ "$(unzip -p "$T/a/$Z" vkIFrames/package.json | python3 -I -c "import json,sys; print(json.load(sys.stdin)[\"version\"])")" = "$VERSION" ]'
ok "the repository package.json has it too" '[ "$(python3 -I -c "import json,sys; print(json.load(open(sys.argv[1]))[\"version\"])" "$REPO/decky/package.json")" = "$VERSION" ]'
ok "plugin.json has name, author, flags, api_version and no root flag" 'unzip -p "$T/a/$Z" vkIFrames/plugin.json | python3 -I -c "import json,sys; d=json.load(sys.stdin); assert d[\"name\"] and d[\"author\"] and d[\"api_version\"]==1 and \"root\" not in d[\"flags\"]"'
ok "main.py compiles" 'unzip -p "$T/a/$Z" vkIFrames/main.py > "$T/main.py" && python3 -I -m py_compile "$T/main.py"'
ok "library mode 755, others 644, one top folder" 'python3 -I -c "
import zipfile,sys
z=zipfile.ZipFile(sys.argv[1])
for i in z.infolist():
    m=(i.external_attr>>16)&0o777
    assert i.filename.startswith(\"vkIFrames/\")
    assert m==(0o755 if i.filename.endswith(\".so\") else 0o644),(i.filename,oct(m))
" "$T/a/$Z"'
ok "refuses a dirty version string" '! "$REPO/decky/scripts/make-zip.sh" --lib "$T/lib.so" --version 0.1.0-dirty --out "$T/c" >/dev/null 2>&1'
ok "refuses a non-ELF library" 'echo nope > "$T/x.so"; ! "$REPO/decky/scripts/make-zip.sh" --lib "$T/x.so" --version "$VERSION" --out "$T/c" >/dev/null 2>&1'
if [ $fails -eq 0 ]; then echo "decky zip: ok"; else echo "decky zip: $fails FAILED"; fi
exit $((fails > 0))
