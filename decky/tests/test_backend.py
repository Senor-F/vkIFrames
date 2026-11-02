import asyncio, importlib.util, json, os, sys, tempfile, types

root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
tmp = tempfile.mkdtemp()
fake = types.ModuleType("decky")
fake.DECKY_USER_HOME = os.path.join(tmp, "home"); os.makedirs(fake.DECKY_USER_HOME)       # the home folder exists (the plugin never creates it)
fake.DECKY_PLUGIN_SETTINGS_DIR = os.path.join(tmp, "settings")
fake.DECKY_PLUGIN_DIR = root
class L:
    def info(self, *a): pass
    def error(self, *a): pass
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
run(p.set_value("TARGET_FPS", 999)); assert p.values["TARGET_FPS"] == 600
run(p.set_value("MIN_GAIN", 1.123)); assert p.values["MIN_GAIN"] == 1.12, p.values["MIN_GAIN"]
run(p.set_value("MAX_SYNTHETIC", 0)); assert p.values["MAX_SYNTHETIC"] == 1
run(p.set_value("INTERP_BIDIR", True)); assert p.values["INTERP_BIDIR"] == 1
assert p.rev > rev0 and "VKIFRAME_TARGET_FPS=600" in open(main.LIVE_CONF).read()
# persistence
p2 = main.Plugin(); run(p2._main()); assert p2.values["TARGET_FPS"] == 600
# Target FPS maximum = the layer's limit (600), whatever the screen's refresh rate
assert main._parse_xrandr("Screen 0\nDP-1 connected primary 2560x1440\n   2560x1440    143.97*+ 120.00  59.95\n   1920x1080    60.00 ") == 143.97
assert main._parse_xrandr("eDP-1 connected\n   800x1280   90.00*+  60.00\n") == 90.0 and main._parse_xrandr("") == 0.0
def tmax(hz):
    p.refresh_hz = hz; return [i for i in p._schema() if i["key"] == "TARGET_FPS"][0]["max"]
assert tmax(0) == 600 and tmax(90) == 600 and tmax(240) == 600 and tmax(599.2) == 600 and tmax(700) == 600 and tmax(1000) == 600
run(p.set_value("BASE_FPS_CAP", 500)); assert p.values["BASE_FPS_CAP"] == 240      # the cap reaches 240
run(p.set_value("BASE_FPS_CAP", 120)); assert p.values["BASE_FPS_CAP"] == 120
status = {"refresh_hz": 165.0, "age_s": 1}
run(p._update_display(status)); assert p.refresh_hz == 165.0 and p.refresh_src == "layer"
run(p.set_value("TARGET_FPS", 999)); assert p.values["TARGET_FPS"] == 600
assert json.load(open(main.DISPLAY_FILE))["refresh_hz"] == 165.0
assert "display: refresh rate 165.0 Hz (layer)" in open(main.LIVE_LOG).read()
q = main.Plugin(); run(q._main()); assert q.refresh_hz == 165.0 and q.values["TARGET_FPS"] == 600   # the refresh rate is remembered across restarts, the limit is 600
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
# a live.log left at 0644 by an older version is tightened to 0600 on the next write
os.chmod(main.LIVE_LOG, 0o644); main._log("mode test"); assert (os.stat(main.LIVE_LOG).st_mode & 0o777) == 0o600, oct(os.stat(main.LIVE_LOG).st_mode)
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
assert "plugin  panel: TARGET_FPS 60 -> 600" in log and "asked for 999, clamped" in log and "all settings reset" in log, log
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
old = main.Plugin(); run(old._main()); assert old.values["OPTICAL_FLOW"] == 0 and old.values["INTERP_BIDIR"] == 1, old.values
# optical flow is opt-in, and its key was renamed (WARP -> INTERPOLATE -> OPTICAL_FLOW): what an older plugin saved must keep its meaning, newest name first
def fresh(saved):
    open(main.SETTINGS_FILE, "w").write(json.dumps(saved))
    q = main.Plugin(); run(q._main()); return q
