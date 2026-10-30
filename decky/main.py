"""vkIFrames Decky plugin backend.

Writes the layer's live config file and installs/uninstalls the Vulkan layer manifest.
Standard library only (Decky runs a frozen Python).
Live config protocol: see LIVECONF.md.
"""
import asyncio
import json
import math
import re
import os
import shutil
import subprocess
import time

import decky

PLUGIN_DIR = getattr(decky, "DECKY_PLUGIN_DIR", os.path.dirname(os.path.abspath(__file__)))
USER_HOME = decky.DECKY_USER_HOME
SETTINGS_DIR = decky.DECKY_PLUGIN_SETTINGS_DIR

CONF_DIR = os.path.join(USER_HOME, ".config", "vkiframes")
LIVE_CONF = os.path.join(CONF_DIR, "live.conf")      # plugin -> layer
STATUS_JSON = os.path.join(CONF_DIR, "status.json")  # layer -> plugin (optional)
LIVE_LOG = os.path.join(CONF_DIR, "live.log")        # verbose log of every change; the layer appends to the same file
DATA_DIR = os.path.join(USER_HOME, ".local", "share", "vkiframes")
LAYER_DIR = os.path.join(USER_HOME, ".local", "share", "vulkan", "implicit_layer.d")
MANIFEST = os.path.join(LAYER_DIR, "VkLayer_frame_gen.json")
INSTALLED_LIB = os.path.join(DATA_DIR, "libvkIFrames.so")
BUNDLED_LIB = os.path.join(PLUGIN_DIR, "bin", "libvkIFrames.so")
SETTINGS_FILE = os.path.join(SETTINGS_DIR, "settings.json")
SCHEMA_FILE = os.path.join(PLUGIN_DIR, "settings_schema.json")
DMI_PRODUCT = "/sys/class/dmi/id/product_name"   # "Galileo" = Steam Deck OLED (90 Hz), "Jupiter" = LCD (60 Hz)
DISPLAY_FILE = os.path.join(SETTINGS_DIR, "display.json")   # last refresh rate seen (from the layer or xrandr)
TARGET_FLOOR = 120   # the Target FPS slider reaches at least this, or the screen's refresh rate when that is higher
PREFIX = "VKIFRAME_"   # the layer also accepts the old FRAMEGEN_ spelling


def _load_schema():
    with open(SCHEMA_FILE, "r", encoding="utf-8") as f:
        return json.load(f)


def _parse_xrandr(text):
    """Highest refresh rate marked active (*) in `xrandr --current` output, or 0.0."""
    best = 0.0
    for line in text.splitlines():
        if "*" not in line or not line.startswith(" "):
            continue
        for tok in line.split()[1:]:
            if "*" in tok:
                try:
                    best = max(best, float(tok.replace("*", "").replace("+", "")))
                except ValueError:
                    pass
    return best


def _xrandr_hz():
    for disp in (os.environ.get("DISPLAY"), ":0", ":1"):
        if not disp:
            continue
        try:
            out = subprocess.run(["xrandr", "--current"], capture_output=True, text=True, timeout=2,
                                 env=dict(os.environ, DISPLAY=disp)).stdout
        except (OSError, subprocess.SubprocessError):
            continue
        hz = _parse_xrandr(out)
        if hz > 0:
            return hz
    return 0.0


def _clamp(item, value):
    if item["type"] == "toggle":
        return 1 if value in (1, True, "1", "true", "on") else 0
    lo, hi = item["min"], item["max"]
    step = item.get("fine", item["step"])            # values snap to the finest step the panel offers
    v = max(lo, min(hi, float(value)))
    v = lo + round((v - lo) / step) * step
    v = max(lo, min(hi, v))
    return int(round(v)) if float(step).is_integer() and float(lo).is_integer() else round(v, 4)


def _log(msg):
    """One line in live.log (the layer writes there too): wall-clock time, 'plugin', the message."""
    try:
        _private_dir(CONF_DIR)
        t = time.time()
        stamp = time.strftime("%Y-%m-%d %H:%M:%S", time.localtime(t)) + ".%03d" % int((t % 1) * 1000)
        fd = os.open(LIVE_LOG, os.O_WRONLY | os.O_APPEND | os.O_CREAT | os.O_NOFOLLOW, 0o600)
        with os.fdopen(fd, "a", encoding="utf-8") as f:
            f.write("%s plugin  %s\n" % (stamp, msg))
        _give_to_user(LIVE_LOG)
    except OSError:
        pass


