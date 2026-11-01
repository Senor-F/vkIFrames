"""The backend must not follow a symbolic link in ANY folder below the home folder (not only the last component): with ~/.local/share or ~/.config pointing
at a folder outside the home folder, nothing is created, written, chmodded, chowned, read or removed there. That matters if the backend runs as root (then a link
to /etc would register a user-controlled layer system-wide); as the user it still must not write where it was not meant to. Which user Decky starts the plugin as
is not verified; the code has to be safe either way. The test runs as a normal user: "outside" is a folder of the test user, so the check is by content.
usage: python3 -I test_ancestor_symlinks.py"""
import asyncio, importlib.util, json, os, sys, tempfile, types

root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
run = asyncio.run
n_load = [0]


def load(tmp, home_link=False, settings_in_home=False):
    """A fresh copy of main.py bound to a new home folder (and a plugin folder with a bundled library)."""
    n_load[0] += 1
    real_home = os.path.join(tmp, "home")
    os.makedirs(real_home, exist_ok=True)
    home = real_home
    if home_link:
        home = os.path.join(tmp, "homelink"); os.symlink(real_home, home)        # /home itself reached through a link: allowed
    plugin = os.path.join(tmp, "plugin"); os.makedirs(os.path.join(plugin, "bin"), exist_ok=True)
    with open(os.path.join(plugin, "bin", "libvkIFrames.so"), "wb") as f: f.write(b"\x7fELF fake")
    with open(os.path.join(root, "settings_schema.json")) as f, open(os.path.join(plugin, "settings_schema.json"), "w") as g: g.write(f.read())
    fake = types.ModuleType("decky")
    fake.DECKY_USER_HOME = home
    fake.DECKY_PLUGIN_SETTINGS_DIR = os.path.join(home, "homebrew", "settings", "vkIFrames") if settings_in_home else os.path.join(tmp, "settings")
    fake.DECKY_PLUGIN_DIR = plugin
    class L:
        def info(self, *a): pass
        def error(self, *a): pass
    fake.logger = L()
    sys.modules["decky"] = fake
    spec = importlib.util.spec_from_file_location("main%d" % n_load[0], os.environ.get("VKIFRAMES_TEST_MAIN") or os.path.join(root, "main.py"))   # (the variable only lets a reviewer run this file against an older main.py)
    m = importlib.util.module_from_spec(spec); spec.loader.exec_module(m)
    return m, home


def tree(path):
    out = []
    for d, dirs, files in os.walk(path):
        for x in dirs + files: out.append(os.path.relpath(os.path.join(d, x), path))
    return sorted(out)


def must_fail(fn, what=""):
    try: fn()
    except (OSError, ValueError): return
    raise SystemExit("an operation through a symbolic link succeeded: %s" % what)


def scenario(link_at, label):
    """link_at: the path below the home folder that is a symbolic link to a folder outside it."""
    tmp = tempfile.mkdtemp()
    m, home = load(tmp)
    outside = os.path.join(tmp, "outside"); os.makedirs(outside)
    parent = os.path.join(home, os.path.dirname(link_at))
    os.makedirs(parent, exist_ok=True)
    os.symlink(outside, os.path.join(home, link_at))
    lib = os.path.join(tmp, "libgood.so"); open(lib, "wb").write(b"\x7fELF"); os.chmod(lib, 0o755)
    p = m.Plugin()
    # every write path of the backend
    run(p._main())                                                                    # writes settings.json (its own folder) and tries live.conf: must not raise
    data_hit, conf_hit = link_at in (".local", ".local/share", ".local/share/vulkan"), link_at == ".config"        # which of the plugin's folders lie behind the link
    for what, fn, hit in (("install_layer(bundled)", lambda: run(p.install_layer("")), data_hit), ("install_layer(lib_path)", lambda: run(p.install_layer(lib)), data_hit),
                          ("start_bench", lambda: run(p.start_bench()), conf_hit)):
        if hit: must_fail(fn, label + ": " + what)
        else: fn()                                                                    # a folder that is not behind the link works as usual
    run(p.set_value("TARGET_FPS", 61)); run(p.reset_values()); run(p.uninstall_layer())
    m._log("not into the target")
    assert tree(outside) == [], "%s: files appeared outside the home folder: %s" % (label, tree(outside))
    # the reads: a status file behind the link is not read
    os.makedirs(os.path.join(outside, "vkiframes"), exist_ok=True)
    open(os.path.join(outside, "vkiframes", "status.json"), "w").write('{"native_fps": 1, "age_s": 0}')
    if conf_hit:
        assert m._read_user_file(os.path.join(home, ".config", "vkiframes", "status.json"), 65536) is None, "a status file behind the link was read"
        assert run(p.get_state())["status"] is None, "get_state read a status file behind the link"
        open(os.path.join(outside, "vkiframes", "live.log"), "w").write("2026-01-01 00:00:00.000 plugin  secret line\n")
        assert run(p.get_log()) == [], "get_log read a log behind the link"
    print("%s: ok" % label)


