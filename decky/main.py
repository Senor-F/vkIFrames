"""vkIFrames Decky plugin backend.

Writes the layer's live config file and installs/uninstalls the Vulkan layer manifest.
Standard library only (Decky runs a frozen Python).
Live config protocol: see LIVECONF.md.

Which user this backend runs as is not verified on a live Deck: plugin.json has `"flags": []` (no root flag) and the alpha's own folders were created owned by
the user, so it very likely runs as the user. The file code is nevertheless written for the worst case, a backend running as root: every folder the user
controls is reached folder by folder with O_NOFOLLOW from the home folder down (see _open_dir), nothing is created, written, chmodded, chowned or removed
through a symbolic link anywhere below the home folder, and what the backend creates is given to the user.
"""
import asyncio
import errno
import json
import math
import re
import os
import shutil
import stat
import subprocess
import time

import decky

PLUGIN_DIR = getattr(decky, "DECKY_PLUGIN_DIR", os.path.dirname(os.path.abspath(__file__)))
USER_HOME = decky.DECKY_USER_HOME
SETTINGS_DIR = decky.DECKY_PLUGIN_SETTINGS_DIR

CONF_DIR = os.path.join(USER_HOME, ".config", "vkiframes")
LIVE_CONF = os.path.join(CONF_DIR, "live.conf")      # plugin -> layer
STATUS_JSON = os.path.join(CONF_DIR, "status.json")  # layer -> plugin (optional)
BENCH_JSON = os.path.join(CONF_DIR, "bench.json")    # layer -> plugin: result of a benchmark run (numbers only)
BENCH_REQ = os.path.join(CONF_DIR, "bench.req")      # plugin -> layer: the benchmark request number, one line; not a setting and not in live.conf
LOG_CAP_BYTES = 8 * 1024 * 1024                        # live.log is rotated past this size (the layer does the same)
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


def _layer_running():
    """status.json is rewritten about once a second while a game runs with the layer: when it is fresh the layer rotates live.log itself (two rotations at once could drop lines)."""
    try:
        return time.time() - os.lstat(STATUS_JSON).st_mtime < 10
    except OSError:
        return False


def _owner():
    """uid/gid of the user whose home this is. Which user the backend itself runs as is not verified on a live Deck: plugin.json has `"flags": []` (no root
    flag) and the alpha's folders were created owned by the user, so it very likely runs as the user. The code is written to be safe either way: as root it
    writes only into folders the user owns, never follows a symbolic link below the home folder, and gives what it creates to the user."""
    try:
        st = os.stat(USER_HOME)
        return st.st_uid, st.st_gid
    except OSError:
        return None


def _give_to_user_fd(fd):
    own = _owner()
    if own and os.geteuid() == 0:
        try:
            os.fchown(fd, own[0], own[1])
        except OSError:
            pass


def _give_to_user(path):
    own = _owner()
    if own and os.geteuid() == 0 and path.startswith(USER_HOME.rstrip("/") + "/"):
        try:
            os.chown(path, own[0], own[1], follow_symlinks=False)
        except OSError:
            pass


def _makedirs(path, mode=0o755):
    """Like os.makedirs, but every folder created under the user's home belongs to that user. Only for the plugin's own settings folder (Decky's); the user's
    folders go through _open_dir."""
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


