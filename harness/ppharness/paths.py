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

BUILD_DESCRIPTOR = "build.json"


def _build_descriptor() -> dict:
    """The build a template user folder is of, as harness/tools/mod_template.py wrote it beside
    the folder's files: `<template>/build.json` {"folder": the build's folder on its SD card,
    "offlineLauncher" / "netplayLauncher": its own launcher files when it has them}. {} for P+'s
    own template (none) and for a template made before 2026-11-16."""
    import json
    template = os.environ.get("PPHARNESS_TEMPLATE")
    if not template:
        return {}
    try:
        d = json.loads((Path(template).expanduser() / BUILD_DESCRIPTOR).read_text())
    except (OSError, ValueError):
        return {}
    return d if isinstance(d, dict) else {}


def _apply_build_descriptor() -> None:
    """The template names its build (above), so PPHARNESS_TEMPLATE alone says which build the
    harness runs: what the tools and tests read of a build (its folder, its launchers) is set
    from the template's descriptor here, once, when the harness is imported. A variable set by
    hand still wins (a template without a descriptor needs them)."""
    d = _build_descriptor()
    for key, name in (("folder", "PPHARNESS_MOD_FOLDER"), ("offlineLauncher", "PPHARNESS_OFFLINE_LAUNCHER"),
                      ("netplayLauncher", "PPHARNESS_NETPLAY_LAUNCHER")):
        if isinstance(d.get(key), str) and d[key] and not os.environ.get(name):
            os.environ[name] = d[key]


_apply_build_descriptor()

# A build with launchers of its own (REX: "REX Offline Launcher.elf") names them, as files of the
# template's Launcher folder (its descriptor's, or PPHARNESS_OFFLINE_LAUNCHER / _NETPLAY_LAUNCHER).
OFFLINE_LAUNCHER = Path("Launcher") / os.environ.get("PPHARNESS_OFFLINE_LAUNCHER", "Project+ Offline Launcher.dol")
NETPLAY_LAUNCHER = Path("Launcher") / os.environ.get("PPHARNESS_NETPLAY_LAUNCHER", "Project+ Netplay Launcher.dol")


def _env_path(name: str) -> Path | None:
    v = os.environ.get(name)
    return Path(v).expanduser() if v else None


def workspace_root() -> Path:
    return _env_path("PPHARNESS_ROOT") or Path(__file__).resolve().parents[2]


# The game plugin is built once per kind of build (game-code/build.sh, the plugin's variant.h):
# PPOnline.rel for Project+, PPOnline-pex.rel for P+ v3.2 with BrawlEx (Project+ Ex, Cobalt),
# PPOnline-rex.rel for REX. Which one a build takes is its entry's `plugin` in mm's games table
# (server/config/games.json), found by the build's card folder.
PLUGIN_VARIANTS = ("pplus", "pex", "rex")


def plugin_variant() -> str:
    """The plugin variant of the build the harness runs (PPHARNESS_TEMPLATE's descriptor names
    its folder): its entry's `plugin` in the games table, "pplus" for none. PPHARNESS_PLUGIN_VARIANT
    picks another (a test of the wrong plugin on a build)."""
    v = os.environ.get("PPHARNESS_PLUGIN_VARIANT")
    if not v:
        import json
        folder = os.environ.get("PPHARNESS_MOD_FOLDER") or "Project+"
        try:
            games = json.loads((workspace_root() / "server" / "config" / "games.json").read_text())["games"]
        except (OSError, ValueError, KeyError):
            games = []
        v = next((g.get("plugin") for g in games if g.get("folder") == folder), None) or "pplus"
    if v not in PLUGIN_VARIANTS:
        raise ValueError(f"plugin variant {v!r}: one of {PLUGIN_VARIANTS}")
    return v


def plugin_path(variant: str | None = None) -> Path:
    """The packed plugin put on an instance's SD card: PPHARNESS_PLUGIN (a frozen copy, e.g. the
    one before a change) or game-code's build of the build's variant."""
    frozen = _env_path("PPHARNESS_PLUGIN")
    if frozen and variant is None:
        return frozen
    v = variant or plugin_variant()
    return workspace_root() / "game-code" / "PPOnline" / ("PPOnline.rel" if v == "pplus" else f"PPOnline-{v}.rel")


def plugin_map(plugin: Path | None = None) -> Path:
    """The link map beside a plugin (its statics' addresses, which differ between the variants):
    PPOnlineMain.map beside PPOnline.rel, PPOnlineMain-pex.map beside PPOnline-pex.rel."""
    p = plugin or plugin_path()
    return p.with_name(p.stem.replace("PPOnline", "PPOnlineMain", 1) + ".map")


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
