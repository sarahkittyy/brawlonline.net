"""EmissaryRecomp instances under the harness (docs/design/brawl-online-core.md §10).

A RecompInstance is a DolphinInstance whose game is EmissaryRecomp running Brawl Online's core
in-process: the same user folder (Config/Dolphin.ini with the online account, Online/, Logs/
dolphin.log, Wii/sd.raw with the plugin the test put there), the same harness protocol (the recomp
answers the commands its online tests use, brawl_online_harness.cpp), so a test drives it as it
drives Dolphin, and a test can pair one with a Dolphin instance (crossplay).

The recomp build (PPHARNESS_RECOMP: its native build folder, with WiiCompiled.exe and UserData/)
is used through hard links in ``<user dir>/recomp`` (a portable layout of its own per instance);
the card is linked read-only (BRAWL_SD_VOLATILE_WRITES).
"""

from __future__ import annotations

import os
import shutil
import subprocess
from pathlib import Path
from typing import Any

from .instance import DolphinInstance, InstanceError

LINKED_FILES = ("WiiCompiled.exe", "WiiCompiled", "cacert.pem", "dsp_coef.bin", "initial_pipeline_cache.db",
                "portable.txt")
COPIED_DIRS = ("Sys", "wii_bootstrap")


def recomp_build() -> Path | None:
    p = os.environ.get("PPHARNESS_RECOMP")
    return Path(p) if p else None


def wants_recomp(name: str) -> bool:
    """PPHARNESS_RECOMP set and the instance's name matches PPHARNESS_RECOMP_NAMES (a regex, all by
    default)."""
    import re
    if not recomp_build():
        return False
    pattern = os.environ.get("PPHARNESS_RECOMP_NAMES", "")
    return not pattern or re.search(pattern, name) is not None


def _link(src: Path, dst: Path) -> None:
    if dst.exists():
        dst.unlink()
    try:
        os.link(src, dst)
    except OSError:
        shutil.copyfile(src, dst)


def _mute(config: Path) -> None:
    """Test runs are silent, as Dolphin's (InstanceConfig.audio): [audio] muted, volume 0."""
    lines = config.read_text(encoding="utf-8").splitlines()
    keep = [ln for ln in lines if not ln.strip().startswith(("muted", "volume ", "volume="))]
    if "[audio]" not in (ln.strip() for ln in keep):
        keep.append("[audio]")
    i = [ln.strip() for ln in keep].index("[audio]")
    keep[i + 1:i + 1] = ["muted = true", "volume = 0.0"]
    config.write_text("\n".join(keep) + "\n", encoding="utf-8")


class RecompInstance(DolphinInstance):
    """DolphinInstance's user folder and harness client, EmissaryRecomp's game."""

    def __init__(self, name: str = "instance", **kw: Any):
        kw.pop("exe", None)
        kw.setdefault("boot", None)
        super().__init__(name, exe=os.devnull, **kw)
        self.build = recomp_build()
        if self.build is None:
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

    def build_args(self) -> list[str]:
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
        self._prepare_recomp()
        self.env.update({
            "BRAWL_ONLINE_CORE": str(d),
            "BRAWL_SD_VOLATILE_WRITES": "1",
            "SDL_JOYSTICK_HIDAPI_GAMECUBE": "0",
            "SDL_JOYSTICK_HIDAPI": "0",
        })
        if self.port is None:
            from .instance import find_free_port
            self.port = find_free_port()
        self.env["BRAWL_ONLINE_HARNESS_PORT"] = str(self.port)
        # DolphinInstance.launch with the recomp's command line and folder.
        self.command_prefix = [str(self.build_args()[0])]
        return super().launch()
