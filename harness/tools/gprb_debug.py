"""Debug helpers for the gameplay rollback tools: symbol names from the Brawl decomp's
symbols.txt and backtraces of sleeping OS threads (from their saved OSContext)."""

from __future__ import annotations

import bisect
import re
import struct
from functools import lru_cache
from pathlib import Path
from typing import List

SYMBOLS = Path(__file__).resolve().parents[2] / "refs" / "brawl-decomp" / "config" / "RSBE01_02" / "symbols.txt"


@lru_cache(maxsize=1)
def _symbols():
    syms = []
    if SYMBOLS.exists():
        for line in SYMBOLS.read_text(errors="replace").splitlines():
            m = re.match(r"(\S+) = \.text:0x([0-9A-F]+); // type:function size:0x([0-9A-F]+)", line)
            if m:
                syms.append((int(m.group(2), 16), int(m.group(3), 16), m.group(1)))
    syms.sort()
    return syms, [s[0] for s in syms]


def sym(addr: int) -> str:
    syms, starts = _symbols()
    i = bisect.bisect_right(starts, addr) - 1
    if i >= 0 and syms[i][0] <= addr < syms[i][0] + syms[i][1]:
        return f"{syms[i][2]}+{addr - syms[i][0]:#x}"
    return "?"


def thread_backtrace(c, thread: int, depth: int = 20) -> List[str]:
    """OSThread at `thread`: its saved srr0/lr and the stack's return addresses (DOL symbols only)."""
    def u(a: int) -> int:
        return struct.unpack(">I", c.read_mem(a, 4))[0]
    sp, lr, srr0 = u(thread + 4), u(thread + 0x84), u(thread + 0x198)
    out = [f"srr0 {srr0:08x} {sym(srr0)}", f"lr {lr:08x} {sym(lr)}"]
    for _ in range(depth):
        if not 0x80000000 <= sp < 0x94000000:
            break
        nxt = u(sp)
        if not 0x80000000 <= nxt < 0x94000000:
            break
        ra = u(nxt + 4)
        out.append(f"{ra:08x} {sym(ra)}")
        sp = nxt
    return out
