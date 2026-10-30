#!/usr/bin/env bash
# Steam launch wrapper for Game Mode tests (non-Steam game or a normal game):
#   Launch options:  [VKIFRAME_BASE_FPS_CAP=40] /home/deck/vkIFrames/scripts/gamemode_launch.sh %command% [game args]
# Loads the layer through environment variables and writes everything the layer prints to build/gamemode_<time>.log.
# Optional env (put before the script path): VKIFRAME_TARGET_FPS (default 60), VKIFRAME_BASE_FPS_CAP, VKIFRAME_PRESENTER=0 ...
ROOT="$(cd "$(dirname "$(readlink -f "$0")")/.." && pwd)"
LOG="$ROOT/build/gamemode_$(date +%Y%m%d_%H%M%S).log"
mkdir -p "$ROOT/build"
{
  echo "== gamemode_launch $(date -u +%FT%TZ) $(uname -sr)"
  echo "== command: $*"
  echo "== session: XDG_SESSION_TYPE=${XDG_SESSION_TYPE:-?} WAYLAND_DISPLAY=${WAYLAND_DISPLAY:-?} DISPLAY=${DISPLAY:-?} GAMESCOPE=$(pgrep -x gamescope >/dev/null && echo running || echo no)"
  echo "== layer manifest: $(ls "$ROOT/build/VkLayer_frame_gen.json" 2>&1)"
} > "$LOG"
export VK_LAYER_PATH="$ROOT/build${VK_LAYER_PATH:+:$VK_LAYER_PATH}"
export VK_LOADER_LAYERS_ENABLE=VK_LAYER_CUSTOM_frame_gen
export VKIFRAME_DUMP_DIR="${VKIFRAME_DUMP_DIR:-${FRAMEGEN_DUMP_DIR:-$ROOT/dump}}"
export VKIFRAME_TARGET_FPS="${VKIFRAME_TARGET_FPS:-${FRAMEGEN_TARGET_FPS:-60}}"
exec "$@" >>"$LOG" 2>&1
