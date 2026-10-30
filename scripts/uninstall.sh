#!/usr/bin/env bash
# Removes the implicit-layer manifest written by scripts/install.sh.
F="${XDG_DATA_HOME:-$HOME/.local/share}/vulkan/implicit_layer.d/VkLayer_frame_gen.json"
if [ -f "$F" ]; then rm -f "$F" && echo "removed $F"; else echo "nothing to remove ($F)"; fi
