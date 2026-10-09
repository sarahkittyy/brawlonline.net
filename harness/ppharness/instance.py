"""One isolated Dolphin process with its own user directory.

::

    with DolphinInstance("boot-smoke") as d:          # copy template, launch, connect
        d.client.wait_frame(600)
        d.wait_for_log(r"Booting")
    # graceful quit (then kill), dir removed on success, kept on failure

Instance directories are ``<instances_root>/<name>-<n>``. They are deleted when the
``with`` block exits normally, and kept when it exits with an exception, when
``keep=True``, when ``PPHARNESS_KEEP=1`` is set, or after ``mark_failed()``.
"""

from __future__ import annotations

import atexit
import contextlib
import json
import logging
import os
import re
import shutil
import socket
import subprocess
import threading
import time
import weakref
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any, Iterable, Mapping, Sequence

from . import _platform, paths
from .client import HarnessClient, HarnessError
from .inifile import IniFile
from .logtail import LogTail

log = logging.getLogger("ppharness.instance")

SIDEVICE_NONE = 0
SIDEVICE_STANDARD_CONTROLLER = 6
#: SDL hint (Dolphin.ini [SDL_Hints]) that lets SDL open a GameCube adapter; every instance sets it 0.
SDL_HINT_GC_ADAPTER = "SDL_JOYSTICK_HIDAPI_GAMECUBE"

#: Template subdirectories that Dolphin only reads; linked instead of copied.
DEFAULT_LINK_DIRS = ("Load",)
#: Template subdirectories whose *contents* are not copied (stale output).
DEFAULT_EMPTY_DIRS = ("Logs", "Cache", "Dump", "ScreenShots", "StateSaves", "Backup")


class InstanceError(HarnessError):
    pass


# --------------------------------------------------------------------------- config


@dataclass
class InstanceConfig:
    """Overrides written into the instance's Config/*.ini before launch."""

    #: Run the CPU on its own thread (dual core). None keeps the template value.
    cpu_thread: bool | None = None
    #: Video backend ("Null", "OGL", "Vulkan", "D3D11", ...). Also passed as ``-v``.
    video_backend: str = "Null"
    #: Ports (0-3) configured as a Standard Controller (SIDevice = 6).
    standard_controllers: Sequence[int] = (0, 1)
    #: What to do with the other ports: "none" (SIDevice = 0) or "keep" (template value).
    other_ports: str = "none"
    #: Remove the physical bindings of every GC pad so the user's keyboard/pad can't leak in.
    isolate_input: bool = True
    #: Sound. Off by default so test runs are silent ([DSP] Muted + -C Dolphin.DSP.Muted).
    audio: bool = False
    #: Emulation speed (1.0 = 100 %, 0 = unlimited). None keeps the template value.
    emulation_speed: float | None = None
    #: Direct netplay connections (no traversal server, no UPnP).
    netplay_direct: bool = True
    #: Disable analytics/auto-update prompts.
    quiet: bool = True
    #: Extra ``{section: {key: value}}`` for Dolphin.ini / GCPadNew.ini.
    dolphin_ini: Mapping[str, Mapping[str, Any]] = field(default_factory=dict)
    gcpad_ini: Mapping[str, Mapping[str, Any]] = field(default_factory=dict)
    #: Extra ``-C System.Section.Key=Value`` overrides (not saved to disk by Dolphin).
    config_args: Sequence[str] = ()
    #: Extra raw command-line arguments.
    extra_args: Sequence[str] = ()

    def dolphin_ini_values(self, linked_load: bool) -> dict[str, dict[str, Any]]:
        v: dict[str, dict[str, Any]] = {"Core": {}, "DSP": {}, "Input": {}}
        v["DSP"]["Muted"] = not self.audio
        v["Input"]["BackgroundInput"] = False
        if self.cpu_thread is not None:
            v["Core"]["CPUThread"] = bool(self.cpu_thread)
        if self.video_backend:
            v["Core"]["GFXBackend"] = self.video_backend
        if self.emulation_speed is not None:
            v["Core"]["EmulationSpeed"] = float(self.emulation_speed)
        std = {int(p) for p in self.standard_controllers}
        for p in std:
            if not 0 <= p <= 3:
                raise ValueError(f"invalid controller port {p}")
        for p in range(4):
            if p in std:
                v["Core"][f"SIDevice{p}"] = SIDEVICE_STANDARD_CONTROLLER
            elif self.other_ports == "none":
                v["Core"][f"SIDevice{p}"] = SIDEVICE_NONE
        if linked_load:
            # The linked Load dir is shared with the template: never let Dolphin write it.
            v["Core"]["WiiSDCardEnableFolderSync"] = False
        if self.netplay_direct:
            v["NetPlay"] = {"TraversalChoice": "direct", "UseUPNP": False}
        # Never let SDL's HIDAPI driver open a GameCube adapter (WUP-028): Dolphin sets the hint to
        # "1" when no port is a Wii U adapter (SDL.cpp), and an instance holding the adapter locks
        # the user's own Dolphin out of it ("access denied"). [SDL_Hints] in Dolphin.ini is applied
        # after that default; a -C override would not be (the hints are read from the base layer).
        v["SDL_Hints"] = {SDL_HINT_GC_ADAPTER: "0"}
        if self.quiet:
            v["Analytics"] = {"Enabled": False, "PermissionAsked": True}
            v["AutoUpdate"] = {"UpdateTrack": ""}
            v.setdefault("Interface", {})["ConfirmStop"] = False
        for section, kv in self.dolphin_ini.items():
            v.setdefault(section, {}).update(kv)
        return v

    def command_line_config(self) -> list[str]:
        out = []
        if not self.audio:
            out.append("Dolphin.DSP.Muted=True")
        out.extend(self.config_args)
        return out