scenario(".local/share", "ancestor ~/.local/share is a link")
scenario(".local", "ancestor ~/.local is a link")
scenario(".local/share/vulkan", "ancestor ~/.local/share/vulkan (above implicit_layer.d) is a link")
scenario(".config", "ancestor ~/.config is a link")

# uninstall and the manifest read never go through a link: files with the layer's names behind it stay
tmp = tempfile.mkdtemp()
m, home = load(tmp)
p = m.Plugin(); run(p._main()); run(p.install_layer(""))
assert os.path.exists(m.MANIFEST) and os.path.exists(m.INSTALLED_LIB)
other = os.path.join(tmp, "other"); os.makedirs(os.path.join(other, "vulkan", "implicit_layer.d")); os.makedirs(os.path.join(other, "vkiframes"))
keep = [os.path.join(other, "vulkan", "implicit_layer.d", "VkLayer_frame_gen.json"), os.path.join(other, "vkiframes", "libvkIFrames.so"), os.path.join(other, "vkiframes", "libvkIFrames.so.new")]
for k in keep: open(k, "w").write(json.dumps({"layer": {"library_path": "/x/y.so"}}))
share = os.path.join(home, ".local", "share"); os.rename(share, share + ".real"); os.symlink(other, share)
run(p.uninstall_layer())
assert all(os.path.exists(k) for k in keep), "uninstall removed files behind a symbolic link"
info = p._layer_info()
assert info["library"] is None and not info["installed"], info            # the manifest behind the link is not read either
os.unlink(share); os.rename(share + ".real", share)
run(p.uninstall_layer()); assert not os.path.exists(m.MANIFEST) and not os.path.exists(m.INSTALLED_LIB)      # real folders: removed
print("uninstall/read through a link: ok")

# lib_path: the manifest gets the real path that was checked, not the name that could be repointed
tmp = tempfile.mkdtemp()
m, home = load(tmp)
p = m.Plugin()
real = os.path.join(tmp, "real.so"); open(real, "wb").write(b"\x7fELF"); os.chmod(real, 0o755)
alias = os.path.join(tmp, "alias.so"); os.symlink(real, alias)
run(p.install_layer(alias))
assert json.load(open(m.MANIFEST))["layer"]["library_path"] == os.path.realpath(real)
print("lib_path realpath: ok")

# a link to the home folder itself (and to anything above it) is allowed: the home folder is opened as given
tmp = tempfile.mkdtemp()
m, home = load(tmp, home_link=True)
p = m.Plugin(); run(p._main()); run(p.install_layer("")); assert os.path.exists(m.MANIFEST) and os.path.exists(m.LIVE_CONF)
run(p.uninstall_layer()); assert not os.path.exists(m.MANIFEST)
print("home folder reached through a link: ok")
# the plugin's own settings folder (~/homebrew/settings/vkIFrames on a Deck) is walked like the others: a link above it is refused, nothing is written or read there
tmp = tempfile.mkdtemp()
m, home = load(tmp, settings_in_home=True)
outside = os.path.join(tmp, "outside"); os.makedirs(os.path.join(outside, "vkIFrames"))
open(os.path.join(outside, "vkIFrames", "settings.json"), "w").write(json.dumps({"TARGET_FPS": 77}))      # planted behind the link
open(os.path.join(outside, "vkIFrames", "display.json"), "w").write(json.dumps({"refresh_hz": 144.0}))
os.makedirs(os.path.join(home, "homebrew")); os.symlink(outside, os.path.join(home, "homebrew", "settings"))
before = tree(outside)
p = m.Plugin(); run(p._main())                                                        # must not raise: live.conf is still written
assert tree(outside) == before, "the plugin wrote into the folder behind the settings link: %s" % tree(outside)
assert p.values["TARGET_FPS"] != 77 and p.refresh_hz != 144.0, "settings read through the link"
assert os.path.exists(m.LIVE_CONF)
run(p.set_value("TARGET_FPS", 61)); assert "VKIFRAME_TARGET_FPS=61" in open(m.LIVE_CONF).read() and tree(outside) == before
os.unlink(os.path.join(home, "homebrew", "settings")); os.makedirs(os.path.join(home, "homebrew", "settings"))      # real folder: normal operation
p = m.Plugin(); run(p._main()); run(p.set_value("TARGET_FPS", 62)); assert json.load(open(m.SETTINGS_FILE))["TARGET_FPS"] == 62
print("settings folder behind a link: ok")
print("ancestor symlink tests passed")
