"""EmissaryRecomp instances under the harness (docs/design/brawl-online-core.md §10).

A RecompInstance is a DolphinInstance whose game is EmissaryRecomp running Brawl Online's core
in-process: the same user folder (Config/Dolphin.ini with the online account, Online/, Logs/
dolphin.log, Wii/sd.raw with the plugin the test put there), the same harness protocol (the recomp
answers the commands its online tests use, brawl_online_harness.cpp), so a test drives it as it
drives Dolphin, and a test can pair one with a Dolphin instance (crossplay).

The recomp build (PPHARNESS_RECOMP: its native build folder, with WiiCompiled.exe and UserData/)
is used through hard links in ``<user dir>/recomp`` (a portable layout of its own per instance);
the card is linked read-only (BRAWL_SD_VOLATILE_WRITES).

The Linux build plays from Windows through WSL (PPHARNESS_RECOMP_WSL=<distro>:<its native build
folder in the distro>): the instance's program folder is made in the distro (hard links on its own
disk: a program on /mnt pages in slowly), the core reads the instance's user folder through /mnt,
and the harness, the servers and the other game reach it on 127.0.0.1, which needs WSL's mirrored
networking (.wslconfig ``[wsl2] networkingMode=mirrored``). PPHARNESS_RECOMP_WSL_NAMES picks which
recomp instances play there (all by default), so one test can pair a Windows recomp
(PPHARNESS_RECOMP) with a Linux one. WSL renders with llvmpipe: give the tests time, judge them by
their checksums.
"""

from __future__ import annotations

import os
import re
import shlex
import shutil
import subprocess
from pathlib import Path, PureWindowsPath
from typing import Any

from .instance import DolphinInstance, InstanceError

LINKED_FILES = ("WiiCompiled.exe", "WiiCompiled", "cacert.pem", "dsp_coef.bin", "initial_pipeline_cache.db",
                "portable.txt")
COPIED_DIRS = ("Sys", "wii_bootstrap")


def recomp_build() -> Path | None:
    p = os.environ.get("PPHARNESS_RECOMP")
    return Path(p) if p else None


def wsl_build() -> tuple[str, str] | None:
    """PPHARNESS_RECOMP_WSL=<distro>:<Linux build folder> (WiiCompiled, and UserData/ with Config.toml,
    NAND and Codes, as PPHARNESS_RECOMP's), or None."""
    v = os.environ.get("PPHARNESS_RECOMP_WSL", "")
    if not v:
        return None
    distro, sep, folder = v.partition(":")
    if not sep or not distro or not folder.startswith("/"):
        raise InstanceError(f"PPHARNESS_RECOMP_WSL={v!r}: expected <distro>:</linux/build/folder>")
    return distro, folder.rstrip("/")


def wants_recomp(name: str) -> bool:
    """PPHARNESS_RECOMP (or PPHARNESS_RECOMP_WSL) set and the instance's name matches
    PPHARNESS_RECOMP_NAMES (a regex, all by default)."""
    if not recomp_build() and not wsl_build():
        return False
    pattern = os.environ.get("PPHARNESS_RECOMP_NAMES", "")
    return not pattern or re.search(pattern, name) is not None


def runs_in_wsl(name: str) -> bool:
    """A recomp instance that plays the Linux build in WSL: PPHARNESS_RECOMP_WSL set and the name
    matches PPHARNESS_RECOMP_WSL_NAMES (all recomp instances by default)."""
    if not wsl_build():
        return False
    pattern = os.environ.get("PPHARNESS_RECOMP_WSL_NAMES", "")
    return not pattern or re.search(pattern, name) is not None


def wsl_path(path: Path) -> str:
    """A Windows path as WSL sees it (/mnt/<drive>/...)."""
    p = PureWindowsPath(os.path.abspath(path))
    drive = p.drive.rstrip(":").lower()
    if len(drive) != 1:
        raise InstanceError(f"{path}: WSL reaches only drive-letter paths")
    return f"/mnt/{drive}/" + "/".join(p.parts[1:])


def _link(src: Path, dst: Path) -> None:
    if dst.exists():
        dst.unlink()
    try:
        os.link(src, dst)
    except OSError:
        shutil.copyfile(src, dst)