def _owner():
    """uid/gid of the user whose home this is. Decky runs the backend as root; the game runs as that user and the layer ignores files owned by anyone else."""
    try:
        st = os.stat(USER_HOME)
        return st.st_uid, st.st_gid
    except OSError:
        return None


def _give_to_user(path):
    own = _owner()
    if own and os.geteuid() == 0 and path.startswith(USER_HOME.rstrip("/") + "/"):
        try:
            os.chown(path, own[0], own[1], follow_symlinks=False)
        except OSError:
            pass


def _makedirs(path, mode=0o755):
    """Like os.makedirs, but every folder created under the user's home belongs to that user."""
    todo = []
    p = path
    while p and not os.path.isdir(p):
        todo.append(p)
        p = os.path.dirname(p)
    for d in reversed(todo):
        try:
            os.mkdir(d, mode)
        except FileExistsError:
            pass
        _give_to_user(d)


def _private_dir(path):
    """A folder only the user can enter (config, data): created 0700, owned by the user."""
    _makedirs(path, 0o700)
    try:
        os.chmod(path, 0o700)
    except OSError:
        pass
    _give_to_user(path)


def _atomic_write(path, text):
    _makedirs(os.path.dirname(path))
    tmp = "%s.tmp.%d" % (path, os.getpid())
    fd = os.open(tmp, os.O_WRONLY | os.O_CREAT | os.O_TRUNC | os.O_NOFOLLOW, 0o600)
    with os.fdopen(fd, "w", encoding="utf-8") as f:
        f.write(text)
        f.flush()
        os.fsync(f.fileno())
    os.chmod(tmp, 0o644 if path == MANIFEST else 0o600)   # the Vulkan loader reads the manifest; everything else is private
    _give_to_user(tmp)
    os.replace(tmp, path)  # atomic: the layer never sees a half-written file


def _check_library(path):
    """A library the manifest will point at is loaded into every game: it must be a .so owned by the user (or root) that nobody else can write."""
    real = os.path.realpath(path)
    if not re.search(r"\.so(\.[0-9]+)*$", os.path.basename(real)):     # libx.so, libx.so.1, libx.so.1.2 (not x.so.txt)
        raise ValueError("not a shared library (.so): %s" % path)
    if any(c in path for c in '"\\\n\r') or any(ord(c) < 32 for c in path):
        raise ValueError("unsafe characters in the library path")
    st = os.stat(real)
    own = _owner()
    if st.st_uid not in ({own[0]} if own else set()) | {0}:
        raise ValueError("library is owned by another user: %s" % path)
    if st.st_mode & 0o022:
        raise ValueError("library is writable by group or others: %s" % path)