assert "OPTICAL_FLOW" in main.Plugin()._defaults() and main.Plugin()._defaults()["OPTICAL_FLOW"] == 0 and "INTERPOLATE" not in main.Plugin()._defaults()
os.remove(main.SETTINGS_FILE); assert fresh({}).values["OPTICAL_FLOW"] == 0          # nothing saved: the cross-fade
assert fresh({"WARP": 1}).values["OPTICAL_FLOW"] == 1
assert fresh({"INTERPOLATE": 1}).values["OPTICAL_FLOW"] == 1                         # saved by alpha.1 (the panel wrote the whole values)
assert fresh({"OPTICAL_FLOW": 1}).values["OPTICAL_FLOW"] == 1                        # saved by this release
assert fresh({"INTERPOLATE": 1, "OPTICAL_FLOW": 0}).values["OPTICAL_FLOW"] == 0     # the new name wins
assert fresh({"INTERPOLATE": 0, "WARP": 1}).values["OPTICAL_FLOW"] == 0             # INTERPOLATE is newer than WARP
# upgrade: alpha.1 left a full settings.json and a live.conf with INTERPOLATE=1; after the new backend starts the flow is still on and both files use the new key only
alpha1 = {k: v for k, v in main.Plugin()._defaults().items() if k != "OPTICAL_FLOW"}; alpha1["INTERPOLATE"] = 1
open(main.SETTINGS_FILE, "w").write(json.dumps(alpha1))
open(main.LIVE_CONF, "w").write("# written by the vkIFrames Decky plugin\nREV=1\nVKIFRAME_TARGET_FPS=75\nVKIFRAME_INTERPOLATE=1\n")
up = main.Plugin(); run(up._main())
assert up.values["OPTICAL_FLOW"] == 1, up.values
lc = open(main.LIVE_CONF).read(); sj = json.load(open(main.SETTINGS_FILE))
assert "VKIFRAME_OPTICAL_FLOW=1" in lc and "INTERPOLATE" not in lc and "WARP" not in lc, lc
assert sj["OPTICAL_FLOW"] == 1 and "INTERPOLATE" not in sj, sj
up2 = main.Plugin(); run(up2._main()); assert up2.values["OPTICAL_FLOW"] == 1       # and it stays on at the next start
# install.sh leaves a commented template as live.conf for people without the panel: when the plugin starts, the file becomes the plugin's (header, REV, every key with the
# VKIFRAME_ prefix), nothing of the template is left, no stray temporary file
import shutil
shutil.copyfile(os.path.join(os.path.dirname(root), "manifest", "live.conf.template"), main.LIVE_CONF)
os.remove(main.SETTINGS_FILE)
tp = main.Plugin(); run(tp._main())
tc = open(main.LIVE_CONF).read()
assert tc.startswith("# written by the vkIFrames Decky plugin") and "REV=" in tc and "VKIFRAME_OPTICAL_FLOW=0" in tc and "install.sh created this file" not in tc, tc
assert not [f for f in os.listdir(main.CONF_DIR) if ".tmp." in f or f.startswith(".live.conf")], os.listdir(main.CONF_DIR)
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
st = run(p.get_state()); assert st["status"] is None and len(st["schema"]) == len(json.load(open(os.path.join(root, "settings_schema.json"))))
# benchmark request: the number goes to its own file bench.req (one line, 0600), never into live.conf; the result is read back from bench.json
r1 = run(p.start_bench())["token"]
assert open(main.BENCH_REQ).read() == "%d\n" % r1 and (os.lstat(main.BENCH_REQ).st_mode & 0o777) == 0o600, open(main.BENCH_REQ).read()
assert "BENCH" not in open(main.LIVE_CONF).read(), "the request must not be in live.conf"
live_before = open(main.LIVE_CONF).read()
run(p.set_value("TARGET_FPS", 62)); assert "BENCH" not in open(main.LIVE_CONF).read()
assert open(main.BENCH_REQ).read() == "%d\n" % r1                      # a panel write leaves the request alone
r2 = run(p.start_bench())["token"]; assert r2 > r1                      # a second request is a new number, so the layer sees it
assert open(main.BENCH_REQ).read() == "%d\n" % r2
assert not [f for f in os.listdir(main.CONF_DIR) if f.startswith("bench.req.")]      # no temporary file is left
assert run(p.get_state())["bench"] is None
open(main.BENCH_JSON, "w").write('{"status":"complete","reason":"","derived":{"layer_cost_mean_ms":3.5,"gpu_share":0.22}}')
b = run(p.get_state())["bench"]; assert b["status"] == "complete" and b["layer_cost_mean_ms"] == 3.5 and b["gpu_share"] == 0.22, b
open(main.BENCH_JSON, "w").write("not json"); assert run(p.get_state())["bench"] is None