def _open_dir(path, create=False, mode=0o755, private=False):
    """An open file descriptor for the folder `path`, which must be the home folder or below it. Every component below the home folder is opened with
    O_NOFOLLOW|O_DIRECTORY relative to the one before it: a symbolic link anywhere in the path (also an ancestor such as ~/.local/share) is refused with an
    OSError, and nothing is created, chmodded, chowned or unlinked through it. Each folder must belong to the user (or root). The home folder itself may be
    reached through a link (/home may be a link on some systems). With create, missing folders are made (the last one with `mode`, or 0700 with `private`;
    the ones above it 0755) and given to the user when the backend runs as root; private also sets the last folder to 0700. Callers do every file operation
    relative to the returned descriptor and close it."""
    home = USER_HOME.rstrip("/") or "/"
    rel = os.path.relpath(path, home)
    if rel == ".":
        parts = []
    elif rel == ".." or rel.startswith(".." + os.sep) or os.path.isabs(rel):
        raise OSError(errno.EINVAL, "not inside the home folder: %s" % path)
    else:
        parts = rel.split(os.sep)
    own = _owner()
    allowed = ({own[0]} if own else set()) | {0, os.geteuid()}
    flags = os.O_RDONLY | os.O_DIRECTORY | os.O_NOFOLLOW | os.O_CLOEXEC
    fd = os.open(home, os.O_RDONLY | os.O_DIRECTORY | os.O_CLOEXEC)
    try:
        for i, part in enumerate(parts):
            last = i == len(parts) - 1
            try:
                nfd = os.open(part, flags, dir_fd=fd)
            except FileNotFoundError:
                if not create:
                    raise
                try:
                    os.mkdir(part, (0o700 if private else mode) if last else 0o755, dir_fd=fd)
                    if own and os.geteuid() == 0:
                        os.chown(part, own[0], own[1], dir_fd=fd, follow_symlinks=False)
                except FileExistsError:
                    pass
                nfd = os.open(part, flags, dir_fd=fd)
            except OSError as e:
                if e.errno in (errno.ELOOP, errno.ENOTDIR):
                    raise OSError(errno.ELOOP, "a symbolic link or not a folder: %s" % os.path.join(home, *parts[:i + 1]))
                raise
            os.close(fd)
            fd = nfd
            if os.fstat(fd).st_uid not in allowed:
                raise OSError(errno.EPERM, "folder belongs to another user: %s" % os.path.join(home, *parts[:i + 1]))
        if private and parts:
            try:
                st = os.fstat(fd)
                if st.st_uid in ({own[0]} if own else set()) | {os.geteuid()}:
                    os.fchmod(fd, 0o700)
            except OSError:
                pass
        return fd
    except BaseException:
        os.close(fd)
        raise


def _private_dir(path):
    """A folder only the user can enter (config, data): created 0700, owned by the user. Raises OSError if any part of the path below the home folder is a
    symbolic link, is not a folder or belongs to someone else: the callers then write nothing there."""
    os.close(_open_dir(path, create=True, private=True))


def _read_user_file(path, limit=None, tail=None):
    """Bytes of a regular file in the user's folders, or None. A symlink in the path or as the file is refused (every component is opened with O_NOFOLLOW),
    as is anything that is not a regular file or belongs to neither the user nor root. With a limit, a bigger file is refused (None); with a tail,
    only the last `tail` bytes are read."""
    try:
        dfd = _open_dir(os.path.dirname(path))
    except OSError:
        return None
    try:
        fd = os.open(os.path.basename(path), os.O_RDONLY | os.O_NOFOLLOW | os.O_CLOEXEC | os.O_NONBLOCK, dir_fd=dfd)
    except OSError:
        return None
    finally:
        os.close(dfd)
    try:
        st = os.fstat(fd)
        own = _owner()
        if not stat.S_ISREG(st.st_mode) or st.st_uid not in ({own[0]} if own else set()) | {0, os.geteuid()}:
            return None
        if limit is not None and st.st_size > limit:
            return None
        if tail is not None and st.st_size > tail:
            os.lseek(fd, st.st_size - tail, os.SEEK_SET)
        with os.fdopen(os.dup(fd), "rb") as f:
            return f.read()
    except OSError:
        return None
    finally:
        os.close(fd)