# --------------------------------------------------------------------------- helpers


def find_free_port(host: str = "127.0.0.1", kind: int = socket.SOCK_STREAM) -> int:
    """Ask the OS for a free port. (There is an unavoidable small race until it's used.)"""
    with socket.socket(socket.AF_INET, kind) as s:
        s.bind((host, 0))
        return s.getsockname()[1]


def sanitize_name(name: str) -> str:
    s = re.sub(r"[^A-Za-z0-9._-]+", "_", name).strip("._-")
    return (s or "instance")[:80]


def _tree_size(path: Path, skip: Iterable[str] = ()) -> int:
    skip_set = {s.lower() for s in skip}
    total = 0
    for root, dirs, files in os.walk(path):
        if Path(root) == path:
            dirs[:] = [d for d in dirs if d.lower() not in skip_set]
        for f in files:
            with contextlib.suppress(OSError):
                total += os.stat(os.path.join(root, f)).st_size
    return total


def copy_template(template: Path, dest: Path, *, link_dirs: Iterable[str] = DEFAULT_LINK_DIRS,
                  empty_dirs: Iterable[str] = DEFAULT_EMPTY_DIRS) -> list[str]:
    """Copy a Dolphin user dir. Returns the names of linked (not copied) top-level dirs.

    * big read-only dirs (``Load``: texture packs) are linked (symlink, or a junction on
      Windows without symlink rights), falling back to a copy if linking fails;
    * output dirs (``Logs`` etc.) are recreated empty;
    * every file is copied with the OS fast path (CopyFileW / reflink / clonefile), which
      matters for the 2 GB ``Wii/sd.raw``.
    """
    link_set = {d.lower() for d in link_dirs}
    empty_set = {d.lower() for d in empty_dirs}
    linked: list[str] = []
    dest.mkdir(parents=True, exist_ok=True)
    for entry in os.scandir(template):
        src = Path(entry.path)
        dst = dest / entry.name
        low = entry.name.lower()
        if entry.is_dir():
            if low in empty_set:
                dst.mkdir(exist_ok=True)
                continue
            if low in link_set:
                try:
                    _platform.link_dir(src.resolve(), dst)
                    linked.append(entry.name)
                    continue
                except OSError as e:
                    log.warning("cannot link %s (%s); copying instead", src, e)
            shutil.copytree(src, dst, copy_function=_copy_fn, dirs_exist_ok=True)
        else:
            _platform.fast_copy_file(src, dst)
    return linked


def _copy_fn(src: str, dst: str) -> str:
    _platform.fast_copy_file(src, dst)
    return dst


