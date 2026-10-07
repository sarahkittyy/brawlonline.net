#!/usr/bin/env python3
"""List the code addresses P+'s codeset patches, to check our hook sites for conflicts.

    python tools/gamecode/pplus_hooks.py [--sd sd.raw] [--codeset NETPLAY.txt|RSBE01.txt] [ADDR ...]

Parses GCTRealMate sources from the SD card (following .include), collecting
`HOOK @ $X`, `op ... @ $X`, `CODE @ $X`, and raw gecko lines (04/C2/06 types).
With ADDR arguments, reports any patch within 0x10 bytes of each address.
"""
from __future__ import annotations
import argparse, re, sys
from pathlib import Path
ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools" / "sdcard"))
import fat32  # noqa: E402

AT = re.compile(r"@\s*\$([0-9A-Fa-f]{8})")
RAW = re.compile(r"^\s*\*?\s*(04|05|C2|C3|06|07)([0-9A-Fa-f]{6})\s+[0-9A-Fa-f]{8}")


def collect(fs, path: str, seen: set, out: list) -> None:
    if path.lower() in seen:
        return
    seen.add(path.lower())
    try:
        text = fs.read_file(path).decode("latin-1")
    except FileNotFoundError:
        return
    base = "/Project+/"
    for n, line in enumerate(text.splitlines(), 1):
        s = line.split("#", 1)[0]
        m = re.match(r"\s*\.include\s+(\S+)", s)
        if m:
            collect(fs, base + m.group(1), seen, out)
            continue
        for a in AT.findall(s):
            out.append((int(a, 16), path, n, line.strip()[:90]))
        m = RAW.match(s)
        if m:
            hi = 0x80000000 if m.group(1) in ("04", "C2", "06") else 0x81000000
            out.append((hi | int(m.group(2), 16), path, n, line.strip()[:90]))


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--sd", default=str(ROOT / "run/template-user/Wii/sd.raw"))
    ap.add_argument("--codeset", action="append")
    ap.add_argument("addrs", nargs="*")
    a = ap.parse_args()
    fs = fat32.open_image(a.sd)
    out: list = []
    for cs in a.codeset or ["NETPLAY.txt", "RSBE01.txt", "BOOST.txt", "NETBOOST.txt"]:
        collect(fs, "/Project+/" + cs, set(), out)
    out = sorted(set(out))
    if not a.addrs:
        for addr, p, n, l in out:
            print(f"{addr:08X} {p}:{n} {l}")
        return 0
    bad = 0
    for s in a.addrs:
        x = int(s, 16)
        hits = [o for o in out if abs(o[0] - x) <= 0x10]
        print(f"{x:08X}: {'clear' if not hits else ''}")
        for addr, p, n, l in hits:
            bad += 1
            print(f"    {addr:08X} {p}:{n} {l}")
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