def set_toml(text: str, settings: dict[str, dict[str, str]]) -> str:
    """Each `key = value` set in its [section] (the section added when missing); other lines kept."""
    lines = text.splitlines()
    for section, kv in settings.items():
        out, current = [], ""
        for ln in lines:
            if ln.strip().startswith("["):
                current = ln.strip()
            name = ln.split("=", 1)[0].strip() if "=" in ln else None
            if current == f"[{section}]" and name in kv:
                continue
            out.append(ln)
        if f"[{section}]" not in (ln.strip() for ln in out):
            out.append(f"[{section}]")
        i = [ln.strip() for ln in out].index(f"[{section}]")
        out[i + 1:i + 1] = [f"{k} = {v}" for k, v in kv.items()]
        lines = out
    return "\n".join(lines) + "\n"


def test_config(text: str) -> str:
    """Test runs are silent, as Dolphin's (InstanceConfig.audio): [audio] muted, volume 0. Plus
    PPHARNESS_RECOMP_TOML ("section.key=value;..."), such as video.resolution_multiplier=0.5 for
    WSL's software renderer."""
    settings: dict[str, dict[str, str]] = {"audio": {"muted": "true", "volume": "0.0"}}
    for item in os.environ.get("PPHARNESS_RECOMP_TOML", "").split(";"):
        key, sep, value = item.partition("=")
        section, dot, name = key.strip().partition(".")
        if sep and dot and section and name:
            settings.setdefault(section, {})[name] = value.strip()
    return set_toml(text, settings)


def _mute(config: Path) -> None:
    config.write_text(test_config(config.read_text(encoding="utf-8")), encoding="utf-8")