def remove_tree(path: Path, linked: Iterable[str] = (), attempts: int = 20) -> None:
    """Delete an instance dir, unlinking linked dirs first so the template is safe.

    Retries for a while: on Windows a just-exited process may still hold handles.
    """
    for name in linked:
        p = path / name
        if os.path.islink(p) or os.path.isjunction(p):
            _platform.unlink_dir(p)
    # Belt and braces: never descend into any other link either.
    if path.is_dir():
        for entry in os.scandir(path):
            if entry.is_symlink() or entry.is_junction():
                if entry.is_dir():
                    _platform.unlink_dir(Path(entry.path))
                else:
                    os.unlink(entry.path)

    def onexc(func: Any, p: str, exc: BaseException) -> None:
        # read-only files (Windows): clear the bit and retry once
        if isinstance(exc, PermissionError):
            with contextlib.suppress(OSError):
                os.chmod(p, 0o700)
                func(p)
                return
        raise exc

    last: Exception | None = None
    for i in range(attempts):
        if not path.exists():
            return
        try:
            shutil.rmtree(path, onexc=onexc)
            return
        except OSError as e:
            last = e
            time.sleep(min(0.05 * (i + 1), 0.5))
    raise InstanceError(f"could not remove {path}: {last}")


# Live instances, killed at interpreter exit no matter what.
_live: "weakref.WeakSet[DolphinInstance]" = weakref.WeakSet()
_live_lock = threading.Lock()


@atexit.register
def _kill_all_at_exit() -> None:
    with _live_lock:
        items = list(_live)
    for inst in items:
        with contextlib.suppress(Exception):
            if inst.process is not None and inst.process.poll() is None and not inst.detached:
                _platform.kill_process_tree(inst.process)


def _env_keep() -> bool:
    return os.environ.get("PPHARNESS_KEEP", "").lower() in ("1", "true", "yes")


# --------------------------------------------------------------------------- instance