# ---- hardening: root reads and writes in folders the user controls (symlinks), user-editable settings.json
import stat as _st
# a symlink in place of a private folder: _private_dir must not change the mode of the target
tgt = os.path.join(tmp, "elsewhere"); os.makedirs(tgt, 0o755); os.chmod(tgt, 0o755)
lnk = os.path.join(main.USER_HOME, "linkdir"); os.symlink(tgt, lnk)
try: main._private_dir(lnk); raise SystemExit("_private_dir accepted a symlink")
except OSError: pass
assert _st.S_IMODE(os.stat(tgt).st_mode) == 0o755, "chmod followed a symlink"
plain = os.path.join(main.USER_HOME, "plaindir"); main._private_dir(plain); assert _st.S_IMODE(os.stat(plain).st_mode) == 0o700
# the config and data folders replaced by symlinks: nothing is written into the target (live.conf, live.log, the library, the manifest folder)
real_conf = main.CONF_DIR; moved = real_conf + ".real"; os.rename(real_conf, moved)
bait = os.path.join(tmp, "bait_conf"); os.makedirs(bait, 0o755); os.symlink(bait, real_conf)
before = sorted(os.listdir(bait)); rev_before = p.rev
run(p.set_value("TARGET_FPS", 61))                                    # must not raise; settings.json (the plugin's own folder) is still written
assert sorted(os.listdir(bait)) == before == [], os.listdir(bait)
assert json.load(open(main.SETTINGS_FILE))["TARGET_FPS"] == 61
main._log("must not land in the target"); assert os.listdir(bait) == []
try: run(p.start_bench()); raise SystemExit("start_bench claimed a request it could not write")      # the panel is told, nothing lands in the target
except ValueError: pass
assert os.listdir(bait) == []
os.unlink(real_conf); os.rename(moved, real_conf)
had_data = os.path.exists(main.DATA_DIR)
if had_data:
    d_moved = main.DATA_DIR + ".real"; os.rename(main.DATA_DIR, d_moved)
bait2 = os.path.join(tmp, "bait_data"); os.makedirs(bait2, 0o755); os.symlink(bait2, main.DATA_DIR)
bundled0 = os.path.join(root, "bin", "libvkIFrames.so"); made_bin = not os.path.exists(bundled0)
os.makedirs(os.path.dirname(bundled0), exist_ok=True)
if made_bin: open(bundled0, "wb").write(b"\x7fELF fake")
try:
    try: run(p.install_layer()); raise SystemExit("install_layer wrote into a symlinked data folder")
    except ValueError: pass
    assert os.listdir(bait2) == [] and _st.S_IMODE(os.stat(bait2).st_mode) == 0o755
finally:
    if made_bin: os.unlink(bundled0)
    os.unlink(main.DATA_DIR)
    if had_data: os.rename(d_moved, main.DATA_DIR)
run(p.set_value("TARGET_FPS", 62))                                    # back to normal
assert "VKIFRAME_TARGET_FPS=62" in open(main.LIVE_CONF).read()
# status.json, bench.json and live.log as symlinks are not read
secret = os.path.join(tmp, "secret.json"); open(secret, "w").write('{"native_fps": 1, "status": "complete", "derived": {}}')
for f in (main.STATUS_JSON, main.BENCH_JSON):
    if os.path.lexists(f): os.unlink(f)
    os.symlink(secret, f)