class Plugin:
    values = None
    rev = 0
    experimental = False   # EXPERIMENTAL on: the "more" settings can be changed and Most generated frames reaches 10
    refresh_hz = 0.0       # refresh rate of the screen the game renders on; 0 = not known yet
    refresh_src = ""       # "layer" (what the running game's swapchain reports), "xrandr", or "saved"

    # ---- screen refresh rate -> upper end of the Target FPS slider ---------
    def _load_display(self):
        try:
            with open(DISPLAY_FILE, "r", encoding="utf-8") as f:
                d = json.load(f)
            self.refresh_hz, self.refresh_src = float(d["refresh_hz"]), "saved"
        except (OSError, ValueError, KeyError):
            pass

    def _target_max(self):
        return max(TARGET_FLOOR, int(math.ceil(self.refresh_hz)))

    def _schema(self, status=None):
        """The schema with the Target FPS maximum set to the higher of 120 and the screen's refresh rate."""
        schema = _load_schema()
        for item in schema:
            if item["key"] == "TARGET_FPS":
                item["max"] = self._target_max()
            if item["key"] == "MAX_SYNTHETIC" and self.experimental:
                item["max"] = 10
        return schema

    async def _update_display(self, status):
        hz, src = 0.0, ""
        if status and status.get("refresh_hz", 0) > 0 and status.get("age_s", 99) < 600:
            hz, src = float(status["refresh_hz"]), "layer"    # the game's own swapchain: the screen it renders on
        elif not self.refresh_hz or self.refresh_src != "layer":
            hz = await asyncio.to_thread(_xrandr_hz)
            src = "xrandr"
        if hz > 0 and (round(hz, 1) != round(self.refresh_hz, 1) or src != self.refresh_src):
            old = self._target_max()
            self.refresh_hz, self.refresh_src = hz, src
            try:
                _atomic_write(DISPLAY_FILE, json.dumps({"refresh_hz": hz, "source": src}) + "\n")
            except OSError:
                pass
            _log("display: refresh rate %.1f Hz (%s); Target FPS slider now reaches %d (was %d)" % (hz, src, self._target_max(), old))

    async def _main(self):
        self._load_display()
        self.values = self._read_settings()
        self.rev = int(time.time())
        self._write_live()
        decky.logger.info("vkIFrames plugin ready, live config at %s", LIVE_CONF)

    async def _unload(self):
        decky.logger.info("vkIFrames plugin unloaded")

    async def _uninstall(self):
        self._remove_layer()

    # ---- settings -------------------------------------------------------
    def _defaults(self):
        d = {i["key"]: i["default"] for i in _load_schema()}
        try:
            with open(DMI_PRODUCT, "r", encoding="utf-8") as f:
                oled = f.read().strip().startswith("Galileo")
        except OSError:
            oled = False
        d["TARGET_FPS"] = 90 if oled else 60            # same rule as the layer: 90 on the Deck OLED, 60 elsewhere
        return d

    def _read_settings(self):
        vals = self._defaults()
        RENAMED = {"WARP": "INTERPOLATE", "WARP_BIDIR": "INTERP_BIDIR"}   # saved by an older version of the plugin
        try:
            with open(SETTINGS_FILE, "r", encoding="utf-8") as f:
                saved = json.load(f)
            for old, new in RENAMED.items():
                if old in saved and new not in saved:
                    saved[new] = saved[old]
            self.experimental = bool(saved.get("EXPERIMENTAL", 0))     # decides the Most generated frames range below
            for item in self._schema():
                if item["key"] in saved:
                    vals[item["key"]] = _clamp(item, saved[item["key"]])
        except (OSError, ValueError):
            pass
        return vals

    def _write_live(self):
        self.rev += 1
        lines = ["# written by the vkIFrames Decky plugin; do not edit while a game runs",
                 "REV=%d" % self.rev]
        for item in self._schema():
            lines.append("%s%s=%s" % (PREFIX, item["key"], self.values[item["key"]]))
        _private_dir(CONF_DIR)
        _atomic_write(LIVE_CONF, "\n".join(lines) + "\n")
        _atomic_write(SETTINGS_FILE, json.dumps(self.values, indent=2) + "\n")

    async def get_state(self):
        status = None
        try:
            with open(STATUS_JSON, "r", encoding="utf-8") as f:
                status = json.load(f)
            status["age_s"] = round(time.time() - os.path.getmtime(STATUS_JSON), 1)
        except (OSError, ValueError):
            pass
        await self._update_display(status)
        return {
            "schema": self._schema(),
            "refresh_hz": self.refresh_hz,
            "refresh_src": self.refresh_src,
            "values": self.values,
            "layer": self._layer_info(),
            "live_conf": LIVE_CONF,
            "status": status,
        }

    async def set_value(self, key: str, value: float):
        for item in self._schema():
            if item["key"] == key:
                if item.get("group") == "more" and not self.experimental:
                    _log("panel: %s not changed, Experimental is off" % key)
                    raise ValueError("%s can only be changed with Experimental on" % key)
                old = self.values.get(key)
                self.values[key] = _clamp(item, value)
                if key == "EXPERIMENTAL":
                    self.experimental = bool(self.values[key])
                    if not self.experimental:      # back to the normal range
                        self.values["MAX_SYNTHETIC"] = min(self.values["MAX_SYNTHETIC"], 4)
                self._write_live()
                _log("panel: %s %s -> %s%s (REV %d written to live.conf%s)" % (
                    key, old, self.values[key], "" if self.values[key] == value else " (asked for %s, clamped)" % value,
                    self.rev, "; read by the layer within a second" if item.get("needsLayer") == "live" else "; read when the next game starts"))
                return self.values
        raise ValueError("unknown setting %r" % key)

    async def reset_values(self):
        self.values = self._defaults()
        self.experimental = False
        self._write_live()
        _log("panel: all settings reset to their defaults (REV %d)" % self.rev)
        return self.values

    async def get_log(self, lines: int = 60):
        """The last change lines of live.log (what changed, when), newest last. The log itself keeps everything; this view drops the layer's fps and status chatter and shortens each line."""
        try:
            with open(LIVE_LOG, "rb") as fb:
                size = fb.seek(0, os.SEEK_END)
                fb.seek(max(0, size - 256 * 1024))           # only the end of the file: live.log can be megabytes
                all_lines = fb.read().decode("utf-8", errors="replace").splitlines()
            if size > 256 * 1024:
                all_lines = all_lines[1:]                    # the first line is cut off
            if True:
                keep = []
                for ln in all_lines:
                    if " plugin " in ln or "live: CHANGE " in ln:
                        # "2026-10-30 19:02:15.030 plugin  panel: X 1 -> 2 (REV ...)" -> "19:02:15 X 1 -> 2 [plugin]"
                        t = ln[11:19] if len(ln) > 19 else ""
                        body = ln[24:] if len(ln) > 24 else ln
                        who = "plugin" if " plugin " in ln else "layer"
                        body = body.replace("plugin  panel: ", "").replace("[FrameGen] live: CHANGE ", "").replace("(live.conf)", "")
                        body = body.split(" (REV")[0].strip()
                        keep.append("%s %s [%s]" % (t, body, who))
                return keep[-max(1, min(int(lines), 500)):]
        except OSError:
            return []

    # ---- layer install --------------------------------------------------
    def _layer_info(self):
        lib = None
        try:
            with open(MANIFEST, "r", encoding="utf-8") as f:
                lib = json.load(f)["layer"]["library_path"]
        except (OSError, ValueError, KeyError):
            pass
        return {
            "installed": lib is not None and os.path.exists(lib),
            "manifest": MANIFEST,
            "library": lib,
            "bundled_available": os.path.exists(BUNDLED_LIB),
        }

    async def install_layer(self, lib_path: str = ""):
        """Install from the plugin's bin/ folder, or point the manifest at lib_path
        (for example the repo's build/ directory, so rebuilds go live without copying)."""
        if lib_path:
            if not os.path.isabs(lib_path) or not os.path.isfile(lib_path):
                raise ValueError("library not found: %s" % lib_path)
            _check_library(lib_path)
            target = lib_path
        else:
            if not os.path.exists(BUNDLED_LIB):
                raise ValueError("no libvkIFrames.so bundled in the plugin's bin/ folder")
            _private_dir(DATA_DIR)
            tmp = INSTALLED_LIB + ".new"
            shutil.copyfile(BUNDLED_LIB, tmp)
            os.chmod(tmp, 0o755)
            _give_to_user(tmp)
            os.replace(tmp, INSTALLED_LIB)
            target = INSTALLED_LIB
        manifest = {
            "file_format_version": "1.1.2",
            "layer": {
                "name": "VK_LAYER_CUSTOM_frame_gen",
                "type": "GLOBAL",
                "library_path": target,
                "api_version": "1.3.0",
                "implementation_version": "1",
                "description": "vkIFrames dynamic frame interpolation layer",
                "functions": {
                    "vkNegotiateLoaderLayerInterfaceVersion": "FrameGen_NegotiateLoaderLayerInterfaceVersion",
                    "vkGetInstanceProcAddr": "FrameGen_GetInstanceProcAddr",
                    "vkGetDeviceProcAddr": "FrameGen_GetDeviceProcAddr",
                },
                "enable_environment": {"VKIFRAME_ENABLE": "1"},
                "disable_environment": {"VKIFRAME_DISABLE": "1"},
            },
        }
        _atomic_write(MANIFEST, json.dumps(manifest, indent=2) + "\n")
        decky.logger.info("layer manifest written, library %s", target)
        return self._layer_info()

    def _remove_layer(self):
        for p in (MANIFEST, INSTALLED_LIB):
            try:
                os.remove(p)
            except OSError:
                pass

    async def uninstall_layer(self):
        self._remove_layer()
        return self._layer_info()