class DolphinInstance:
    """An isolated Dolphin process driven through the harness protocol."""

    def __init__(
        self,
        name: str = "instance",
        *,
        exe: str | os.PathLike[str] | Sequence[str] | None = None,
        template: str | os.PathLike[str] | None = None,
        instances_root: str | os.PathLike[str] | None = None,
        config: InstanceConfig | None = None,
        boot: str | os.PathLike[str] | None = "offline",
        platform: str = "headless",
        port: int | None = None,
        keep: bool = False,
        env: Mapping[str, str] | None = None,
        connect_timeout: float = 60.0,
        quit_timeout: float = 15.0,
        client_timeout: float = 10.0,
        link_dirs: Iterable[str] = DEFAULT_LINK_DIRS,
        audio: bool | None = None,
        detach: bool = False,
    ):
        """
        exe:      DolphinNoGUI path, or a command prefix list (e.g. fake_dolphin.command()).
        boot:     "offline" / "netplay" (the P+ launchers in the user dir), a path, or None
                  to start without a game (netplay setups).
        audio:    shortcut for ``config.audio``.
        detach:   leave the process running when this object goes away (CLI ``--detach``).
        """
        self.name = sanitize_name(name)
        if exe is None:
            exe = paths.dolphin_nogui()
        self.command_prefix: list[str] = (
            [str(x) for x in exe] if isinstance(exe, (list, tuple)) else [str(exe)])
        self.template = Path(template) if template else paths.template_user_dir()
        self.instances_root = Path(instances_root) if instances_root else paths.instances_root()
        self.config = config or InstanceConfig()
        if audio is not None:
            self.config.audio = bool(audio)
        self.boot = boot
        self.platform = platform
        self.port = port
        self.keep = keep or _env_keep()
        self.env = dict(env or {})
        self.connect_timeout = connect_timeout
        self.quit_timeout = quit_timeout
        self.client_timeout = client_timeout
        self.link_dirs = tuple(link_dirs)
        self.detach = detach

        self.user_dir: Path | None = None
        self.linked_dirs: list[str] = []
        self.process: subprocess.Popen[bytes] | None = None
        self.client: HarnessClient | None = None
        self.exit_code: int | None = None
        self.killed = False
        self.failed = False
        self.args: list[str] = []
        self._stdout_f: Any = None
        self._stderr_f: Any = None

    # ---------------------------------------------------------------- paths

    def _need_dir(self) -> Path:
        if self.user_dir is None:
            raise InstanceError("instance directory not created yet (call create())")
        return self.user_dir

    @property
    def log_path(self) -> Path:
        return self._need_dir() / "Logs" / "dolphin.log"

    @property
    def stdout_path(self) -> Path:
        return self._need_dir() / "harness-stdout.txt"

    @property
    def stderr_path(self) -> Path:
        return self._need_dir() / "harness-stderr.txt"

    @property
    def log(self) -> LogTail:
        return LogTail(self.log_path)

    def boot_path(self) -> Path | None:
        if self.boot is None:
            return None
        if self.boot == "offline":
            return self._need_dir() / paths.OFFLINE_LAUNCHER
        if self.boot == "netplay":
            return self._need_dir() / paths.NETPLAY_LAUNCHER
        return Path(self.boot)

    def netplay_launcher(self) -> Path:
        return self._need_dir() / paths.NETPLAY_LAUNCHER

    # ---------------------------------------------------------------- setup

    def create(self) -> Path:
        """Copy the template into a fresh ``<instances_root>/<name>-<n>`` and configure it."""
        if self.user_dir is not None:
            return self.user_dir
        if not self.template.is_dir():
            raise InstanceError(f"template user dir not found: {self.template}")
        self.instances_root.mkdir(parents=True, exist_ok=True)
        self._check_disk_space()
        n = 0
        while True:
            d = self.instances_root / f"{self.name}-{n}"
            try:
                d.mkdir()
                break
            except FileExistsError:
                n += 1
        self.user_dir = d
        t0 = time.monotonic()
        try:
            self.linked_dirs = copy_template(self.template, d, link_dirs=self.link_dirs)
            self._write_config()
        except BaseException:
            with contextlib.suppress(Exception):
                remove_tree(d, self.linked_dirs)
            self.user_dir = None
            raise
        (d / "harness-instance.json").write_text(json.dumps(
            {"linked_dirs": self.linked_dirs, "template": str(self.template)}, indent=2))
        log.info("created %s in %.2fs", d, time.monotonic() - t0)
        return d

    def _check_disk_space(self) -> None:
        need = _tree_size(self.template, skip=self.link_dirs) + 256 * 1024 * 1024
        free = shutil.disk_usage(self.instances_root).free
        if free < need:
            raise InstanceError(
                f"not enough disk space in {self.instances_root}: need ~{need >> 20} MiB, "
                f"have {free >> 20} MiB free (old instances can be removed with "
                f"`python -m ppharness clean`)")

    def _write_config(self) -> None:
        d = self._need_dir()
        cfg = self.config
        ini_path = d / "Config" / "Dolphin.ini"
        ini = IniFile.load(ini_path)
        ini.update(cfg.dolphin_ini_values(linked_load="Load" in self.linked_dirs))
        iso = paths.game_iso()
        if iso is None:
            # The template stores an absolute host path; it may not exist on this OS.
            cur = ini.get("Core", "DefaultISO")
            if cur and not Path(cur).is_file() and paths.fallback_iso().is_file():
                iso = paths.fallback_iso()
        if iso is not None:
            ini.set("Core", "DefaultISO", str(iso))
        ini.save(ini_path)

        pad_path = d / "Config" / "GCPadNew.ini"
        pad = IniFile.load(pad_path)
        if cfg.isolate_input:
            for p in range(4):
                # No device and no bindings: only the harness drives these pads.
                pad.replace_section(f"GCPad{p + 1}", {"Device": "Harness/0/None"})
        pad.update(cfg.gcpad_ini)
        pad.save(pad_path)

    # ---------------------------------------------------------------- launch

    def build_args(self) -> list[str]:
        d = self._need_dir()
        args = [*self.command_prefix, "-u", str(d), "-p", self.platform]
        if self.config.video_backend:
            args += ["-v", self.config.video_backend]
        for c in self.config.command_line_config():
            args += ["-C", c]
        bp = self.boot_path()
        if bp is not None:
            args += ["-e", str(bp)]
        args += ["--harness-port", str(self.port)]
        args += list(self.config.extra_args)
        return args

    def launch(self) -> subprocess.Popen[bytes]:
        if self.process is not None:
            raise InstanceError("already launched")
        d = self.create()
        if self.port is None:
            self.port = find_free_port()
        exe = Path(self.command_prefix[0])
        if not exe.exists() and shutil.which(self.command_prefix[0]) is None:
            raise InstanceError(f"Dolphin executable not found: {exe}")
        self.args = self.build_args()
        env = os.environ.copy()
        env.pop("PPR_HARNESS_PORT", None)
        env.pop("PPR_HARNESS_AUDIO", None)
        if self.config.audio:
            # The Dolphin server mutes every harness session unless this is set.
            env["PPR_HARNESS_AUDIO"] = "1"
        env.update(self.env)
        self._stdout_f = open(self.stdout_path, "wb")
        self._stderr_f = open(self.stderr_path, "wb")
        kw = _platform.popen_kwargs(dolphin=True)
        if self.detach and _platform.IS_WINDOWS:
            kw["creationflags"] |= 0x01000000  # CREATE_BREAKAWAY_FROM_JOB
        log.info("launching %s", " ".join(self.args))
        try:
            try:
                self.process = subprocess.Popen(
                    self.args, stdin=subprocess.DEVNULL, stdout=self._stdout_f,
                    stderr=self._stderr_f, cwd=str(d), env=env, **kw)
            except PermissionError:
                if not (self.detach and _platform.IS_WINDOWS):
                    raise
                kw["creationflags"] &= ~0x01000000  # job forbids breakaway
                self.process = subprocess.Popen(
                    self.args, stdin=subprocess.DEVNULL, stdout=self._stdout_f,
                    stderr=self._stderr_f, cwd=str(d), env=env, **kw)
        except OSError as e:
            self._close_files()
            raise InstanceError(f"cannot start {self.args[0]}: {e}") from e
        if not self.detach:
            _platform.bind_to_parent(self.process)
        with _live_lock:
            _live.add(self)
        (d / "harness-instance.json").write_text(json.dumps({
            "linked_dirs": self.linked_dirs, "template": str(self.template),
            "pid": self.process.pid, "port": self.port, "args": self.args}, indent=2))
        return self.process

    def is_running(self) -> bool:
        return self.process is not None and self.process.poll() is None

    def connect(self, timeout: float | None = None) -> HarnessClient:
        if self.process is None:
            raise InstanceError("not launched")
        if self.client is not None and self.client.connected:
            return self.client
        try:
            self.client = HarnessClient.connect_with_retry(
                self.port,  # type: ignore[arg-type]
                total_timeout=self.connect_timeout if timeout is None else timeout,
                is_alive=self.is_running,
                timeout=self.client_timeout,
            )
        except HarnessError as e:
            raise InstanceError(f"{self.name}: {e}\n{self.diagnostics()}") from e
        return self.client

    def start(self) -> "DolphinInstance":
        """create + launch + connect."""
        self.launch()
        self.connect()
        return self

    # ---------------------------------------------------------------- stop

    def stop(self, timeout: float | None = None) -> int | None:
        """Graceful ``quit``; then SIGTERM (POSIX); then kill. Returns the exit code."""
        timeout = self.quit_timeout if timeout is None else timeout
        proc = self.process
        if proc is None:
            return None
        if proc.poll() is None:
            client = self.client
            if client is None or not client.connected:
                # Never connected (or lost it): one quick attempt so logs get flushed.
                with contextlib.suppress(HarnessError):
                    client = HarnessClient.connect(self.port, timeout=1.0)  # type: ignore[arg-type]
            if client is not None and client.connected:
                with contextlib.suppress(HarnessError):
                    client.quit(timeout=min(5.0, timeout))
            try:
                proc.wait(timeout)
            except subprocess.TimeoutExpired:
                if _platform.request_graceful_exit(proc):
                    with contextlib.suppress(subprocess.TimeoutExpired):
                        proc.wait(5.0)
            if proc.poll() is None:
                log.warning("%s did not exit within %.1fs; killing pid %d",
                            self.name, timeout, proc.pid)
                self.killed = True
                _platform.kill_process_tree(proc)
                with contextlib.suppress(subprocess.TimeoutExpired):
                    proc.wait(10.0)
        if self.client is not None:
            self.client.close()
        self.exit_code = proc.poll()
        self._close_files()
        with _live_lock:
            _live.discard(self)
        return self.exit_code

    def kill(self) -> None:
        if self.process is not None and self.process.poll() is None:
            self.killed = True
            _platform.kill_process_tree(self.process)
            with contextlib.suppress(subprocess.TimeoutExpired):
                self.process.wait(10.0)
        self.stop(timeout=0)

    def _close_files(self) -> None:
        for f in (self._stdout_f, self._stderr_f):
            if f is not None:
                with contextlib.suppress(OSError):
                    f.close()
        self._stdout_f = self._stderr_f = None

    def mark_failed(self) -> None:
        """Keep the instance dir when cleaning up (call from test failure hooks)."""
        self.failed = True

    def cleanup(self, success: bool | None = None) -> bool:
        """Stop the process and delete the dir unless it should be kept. Returns True if removed."""
        if self.detach and self.is_running():
            return False
        self.stop()
        ok = (not self.failed) if success is None else (success and not self.failed)
        if self.user_dir is None:
            return False
        if self.keep or not ok:
            log.warning("keeping instance dir %s", self.user_dir)
            return False
        remove_tree(self.user_dir, self.linked_dirs)
        return True

    def __enter__(self) -> "DolphinInstance":
        try:
            self.start()
        except BaseException:
            self.failed = True
            self.cleanup(False)
            raise
        return self

    def __exit__(self, exc_type: Any, exc: Any, tb: Any) -> None:
        if exc_type is not None:
            self.failed = True
            if self.user_dir is not None:
                log.error("%s failed; instance dir kept at %s", self.name, self.user_dir)
        self.cleanup(exc_type is None)

    def __repr__(self) -> str:
        return f"<DolphinInstance {self.name} dir={self.user_dir} port={self.port} " \
               f"pid={self.process.pid if self.process else None}>"

    # ---------------------------------------------------------------- logs / output

    def stdout_text(self) -> str:
        return _read_text(self.stdout_path)

    def stderr_text(self) -> str:
        return _read_text(self.stderr_path)

    def wait_for_log(self, pattern: str, timeout: float = 30.0, since: int = 0) -> re.Match[str]:
        """Wait for a regex in dolphin.log; fails fast if the process exits."""
        def abort() -> str | None:
            if self.process is not None and self.process.poll() is not None:
                # One last read happens on the next loop iteration only if we don't abort,
                # so give the file a final chance first.
                return f"process exited with code {self.process.returncode}"
            return None
        try:
            return self.log.wait_for(pattern, timeout=timeout, since=since, abort=abort)
        except TimeoutError as e:
            # A line written right before exit: check once more without abort.
            m = None
            with contextlib.suppress(TimeoutError):
                m = self.log.wait_for(pattern, timeout=0, since=since)
            if m is not None:
                return m
            raise InstanceError(f"{e}\n{self.diagnostics(log_lines=0)}") from e

    def mark(self, text: str, timeout: float = 5.0) -> int:
        """``log_mark`` + wait until it is in dolphin.log. Returns the log offset after it."""
        if self.client is None:
            raise InstanceError("not connected")
        start = self.log.mark()
        self.client.log_mark(text)
        self.wait_for_log(re.escape(f"[HARNESS] {text}"), timeout=timeout, since=start)
        return self.log.mark()

    def diagnostics(self, log_lines: int = 30) -> str:
        parts = []
        if self.process is not None:
            parts.append(f"pid {self.process.pid}, exit code {self.process.poll()}")
        if self.user_dir is not None:
            parts.append(f"instance dir: {self.user_dir}")
            out, err = self.stdout_text().strip(), self.stderr_text().strip()
            if out:
                parts.append("--- stdout (tail) ---\n" + "\n".join(out.splitlines()[-15:]))
            if err:
                parts.append("--- stderr (tail) ---\n" + "\n".join(err.splitlines()[-15:]))
            if log_lines:
                tail = self.log.tail(log_lines)
                if tail:
                    parts.append("--- dolphin.log (tail) ---\n" + tail)
        return "\n".join(parts)


