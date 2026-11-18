#!/usr/bin/env python3
"""The key of a game build, as Dolphin sends it to mm (Core/Online/GameBuild.h MakeKey;
docs/design/pplus-mods.md §2): "<folder>:<32 hex digits>" from the SHA-256 of the booted file and
of the four codesets in the build's folder on the SD card.

    python tools/gamecode/game_key.py CARD.raw FOLDER BOOT_FILE      # one key
    python tools/gamecode/game_key.py CARD.raw FOLDER BOOT... --json # with the parts

FOLDER is the build's folder on the card ("Project+", "Cobalt", "P+Ex"); BOOT_FILE the file
Dolphin boots (the launcher .dol). The keys of the builds mm lets play are in
server/config/games.json.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "sdcard"))
import fat32  # noqa: E402

CODESETS = ("NETPLAY.GCT", "NETBOOST.GCT", "RSBE01.GCT", "BOOST.GCT")


def make_key(folder: str, boot_sha256: str, codeset_sha256: list[str]) -> str:
    """`codeset_sha256`: one per CODESETS, "" for a file that is not there."""
    if not folder:
        return ""
    text = f"boot={boot_sha256 or '-'}\n" + "".join(f"{n}={h or '-'}\n" for n, h in zip(CODESETS, codeset_sha256))
    return f"{folder}:{hashlib.sha256(text.encode()).hexdigest()[:32]}"


def card_codesets(image: Path, folder: str) -> list[str]:
    fs = fat32.open_image(image)
    out = []
    for name in CODESETS:
        try:
            out.append(hashlib.sha256(fs.read_file(f"/{folder}/{name}")).hexdigest())
        except (FileNotFoundError, fat32.FatError):
            out.append("")
    return out


def game_key(image: Path, folder: str, boot: Path) -> dict:
    boot_sha = hashlib.sha256(boot.read_bytes()).hexdigest()
    codesets = card_codesets(image, folder)
    return {"key": make_key(folder, boot_sha, codesets), "folder": folder, "boot": boot_sha,
            "codesets": dict(zip(CODESETS, codesets))}


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("card", type=Path)
    ap.add_argument("folder")
    ap.add_argument("boot", type=Path, nargs="+")
    ap.add_argument("--json", action="store_true")
    a = ap.parse_args()
    for boot in a.boot:
        k = game_key(a.card, a.folder, boot)
        print(json.dumps(dict(k, bootFile=boot.name)) if a.json else f"{k['key']}  {boot.name}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
