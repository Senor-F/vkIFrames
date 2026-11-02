#!/usr/bin/env bash
# scripts/check_so.sh must notice a library without full RELRO / bind-now and accept one that has them (the other checks fail on this toy library, which is
# fine: only the hardening lines are looked at). usage: test_check_so.sh <repo root>
set -u
REPO=${1:?usage: test_check_so.sh <repo root>}
CC=${CC:-cc}
command -v "$CC" >/dev/null 2>&1 || { echo "NOTE: no C compiler, skipped"; exit 0; }
T=$(mktemp -d /tmp/vkicheck.XXXXXX); trap 'rm -rf "$T"' EXIT
fails=0
ok() { if eval "$2"; then :; else echo "FAIL: $1 [$2]"; fails=$((fails+1)); fi; }
printf 'int helper(int);\nint f(int x) { return helper(x) + 1; }\n' > "$T/a.c"
printf 'int helper(int x) { return x * 2; }\n' > "$T/b.c"
"$CC" -shared -fPIC -o "$T/b.so" "$T/b.c"
"$CC" -shared -fPIC -Wl,-z,norelro -Wl,-z,lazy -o "$T/weak.so" "$T/a.c" "$T/b.so" 2>/dev/null || "$CC" -shared -fPIC -Wl,-z,norelro -Wl,-z,lazy -o "$T/weak.so" "$T/a.c" "$T/b.c"
"$CC" -shared -fPIC -Wl,-z,relro -Wl,-z,now -o "$T/strong.so" "$T/a.c" "$T/b.c"
# shellcheck disable=SC2034  # used inside the eval strings of ok()
weak=$("$REPO/scripts/check_so.sh" "$T/weak.so" 2>&1)
# shellcheck disable=SC2034
strong=$("$REPO/scripts/check_so.sh" "$T/strong.so" 2>&1)
ok "no GNU_RELRO is reported" 'echo "$weak" | grep -q "no GNU_RELRO segment"'
ok "no bind-now is reported" 'echo "$weak" | grep -q "not bound at load"'
ok "a library with full RELRO and bind-now gets neither complaint" '! echo "$strong" | grep -q "no GNU_RELRO segment" && ! echo "$strong" | grep -q "not bound at load"'
# compiler hardening: a library built without the stack protector and FORTIFY is reported, one built with them is not
printf '#include <string.h>\nint hf(const char* s, int n) { char buf[16]; memcpy(buf, s, (unsigned)n); return buf[0] + buf[n & 7]; }\n' > "$T/h.c"
"$CC" -shared -fPIC -O2 -fno-stack-protector -U_FORTIFY_SOURCE -D_FORTIFY_SOURCE=0 -fcf-protection=none -Wl,-z,relro -Wl,-z,now -o "$T/nohard.so" "$T/h.c"
"$CC" -shared -fPIC -O2 -fstack-protector-strong -U_FORTIFY_SOURCE -D_FORTIFY_SOURCE=2 -fcf-protection=full -Wl,-z,relro -Wl,-z,now -o "$T/hard.so" "$T/h.c"
# shellcheck disable=SC2034
nohard=$("$REPO/scripts/check_so.sh" "$T/nohard.so" 2>&1)
# shellcheck disable=SC2034
hard=$("$REPO/scripts/check_so.sh" "$T/hard.so" 2>&1)
ok "no stack protector is reported" 'echo "$nohard" | grep -q "no __stack_chk_fail import"'
ok "no fortified import is reported" 'echo "$nohard" | grep -q "no fortified (\*_chk) libc import"'
ok "no IBT/SHSTK note is reported (x86_64)" '[ "$(uname -m)" != x86_64 ] || echo "$nohard" | grep -q "no IBT property note"'
ok "a library built with the flags gets none of these complaints" '! echo "$hard" | grep -Eq "__stack_chk_fail|_chk\) libc|IBT property|SHSTK property"'

# NEEDED is an allowlist: a library that links anything besides libc, libm and the loader is reported, whatever its name
printf 'int g(void);\nint h(int x) { return x + g(); }\n' > "$T/c.c"; printf 'int g(void) { return 1; }\n' > "$T/libextra.c"
"$CC" -shared -fPIC -o "$T/libextra.so" "$T/libextra.c"
"$CC" -shared -fPIC -Wl,-z,relro -Wl,-z,now -o "$T/withextra.so" "$T/c.c" -L"$T" -lextra
# shellcheck disable=SC2034
withextra=$("$REPO/scripts/check_so.sh" "$T/withextra.so" 2>&1)
ok "an extra NEEDED library is reported" 'echo "$withextra" | grep -q "NEEDED lists libextra.so"'
ok "libc, libm and the loader alone are not reported" '! echo "$strong" | grep -q "NEEDED lists"'
if [ $fails -eq 0 ]; then echo "check_so hardening checks: ok"; else echo "check_so hardening checks: $fails FAILED"; fi
exit $((fails > 0))
