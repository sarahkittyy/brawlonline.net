#!/usr/bin/env python3
"""A harness template for a mod of P+ (docs/design/pplus-mods.md §5): the P+ template user folder
with the mod's SD card as its Wii/sd.raw. A P+Ex build boots from P+'s own launcher files, which
the template already has; a build with launchers of its own (REX 0.5: "REX Offline Launcher.elf"
and "REX Netplay Launcher.elf") gets them in the template's Launcher folder, each with Dolphin
game settings that turn MMU emulation off, as online play has it (docs/design/pplus-mods.md §3).

    python harness/tools/mod_template.py --card "Cobalt/SD.raw" --out run/mods/cobalt-1.34
    python harness/tools/mod_template.py --card "REX/user/Wii/sd.raw" --out run/mods/rex-0.5 \
        --offline-launcher "REX/user/Launcher/REX Offline Launcher.elf" \
        --netplay-launcher "REX/user/Launcher/REX Netplay Launcher.elf"

Prints the build's folder on the card and its keys for mm's games table (tools/gamecode/game_key.py).
The template says which build it is (`<out>/build.json`: the folder, and the launchers of a build
with its own), so with PPHARNESS_TEMPLATE=<out> alone the harness and its tests run on that build
(harness/tools/mod_suite.py --template runs the online ones).
"""
from __future__ import annotations

import argparse
import shutil
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
for sub in ("harness", "tools/sdcard", "tools/gamecode"):
    sys.path.insert(0, str(ROOT / sub))
import fat32  # noqa: E402
import game_key  # noqa: E402
import patch_sd  # noqa: E402
from ppharness import paths  # noqa: E402


def build_folders(image: Path) -> list[str]:
    """The folders on the card that hold a build: a codeset and a pf folder."""
    fs = fat32.open_image(image)
    out = []
    for e in fs.list_dir(fs.lookup("/").first_cluster):
        if not e.is_dir:
            continue
        has_codes = any(fs.lookup(f"/{e.name}/{n}") for n in game_key.CODESETS)
        if has_codes and fs.lookup(f"/{e.name}/pf"):
            out.append(e.name)
    return out


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--card", type=Path, required=True, help="the mod's SD card image")
    ap.add_argument("--out", type=Path, required=True, help="the template folder to make")
    ap.add_argument("--base", type=Path, default=None, help="the P+ template (default: the harness's)")
    ap.add_argument("--offline-launcher", type=Path, help="the build's own offline launcher (.elf or .dol)")
    ap.add_argument("--netplay-launcher", type=Path, help="the build's own netplay launcher (.elf or .dol)")
    a = ap.parse_args()
    if bool(a.offline_launcher) != bool(a.netplay_launcher):
        ap.error("--offline-launcher with --netplay-launcher")
    base = a.base or (ROOT / "run" / "template-user")
    if not (base / "Launcher" / paths.NETPLAY_LAUNCHER.name).exists():
        print(f"{base} is not a P+ template user folder (no {paths.NETPLAY_LAUNCHER})", file=sys.stderr)
        return 1
    folders = build_folders(a.card)
    if len(folders) != 1:
        print(f"{a.card}: expected one build folder on the card, found {folders}", file=sys.stderr)
        return 1
    if a.out.exists():
        print(f"{a.out} exists", file=sys.stderr)
        return 1
    shutil.copytree(base, a.out, ignore=shutil.ignore_patterns("sd.raw"))
    (a.out / "Wii").mkdir(exist_ok=True)
    patch_sd.fast_copy(a.card, a.out / "Wii" / "sd.raw")
    folder = folders[0]
    print(f"template {a.out}: the card's build folder is /{folder}")
    launchers = [paths.NETPLAY_LAUNCHER, paths.OFFLINE_LAUNCHER]
    descriptor = {"folder": folder}
    env = f"PPHARNESS_TEMPLATE={a.out.resolve()}"
    if a.offline_launcher:
        settings = a.out / "GameSettings"
        settings.mkdir(exist_ok=True)
        launchers = []
        for src in (a.netplay_launcher, a.offline_launcher):
            shutil.copy2(src, a.out / "Launcher" / src.name)
            # Dolphin's settings for a boot file are named after it ("ID-<name>.ini"): online play
            # runs with MMU emulation off, whatever the build's own settings say.
            (settings / f"ID-{src.stem}.ini").write_text("[Core]\nCPUThread = True\nMMU = False\n")
            launchers.append(Path("Launcher") / src.name)
        descriptor.update(offlineLauncher=a.offline_launcher.name, netplayLauncher=a.netplay_launcher.name)
    for launcher in launchers:
        k = game_key.game_key(a.out / "Wii" / "sd.raw", folder, a.out / launcher)
        print(f"  {k['key']}  ({launcher.name})")
    # The template says which build it is (ppharness/paths.py): nothing else names it.
    import json
    (a.out / paths.BUILD_DESCRIPTOR).write_text(json.dumps(descriptor, indent=1) + "\n")
    print(f"run the tests on it with: {env}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
