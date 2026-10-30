#!/usr/bin/env bash
# Installs vkIFrames as an implicit Vulkan layer for the current user (no root needed, no launch script).
#   scripts/install.sh [path/to/libvkIFrames.so]      default: <repo>/build/libvkIFrames.so
# Afterwards a game uses the layer when its launch options start with VKIFRAME_ENABLE=1, e.g.
#   VKIFRAME_ENABLE=1 VKIFRAME_TARGET_FPS=90 VKIFRAME_BASE_FPS_CAP=45 VKIFRAME_INTERPOLATE=1 %command%
# Remove it again with scripts/uninstall.sh.
set -eu
ROOT="$(cd "$(dirname "$(readlink -f "$0")")/.." && pwd)"
LIB="$(readlink -f "${1:-$ROOT/build/libvkIFrames.so}")"
[ -f "$LIB" ] || { echo "library not found: $LIB (build it first, or pass its path)" >&2; exit 1; }
# the path goes into a JSON string: escape backslash and double quote, refuse control characters
case "$LIB" in *[[:cntrl:]]*) echo "library path contains control characters" >&2; exit 1;; esac
LIB_JSON="${LIB//\\/\\\\}"; LIB_JSON="${LIB_JSON//\"/\\\"}"
DIR="${XDG_DATA_HOME:-$HOME/.local/share}/vulkan/implicit_layer.d"
mkdir -p "$DIR"
cat > "$DIR/VkLayer_frame_gen.json" <<EOF
{
  "file_format_version": "1.1.2",
  "layer": {
    "name": "VK_LAYER_CUSTOM_frame_gen",
    "type": "GLOBAL",
    "library_path": "$LIB_JSON",
    "api_version": "1.3.0",
    "implementation_version": "1",
    "description": "vkIFrames dynamic frame interpolation layer",
    "enable_environment": { "VKIFRAME_ENABLE": "1" },
    "disable_environment": { "VKIFRAME_DISABLE": "1" },
    "functions": {
      "vkNegotiateLoaderLayerInterfaceVersion": "FrameGen_NegotiateLoaderLayerInterfaceVersion",
      "vkGetInstanceProcAddr": "FrameGen_GetInstanceProcAddr",
      "vkGetDeviceProcAddr": "FrameGen_GetDeviceProcAddr"
    }
  }
}
EOF
echo "installed: $DIR/VkLayer_frame_gen.json -> $LIB"
echo "Use:  VKIFRAME_ENABLE=1 [VKIFRAME_LOG_FILE=\$HOME/vkiframe.log] [settings] %command%"