def _log(msg):
    """One line in live.log (the layer writes there too): wall-clock time, 'plugin', the message."""
    try:
        dfd = _open_dir(CONF_DIR, create=True, private=True)
    except OSError:
        return
    try:
        name = os.path.basename(LIVE_LOG)
        t = time.time()
        stamp = time.strftime("%Y-%m-%d %H:%M:%S", time.localtime(t)) + ".%03d" % int((t % 1) * 1000)
        try:                                                   # live.log is capped like the layer's copy of it: past 8 MiB it becomes live.log.1 (one old file is kept)
            st0 = os.lstat(name, dir_fd=dfd)
            if stat.S_ISREG(st0.st_mode) and st0.st_size > LOG_CAP_BYTES and not _layer_running():
                os.replace(name, name + ".1", src_dir_fd=dfd, dst_dir_fd=dfd)
        except FileNotFoundError:
            pass
        fd = os.open(name, os.O_WRONLY | os.O_APPEND | os.O_CREAT | os.O_NOFOLLOW | os.O_CLOEXEC, 0o600, dir_fd=dfd)
        with os.fdopen(fd, "a", encoding="utf-8") as f:
            st = os.fstat(fd)
            if stat.S_ISREG(st.st_mode) and (st.st_mode & 0o077):      # open() sets the mode only on creation: a file an older version left at 0644 is tightened
                os.fchmod(fd, 0o600)
            f.write("%s plugin  %s\n" % (stamp, msg))
            _give_to_user_fd(fd)
    except OSError:
        pass
    finally:
        os.close(dfd)


def _clean_stale_tmp():
    """Temporary files an earlier version (or a crash) left next to the settings: 'name.tmp.<pid>'. Regular files only, in our own folders."""
    for d in (CONF_DIR, SETTINGS_DIR):
        try:
            dfd = _open_dir(d) if d == CONF_DIR else os.open(d, os.O_RDONLY | os.O_DIRECTORY | os.O_CLOEXEC)
        except OSError:
            continue
        try:
            for n in os.listdir(dfd):
                if ".tmp." in n and n.rsplit(".", 1)[-1].isdigit():
                    try:
                        if stat.S_ISREG(os.lstat(n, dir_fd=dfd).st_mode):
                            os.unlink(n, dir_fd=dfd)
                    except OSError:
                        pass
        except OSError:
            pass
        finally:
            os.close(dfd)


def _atomic_write(path, text, user_dir=True, dir_mode=0o700):
    """Writes `text` to `path` by a temporary file in the same folder and a rename. With user_dir (the user's folders under the home folder) the folder is
    reached through _open_dir: no symbolic link in the path is followed, and the temporary file, the mode, the owner and the rename are all relative to the
    folder's descriptor. Without it, `path` is in the plugin's own settings folder (Decky's)."""
    d, name = os.path.dirname(path), os.path.basename(path)
    if user_dir:
        dfd = _open_dir(d, create=True, mode=dir_mode)
    else:
        _makedirs(d)
        dfd = os.open(d, os.O_RDONLY | os.O_DIRECTORY | os.O_CLOEXEC)
    try:
        # (Decky reloading the plugin while the old process still runs would share this name; unlink + O_EXCL below fail closed, no file is overwritten in place)
        tmp = name + ".tmp"                                   # one fixed name (this process is the only writer): a crash leaves at most one file, which the next write replaces
        try:
            os.unlink(tmp, dir_fd=dfd)                        # a leftover or planted file (or symlink) at the temporary name goes; unlink never follows
        except FileNotFoundError:
            pass
        fd = os.open(tmp, os.O_WRONLY | os.O_CREAT | os.O_EXCL | os.O_NOFOLLOW | os.O_CLOEXEC, 0o600, dir_fd=dfd)
        with os.fdopen(fd, "w", encoding="utf-8") as f:
            f.write(text)
            f.flush()
            os.fsync(f.fileno())
            os.fchmod(f.fileno(), 0o644 if path == MANIFEST else 0o600)   # the Vulkan loader reads the manifest; everything else is private
            _give_to_user_fd(f.fileno())
        os.replace(tmp, name, src_dir_fd=dfd, dst_dir_fd=dfd)  # atomic: the layer never sees a half-written file
    finally:
        os.close(dfd)


