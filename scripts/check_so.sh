#!/bin/sh
# Release checks on the layer library: exports, imports, glibc floor, no C++ runtime dependency, no personal strings.
# usage: scripts/check_so.sh path/to/libvkIFrames.so      exit 0 = all checks pass, 1 = at least one failed, 2 = usage / tool missing
set -u
LIB=${1:-}
[ -n "$LIB" ] && [ -f "$LIB" ] || { echo "usage: $0 path/to/libvkIFrames.so" >&2; exit 2; }
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
[ -z "$ver" ] || bad "imports versioned symbols of the C++ runtime or libgcc: $(echo $ver)"
maxg=$(objdump -T "$LIB" | grep -oE 'GLIBC_[0-9.]+' | sed 's/GLIBC_//' | sort -V | tail -1)
if [ -n "$maxg" ]; then
    top=$(printf '%s\n%s\n' "$FLOOR" "$maxg" | sort -V | tail -1)
    [ "$top" = "$FLOOR" ] || bad "needs GLIBC_$maxg, above the floor GLIBC_$FLOOR"
fi
# (weak undefined references, such as the libitm hooks of the static runtime, are not dependencies)
if nm -D --undefined-only "$LIB" | awk '$1 == "U" {print $2}' | grep -E '^_Z' >/dev/null; then bad "undefined C++ symbols (the runtime is not static)"; fi

# 3. dynamic section: no C++ runtime / libgcc library, no run path
needed=$(readelf -d "$LIB" | awk '/NEEDED/ {gsub(/[][]/,"",$NF); print $NF}')
echo "$needed" | grep -E 'libstdc\+\+|libgcc_s' >/dev/null && bad "NEEDED lists the C++ runtime or libgcc_s: $(echo $needed)"
readelf -d "$LIB" | grep -E 'RPATH|RUNPATH' >/dev/null && bad "RPATH or RUNPATH is set"

# 4. no personal strings: home folders, the user name, the host name
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
