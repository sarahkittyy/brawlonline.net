"""The Special Versus box's bundle as Dolphin's Online/SpecialAssets builds it (docs/design/
special-rules.md §5), for tests whose Dolphin side is a script (roomsim): P+'s main-menu archive
(/Project+/pf/menu2/mu_menumain.pac) from an SD card image, the box's ARC entries kept.

    bundle = special_bundle.from_card("run/template-user/Wii/sd.raw")
"""
from __future__ import annotations

import functools
import struct
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
sys.path.insert(0, str(HERE.parent / "sdcard"))

import brawlarc  # noqa: E402

MAGIC = 0x53504258   # "SPBX"
ROLE_TEXTURES, ROLE_MESSAGES, ROLES = 0, 11, 12


def build(archive: bytes) -> bytes:
    archive = brawlarc.maybe_decompress(archive)
    found: dict[int, tuple[int, bytes]] = {}
    for _i, _t, _idx, _g, top in brawlarc.parse_arc(archive):
        top = bytes(top)
        if top[:4] != b"ARC\0" or top[0x10:0x18] != b"MenuMain":
            continue
        for _j, t, idx, _g2, d in brawlarc.parse_arc(top):
            d = bytes(d)
            if d[:4] == b"bres" and t == 3 and ROLE_TEXTURES not in found and b"MenMainSpecial04Frame\0" in d:
                found[ROLE_TEXTURES] = (t, d)
            elif d[:4] == b"bres" and t == 2:
                for r in range(1, 11):
                    if r not in found and f"MenMainSpecial{r:04}_TopN\0".encode() in d and b"MDL0" in d:
                        found[r] = (t, d)
                        break
            elif d[:4] != b"bres" and t == 1 and idx == 1 and ROLE_MESSAGES not in found and \
                    len(d) > 0xF0 and struct.unpack_from(">I", d, 0)[0] == 0xF0:
                found[ROLE_MESSAGES] = (t, d)
        break
    missing = [r for r in range(ROLES) if r not in found]
    if missing:
        raise ValueError(f"no Special Versus part {missing}")
    head = 0x10 + ROLES * 0x10
    out = bytearray(head + (-head % 0x20))
    for r in range(ROLES):
        t, d = found[r]
        at = len(out)
        out += d
        out += bytes(-len(out) % 0x20)
        struct.pack_into(">HHII", out, 0x10 + r * 0x10, r, t, at, len(d))
    struct.pack_into(">III", out, 0, MAGIC, len(out), ROLES)
    return bytes(out)


@functools.lru_cache(maxsize=2)
def from_card(image: str) -> bytes:
    from fat32 import open_image
    fs = open_image(image)
    return build(fs.read_file("/Project+/pf/menu2/mu_menumain.pac"))
