#!/usr/bin/env bash
# Dev loop. Run on the Deck (desktop mode terminal) or from a PC with DECK=deck@<address>.
#   pnpm run build && scripts/deploy.sh
# Copies the built plugin into Decky's plugin folder and restarts the loader (the only reload Decky has).
set -euo pipefail
cd "$(dirname "$0")/.."
[ -f dist/index.js ] || { echo "run: pnpm run build" >&2; exit 1; }
FILES=(dist main.py plugin.json package.json settings_schema.json LICENSE bin)
if [ -n "${DECK:-}" ]; then
  # copy first, then run sudo in an ssh session with a terminal (sudo asks for the Deck's password there)
  TGZ="$(mktemp --suffix=.tgz)"; trap 'rm -f "$TGZ"' EXIT          # not a fixed, guessable name in /tmp
  tar czf "$TGZ" "${FILES[@]}"
  REMOTE="$(ssh "$DECK" 'mktemp --suffix=.tgz')"
  scp "$TGZ" "$DECK:$REMOTE"
  ssh -t "$DECK" "sudo mkdir -p ~/homebrew/plugins/vkIFrames && sudo tar xzf '$REMOTE' -C ~/homebrew/plugins/vkIFrames && rm -f '$REMOTE' && sudo systemctl restart plugin_loader"
else
  sudo mkdir -p ~/homebrew/plugins/vkIFrames
  tar czf - "${FILES[@]}" | sudo tar xzf - -C ~/homebrew/plugins/vkIFrames
  sudo systemctl restart plugin_loader
fi
echo "deployed; open the quick-access menu (...) and look for vkIFrames"