class RecompInstance(DolphinInstance):
    """DolphinInstance's user folder and harness client, EmissaryRecomp's game."""

    def __init__(self, name: str = "instance", **kw: Any):
        kw.pop("exe", None)
        kw.setdefault("boot", None)
        super().__init__(name, exe=os.devnull, **kw)
        self.wsl = wsl_build() if runs_in_wsl(name) else None
        self.build = recomp_build()
        if self.build is None and self.wsl is None:
            raise InstanceError("PPHARNESS_RECOMP is not set")

    @property
    def recomp_dir(self) -> Path:
        return self._need_dir() / "recomp"

    def _prepare_recomp(self) -> Path:
        d = self.recomp_dir
        d.mkdir(exist_ok=True)
        for name in LINKED_FILES:
            if (self.build / name).exists():
                _link(self.build / name, d / name)
        for dll in self.build.glob("*.dll"):
            _link(dll, d / dll.name)
        for sub in COPIED_DIRS:
            if (self.build / sub).is_dir() and not (d / sub).exists():
                shutil.copytree(self.build / sub, d / sub)
        ud = d / "UserData"
        ud.mkdir(exist_ok=True)
        shutil.copyfile(self.build / "UserData" / "Config.toml", ud / "Config.toml")
        _mute(ud / "Config.toml")
        for sub in ("NAND", "Codes"):
            if (self.build / "UserData" / sub).is_dir() and not (ud / sub).exists():
                shutil.copytree(self.build / "UserData" / sub, ud / sub)
        card = self._need_dir() / "Wii" / "sd.raw"
        if card.exists():
            _link(card, ud / "sd.raw")
        return d

    def close_window(self) -> int:
        """Closes the recomp's window as its X does (WM_CLOSE to the process's visible top-level
        windows: SDL's quit, the runtime's window-close exit). Returns how many windows got it."""
        if os.name != "nt" or self.process is None:
            raise InstanceError("close_window needs a running recomp on Windows")
        import ctypes
        from ctypes import wintypes
        user32 = ctypes.windll.user32
        pid = self.process.pid
        hwnds: list[int] = []
        proc = ctypes.WINFUNCTYPE(wintypes.BOOL, wintypes.HWND, wintypes.LPARAM)

        def each(hwnd: int, _lp: int) -> bool:
            owner = wintypes.DWORD()
            user32.GetWindowThreadProcessId(hwnd, ctypes.byref(owner))
            if owner.value == pid and user32.IsWindowVisible(hwnd):
                hwnds.append(hwnd)
            return True

        user32.EnumWindows(proc(each), 0)
        for h in hwnds:
            user32.PostMessageW(h, 0x0010, 0, 0)  # WM_CLOSE
        return len(hwnds)

    def window_hung(self) -> bool | None:
        """Windows' own test of a window that stopped handling its messages (IsHungAppWindow: none
        for 5 s), for this recomp's visible top-level windows; None without one (or off Windows).
        The recomp handles its window's events on the game's thread, so a game held between two
        frames (a replay paused) must keep doing so (the core's Engine::WhileStandingStill)."""
        if os.name != "nt" or self.process is None:
            return None
        import ctypes
        import ctypes.wintypes as wt
        user32 = ctypes.windll.user32
        pid = self.process.pid
        found: list[int] = []

        @ctypes.WINFUNCTYPE(wt.BOOL, wt.HWND, wt.LPARAM)
        def visit(hwnd: int, _: int) -> bool:
            owner = wt.DWORD()
            user32.GetWindowThreadProcessId(hwnd, ctypes.byref(owner))
            if owner.value == pid and user32.IsWindowVisible(hwnd):
                found.append(hwnd)
            return True
        user32.EnumWindows(visit, 0)
        if not found:
            return None
        return any(bool(user32.IsHungAppWindow(h)) for h in found)

    def _wsl(self, *command: str, timeout: float = 60.0) -> subprocess.CompletedProcess[bytes]:
        assert self.wsl is not None
        return subprocess.run(["wsl.exe", "-d", self.wsl[0], "-u", "root", "--", *command],
                              capture_output=True, timeout=timeout, check=False)

    def _prepare_wsl(self) -> Path:
        """<user dir>/recomp/launch.sh, which makes the instance's program folder in the distro as
        it starts (the program hard-linked from the Linux build; the build's Sys, wii_bootstrap,
        NAND and Codes copied; the test's Config.toml; the instance's card linked; portable.txt),
        then execs the program (its pid in recomp/wsl.pid, for _wsl_reap)."""
        assert self.wsl is not None
        distro, build = self.wsl
        d = self.recomp_dir
        d.mkdir(exist_ok=True)
        # The first command may start the distro (its VM stops when idle): give it time.
        got = self._wsl("cat", f"{build}/UserData/Config.toml", timeout=300)
        if got.returncode != 0:
            raise InstanceError(f"{distro}:{build}/UserData/Config.toml: {got.stderr.decode(errors='replace')}")
        (d / "Config.toml").write_text(test_config(got.stdout.decode("utf-8")), encoding="utf-8", newline="\n")
        user = wsl_path(self._need_dir())
        root = os.environ.get("PPHARNESS_RECOMP_WSL_WORK", "/root/ppharness-recomp").rstrip("/")
        work = f"{root}/{re.sub(r'[^A-Za-z0-9_.-]', '_', self._need_dir().name)}"
        env = dict(self.env, BRAWL_ONLINE_CORE=user)
        q = shlex.quote
        script = [
            "#!/usr/bin/env bash",
            "# ppharness/recomp.py: this instance's EmissaryRecomp, the Linux build, in WSL.",
            "set -euo pipefail",
            f"build={q(build)} work={q(work)} user={q(user)}",
            'rm -rf "$work" && mkdir -p "$work/UserData"',
            'for f in "$build"/*; do',
            '  [[ -f $f ]] || continue',
            '  case ${f##*/} in WiiCompiled|cacert.pem|dsp_coef.bin|*.db|*.so*) ;; *) continue ;; esac',
            '  ln "$f" "$work/" 2>/dev/null || cp "$f" "$work/"',
            "done",
            'for sub in Sys wii_bootstrap; do [[ ! -d $build/$sub ]] || cp -r "$build/$sub" "$work/"; done',
            'for sub in NAND Codes; do [[ ! -d $build/UserData/$sub ]] || cp -r "$build/UserData/$sub" "$work/UserData/"; done',
            'cp "$user/recomp/Config.toml" "$work/UserData/Config.toml"',
            '[[ ! -e $user/Wii/sd.raw ]] || ln -s "$user/Wii/sd.raw" "$work/UserData/sd.raw"',
            'echo "Settings, saves and logs stay in the UserData folder next to this file." > "$work/portable.txt"',
            'echo $$ > "$user/recomp/wsl.pid"',
            'cd "$work"',
            *(f"export {k}={q(v)}" for k, v in sorted(env.items())),
            "exec nice -n 5 ./WiiCompiled",
        ]
        (d / "launch.sh").write_text("\n".join(script) + "\n", encoding="utf-8", newline="\n")
        (d / "wsl.pid").unlink(missing_ok=True)
        return d

    def _wsl_reap(self) -> None:
        """Ends the program in WSL when it outlived its wsl.exe (killing wsl.exe leaves it running)."""
        pid_file = self.recomp_dir / "wsl.pid"
        pid = pid_file.read_text().strip() if pid_file.is_file() else ""
        if self.wsl is not None and pid.isdigit():
            self._wsl("bash", "-c", f"[ ! -e /proc/{pid}/exe ] || kill -9 {pid}", timeout=30)

    def game_path(self, path: Path) -> str:
        """A path of the instance's folders as the game names it (in WSL: /mnt/<drive>/...)."""
        return wsl_path(path) if self.wsl is not None else str(path)

    def stop(self, timeout: float | None = None) -> int | None:
        code = super().stop(timeout)
        if self.wsl is not None and self.user_dir is not None:
            self._wsl_reap()
        return code

    def build_args(self) -> list[str]:
        if self.wsl is not None:
            return ["wsl.exe", "-d", self.wsl[0], "-u", "root", "--", "bash",
                    wsl_path(self.recomp_dir / "launch.sh")]
        exe = self.recomp_dir / ("WiiCompiled.exe" if os.name == "nt" else "WiiCompiled")
        # PPHARNESS_RECOMP_LLDB: run it under lldb, which prints every thread's stack when it stops
        # on an exception (a crash that leaves no trace otherwise).
        lldb = os.environ.get("PPHARNESS_RECOMP_LLDB")
        if lldb:
            return [lldb, "-b", "-o", "process launch", "-o", "thread backtrace all", "-o",
                    "register read", "-o", "quit", "--", str(exe)]
        return [str(exe)]

    def launch(self) -> subprocess.Popen[bytes]:
        if self.process is not None:
            raise InstanceError("already launched")
        d = self.create()
        if self.wsl is None:
            self._prepare_recomp()
        self.env.update({
            "BRAWL_ONLINE_CORE": str(d),
            "BRAWL_SD_VOLATILE_WRITES": "1",
            "SDL_JOYSTICK_HIDAPI_GAMECUBE": "0",
            "SDL_JOYSTICK_HIDAPI": "0",
        })
        # A playback instance's comm file (Dolphin's `-i <comm.json>`, the launcher's playback Dolphin):
        # the recomp follows it from BRAWL_ONLINE_REPLAY_COMM.
        extra = list(self.config.extra_args)
        if "-i" in extra and extra.index("-i") + 1 < len(extra):
            self.env["BRAWL_ONLINE_REPLAY_COMM"] = extra[extra.index("-i") + 1]
        # PPHARNESS_RECOMP_ENV: "NAME=value;NAME=value" for the recomp instances only (a test hook
        # such as PPR_GPRB_TEST_STALL_AT on the recomp side of a crossplay set).
        for item in os.environ.get("PPHARNESS_RECOMP_ENV", "").split(";"):
            name, sep, value = item.partition("=")
            if sep and name.strip():
                self.env[name.strip()] = value
        if self.port is None:
            from .instance import find_free_port
            self.port = find_free_port()
        self.env["BRAWL_ONLINE_HARNESS_PORT"] = str(self.port)
        if self.wsl is not None:
            self._prepare_wsl()
        # DolphinInstance.launch with the recomp's command line and folder.
        self.command_prefix = [str(self.build_args()[0])]
        return super().launch()


def game_output(inst: DolphinInstance) -> str:
    """What the game printed (P+'s OSReport lines among it) and the core logged: Dolphin's log
    (OSREPORT lines), and for an EmissaryRecomp also its stdout, where it prints OSReport
    ("[OSReport] ..."), and its stderr (its runtime's own lines)."""
    d = inst.user_dir
    if d is None:
        return ""
    text = ""
    log = d / "Logs" / "dolphin.log"
    if log.exists():
        text = log.read_text(encoding="utf-8", errors="replace")
    if isinstance(inst, RecompInstance):
        for name in ("harness-stdout.txt", "harness-stderr.txt"):
            f = d / name
            if f.exists():
                text += "\n" + f.read_text(encoding="utf-8", errors="replace")
    return text
