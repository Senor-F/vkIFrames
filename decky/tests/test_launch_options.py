"""The launch options the panel offers to copy (decky/src/index.tsx) are exactly the lines that were tested. The plugin only copies them; it never sets a launch option.
usage: python3 -I test_launch_options.py"""
import os, re, sys

root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
src = open(os.path.join(root, "src", "index.tsx"), encoding="utf-8").read()
fails = 0
def ok(name, cond):
    global fails
    if not cond:
        print("FAIL:", name); fails += 1

def const(name):
    m = re.search(r"^const " + name + r" = (\"[^\"\n]*\"|'[^'\n]*');", src, re.M)
    if not m: return None
    return m.group(1)[1:-1]

expected = {
    "LAUNCH_PLAIN": "VKIFRAME_ENABLE=1",
    "LAUNCH_FULL": "VKIFRAME_ENABLE=1 %command%",
    "LAUNCH_32BIT": "PROTON_USE_WOW64=1 VKIFRAME_ENABLE=1 %command%",
    "LAUNCH_DXVK": 'DXVK_CONFIG="dxvk.enablePresentTiming = False" PROTON_USE_WOW64=1 VKIFRAME_ENABLE=1 %command%',
}
for k, v in expected.items():
    got = const(k)
    ok(k + " is exactly the tested line", got == v)
dx = const("LAUNCH_DXVK") or ""
ok("the DXVK line ends with %command% and holds VKIFRAME_ENABLE=1", dx.endswith(" %command%") and "VKIFRAME_ENABLE=1" in dx)
ok("quotes in the DXVK line are balanced", dx.count('"') == 2)
ok("each constant is copied by a button and nowhere else", all(len(re.findall(r"copy\(" + k + r"\)", src)) == 1 for k in expected))
ok("the button shows the whole line", "{LAUNCH_DXVK}" in src)
ok("the plugin never sets a launch option (no LAUNCH constant is passed to a backend call)", not re.search(r"callable<[^>]*>\(\"[a-z_]*launch", src))
print("launch options: ok" if not fails else "launch options: %d FAILED" % fails)
sys.exit(1 if fails else 0)
