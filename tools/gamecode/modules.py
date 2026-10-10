#!/usr/bin/env python3
"""List the loaded REL modules of a running instance (OSModuleInfo list at 0x800030C8).

    python tools/gamecode/modules.py --port P
Prints id, module base, .text address and name (from the module's name hint if present).
"""
from __future__ import annotations
import argparse, struct, sys
from pathlib import Path
ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "harness"))
from ppharness.client import HarnessClient  # noqa: E402

NAMES = {1: "sora_scene", 2: "sora_menu_main", 3: "sora_menu_sel_char", 4: "sora_menu_sel_stage",
         27: "sora_melee", 18: "sora_minigame?", 11: "sora_menu_title?", 16: "sora_menu_?",
         224: "sy_core", 20560: "PPOnline (plugin)", 20561: "PPOnline (loader)"}


def modules(c: HarnessClient):
    head = c.read_u32(0x800030C8)
    out = []
    p = head
    seen = set()
    while p and p not in seen and len(out) < 64:
        seen.add(p)
        h = c.read_mem(p, 0x40)
        mid, nxt, prv, nsec, secoff = struct.unpack_from(">5I", h, 0)
        secs = c.read_mem(secoff, 8 * nsec) if nsec <= 64 else b""
        text = None
        for i in range(nsec):
            off, size = struct.unpack_from(">II", secs, 8 * i)
            if off & 1 and size:
                text = (off & ~1, size)
                break
        out.append((mid, p, text))
        p = nxt
    return out


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", type=int, required=True)
    a = ap.parse_args()
    c = HarnessClient.connect(a.port, timeout=10)
    for mid, base, text in modules(c):
        t = f"text={text[0]:#010x}+{text[1]:#x}" if text else ""
        print(f"id={mid:<6} base={base:#010x} {t} {NAMES.get(mid, '')}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
