"""The panel's status line turns the layer's status.json mode "passthru" (the rate gate is open but the layer cannot hold frames of this game, so nothing is generated) into
a readable text, and the layer really writes that word. usage: python3 -I test_status_text.py"""
import os, re, sys

root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
tsx = open(os.path.join(root, "src", "index.tsx"), encoding="utf-8").read()
layer = open(os.path.join(os.path.dirname(root), "src", "layer_main.cpp"), encoding="utf-8").read()
fails = 0
def ok(name, cond):
    global fails
    if not cond:
        print("FAIL:", name); fails += 1

ok("the panel maps passthru to a readable text", re.search(r'mode === "passthru" \? "pass-through \(frame holding off\)" : mode', tsx) is not None)
ok("the panel prints the mapped text", "mode: ${modeText}" in tsx)
ok("the layer writes the passthru word to status.json", '"passthru"' in layer and '"interp"' in layer)
print("status text: ok" if not fails else "status text: %d FAILED" % fails)
sys.exit(1 if fails else 0)
