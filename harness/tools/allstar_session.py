"""Two-Dolphin All-Star sessions through netsim (gprb_session.py made into P+'s All-Star, as
the All-Star sync test does, docs/design/special-rules.md section 6): on the stage select both
instances write the set rule's OTHER row 2, the same roster tables, and point sqVsMelee's setup
call (0x806DCD08) at Brawl's Special setup (0x806DEE98).

    python harness/tools/allstar_session.py --preset typical --modes selfdestruct,selfdestruct \
        --p1 fox --p2 marth --stage frigate_husk [gprb_session args]
"""
import struct
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "harness"))
sys.path.insert(0, str(ROOT / "harness" / "tools"))
import gprb_session as GS  # noqa: E402
from ppharness import brawl as B  # noqa: E402

CALL, SP_SETUP = 0x806DCD08, 0x806DEE98
TABLE, TWIN = 0x80588000, 0x80588300
# (character, P+'s costume file, costume, CSS id), the first the one picked on the CSS (--p1 fox
# --p2 marth): P1 Fox, Mewtwo, Squirtle, Ivysaur, Charizard, Lucario; P2 Marth, Diddy Kong, Fox.
ROSTERS = [
    [(0x07, 0, 0, 0x07), (0x33, 0, 0, 0x2E), (0x20, 0, 0, 0x2B), (0x22, 0, 0, 0x2C), (0x1E, 0, 0, 0x2A),
     (0x24, 0, 0, 0x20)],
    [(0x15, 0, 0, 0x13), (0x1C, 0, 0, 0x1A), (0x07, 0, 0, 0x07)],
]


def write_tables(c) -> None:
    for port in range(4):
        a = bytearray(0xA0)
        b = bytearray(0x70)
        a[0x99] = 0xFF
        entries = ROSTERS[port] if port < len(ROSTERS) else []
        for k, (kind, colour, costume, css) in enumerate(entries):
            a[3 * k:3 * k + 3] = bytes((kind, colour, costume))
            b[2 * k:2 * k + 2] = bytes((css, costume))
        if entries:
            a[3 * len(entries)] = 0xCC
            b[0x68] = 2 * len(entries)
            b[0x69] = 0xAC
        c.write_mem(TABLE + 0xA0 * port, bytes(a))
        c.write_mem(TWIN + 0x70 * port, bytes(b))


orig_pick = B.sss_pick_stage


def sss_pick_stage(c, *a, **kw):
    c.write_mem(B.SET_RULE_OBJ + 0x1F, b"\x02")
    write_tables(c)
    rel = (SP_SETUP - CALL) & 0x03FFFFFC
    c.write_mem(CALL, struct.pack(">I", 0x48000001 | rel))
    print("all-star at the SSS:", c.read_mem(B.SET_RULE_OBJ + 0x18, 8).hex(), c.read_mem(CALL, 4).hex(),
          c.read_mem(TABLE, 0x10).hex(), flush=True)
    return orig_pick(c, *a, **kw)


B.sss_pick_stage = sss_pick_stage

if __name__ == "__main__":
    sys.exit(GS.main(sys.argv[1:]))
