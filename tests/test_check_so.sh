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
if [ $fails -eq 0 ]; then echo "check_so hardening checks: ok"; else echo "check_so hardening checks: $fails FAILED"; fi
exit $((fails > 0))