def _read_text(p: Path) -> str:
    try:
        return p.read_bytes().decode("utf-8", errors="replace")
    except OSError:
        return ""


def probe_harness_support(exe: str | os.PathLike[str] | None = None,
                          template: str | os.PathLike[str] | None = None,
                          timeout: float = 15.0) -> tuple[bool, str]:
    """Does this Dolphin build answer ``ping`` on ``--harness-port``?

    Launches it cheaply (scratch user dir with only the template's Config, booting the
    offline launcher straight from the template, Null video, muted), pings, quits.
    Returns (supported, detail). Never raises for an unsupported build.
    """
    import tempfile

    exe_p = Path(exe) if exe else paths.dolphin_nogui()
    tpl = Path(template) if template else paths.template_user_dir()
    if not exe_p.exists():
        return False, f"{exe_p} not found"
    with tempfile.TemporaryDirectory(prefix="ppharness-probe-",
                                     ignore_cleanup_errors=True) as tmp:
        user = Path(tmp)
        if (tpl / "Config").is_dir():
            shutil.copytree(tpl / "Config", user / "Config")
        (user / "Config").mkdir(exist_ok=True)
        ini = IniFile.load(user / "Config" / "Dolphin.ini")
        ini.update({"DSP": {"Muted": True}, "Analytics": {"Enabled": False, "PermissionAsked": True},
                    "Core": {f"SIDevice{p}": 0 for p in range(4)},
                    "SDL_Hints": {SDL_HINT_GC_ADAPTER: "0"}})
        ini.save(user / "Config" / "Dolphin.ini")
        port = find_free_port()
        cmd = [str(exe_p), "-u", str(user), "-p", "headless", "-v", "Null",
               "-C", "Dolphin.DSP.Muted=True"]
        launcher = tpl / paths.OFFLINE_LAUNCHER
        if launcher.exists():
            cmd += ["-e", str(launcher)]
        cmd += ["--harness-port", str(port)]
        out_path = user / "probe-output.txt"
        with open(out_path, "wb") as out:
            proc = subprocess.Popen(cmd, stdout=out, stderr=subprocess.STDOUT,
                                    stdin=subprocess.DEVNULL, cwd=str(user),
                                    **_platform.popen_kwargs(dolphin=True))
            _platform.bind_to_parent(proc)
            try:
                c = HarnessClient.connect_with_retry(port, total_timeout=timeout,
                                                     is_alive=lambda: proc.poll() is None)
                version = c.server_version
                with contextlib.suppress(HarnessError):
                    c.quit()
                return True, f"protocol v{version}"
            except HarnessError as e:
                return False, f"{e}"
            finally:
                if proc.poll() is None:
                    try:
                        proc.wait(10)
                    except subprocess.TimeoutExpired:
                        _platform.kill_process_tree(proc)
                        with contextlib.suppress(subprocess.TimeoutExpired):
                            proc.wait(10)
                # Give Windows a moment to release file handles before the dir is removed.
                time.sleep(0.2)


