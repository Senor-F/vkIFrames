"""manifest/live.conf.template (what install.sh creates as ~/.config/vkiframes/live.conf for people without the Decky panel) must agree with decky/settings_schema.json,
the single source of the settings: the same keys, defaults and ranges, start-of-game keys marked as such, no experimental or hidden key, no old name, and not one active
line (the file must change nothing until a line is uncommented). usage: python3 -I test_live_template.py <repo root>"""
import json, re, sys

root = sys.argv[1]
tpl = open(root + "/manifest/live.conf.template", encoding="utf-8").read().split("\n")
schema = json.load(open(root + "/decky/settings_schema.json", encoding="utf-8"))
items = schema if isinstance(schema, list) else schema.get("settings", schema.get("items"))
by_key = {i["key"]: i for i in items}
fails = 0
def ok(name, cond):
    global fails
    if not cond:
        print("FAIL:", name); fails += 1

ok("no active line (every line is blank or a comment)", all(l.strip() == "" or l.lstrip().startswith("#") for l in tpl))
rows, group = {}, None
for l in tpl:
    if l.startswith("# --- changes apply"): group = "live"
    elif l.startswith("# --- read once"): group = "restart"
    m = re.match(r"^# ([A-Z][A-Z_]*)=(\S+)\s+\((.*)\)\s*$", l)
    if m: rows[m.group(1)] = (m.group(2), m.group(3), group)
excluded = {"EXPERIMENTAL", "ONESIDED_FILL"}          # the experimental gate and what it unlocks: not offered in a template
ok("the keys are the schema's, without the experimental ones", set(rows) == set(by_key) - excluded)
ok("no old name (INTERPOLATE, WARP)", "INTERPOLATE" not in rows and "WARP" not in rows and "INTERPOLATE" not in "\n".join(tpl).replace("INTERP_BIDIR", ""))
for k, (val, text, grp) in rows.items():
    it = by_key.get(k)
    if not it: continue
    ok(k + " default " + val + " equals the schema's " + str(it["default"]), abs(float(val) - float(it["default"])) < 1e-9)
    if it["type"] == "toggle":
        ok(k + " says 0 or 1", "0 or 1" in text)
    else:
        m = re.search(r"(\d+(?:\.\d+)?) to (\d+(?:\.\d+)?)", text)
        ok(k + " range in the text", m is not None and abs(float(m.group(1)) - float(it["min"])) < 1e-9 and abs(float(m.group(2)) - float(it["max"])) < 1e-9)
    ok(k + " is in the right group", (grp == "restart") == (it.get("needsLayer") == "restart"))
    if it.get("needsLayer") == "restart": ok(k + " says it needs a game restart", "needs a game restart" in text)
hdr = re.sub(r"\s+", " ", " ".join(l.lstrip("# ") for l in tpl))
ok("the header says an active line overrides the launch option", "OVERRIDES the same setting in the game's launch options" in hdr)
ok("the header says the Decky panel rewrites the file", "The Decky panel" in hdr and "overwritten" in hdr)
print("live.conf template: ok" if not fails else "live.conf template: %d FAILED" % fails)
sys.exit(1 if fails else 0)
