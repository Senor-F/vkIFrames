#!/bin/sh
# Release checks on the layer library: exports, imports, glibc floor, no C++ runtime dependency, hardening (full RELRO, bind now, no executable stack), no personal strings.
# usage: scripts/check_so.sh path/to/libvkIFrames.so      exit 0 = all checks pass, 1 = at least one failed, 2 = usage / tool missing
set -u
LIB=${1:-}
if [ -z "$LIB" ] || [ ! -f "$LIB" ]; then echo "usage: $0 path/to/libvkIFrames.so" >&2; exit 2; fi
for t in nm objdump readelf strings sort; do command -v "$t" >/dev/null 2>&1 || { echo "check_so: $t not found" >&2; exit 2; }; done
FLOOR=2.34
fail=0
bad() { echo "FAIL: $*"; fail=1; }

# 1. exported symbols: exactly the three entry points
want="FrameGen_GetDeviceProcAddr
FrameGen_GetInstanceProcAddr
FrameGen_NegotiateLoaderLayerInterfaceVersion"
have=$(nm -D --defined-only "$LIB" | awk '$2 ~ /^[TtDdBbRrWwVv]$/ {print $3}' | sed 's/@.*//' | sort -u)
[ "$have" = "$want" ] || bad "exported symbols differ from the three entry points; found:$(printf '\n%s' "$have")"

# 2. imports: no versions from the C++ runtime or libgcc, glibc not above the floor
ver=$(objdump -T "$LIB" | grep -oE '(GLIBCXX|CXXABI|GCC)_[0-9.]+' | sort -u)
[ -z "$ver" ] || bad "imports versioned symbols of the C++ runtime or libgcc: $(printf '%s' "$ver" | tr '\n' ' ')"
maxg=$(objdump -T "$LIB" | grep -oE 'GLIBC_[0-9.]+' | sed 's/GLIBC_//' | sort -V | tail -1)
if [ -n "$maxg" ]; then
    top=$(printf '%s\n%s\n' "$FLOOR" "$maxg" | sort -V | tail -1)
    [ "$top" = "$FLOOR" ] || bad "needs GLIBC_$maxg, above the floor GLIBC_$FLOOR"
fi
# (weak undefined references, such as the libitm hooks of the static runtime, are not dependencies)
if nm -D --undefined-only "$LIB" | awk '$1 == "U" {print $2}' | grep -E '^_Z' >/dev/null; then bad "undefined C++ symbols (the runtime is not static)"; fi

# 3. dynamic section: only these libraries (glibc's own: libc, libm and the dynamic loader), so no C++ runtime, libgcc_s or anything else the host may lack or replace; no run path
needed=$(readelf -d "$LIB" | awk '/NEEDED/ {gsub(/[][]/,"",$NF); print $NF}')
for n in $needed; do
    case "$n" in
        libc.so.6|libm.so.6|ld-linux-x86-64.so.2) ;;
        *) bad "NEEDED lists $n, which is not one of libc.so.6, libm.so.6, ld-linux-x86-64.so.2 (the C++ runtime and libgcc_s must be linked statically)" ;;
    esac
done
readelf -d "$LIB" | grep -E 'RPATH|RUNPATH' >/dev/null && bad "RPATH or RUNPATH is set"

# 3b. compiler hardening that was asked for explicitly in CMakeLists.txt (release builds): stack protector and fortified libc calls were really compiled in
nm -D --undefined-only "$LIB" | awk '{print $NF}' | grep -q '^__stack_chk_fail' || bad "no __stack_chk_fail import: built without -fstack-protector-strong"
nm -D --undefined-only "$LIB" | awk '{print $NF}' | grep -Eq '^__[a-z_0-9]+_chk(@|$)' || bad "no fortified (*_chk) libc import: built without _FORTIFY_SOURCE or without optimisation"
if [ "$(uname -m)" = x86_64 ]; then
    notes=$(readelf -n "$LIB")
    printf '%s\n' "$notes" | grep -q 'IBT' || bad "no IBT property note: built without -fcf-protection"
    printf '%s\n' "$notes" | grep -q 'SHSTK' || bad "no SHSTK property note: built without -fcf-protection"
fi

# 4. hardening: full RELRO (a GNU_RELRO segment and every symbol bound at load, so the GOT is read-only), no executable stack, no text relocations
readelf -lW "$LIB" | grep -q 'GNU_RELRO' || bad "no GNU_RELRO segment (link with -z relro)"
readelf -d "$LIB" | grep -qE '\(BIND_NOW\)|\(FLAGS\).*NOW|\(FLAGS_1\).*NOW' || bad "not bound at load: no BIND_NOW / FLAGS NOW (link with -z now): the GOT stays writable"
readelf -lW "$LIB" | grep 'GNU_STACK' | grep -q 'RWE' && bad "the stack is executable"
readelf -d "$LIB" | grep -q 'TEXTREL' && bad "the library has text relocations"

# 5. no personal strings: home folders, the user name, the host name
s=$(strings -a "$LIB")
echo "$s" | grep -F '/home/' >/dev/null && bad "a string contains /home/"
echo "$s" | grep -F '/Users/' >/dev/null && bad "a string contains /Users/"
user=$(id -un 2>/dev/null || true); host=$(hostname 2>/dev/null || true)
for w in "$user" "$host"; do
    [ ${#w} -ge 3 ] || continue
    echo "$s" | grep -F -w -- "$w" >/dev/null && bad "a string contains the user or host name"
done

[ $fail -eq 0 ] && echo "check_so: all checks pass ($LIB)"
exit $fail
