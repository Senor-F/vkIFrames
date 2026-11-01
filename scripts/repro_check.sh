#!/usr/bin/env bash
# Builds the layer twice from clean checkouts of one commit, in two different folders, and compares the libraries byte for byte.
# usage: scripts/repro_check.sh [commit-ish]        (default HEAD; needs git, cmake, a C++20 compiler, glslangValidator, Vulkan headers)
# environment: VULKAN_INCLUDE_DIR=<dir containing vulkan/vk_layer.h>   if CMake does not find it itself
set -eu
REV=${1:-HEAD}
ROOT=$(git rev-parse --show-toplevel)
SHA=$(git -C "$ROOT" rev-parse "$REV")
T=$(mktemp -d "${TMPDIR:-/tmp}/vkiframes-repro.XXXXXX")
trap 'git -C "$ROOT" worktree remove --force "$T/a/src" 2>/dev/null || true; git -C "$ROOT" worktree remove --force "$T/b/src" 2>/dev/null || true; rm -rf "$T"' EXIT
EXTRA=()
if [ -n "${VULKAN_INCLUDE_DIR:-}" ]; then EXTRA+=("-DFRAMEGEN_VULKAN_INCLUDE_DIR=$VULKAN_INCLUDE_DIR"); fi
for x in a b; do
    mkdir -p "$T/$x"
    git -C "$ROOT" worktree add --detach -q "$T/$x/src" "$SHA"
    [ "$x" = b ] && sleep 2                                  # a different wall-clock time and a different folder for the second build
    cmake -S "$T/$x/src" -B "$T/$x/build" -DCMAKE_BUILD_TYPE=Release -DFRAMEGEN_BUILD_TESTS=OFF "${EXTRA[@]}" >/dev/null
    cmake --build "$T/$x/build" -j"$(nproc)" >/dev/null
done
A=$(sha256sum "$T/a/build/libvkIFrames.so" | cut -d' ' -f1)
B=$(sha256sum "$T/b/build/libvkIFrames.so" | cut -d' ' -f1)
echo "commit $SHA"
echo "build a: $A"
echo "build b: $B"
if [ "$A" = "$B" ]; then echo "reproducible: yes"; exit 0; fi
echo "reproducible: NO"; cmp -l "$T/a/build/libvkIFrames.so" "$T/b/build/libvkIFrames.so" | head -5; exit 1
