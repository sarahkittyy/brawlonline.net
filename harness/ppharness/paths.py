"""Default locations. Every one can be overridden by an environment variable.

=====================  ==========================================  ==========================
what                   default                                     override
=====================  ==========================================  ==========================
workspace root         two levels above this package               ``PPHARNESS_ROOT``
Dolphin binaries dir   ``<root>/dolphin/build/release/x64/Binaries``  ``PPHARNESS_DOLPHIN_DIR``
DolphinNoGUI           ``<binaries>/DolphinNoGUI.exe``             ``PPHARNESS_DOLPHIN``
                       (``dolphin-emu-nogui`` on Linux/macOS)
template user dir      ``<root>/run/template-user``                ``PPHARNESS_TEMPLATE``
game disc              the template's ``[Core] DefaultISO``         ``PPHARNESS_ISO``
                       (``<root>/game/SSBB_NTSC.iso`` if that path
                       doesn't exist here)
instance dirs          ``<root>/run/instances``                    ``PPHARNESS_INSTANCES``
=====================  ==========================================  ==========================
"""

from __future__ import annotations

import os
from pathlib import Path

from . import _platform

OFFLINE_LAUNCHER = Path("Launcher") / "Project+ Offline Launcher.dol"
NETPLAY_LAUNCHER = Path("Launcher") / "Project+ Netplay Launcher.dol"


def _env_path(name: str) -> Path | None:
    v = os.environ.get(name)
    return Path(v).expanduser() if v else None


def workspace_root() -> Path:
    return _env_path("PPHARNESS_ROOT") or Path(__file__).resolve().parents[2]


def binaries_dir() -> Path:
    return _env_path("PPHARNESS_DOLPHIN_DIR") or _platform.default_binaries_dir(
        workspace_root() / "dolphin")


def dolphin_nogui() -> Path:
    return _env_path("PPHARNESS_DOLPHIN") or binaries_dir() / _platform.nogui_exe_name()


def dolphin_gui() -> Path:
    return binaries_dir() / _platform.gui_exe_relpath()


def game_iso() -> Path | None:
    """``PPHARNESS_ISO`` if set, else None.

    With None, DolphinInstance keeps the template's ``[Core] DefaultISO``, unless that path
    doesn't exist on this machine (e.g. a Windows path inside a Linux container) and
    ``fallback_iso()`` does.
    """
    return _env_path("PPHARNESS_ISO")


def fallback_iso() -> Path:
    return workspace_root() / "game" / "SSBB_NTSC.iso"


def template_user_dir() -> Path:
    return _env_path("PPHARNESS_TEMPLATE") or workspace_root() / "run" / "template-user"


def instances_root() -> Path:
    return _env_path("PPHARNESS_INSTANCES") or workspace_root() / "run" / "instances"
