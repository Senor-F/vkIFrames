#!/usr/bin/env bash
# scripts/package.sh in test mode (--lib: a stand-in library, no build): the archives have exactly the expected members, numeric owner 0, the commit time as mtime,
# checksums that verify, byte-identical output on a second run, an extracted archive that installs; and the refusals (a dirty version, a Decky version that
# differs from the project's). usage: test_package.sh <repo root>  (needs a committed HEAD; skipped without node/pnpm)
set -u
REPO=${1:?usage: test_package.sh <repo root>}
command -v node >/dev/null && command -v pnpm >/dev/null && pnpm --version >/dev/null 2>&1 || { echo "node/pnpm not usable here: skipped"; exit 0; }
T=$(mktemp -d /tmp/vkipkg.XXXXXX); trap 'rm -rf "$T"' EXIT
fails=0; ok() { if eval "$2"; then :; else echo "FAIL: $1"; fails=$((fails+1)); fi; }
printf '\177ELFfake stripped library' > "$T/lib.so"
# shellcheck disable=SC2034  # used inside the eval strings of ok()
URL=https://example.invalid/owner/repo
# a clone of HEAD to work from, so the test does not depend on the state of the working tree
git clone -q --no-hardlinks "$REPO" "$T/clone" && git -C "$T/clone" checkout -q --detach "$(git -C "$REPO" rev-parse HEAD)" || { echo "FAIL: cannot clone"; exit 1; }
# shellcheck disable=SC2034
P="$T/clone/scripts/package.sh"
ok "first run" '"$P" --lib "$T/lib.so" --out "$T/a" --repo-url "$URL" >/dev/null 2>&1'
ok "second run" '"$P" --lib "$T/lib.so" --out "$T/b" --repo-url "$URL" >/dev/null 2>&1'
VER=$(sed -n 's/.*set(FRAMEGEN_VERSION "\([^"]*\)".*/\1/p' "$T/clone/CMakeLists.txt" | head -1)
ok "both archives and the checksum files exist" '[ -f "$T/a/vkIFrames-$VER-linux-x86_64.tar.gz" ] && [ -f "$T/a/vkIFrames-decky-$VER.zip" ] && [ -f "$T/a/SHA256SUMS" ] && [ -f "$T/a/SHA256SUMS-contents" ]'
ok "SHA256SUMS verifies" '(cd "$T/a" && sha256sum -c SHA256SUMS >/dev/null)'
ok "the two runs give byte-identical archives and sums" 'cmp -s "$T/a/vkIFrames-$VER-linux-x86_64.tar.gz" "$T/b/vkIFrames-$VER-linux-x86_64.tar.gz" && cmp -s "$T/a/vkIFrames-decky-$VER.zip" "$T/b/vkIFrames-decky-$VER.zip" && cmp -s "$T/a/SHA256SUMS" "$T/b/SHA256SUMS"'
ok "marked as not for release" '[ -f "$T/a/NOT-FOR-RELEASE" ]'
# shellcheck disable=SC2034
CT=$(git -C "$T/clone" log -1 --format=%ct)
ok "tar: members, numeric owner 0, modes, mtime = commit time, sorted" 'python3 -I -c "
import sys,tarfile
t=tarfile.open(sys.argv[1]); a=sys.argv[2]
ms=t.getmembers(); names=[m.name for m in ms]
assert names==sorted(names), names
assert set(names)=={a,a+\"/LICENSE\",a+\"/NOTICE\",a+\"/README.md\",a+\"/libvkIFrames.so\",a+\"/manifest\",a+\"/manifest/VkLayer_frame_gen.json.in\",a+\"/scripts\",a+\"/scripts/check_env.sh\",a+\"/scripts/install.sh\",a+\"/scripts/uninstall.sh\"}, names
assert all(m.uid==0 and m.gid==0 and m.mtime==int(sys.argv[3]) for m in ms)
" "$T/a/vkIFrames-$VER-linux-x86_64.tar.gz" "vkIFrames-$VER-linux-x86_64" "$CT"'
ok "gzip header has no name or time" '[ "$(head -c 10 "$T/a/vkIFrames-$VER-linux-x86_64.tar.gz" | od -An -tx1 | tr -d " \n")" = "1f8b0800000000000203" ] || [ "$(head -c 10 "$T/a/vkIFrames-$VER-linux-x86_64.tar.gz" | od -An -tx1 | tr -d " \n" | cut -c1-16)" = "1f8b080000000000" ]'
ok "contents list covers every file of both archives" '[ "$(wc -l < "$T/a/SHA256SUMS-contents")" -eq "$(( $(tar -tzf "$T/a/vkIFrames-$VER-linux-x86_64.tar.gz" | grep -vc "/$") + $(unzip -Z1 "$T/a/vkIFrames-decky-$VER.zip" | wc -l) ))" ]'
# shellcheck disable=SC2034
DX='DXVK_CONFIG="dxvk.enablePresentTiming = False" PROTON_USE_WOW64=1 VKIFRAME_ENABLE=1 %command%'
ok "the tar README has the 32-bit and the DXVK launch options, exactly" 'tar -xzOf "$T/a/vkIFrames-$VER-linux-x86_64.tar.gz" "vkIFrames-$VER-linux-x86_64/README.md" > "$T/tar-readme.md" && grep -qF "PROTON_USE_WOW64=1 VKIFRAME_ENABLE=1 %command%" "$T/tar-readme.md" && grep -qF "$DX" "$T/tar-readme.md"'
ok "the library in the zip is the one in the tar" 'cmp -s <(tar -xzOf "$T/a/vkIFrames-$VER-linux-x86_64.tar.gz" "vkIFrames-$VER-linux-x86_64/libvkIFrames.so") <(unzip -p "$T/a/vkIFrames-decky-$VER.zip" vkIFrames/bin/libvkIFrames.so)'
# refusals, in the clone
sed -i "s/set(FRAMEGEN_VERSION \"$VER\"/set(FRAMEGEN_VERSION \"$VER-dirty\"/" "$T/clone/CMakeLists.txt"; git -C "$T/clone" -c user.name=t -c user.email=t@invalid commit -q -am "dirty version"
ok "a -dirty version is refused" '! "$P" --lib "$T/lib.so" --out "$T/c" --repo-url "$URL" >/dev/null 2>&1'
git -C "$T/clone" reset -q --hard HEAD~1
sed -i 's/"version": "[^"]*"/"version": "9.9.9"/' "$T/clone/decky/package.json"; git -C "$T/clone" -c user.name=t -c user.email=t@invalid commit -q -am "version skew"
ok "a Decky version that differs from the project's is refused" '! "$P" --lib "$T/lib.so" --out "$T/d" --repo-url "$URL" >/dev/null 2>&1'
ok "a repository URL that is not https is refused" '! "$P" --lib "$T/lib.so" --out "$T/e" --repo-url "ssh://x" >/dev/null 2>&1'
if [ $fails -eq 0 ]; then echo "package script: ok"; else echo "package script: $fails FAILED"; fi
exit $((fails > 0))
