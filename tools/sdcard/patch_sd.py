#!/usr/bin/env python3
"""Make a patched copy of P+'s virtual SD card (``sd.raw``) with our files added.

The source image is never modified: it is copied to DST first (CopyFileW / reflink fast path
through the harness helper when available), and only DST is written.

    python tools/sdcard/patch_sd.py SRC DST --add LOCAL=SD_PATH [--add ...] [--remove SD_PATH]
    python tools/sdcard/patch_sd.py SRC DST --plugin game-code/PPOnline/PPOnline.rel
    python tools/sdcard/patch_sd.py --in-place DST --plugin ...      # DST must not be the template
    python tools/sdcard/patch_sd.py --list SRC [SD_DIR]
    python tools/sdcard/patch_sd.py --check SRC

``--plugin X.rel`` is shorthand for ``--add X.rel=/Project+/pf/plugins/X.rel``: P+ v3.2 loads
Syriinge 0.6.0 (sy_core.rel, file 18 of /Project+/pf/system/common2.pac) through its codeset
(Source/Community/Syringe.asm), and sy_core loads every ``/Project+/pf/plugins/*.rel``
("%spf/%s/*.rel" with the mod folder "/Project+/"). Nothing else on the card has to change.

After writing, the image is re-opened and checked: every added file reads back byte-identical,
and the FAT has no shared or lost clusters.
"""

from __future__ import annotations

import argparse
import os
import shutil
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import fat32  # noqa: E402

ROOT = Path(__file__).resolve().parents[2]
TEMPLATE_SD = ROOT / "run" / "template-user" / "Wii" / "sd.raw"
PLUGIN_DIR = "/Project+/pf/plugins"


def fast_copy(src: Path, dst: Path) -> None:
    try:
        sys.path.insert(0, str(ROOT / "harness"))
        from ppharness import _platform  # type: ignore
        _platform.fast_copy_file(src, dst)
    except Exception:
        shutil.copyfile(src, dst)


def _same(a: Path, b: Path) -> bool:
    try:
        return a.resolve() == b.resolve() or (a.exists() and b.exists() and os.path.samefile(a, b))
    except OSError:
        return False


def patch_image(image: Path, adds: list[tuple[bytes, str]], removes: list[str] = ()) -> list[str]:
    """Apply adds/removes to an image IN PLACE. Refuses the template. Returns a log."""
    if _same(image, TEMPLATE_SD):
        raise SystemExit(f"refusing to modify the template image {image}")
    out = []
    with open(image, "r+b") as f:
        fs = fat32.Fat32(f, writable=True)
        for p in removes:
            if fs.delete(p):
                out.append(f"removed {p}")
        for data, sd_path in adds:
            fs.write_file(sd_path, data)
            out.append(f"added {sd_path} ({len(data)} bytes)")
        fs.flush()
    with open(image, "rb") as f:
        fs = fat32.Fat32(f)
        for data, sd_path in adds:
            if fs.read_file(sd_path) != data:
                raise SystemExit(f"verify failed: {sd_path} does not read back identical")
        problems = fs.check()
        if problems:
            raise SystemExit("FAT check failed:\n  " + "\n  ".join(problems[:20]))
    out.append("verified: files read back identical, FAT consistent")
    return out


def parse_adds(args: argparse.Namespace) -> list[tuple[bytes, str]]:
    adds = []
    for a in args.add or []:
        local, _, sd_path = a.partition("=")
        if not sd_path:
            raise SystemExit(f"--add needs LOCAL=SD_PATH, got {a!r}")
        adds.append((Path(local).read_bytes(), sd_path))
    for p in args.plugin or []:
        pp = Path(p)
        adds.append((pp.read_bytes(), f"{PLUGIN_DIR}/{pp.name}"))
    return adds


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("src", nargs="?", help="source image (default: run/template-user/Wii/sd.raw)")
    ap.add_argument("dst", nargs="?", help="output image (created; must differ from src)")
    ap.add_argument("--add", action="append", metavar="LOCAL=SD_PATH")
    ap.add_argument("--plugin", action="append", metavar="FILE.rel")
    ap.add_argument("--remove", action="append", metavar="SD_PATH", default=[])
    ap.add_argument("--in-place", metavar="IMAGE", help="patch IMAGE directly (never the template)")
    ap.add_argument("--list", action="store_true", help="list SRC (optionally a directory: dst arg)")
    ap.add_argument("--check", action="store_true", help="FAT consistency check of SRC")
    args = ap.parse_args()

    if args.list or args.check:
        img = Path(args.src or TEMPLATE_SD)
        with open(img, "rb") as f:
            fs = fat32.Fat32(f)
            if args.check:
                probs = fs.check()
                print("clean" if not probs else "\n".join(probs))
                return 1 if probs else 0
            d = args.dst or "/"
            e = fs.lookup(d)
            if e is None:
                raise SystemExit(f"{d}: not found")
            for ent in fs.list_dir(e.first_cluster):
                print(f"{'d' if ent.is_dir else '-'} {ent.size:>10}  {ent.name}")
        return 0

    adds = parse_adds(args)
    if args.in_place:
        for line in patch_image(Path(args.in_place), adds, args.remove):
            print(line)
        return 0
    src = Path(args.src or TEMPLATE_SD)
    if not args.dst:
        raise SystemExit("need DST (or --in-place IMAGE)")
    dst = Path(args.dst)
    if _same(src, dst):
        raise SystemExit("DST must differ from SRC")
    dst.parent.mkdir(parents=True, exist_ok=True)
    fast_copy(src, dst)
    for line in patch_image(dst, adds, args.remove):
        print(line)
    print(f"wrote {dst}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