def _check_library(path):
    """A library the manifest will point at is loaded into every game: it must be a .so owned by the user (or root) that nobody else can write. Returns the real
    path that was checked (the caller writes that one into the manifest, not the name it was given, which could be repointed after the check)."""
    real = os.path.realpath(path)
    if not re.search(r"\.so(\.[0-9]+)*$", os.path.basename(real)):     # libx.so, libx.so.1, libx.so.1.2 (not x.so.txt)
        raise ValueError("not a shared library (.so): %s" % path)
    if any(c in real for c in '"\\\n\r') or any(ord(c) < 32 for c in real):
        raise ValueError("unsafe characters in the library path")
    st = os.stat(real)
    own = _owner()
    if st.st_uid not in ({own[0]} if own else set()) | {0}:
        raise ValueError("library is owned by another user: %s" % path)
    if st.st_mode & 0o022:
        raise ValueError("library is writable by group or others: %s" % path)
    return real


class Plugin:
    values = None
    rev = 0
    bench_token = 0        # the number last written to bench.req: a new number asks the layer for one benchmark run (6 pairs of 60 s, about 13 minutes)
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
                _atomic_write(DISPLAY_FILE, json.dumps({"refresh_hz": hz, "source": src}) + "\n", user_dir=False)
            except OSError:
                pass
            _log("display: refresh rate %.1f Hz (%s); Target FPS slider now reaches %d (was %d)" % (hz, src, self._target_max(), old))

    async def _main(self):
        _clean_stale_tmp()
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
        except (OSError, ValueError):
            return vals
        if not isinstance(saved, dict):
            return vals
        for old, new in RENAMED.items():
            if old in saved and new not in saved:
                saved[new] = saved[old]
        try:
            self.experimental = bool(saved.get("EXPERIMENTAL", 0))     # decides the Most generated frames range below
        except (TypeError, ValueError):
            self.experimental = False
        for item in self._schema():                                    # the file is user-editable: one bad value keeps that setting's default, nothing else
            if item["key"] in saved:
                try:
                    vals[item["key"]] = _clamp(item, saved[item["key"]])
                except (TypeError, ValueError, OverflowError):
                    pass
        return vals

    def _write_live(self):
        self.rev += 1
        lines = ["# written by the vkIFrames Decky plugin; do not edit while a game runs",
                 "REV=%d" % self.rev]
        for item in self._schema():
            lines.append("%s%s=%s" % (PREFIX, item["key"], self.values[item["key"]]))
        _atomic_write(SETTINGS_FILE, json.dumps(self.values, indent=2) + "\n", user_dir=False)    # the plugin's own folder: always
        try:
            _private_dir(CONF_DIR)
            _atomic_write(LIVE_CONF, "\n".join(lines) + "\n")
        except OSError as e:                                # the config folder is a symlink or not the user's: live.conf is not written
            decky.logger.error("live.conf not written: %s", e)
            return False
        return True

    async def get_state(self):
        status = None
        try:
            raw = _read_user_file(STATUS_JSON, 65536)
            if raw is not None:
                status = json.loads(raw.decode("utf-8", errors="replace"))
                status["age_s"] = round(time.time() - os.lstat(STATUS_JSON).st_mtime, 1)
        except (OSError, ValueError, TypeError):
            status = None
        await self._update_display(status)
        bench = None
        try:
            raw = _read_user_file(BENCH_JSON, 65536)
            if raw is not None:
                b = json.loads(raw.decode("utf-8", errors="replace"))
                d = b.get("derived") or {}
                bench = {"status": b.get("status"), "reason": b.get("reason", ""), "age_s": round(time.time() - os.lstat(BENCH_JSON).st_mtime),
                         "layer_cost_mean_ms": d.get("layer_cost_mean_ms"), "gpu_share": d.get("gpu_share")}
        except (OSError, ValueError, AttributeError):
            pass
        return {
            "schema": self._schema(),
            "refresh_hz": self.refresh_hz,
            "refresh_src": self.refresh_src,
            "values": self.values,
            "layer": self._layer_info(),
            "live_conf": LIVE_CONF,
            "status": status,
            "bench": bench,
            "bench_file": BENCH_JSON,
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

    async def start_bench(self):
        """Asks the running game's layer for one benchmark run: 30 s warm-up, then 6 pairs of 60 s (layer off, on; every second pair on, off), about 13 minutes. The layer writes bench.json."""
        self.bench_token = max(int(time.time()), self.bench_token + 1)
        try:
            _private_dir(CONF_DIR)
            _atomic_write(BENCH_REQ, "%d\n" % self.bench_token)     # its own file: live.conf is not touched, so the request cannot count as a changed setting
        except OSError as e:
            decky.logger.error("bench.req not written: %s", e)
            _log("panel: benchmark NOT requested, bench.req could not be written (%s)" % e)
            raise ValueError("the benchmark request could not be written: %s" % e)
        _log("panel: benchmark requested (bench.req %d); the layer starts it within a second if a game is running" % self.bench_token)
        return {"token": self.bench_token}

    async def reset_values(self):
        self.values = self._defaults()
        self.experimental = False
        self._write_live()
        _log("panel: all settings reset to their defaults (REV %d)" % self.rev)
        return self.values

    async def get_log(self, lines: int = 60):
        """The last change lines of live.log (what changed, when), newest last. The log itself keeps everything; this view drops the layer's fps and status chatter and shortens each line."""
        cut = 256 * 1024                                     # only the end of the file: live.log can be megabytes
        raw = _read_user_file(LIVE_LOG, tail=cut + 1)
        if raw is None:
            return []
        all_lines = raw.decode("utf-8", errors="replace").splitlines()
        if len(raw) > cut:
            all_lines = all_lines[1:]                        # the first line is cut off
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

    # ---- layer install --------------------------------------------------
    def _layer_info(self):
        lib = None
        try:
            raw = _read_user_file(MANIFEST, 65536)                 # no symbolic link in the path, nothing but the user's own regular file
            lib = json.loads(raw.decode("utf-8", errors="replace"))["layer"]["library_path"] if raw is not None else None
        except (ValueError, KeyError, TypeError, AttributeError):
            lib = None
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
            target = _check_library(lib_path)               # the real path that was checked goes into the manifest, not the name that could be repointed afterwards
        else:
            if not os.path.exists(BUNDLED_LIB):
                raise ValueError("no libvkIFrames.so bundled in the plugin's bin/ folder")
            try:
                dfd = _open_dir(DATA_DIR, create=True, private=True)     # no symbolic link anywhere below the home folder; every step below is relative to dfd
            except OSError as e:
                raise ValueError("the data folder is not a private folder of the user: %s" % e)
            try:
                final = os.path.basename(INSTALLED_LIB)
                tmp = final + ".new"
                try:
                    os.unlink(tmp, dir_fd=dfd)                       # a leftover or planted file (or symlink) goes; unlink never follows
                except FileNotFoundError:
                    pass
                fd = os.open(tmp, os.O_WRONLY | os.O_CREAT | os.O_EXCL | os.O_NOFOLLOW | os.O_CLOEXEC, 0o755, dir_fd=dfd)
                with os.fdopen(fd, "wb") as out, open(BUNDLED_LIB, "rb") as src:
                    shutil.copyfileobj(src, out)
                    out.flush()
                    os.fchmod(out.fileno(), 0o755)
                    _give_to_user_fd(out.fileno())
                os.replace(tmp, final, src_dir_fd=dfd, dst_dir_fd=dfd)
            finally:
                os.close(dfd)
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
        _atomic_write(MANIFEST, json.dumps(manifest, indent=2) + "\n", dir_mode=0o755)
        decky.logger.info("layer manifest written, library %s", target)
        return self._layer_info()

    def _remove_layer(self):
        for p in (MANIFEST, INSTALLED_LIB, INSTALLED_LIB + ".new"):
            try:
                dfd = _open_dir(os.path.dirname(p))                  # a symbolic link anywhere in the path: nothing is removed through it
            except OSError:
                continue
            try:
                os.unlink(os.path.basename(p), dir_fd=dfd)
            except OSError:
                pass
            finally:
                os.close(dfd)

    async def uninstall_layer(self):
        self._remove_layer()
        return self._layer_info()
