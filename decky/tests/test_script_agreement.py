"""The install scripts and the Decky plugin must agree on paths, modes and the manifest: install with one, remove with the other, in both directions.
usage: python3 test_script_agreement.py   (run from anywhere; finds the repository from this file)"""
import asyncio, importlib.util, json, os, stat, subprocess, sys, tempfile, types

here = os.path.dirname(os.path.abspath(__file__))
plugin_dir = os.path.dirname(here)
repo = os.path.dirname(plugin_dir)
tmp = tempfile.mkdtemp()
home = os.path.join(tmp, "home dir")
os.makedirs(home)
bundle = os.path.join(tmp, "plugin")
os.makedirs(os.path.join(bundle, "bin"))
elf = b"\x7fELF" + b"fake library"
open(os.path.join(bundle, "bin", "libvkIFrames.so"), "wb").write(elf)

fake = types.ModuleType("decky")
fake.DECKY_USER_HOME = home
fake.DECKY_PLUGIN_SETTINGS_DIR = os.path.join(tmp, "settings")
fake.DECKY_PLUGIN_DIR = bundle
class L:
    def info(self, *a): pass
    def error(self, *a): pass
fake.logger = L()
sys.modules["decky"] = fake
spec = importlib.util.spec_from_file_location("main", os.path.join(plugin_dir, "main.py"))
main = importlib.util.module_from_spec(spec); spec.loader.exec_module(main)

rel = os.path.join(tmp, "release")
os.makedirs(os.path.join(rel, "scripts")); os.makedirs(os.path.join(rel, "manifest"))
for s in ("install.sh", "uninstall.sh"):
    subprocess.check_call(["cp", os.path.join(repo, "scripts", s), os.path.join(rel, "scripts", s)])
subprocess.check_call(["cp", os.path.join(repo, "manifest", "VkLayer_frame_gen.json.in"), os.path.join(rel, "manifest")])
open(os.path.join(rel, "libvkIFrames.so"), "wb").write(elf)
env = dict(os.environ, HOME=home); env.pop("XDG_DATA_HOME", None)
def script(name, *args):
    return subprocess.run([os.path.join(rel, "scripts", name), *args], env=env, capture_output=True, text=True)
def mode(p): return stat.S_IMODE(os.lstat(p).st_mode)
run = asyncio.run
p = main.Plugin()

# 1. script installs, plugin removes
assert script("install.sh").returncode == 0
assert os.path.exists(main.MANIFEST) and os.path.exists(main.INSTALLED_LIB)       # the plugin looks in the same places
script_manifest = json.load(open(main.MANIFEST))
info = run(p.uninstall_layer())
assert not os.path.exists(main.MANIFEST) and not os.path.exists(main.INSTALLED_LIB), "the plugin did not remove what the script installed"

# 2. plugin installs, script removes
run(p.install_layer())
assert os.path.exists(main.MANIFEST) and os.path.exists(main.INSTALLED_LIB)
plugin_manifest = json.load(open(main.MANIFEST))
r = script("uninstall.sh")
assert r.returncode == 0, r.stderr
assert not os.path.exists(main.MANIFEST) and not os.path.exists(main.INSTALLED_LIB), "the script did not remove what the plugin installed"

# 3. the two write the same manifest (same keys and values) and the same modes
assert script_manifest == plugin_manifest, (script_manifest, plugin_manifest)
assert script("install.sh").returncode == 0
a = (mode(main.DATA_DIR), mode(main.INSTALLED_LIB), mode(main.MANIFEST), os.stat(main.INSTALLED_LIB).st_uid)
script("uninstall.sh"); run(p.install_layer())
b = (mode(main.DATA_DIR), mode(main.INSTALLED_LIB), mode(main.MANIFEST), os.stat(main.INSTALLED_LIB).st_uid)
assert a == b == (0o700, 0o755, 0o644, os.getuid()), (a, b)
# 4. the plugin installs over the script's install and the script over the plugin's: both keep working
assert script("install.sh").returncode == 0 and json.load(open(main.MANIFEST)) == plugin_manifest
run(p.install_layer()); assert json.load(open(main.MANIFEST)) == plugin_manifest
print("script and plugin agree: ok")
