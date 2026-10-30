import asyncio, importlib.util, json, os, sys, tempfile, types

root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
tmp = tempfile.mkdtemp()
fake = types.ModuleType("decky")
fake.DECKY_USER_HOME = os.path.join(tmp, "home")
fake.DECKY_PLUGIN_SETTINGS_DIR = os.path.join(tmp, "settings")
fake.DECKY_PLUGIN_DIR = root
class L:
    def info(self, *a): pass
fake.logger = L()
sys.modules["decky"] = fake
spec = importlib.util.spec_from_file_location("main", os.path.join(root, "main.py"))
main = importlib.util.module_from_spec(spec); spec.loader.exec_module(main)
run = asyncio.run

p = main.Plugin(); run(p._main())
conf = open(main.LIVE_CONF).read()
assert "VKIFRAME_TARGET_FPS=60" in conf and "VKIFRAME_PHASE_LOCK=1" in conf, conf
rev0 = p.rev
p.experimental = True        # these checks change gated settings
# clamp and step
run(p.set_value("TARGET_FPS", 999)); assert p.values["TARGET_FPS"] == 120
run(p.set_value("MIN_GAIN", 1.123)); assert p.values["MIN_GAIN"] == 1.12, p.values["MIN_GAIN"]
run(p.set_value("MAX_SYNTHETIC", 0)); assert p.values["MAX_SYNTHETIC"] == 1
run(p.set_value("INTERP_BIDIR", True)); assert p.values["INTERP_BIDIR"] == 1
assert p.rev > rev0 and "VKIFRAME_TARGET_FPS=120" in open(main.LIVE_CONF).read()
# persistence
p2 = main.Plugin(); run(p2._main()); assert p2.values["TARGET_FPS"] == 120
# Target FPS maximum = the higher of 120 and the screen's refresh rate
assert main._parse_xrandr("Screen 0\nDP-1 connected primary 2560x1440\n   2560x1440    143.97*+ 120.00  59.95\n   1920x1080    60.00 ") == 143.97
assert main._parse_xrandr("eDP-1 connected\n   800x1280   90.00*+  60.00\n") == 90.0 and main._parse_xrandr("") == 0.0
def tmax(hz):
    p.refresh_hz = hz; return [i for i in p._schema() if i["key"] == "TARGET_FPS"][0]["max"]
