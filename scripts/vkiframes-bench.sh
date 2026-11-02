#!/usr/bin/env bash
# Runs the layer benchmark in the game that is running now: 30 s warm-up, then 6 x (60 s layer off, 60 s on), about 13 minutes.
# It asks the layer through the file bench.req in the config folder (a request number; the layer checks it once a second, it is not a setting and not in
# live.conf). The layer writes bench.json next to it, and this script prints it.
#   scripts/vkiframes-bench.sh              start a run and wait for the result
#   scripts/vkiframes-bench.sh --no-wait    start a run and return at once
#   scripts/vkiframes-bench.sh --cancel     stop a run (the layer puts your settings back)
# Start the game first (with the layer on) and stay in a demanding scene. Do not change settings during the run: that stops it.
set -euo pipefail
umask 077
dir="${XDG_CONFIG_HOME:-$HOME/.config}/vkiframes"
req="$dir/bench.req"
# A symbolic link at the folder or at the request file would send the write (and the chmod) somewhere else: refuse, before anything is created or changed.
if [ -L "$dir" ]; then echo "$0: $dir is a symbolic link: refused (remove it or point XDG_CONFIG_HOME elsewhere)" >&2; exit 1; fi
if [ -L "$req" ]; then echo "$0: $req is a symbolic link: refused (remove it)" >&2; exit 1; fi
mkdir -p "$dir"; chmod 700 "$dir"

wait=1
want="$(date +%s)"
case "${1:-}" in
  "") ;;
  --no-wait) wait=0 ;;
  --cancel) want=0 ;;
  *) echo "usage: $0 [--no-wait | --cancel]" >&2; exit 2 ;;
esac

tmp="$(mktemp "$dir/bench.req.XXXXXX")"
trap 'rm -f "$tmp"' EXIT
echo "$want" > "$tmp"
mv "$tmp" "$req"; trap - EXIT

if [ "$want" = 0 ]; then echo "Cancel requested."; exit 0; fi
echo "Benchmark requested. It starts within a second if a game with the layer is running, and takes about 13 minutes."
[ "$wait" = 1 ] || exit 0

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
