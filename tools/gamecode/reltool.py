#!/usr/bin/env python3
"""Inspect Wii .rel modules (and main.dol): sections, imports, relocations, disassembly.

    python tools/gamecode/reltool.py info FILE.rel
    python tools/gamecode/reltool.py dis FILE.rel SECTION OFFSET [COUNT]      # needs capstone
    python tools/gamecode/reltool.py relocs FILE.rel [--to MODULE]
    python tools/gamecode/reltool.py dol main.dol ADDR [COUNT]                # disassemble the DOL
    python tools/gamecode/reltool.py find FILE.rel HEXWORDS                   # find byte pattern in .text
    python tools/gamecode/reltool.py diff A.rel B.rel                          # per-section size/equality

Relocations against module 0 (the DOL) are shown symbolically when a symbol map is given
with --map (BrawlHeaders RSBE01.lst format "addr:name"), and relocated branch targets in the
disassembly are annotated from the relocation table (so `bl 0` shows the real callee).

capstone is optional (pip install --target run/scratch/gc/pylib capstone; PYTHONPATH=...).
"""

from __future__ import annotations

import argparse
import struct
import sys
from dataclasses import dataclass
from pathlib import Path

R_NAMES = {0: "NONE", 1: "ADDR32", 2: "ADDR24", 3: "ADDR16", 4: "ADDR16_LO", 5: "ADDR16_HI",
           6: "ADDR16_HA", 10: "REL24", 11: "REL14", 201: "RVL_NONE", 202: "RVL_SECT", 203: "RVL_STOP"}


@dataclass
class Section:
    idx: int
    offset: int
    size: int
    exec: bool
    data: bytes


@dataclass
class Reloc:
    module: int
    section: int        # section patched in this module
    offset: int         # offset in that section
    type: int
    target_section: int
    addend: int