assert tmax(0) == 120 and tmax(90) == 120 and tmax(120) == 120 and tmax(143.97) == 144 and tmax(240) == 240
status = {"refresh_hz": 165.0, "age_s": 1}
run(p._update_display(status)); assert p.refresh_hz == 165.0 and p.refresh_src == "layer"
run(p.set_value("TARGET_FPS", 999)); assert p.values["TARGET_FPS"] == 165
assert json.load(open(main.DISPLAY_FILE))["refresh_hz"] == 165.0
assert "display: refresh rate 165.0 Hz (layer)" in open(main.LIVE_LOG).read()
q = main.Plugin(); run(q._main()); assert q.refresh_hz == 165.0 and q.values["TARGET_FPS"] == 165   # remembered across restarts
p.refresh_hz = 0.0; p.refresh_src = ""; run(p.set_value("TARGET_FPS", 60))
p.experimental = False
# Experimental gates the "more" settings and widens Most generated frames to 10
run(p.reset_values())
try: run(p.set_value("MIN_GAIN", 1.2)); raise SystemExit("gated setting changed without Experimental")
except ValueError: pass
run(p.set_value("MAX_SYNTHETIC", 9)); assert p.values["MAX_SYNTHETIC"] == 4
run(p.set_value("EXPERIMENTAL", 1)); run(p.set_value("MIN_GAIN", 1.2)); assert p.values["MIN_GAIN"] == 1.2
run(p.set_value("MAX_SYNTHETIC", 9)); assert p.values["MAX_SYNTHETIC"] == 9
assert "VKIFRAME_EXPERIMENTAL=1" in open(main.LIVE_CONF).read() and "VKIFRAME_MAX_SYNTHETIC=9" in open(main.LIVE_CONF).read()
r = main.Plugin(); run(r._main()); assert r.experimental and r.values["MAX_SYNTHETIC"] == 9     # kept across restarts
run(p.set_value("EXPERIMENTAL", 0)); assert p.values["MAX_SYNTHETIC"] == 4 and not p.experimental
# a library the manifest points at: a .so owned by the user that others cannot write
lib = os.path.join(tmp, "libx.so"); open(lib, "wb").write(b"x"); os.chmod(lib, 0o755); main.USER_HOME = tmp
main._check_library(lib)
os.chmod(lib, 0o775)
try: main._check_library(lib); raise SystemExit("group-writable library accepted")
except ValueError: pass
txt = os.path.join(tmp, "notalib.txt"); open(txt, "w").write("x"); os.chmod(txt, 0o644)
try: main._check_library(txt); raise SystemExit("non-.so accepted")
except ValueError: pass
bad = os.path.join(tmp, "x.so.txt"); open(bad, "wb").write(b"x"); os.chmod(bad, 0o755)
try: main._check_library(bad); raise SystemExit("x.so.txt accepted")
except ValueError: pass
ok = os.path.join(tmp, "libx.so.1.2"); open(ok, "wb").write(b"x"); os.chmod(ok, 0o755); main._check_library(ok)
# the log view reads only the end of a big file
open(main.LIVE_LOG, "a").write("2026-10-30 19:00:00.000 plugin  junk\n" * 20000)
run(p.set_value("TARGET_FPS", 61)); assert any("TARGET_FPS" in l for l in run(p.get_log(8))) and len(run(p.get_log(8))) <= 8
# unknown key
try: run(p.set_value("NOPE", 1)); raise SystemExit("unknown key accepted")
except ValueError: pass
# reset
run(p.reset_values()); assert p.values["TARGET_FPS"] == 60
p.experimental = True
# the verbose log records every change, who made it and when it applies
log = open(main.LIVE_LOG).read()
assert "plugin  panel: TARGET_FPS 60 -> 120" in log and "asked for 999, clamped" in log and "all settings reset" in log, log
assert any("TARGET_FPS" in l for l in run(p.get_log(10)))
run(p.set_value("EXTRAPOLATE", 1)); assert "read when the next game starts" in open(main.LIVE_LOG).read()
assert "VKIFRAME_EXTRAPOLATE=1" in open(main.LIVE_CONF).read()
# default target: 90 on the Deck OLED (DMI board name Galileo), 60 elsewhere
dmi = os.path.join(tmp, "dmi"); main.DMI_PRODUCT = dmi
open(dmi, "w").write("Galileo\n"); assert p._defaults()["TARGET_FPS"] == 90
open(dmi, "w").write("Jupiter\n"); assert p._defaults()["TARGET_FPS"] == 60
main.DMI_PRODUCT = os.path.join(tmp, "missing"); assert p._defaults()["TARGET_FPS"] == 60
# settings saved under the old names (warp) are carried over to the new ones
open(main.SETTINGS_FILE, "w").write(json.dumps({"WARP": 0, "WARP_BIDIR": 1}))
old = main.Plugin(); run(old._main()); assert old.values["INTERPOLATE"] == 0 and old.values["INTERP_BIDIR"] == 1, old.values
# no stray temp files left behind
assert not [f for f in os.listdir(main.CONF_DIR) if ".tmp." in f]
# install with a given library
lib = os.path.join(tmp, "libvkIFrames.so"); open(lib, "wb").write(b"x")
info = run(p.install_layer(lib)); assert info["installed"] and info["library"] == lib
m = json.load(open(main.MANIFEST))["layer"]
assert m["enable_environment"] == {"VKIFRAME_ENABLE": "1"} and "disable_environment" in m
try: run(p.install_layer("/nonexistent/lib.so")); raise SystemExit("bad path accepted")
except ValueError: pass
# bundled install requires the file
try: run(p.install_layer()); raise SystemExit("installed without bundled lib")
except ValueError: pass
assert not run(p.uninstall_layer())["installed"]
st = run(p.get_state()); assert st["status"] is None and len(st["schema"]) == 13
print("backend tests passed")
