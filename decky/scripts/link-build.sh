#!/usr/bin/env bash
# Dev helper, run on the Deck: write the layer manifest so it loads straight from a local build
# (rebuilds then go live without copying). Usage: scripts/link-build.sh /path/to/vkIFrames/build/libvkIFrames.so
set -euo pipefail
LIB="$(readlink -f "${1:?path to libvkIFrames.so}")"
[ -f "$LIB" ] || { echo "not found: $LIB" >&2; exit 1; }
case "$LIB" in *[[:cntrl:]]*) echo "library path contains control characters" >&2; exit 1;; esac
LIB_JSON="${LIB//\\/\\\\}"; LIB_JSON="${LIB_JSON//\"/\\\"}"      # the path goes into a JSON string
D="$HOME/.local/share/vulkan/implicit_layer.d"; mkdir -p "$D"
cat > "$D/VkLayer_frame_gen.json" <<JSON
{
  "file_format_version": "1.1.2",
  "layer": {
    "name": "VK_LAYER_CUSTOM_frame_gen",
    "type": "GLOBAL",
    "library_path": "$LIB_JSON",
    "api_version": "1.3.0",
    "implementation_version": "1",
    "description": "vkIFrames dynamic frame interpolation layer",
    "functions": {
      "vkNegotiateLoaderLayerInterfaceVersion": "FrameGen_NegotiateLoaderLayerInterfaceVersion",
      "vkGetInstanceProcAddr": "FrameGen_GetInstanceProcAddr",
      "vkGetDeviceProcAddr": "FrameGen_GetDeviceProcAddr"
    },
    "enable_environment": { "VKIFRAME_ENABLE": "1" },
    "disable_environment": { "VKIFRAME_DISABLE": "1" }
  }
}
JSON
echo "manifest -> $D/VkLayer_frame_gen.json (library $LIB)"