def clean_instances(root: Path | None = None, *, dry_run: bool = False,
                    min_age_s: float = 600.0, prefix: str | None = None,
                    include_kept: bool = False) -> list[tuple[Path, str]]:
    """Remove instance dirs whose process is gone. Returns [(dir, action)].

    Skips dirs whose Dolphin is still running, dirs without a recorded pid that are younger
    than ``min_age_s`` (not launched yet), and dirs another process still has files open in
    (the rename-then-delete fails on Windows), so it is safe while other sessions run.

    Only dirs whose name starts with ``prefix`` are considered (``None`` means all). Dirs kept
    after a failure (``kept-*``) are evidence another session chose to keep, so they are only
    removed with ``include_kept``.
    """
    root = root or paths.instances_root()
    results: list[tuple[Path, str]] = []
    if not root.is_dir():
        return results
    for d in sorted(root.iterdir()):
        if not d.is_dir():
            continue
        if d.name.startswith("kept-") and not include_kept:
            results.append((d, "skipped (kept; pass --include-kept)"))
            continue
        name = d.name.split("-", 2)[-1] if d.name.startswith("kept-") else d.name
        if prefix is not None and not (d.name.startswith(prefix) or name.startswith(prefix)):
            continue
        meta: dict[str, Any] = {}
        with contextlib.suppress(OSError, ValueError):
            meta = json.loads((d / "harness-instance.json").read_text())
        pid = meta.get("pid")
        if pid and _platform.pid_alive(int(pid)):
            results.append((d, f"skipped (pid {pid} still running)"))
            continue
        if not pid and time.time() - d.stat().st_mtime < min_age_s:
            # Created but not launched yet: another driver may be about to start it.
            results.append((d, "skipped (no pid yet, recently created)"))
            continue
        if dry_run:
            results.append((d, "would remove"))
            continue
        # Rename first: on Windows this fails while any process (e.g. the Python driver that
        # still holds harness-stdout.txt) has a handle inside, so a dir that another session is
        # still using is never half-deleted.
        tomb = d.with_name(d.name + ".deleting")
        try:
            os.rename(d, tomb)
        except OSError as e:
            results.append((d, f"skipped (in use: {e.strerror or e})"))
            continue
        try:
            remove_tree(tomb, meta.get("linked_dirs", ["Load"]), attempts=3)
            results.append((d, "removed"))
        except InstanceError as e:
            results.append((d, f"failed: {e}"))
    return results
