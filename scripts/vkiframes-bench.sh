#!/usr/bin/env bash
# Runs the layer benchmark in the game that is running now: 30 s warm-up, then 6 x (60 s layer off, 60 s on), about 13 minutes.
# It asks the layer through live.conf (BENCH=<number>); the layer writes bench.json in the config folder, and this script prints it.
#   scripts/vkiframes-bench.sh            start a run and wait for the result
#   scripts/vkiframes-bench.sh --cancel   stop a run (the layer puts your settings back)
# Start the game first (with the layer on) and stay in a demanding scene. Do not change settings during the run: that stops it.
set -euo pipefail
umask 077
dir="${XDG_CONFIG_HOME:-$HOME/.config}/vkiframes"
conf="$dir/live.conf"
mkdir -p "$dir"; chmod 700 "$dir"

want="$(date +%s)"
[ "${1:-}" = "--cancel" ] && want=0

tmp="$(mktemp "$dir/live.conf.XXXXXX")"
trap 'rm -f "$tmp"' EXIT
if [ -f "$conf" ]; then grep -v -E '^(VKIFRAME_|FRAMEGEN_)?BENCH=' "$conf" > "$tmp" || true; fi
echo "VKIFRAME_BENCH=$want" >> "$tmp"
mv "$tmp" "$conf"; trap - EXIT

if [ "$want" = 0 ]; then echo "Cancel requested."; exit 0; fi
echo "Benchmark requested. It starts within a second if a game with the layer is running, and takes about 13 minutes."

start="$(date +%s)"
last=""
while :; do
  now="$(date +%s)"
  if [ -f "$dir/bench.json" ] && [ "$(stat -c %Y "$dir/bench.json")" -ge "$start" ]; then
    echo; cat "$dir/bench.json"; exit 0
  fi
  if [ $((now - start)) -gt 900 ]; then echo "Timed out after 15 minutes: is a game running with the layer on? See $dir/live.log" >&2; exit 1; fi
  state="$(grep -o '"bench":"[a-z]*"' "$dir/status.json" 2>/dev/null | cut -d'"' -f4 || true)"
  if [ "$state" != "$last" ]; then echo "  [$((now - start)) s] ${state:-waiting for the game}"; last="$state"; fi
  sleep 2
done
