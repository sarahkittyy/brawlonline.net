"""Default locations. Every one can be overridden by an environment variable.

=====================  ==========================================  ==========================
what                   default                                     override
=====================  ==========================================  ==========================
workspace root         two levels above this package               ``PPHARNESS_ROOT``
Dolphin binaries dir   ``<root>/dolphin/build/release/x64/Binaries``  ``PPHARNESS_DOLPHIN_DIR``
DolphinNoGUI           ``<binaries>/DolphinNoGUI[.exe]``           ``PPHARNESS_DOLPHIN``
template user dir      ``<root>/run/template-user``                ``PPHARNESS_TEMPLATE``
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
    return _env_path("PPHARNESS_DOLPHIN") or binaries_dir() / _platform.exe_name("DolphinNoGUI")


def dolphin_gui() -> Path:
    return binaries_dir() / _platform.exe_name("Dolphin")


def template_user_dir() -> Path:
    return _env_path("PPHARNESS_TEMPLATE") or workspace_root() / "run" / "template-user"


def instances_root() -> Path:
    return _env_path("PPHARNESS_INSTANCES") or workspace_root() / "run" / "instances"