st = run(p.get_state()); assert st["status"] is None and st["bench"] is None, st
if os.path.lexists(main.LIVE_LOG): os.unlink(main.LIVE_LOG)
open(secret, "w").write("2026-10-30 19:02:15.030 plugin  panel: X 1 -> 2 (REV 1)\n"); os.symlink(secret, main.LIVE_LOG)
assert run(p.get_log()) == []
os.unlink(main.LIVE_LOG); open(main.LIVE_LOG, "w").write("2026-10-30 19:02:15.030 plugin  panel: X 1 -> 2 (REV 1)\n")
assert run(p.get_log()) == ["19:02:15 X 1 -> 2 [plugin]"], run(p.get_log())
for f in (main.STATUS_JSON, main.BENCH_JSON): os.unlink(f)
# the bundled library is copied without following a symlink planted at the .new name; a leftover file there goes
os.makedirs(os.path.join(root, "bin"), exist_ok=True)
bundled = os.path.join(root, "bin", "libvkIFrames.so"); had_bundled = os.path.exists(bundled)
if not had_bundled: open(bundled, "wb").write(b"\x7fELF fake")
try:
    os.makedirs(main.DATA_DIR, exist_ok=True)
    victim = os.path.join(tmp, "victim"); open(victim, "w").write("keep"); os.chmod(victim, 0o600)
    new = main.INSTALLED_LIB + ".new"
    if os.path.lexists(new): os.unlink(new)
    os.symlink(victim, new)
    run(p.install_layer())
    assert open(victim).read() == "keep" and _st.S_IMODE(os.stat(victim).st_mode) == 0o600, "install wrote through a symlink"
    assert not os.path.islink(main.INSTALLED_LIB) and os.access(main.INSTALLED_LIB, os.X_OK)
    assert open(main.INSTALLED_LIB, "rb").read() == open(bundled, "rb").read()
    run(p.uninstall_layer())
finally:
    if not had_bundled: os.unlink(bundled)
# settings.json: one bad value keeps that setting's default and nothing else; a non-object file is ignored
good = {"TARGET_FPS": 77, "MIN_GAIN": [1, 2], "MAX_SYNTHETIC": 1e999, "BASE_FPS_CAP": None, "INTERP_BIDIR": 0, "EXPERIMENTAL": [1]}
open(main.SETTINGS_FILE, "w").write(json.dumps(good).replace("Infinity", "1e999"))
p3 = main.Plugin(); run(p3._main())                                    # must not raise
d = p3._defaults()
assert p3.values["TARGET_FPS"] == 77 and p3.values["INTERP_BIDIR"] == 0, p3.values
assert p3.values["MIN_GAIN"] == d["MIN_GAIN"] and p3.values["BASE_FPS_CAP"] == d["BASE_FPS_CAP"], p3.values
open(main.SETTINGS_FILE, "w").write("[1, 2]")
p4 = main.Plugin(); run(p4._main()); assert p4.values == p4._defaults()
print("backend tests passed")

# --- log and temporary-file caps
main.LOG_CAP_BYTES = 4096
main._log("start"); cap_size_ok = os.path.getsize(main.LIVE_LOG) < 4096
for i in range(400): main._log("filler line %d with some text to grow the file" % i)
assert os.path.exists(main.LIVE_LOG + ".1"), "live.log was not rotated"
assert os.path.getsize(main.LIVE_LOG) <= 4096 + 200 and os.path.getsize(main.LIVE_LOG + ".1") <= 4096 + 200
assert not os.path.exists(main.LIVE_LOG + ".2")
assert (os.stat(main.LIVE_LOG).st_mode & 0o777) == 0o600
# stale temporary files of an earlier version are removed at start, our own files stay
stale = [main.LIVE_CONF + ".tmp.123", main.LIVE_CONF + ".tmp.99999"]
for f in stale: open(f, "w").write("x")
keep = os.path.join(os.path.dirname(main.LIVE_CONF), "notes.tmp.txt"); open(keep, "w").write("x")
p3 = main.Plugin(); run(p3._main())
assert not any(os.path.exists(f) for f in stale) and os.path.exists(keep) and os.path.exists(main.LIVE_CONF)
assert not any(".tmp" in n for n in os.listdir(os.path.dirname(main.LIVE_CONF)) if n != "notes.tmp.txt"), os.listdir(os.path.dirname(main.LIVE_CONF))
# while the layer's status.json is fresh the layer rotates live.log, not the plugin
open(main.STATUS_JSON, "w").write("{}")
main.LOG_CAP_BYTES = 100
before = os.path.getsize(main.LIVE_LOG); os.unlink(main.LIVE_LOG + ".1") if os.path.exists(main.LIVE_LOG + ".1") else None
main._log("one more line while the layer runs"); assert os.path.getsize(main.LIVE_LOG) > before and not os.path.exists(main.LIVE_LOG + ".1")
os.unlink(main.STATUS_JSON)
print("log caps: ok")