class Rel:
    def __init__(self, path: str | Path):
        self.raw = b = Path(path).read_bytes()
        (self.id, _n, _p, self.nsec, self.sec_off, self.name_off, self.name_size, self.version,
         self.bss_size, self.rel_off, self.imp_off, self.imp_size) = struct.unpack_from(">12I", b, 0)
        self.prolog_sec, self.epilog_sec, self.unres_sec, self.bss_sec = b[0x30], b[0x31], b[0x32], b[0x33]
        self.prolog, self.epilog, self.unresolved = struct.unpack_from(">3I", b, 0x34)
        self.sections: list[Section] = []
        for i in range(self.nsec):
            off, size = struct.unpack_from(">II", b, self.sec_off + 8 * i)
            ex = bool(off & 1)
            off &= ~3
            self.sections.append(Section(i, off, size, ex, b[off:off + size] if off else b""))
        self.imports: list[tuple[int, int]] = []
        for i in range(self.imp_size // 8):
            self.imports.append(struct.unpack_from(">II", b, self.imp_off + 8 * i))
        self.relocs: list[Reloc] = []
        for mod, off in self.imports:
            p = off
            sec = 0
            pos = 0
            while True:
                o, t, s, add = struct.unpack_from(">HBBI", b, p)
                p += 8
                if t == 203:
                    break
                if t == 202:
                    sec = s
                    pos = 0
                    continue
                pos += o
                if t == 201:
                    continue
                self.relocs.append(Reloc(mod, sec, pos, t, s, add))

    def text(self) -> Section:
        return next(s for s in self.sections if s.exec and s.size)


def load_map(path: str | None) -> dict[int, str]:
    out: dict[int, str] = {}
    if not path:
        return out
    for line in Path(path).read_text(errors="replace").splitlines():
        line = line.strip()
        if not line or line.startswith("//") or ":" not in line:
            continue
        a, _, n = line.partition(":")
        try:
            out[int(a, 16)] = n.strip()
        except ValueError:
            pass
    return out


def disasm(code: bytes, base: int, annotate: dict[int, str] | None = None) -> None:
    try:
        import capstone  # type: ignore
    except ImportError:
        for i in range(0, len(code), 4):
            print(f"{base + i:08X}: {code[i:i + 4].hex()}")
        return
    md = capstone.Cs(capstone.CS_ARCH_PPC, capstone.CS_MODE_32 | capstone.CS_MODE_BIG_ENDIAN)
    for i in range(0, len(code), 4):
        w = code[i:i + 4]
        ins = list(md.disasm(w, base + i))
        txt = f"{ins[0].mnemonic} {ins[0].op_str}" if ins else f".word 0x{w.hex()}"
        note = (annotate or {}).get(base + i, "")
        print(f"{base + i:08X}: {w.hex()}  {txt:<40} {note}")


def cmd_info(a) -> None:
    r = Rel(a.file)
    print(f"id={r.id} version={r.version} nsec={r.nsec} bss={r.bss_size:#x}")
    print(f"prolog sec{r.prolog_sec}+{r.prolog:#x} epilog sec{r.epilog_sec}+{r.epilog:#x} "
          f"unresolved sec{r.unres_sec}+{r.unresolved:#x}")
    for s in r.sections:
        if s.size:
            print(f"  sec{s.idx}: off={s.offset:#x} size={s.size:#x} {'X' if s.exec else ''}")
    for mod, off in r.imports:
        n = sum(1 for x in r.relocs if x.module == mod)
        print(f"  import module {mod}: {n} relocs")


def reloc_notes(r: Rel, sec: int, symmap: dict[int, str]) -> dict[int, str]:
    out = {}
    for x in r.relocs:
        if x.section != sec:
            continue
        if x.module == 0:
            tgt = symmap.get(x.addend, "")
            out[x.offset] = f"; {R_NAMES.get(x.type, x.type)} -> {x.addend:#010x} {tgt}"
        else:
            out[x.offset] = f"; {R_NAMES.get(x.type, x.type)} -> mod{x.module} sec{x.target_section}+{x.addend:#x}"
    return out


def cmd_dis(a) -> None:
    r = Rel(a.file)
    s = r.sections[a.section]
    off = int(a.offset, 16)
    n = a.count * 4
    notes = reloc_notes(r, a.section, load_map(a.map))
    disasm(s.data[off:off + n], off, notes)


def cmd_relocs(a) -> None:
    r = Rel(a.file)
    symmap = load_map(a.map)
    for x in r.relocs:
        if a.to is not None and x.module != a.to:
            continue
        tgt = f"{x.addend:#010x} {symmap.get(x.addend, '')}" if x.module == 0 else \
            f"mod{x.module} sec{x.target_section}+{x.addend:#x}"
        print(f"sec{x.section}+{x.offset:#07x} {R_NAMES.get(x.type, x.type):<9} {tgt}")


def dol_read(path: str):
    b = Path(path).read_bytes()
    offs = struct.unpack_from(">18I", b, 0)
    addrs = struct.unpack_from(">18I", b, 0x48)
    sizes = struct.unpack_from(">18I", b, 0x90)
    return b, list(zip(offs, addrs, sizes))


def cmd_dol(a) -> None:
    b, secs = dol_read(a.file)
    addr = int(a.addr, 16)
    for off, ad, sz in secs:
        if ad <= addr < ad + sz:
            o = off + addr - ad
            disasm(b[o:o + a.count * 4], addr)
            return
    raise SystemExit("address not in DOL")


def cmd_find(a) -> None:
    r = Rel(a.file)
    pat = bytes.fromhex(a.hex)
    for s in r.sections:
        i = s.data.find(pat)
        while i >= 0:
            print(f"sec{s.idx}+{i:#x}")
            i = s.data.find(pat, i + 1)


def unresolved_calls(r: Rel) -> list[int]:
    """Offsets of `bl` instructions that call themselves and carry no relocation: the linker
    left an undefined symbol (we link with --unresolved-symbols=ignore-all), and the call
    would loop forever at runtime. (A plain `b .` is a deliberate infinite loop; not flagged.)"""
    t = r.text()
    relocated = {x.offset for x in r.relocs if x.section == t.idx}
    out = []
    for i in range(0, len(t.data), 4):
        w = struct.unpack_from(">I", t.data, i)[0]
        if w == 0x48000001 and i not in relocated:
            out.append(i)
    return out


def cmd_check(a) -> None:
    r = Rel(a.file)
    bad = unresolved_calls(r)
    for i in bad:
        print(f"unresolved branch at text+{i:#x}")
    if bad:
        raise SystemExit(f"{len(bad)} unresolved branch(es): add the symbols to EXTRA.lst or implement them")
    print("ok: no unresolved branches")


def cmd_diff(a) -> None:
    x, y = Rel(a.a), Rel(a.b)
    for sa, sb in zip(x.sections, y.sections):
        if sa.size or sb.size:
            print(f"sec{sa.idx}: {sa.size:#x} vs {sb.size:#x} {'same' if sa.data == sb.data else 'DIFF'}")


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--map", help="symbol map (RSBE01.lst format)")
    sub = ap.add_subparsers(dest="cmd", required=True)
    p = sub.add_parser("info"); p.add_argument("file"); p.set_defaults(func=cmd_info)
    p = sub.add_parser("dis"); p.add_argument("file"); p.add_argument("section", type=int)
    p.add_argument("offset"); p.add_argument("count", type=int, nargs="?", default=32); p.set_defaults(func=cmd_dis)
    p = sub.add_parser("relocs"); p.add_argument("file"); p.add_argument("--to", type=int); p.set_defaults(func=cmd_relocs)
    p = sub.add_parser("dol"); p.add_argument("file"); p.add_argument("addr")
    p.add_argument("count", type=int, nargs="?", default=32); p.set_defaults(func=cmd_dol)
    p = sub.add_parser("find"); p.add_argument("file"); p.add_argument("hex"); p.set_defaults(func=cmd_find)
    p = sub.add_parser("check"); p.add_argument("file"); p.set_defaults(func=cmd_check)
    p = sub.add_parser("diff"); p.add_argument("a"); p.add_argument("b"); p.set_defaults(func=cmd_diff)
    a = ap.parse_args()
    a.func(a)
    return 0


if __name__ == "__main__":
    sys.exit(main())
