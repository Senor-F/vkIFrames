#!/usr/bin/env bash
# Reports whether this machine can build and test vkIFrames. Paste the output back to Claude.
echo "== system"; uname -srm; grep PRETTY_NAME /etc/os-release 2>/dev/null
echo "session: ${XDG_SESSION_TYPE:-unknown}  display: ${WAYLAND_DISPLAY:-${DISPLAY:-none}}"
echo "== steamos / hardware"
# shellcheck source=/dev/null
. /etc/os-release 2>/dev/null; echo "os id: ${ID:-?} ${VARIANT_ID:+(variant $VARIANT_ID)} build: ${BUILD_ID:-?}"
b=$(cat /sys/devices/virtual/dmi/id/board_name 2>/dev/null); echo "board: ${b:-unknown} (Jupiter = Deck LCD, Galileo = Deck OLED)"
(lspci 2>/dev/null | grep -iE "vga|display" | head -2) || true
echo "glibc: $(ldd --version 2>/dev/null | head -1)"
echo "read-only root: $(findmnt -no OPTIONS / 2>/dev/null | grep -q '\bro\b' && echo yes || echo no)"
echo "gamescope: $(command -v gamescope || echo MISSING)   gamescope session: $(pgrep -x gamescope >/dev/null && echo running || echo no)"
echo "distrobox/podman/docker: $(command -v distrobox || echo -) $(command -v podman || echo -) $(command -v docker || echo -)"
echo "== vulkan layers already installed"
for d in /usr/share/vulkan/implicit_layer.d /etc/vulkan/implicit_layer.d "$HOME/.local/share/vulkan/implicit_layer.d"; do
  [ -d "$d" ] && for f in "$d"/*.json; do [ -e "$f" ] && echo "$f: $(grep -o '"name": *"[^"]*"' "$f" | head -1)"; done; done
echo "== build tools"
for c in cmake g++ git glslangValidator; do printf '%-18s %s\n' "$c" "$(command -v $c || echo MISSING)"; done
cmake --version 2>/dev/null | head -1; g++ --version 2>/dev/null | head -1
echo "== vulkan headers"
f=$(find /usr/include /usr/local/include "${VULKAN_SDK:-/nonexistent}/include" -path '*vulkan/vk_layer.h' 2>/dev/null | head -1)
echo "vk_layer.h: ${f:-MISSING}"
if [ -n "$f" ]; then h="$(dirname "$f")/vulkan_core.h"
  grep -q VK_EXT_PRESENT_TIMING_EXTENSION_NAME "$h" && echo "headers have VK_EXT_present_timing: yes" || echo "headers have VK_EXT_present_timing: NO (need newer Vulkan-Headers)"; fi
echo "== vulkan runtime"
for c in vulkaninfo vkcube vkcube-wayland; do printf '%-18s %s\n' "$c" "$(command -v $c || echo MISSING)"; done
if command -v vulkaninfo >/dev/null; then
  vulkaninfo --summary 2>/dev/null | grep -E "apiVersion|deviceName|driverName|driverInfo" | head -8   # driverInfo shows the Mesa version (present timing: Wayland >= 26.1, X11/XWayland >= 26.2)
  echo -n "present timing on this driver: "; vulkaninfo 2>/dev/null | grep -qi "VK_EXT_present_timing" && echo yes || echo no
  echo -n "present_id2 on this driver:    "; vulkaninfo 2>/dev/null | grep -qi "VK_KHR_present_id2" && echo yes || echo no
fi
echo "== validation layer"
found=""; for f in /usr/share/vulkan/explicit_layer.d/*[Vv]alidation*; do [ -e "$f" ] && { basename "$f"; found=1; }; done
[ -n "$found" ] || echo "VK_LAYER_KHRONOS_validation: not found (optional)"
