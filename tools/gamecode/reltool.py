#!/usr/bin/env python3
"""Inspect Wii .rel modules (and main.dol): sections, imports, relocations, disassembly.

    python tools/gamecode/reltool.py info FILE.rel
    python tools/gamecode/reltool.py dis FILE.rel SECTION OFFSET [COUNT]      # needs capstone
    python tools/gamecode/reltool.py relocs FILE.rel [--to MODULE]
    python tools/gamecode/reltool.py dol main.dol ADDR [COUNT]                # disassemble the DOL
    python tools/gamecode/reltool.py find FILE.rel HEXWORDS                   # find byte pattern in .text
    python tools/gamecode/reltool.py diff A.rel B.rel                          # per-section size/equality
    python tools/gamecode/reltool.py pack LOADER.rel PLUGIN.rel OUT.rel         # PPOnline.rel (below)
    python tools/gamecode/reltool.py check FILE.rel [--elf [PART=]FILE.elf]... # unresolved calls (build.sh)

PPOnline.rel is packed: the loader REL (PPOnlineLoader, the Syriinge plugin sy_core loads into the
Syringe heap) with the plugin REL (PPOnlineMain, id 20560) appended and a 16-byte trailer
{"PPOL", offset, size, version 1}; the loader links the plugin into the Network heap
(game-code/PPOnlineLoader/source/loader.cpp). `info` and `check` look at both; `--part main`
picks the plugin for the other commands.

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


TRAILER = 0x50504F4C   # "PPOL"


def split_packed(b: bytes) -> tuple[bytes, bytes | None]:
    """(loader, plugin) of a packed PPOnline.rel; (b, None) for a plain REL."""
    if len(b) >= 16:
        magic, off, size, ver = struct.unpack_from(">4I", b, len(b) - 16)
        if magic == TRAILER and ver == 1 and off % 32 == 0 and off + size <= len(b) - 16:
            return b[:off], b[off:off + size]
    return b, None


def pack(loader: bytes, plugin: bytes) -> bytes:
    pad = (-len(loader)) % 32
    off = len(loader) + pad
    return loader + b"\0" * pad + plugin + struct.pack(">4I", TRAILER, off, len(plugin), 1)


class Rel:
    def __init__(self, path: str | Path | bytes, part: str = "loader"):
        b = path if isinstance(path, bytes) else Path(path).read_bytes()
        loader, plugin = split_packed(b)
        if part == "main" and plugin is None:
            raise SystemExit("not a packed PPOnline.rel")
        self.raw = b = plugin if part == "main" else loader
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
    _, plugin = split_packed(Path(a.file).read_bytes())
    for part in (["loader", "main"] if plugin is not None and a.part == "loader" else [a.part]):
        if plugin is not None:
            print(f"[{part}]")
        info_one(Rel(a.file, part))


def info_one(r: "Rel") -> None:
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
    r = Rel(a.file, a.part)
    s = r.sections[a.section]
    off = int(a.offset, 16)
    n = a.count * 4
    notes = reloc_notes(r, a.section, load_map(a.map))
    disasm(s.data[off:off + n], off, notes)


def cmd_relocs(a) -> None:
    r = Rel(a.file, a.part)
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
    r = Rel(a.file, a.part)
    pat = bytes.fromhex(a.hex)
    for s in r.sections:
        i = s.data.find(pat)
        while i >= 0:
            print(f"sec{s.idx}+{i:#x}")
            i = s.data.find(pat, i + 1)


def unresolved_calls(r: Rel) -> list[int]:
    """Offsets in .text of calls the linker left unresolved. We link with
    --unresolved-symbols=ignore-all, and elf2rel only warns "Unresolved external symbol" and drops
    the relocation when a symbol is neither in the module nor in the symbol maps, which leaves:
    - `bl .` (0x48000001) with no relocation: a short call, which would loop forever;
    - `lis rD,0` and then `addi rD,rD,0`, neither relocated: the -mlongcall call
      (lis/addi/mtctr/bctrl), which would jump to address 0. A real constant would be `li`;
      `lis rD,0` and then `ori` is one, and is not flagged.
    (A plain `b .` is a deliberate infinite loop; not flagged.) Pointers to such a symbol are
    left 0 too, but only the ELF shows them (dropped_refs)."""
    t = r.text()
    relocated = {x.offset for x in r.relocs if x.section == t.idx}
    words = struct.unpack_from(f">{len(t.data) // 4}I", t.data)
    out = []
    for n, w in enumerate(words):
        i = 4 * n
        if w == 0x48000001 and i not in relocated:
            out.append(i)
        elif w >> 26 == 15 and (w >> 16) & 31 == 0 and w & 0xFFFF == 0 and i + 2 not in relocated:
            rd = (w >> 21) & 31
            for k in range(n + 1, min(n + 8, len(words))):
                v = words[k]
                if v == (14 << 26 | rd << 21 | rd << 16) and 4 * k + 2 not in relocated:
                    out.append(i)
                    break
                op = v >> 26
                dest = (v >> 16) & 31 if op in (24, 25) else (v >> 21) & 31
                if op in (14, 15, 24, 25, 32, 34, 40, 42) and dest == rd:
                    break   # rD set by something else (li/lis/lwz/lbz/lhz/lha, ori/oris): a constant
    return out


# ELF32 (big-endian, relocatable), as ld.lld -r writes a module for elf2rel: just enough to find
# the relocations against undefined symbols.
SHT_SYMTAB, SHT_RELA, SHN_UNDEF, STB_WEAK = 2, 4, 0, 2


@dataclass
class ElfSym:
    name: str
    value: int
    size: int
    bind: int
    shndx: int


class Elf:
    def __init__(self, path: str | Path | bytes):
        b = path if isinstance(path, bytes) else Path(path).read_bytes()
        if b[:4] != b"\x7fELF" or b[4] != 1 or b[5] != 2:
            raise SystemExit(f"{path}: not a 32-bit big-endian ELF")
        shoff, = struct.unpack_from(">I", b, 0x20)
        shentsize, shnum, shstrndx = struct.unpack_from(">3H", b, 0x2E)
        hdrs = [struct.unpack_from(">10I", b, shoff + i * shentsize) for i in range(shnum)]

        def cstr(sec: int, off: int) -> str:
            at = hdrs[sec][4] + off
            return b[at:b.index(b"\0", at)].decode(errors="replace")

        self.sizes = [h[5] for h in hdrs]
        self.symbols: list[ElfSym] = []
        self.relocs: list[tuple[int, int, int, int]] = []   # (section, offset, type, symbol index)
        for h in hdrs:
            if h[1] == SHT_SYMTAB:
                for i in range(h[5] // 16):
                    nm, val, size, info, _other, shndx = struct.unpack_from(">3IBBH", b, h[4] + 16 * i)
                    self.symbols.append(ElfSym(cstr(h[6], nm), val, size, info >> 4, shndx))
        for h in hdrs:
            if h[1] == SHT_RELA:   # sh_info: the section it patches
                for i in range(h[5] // 12):
                    off, info, _addend = struct.unpack_from(">IIi", b, h[4] + 12 * i)
                    self.relocs.append((h[7], off, info & 0xFF, info >> 8))

    def containing(self, sec: int, off: int) -> str:
        """The defined symbol around sec+off (the function, in code), else the nearest one before."""
        best = None
        for s in self.symbols:
            if s.shndx != sec or not s.name or s.value > off:
                continue
            if off < s.value + s.size:
                return s.name
            if best is None or s.value > best.value:
                best = s
        return f"{best.name}+{off - best.value:#x}" if best else f"sec{sec}+{off:#x}"


def elf_matches(e: Elf, r: Rel) -> bool:
    """elf2rel keeps the ELF's section indices: the REL's sections have the same sizes."""
    return all(s.idx < len(e.sizes) and e.sizes[s.idx] == s.size for s in r.sections if s.size)


def dropped_refs(e: Elf, r: Rel) -> list[tuple[str, str, int, int]]:
    """(symbol, user, section, offset) of each relocation in the ELF against an undefined,
    non-weak symbol that has no relocation at the same place in the REL: elf2rel found the
    symbol neither in the module nor in the symbol maps and left the field 0 (a -mlongcall call
    then jumps to address 0, a short one to itself, and a pointer is NULL)."""
    have = {(x.section, x.offset) for x in r.relocs}
    out = []
    for sec, off, typ, si in e.relocs:
        s = e.symbols[si] if si < len(e.symbols) else None
        if typ == 0 or s is None or s.shndx != SHN_UNDEF or not s.name or s.bind == STB_WEAK:
            continue
        if (sec, off) not in have:
            out.append((s.name, e.containing(sec, off), sec, off))
    return out


def far_branches(r: Rel) -> list[Reloc]:
    """Relative branches (REL24/REL14) to another module: out of range for a module linked into
    MEM2 (the packed plugin runs from the Network heap; it is built with -mlongcall)."""
    return [x for x in r.relocs if x.type in (10, 11) and x.module != r.id]


def cmd_check(a) -> None:
    _, plugin = split_packed(Path(a.file).read_bytes())
    parts = ["loader", "main"] if plugin is not None else ["loader"]
    elfs: dict[str, str] = {}
    for spec in a.elf or []:
        part, _, path = spec.partition("=") if "=" in spec else (parts[0], "", spec)
        if part not in parts:
            raise SystemExit(f"--elf {spec}: {a.file} has no part {part!r}")
        elfs[part] = path
    for part in parts:
        r = Rel(a.file, part)
        name = f"{part} (id {r.id})" if plugin is not None else f"id {r.id}"
        problems = []
        # Instructions the ELF has a relocation at: named below if dropped, or a weak symbol's 0.
        explained: set[tuple[int, int]] = set()
        if part in elfs:
            e = Elf(elfs[part])
            if not elf_matches(e, r):
                raise SystemExit(f"{elfs[part]} is not the ELF of {name} in {a.file} (section sizes differ)")
            explained = {(sec, off & ~3) for sec, off, _t, _s in e.relocs}
            uses: dict[tuple[str, str], list[str]] = {}
            for sym, user, sec, off in dropped_refs(e, r):
                uses.setdefault((sym, user), []).append(f"sec{sec}+{off:#x}")
            for (sym, user), at in uses.items():
                problems.append(f"{name}: undefined symbol {sym}, used by {user} ({', '.join(at)})")
        t = r.text()
        for i in unresolved_calls(r):
            if (t.idx, i) not in explained:
                problems.append(f"{name}: unresolved call at sec{t.idx}+{i:#x} (--elf names the symbol)")
        for p in problems:
            print(p)
        if problems:
            raise SystemExit(f"{len(problems)} unresolved reference(s) to symbols in neither the module nor "
                             "the symbol maps (a call would go to address 0 or loop): implement them, or add "
                             "them to EXTRA.lst if the DOL has them")
        if part == "main":
            far = far_branches(r)
            for x in far[:20]:
                print(f"{name}: relative branch to module {x.module} at sec{x.section}+{x.offset:#x}")
            if far:
                raise SystemExit(f"{len(far)} relative branch(es) out of the plugin: build it with -mlongcall")
        print(f"ok: {name}: no unresolved calls" + (" or symbols" if part in elfs else "")
              + (", no far relative branches" if part == "main" else ""))


def cmd_pack(a) -> None:
    out = pack(Path(a.loader).read_bytes(), Path(a.plugin).read_bytes())
    Path(a.out).write_bytes(out)
    print(f"{a.out}: {len(out)} bytes (loader {Path(a.loader).stat().st_size}, plugin {Path(a.plugin).stat().st_size})")


def cmd_diff(a) -> None:
    x, y = Rel(a.a, a.part), Rel(a.b, a.part)
    for sa, sb in zip(x.sections, y.sections):
        if sa.size or sb.size:
            print(f"sec{sa.idx}: {sa.size:#x} vs {sb.size:#x} {'same' if sa.data == sb.data else 'DIFF'}")


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--map", help="symbol map (RSBE01.lst format)")
    ap.add_argument("--part", choices=["loader", "main"], default="loader",
                    help="of a packed PPOnline.rel: the loader (default) or the plugin")
    sub = ap.add_subparsers(dest="cmd", required=True)
    p = sub.add_parser("info"); p.add_argument("file"); p.set_defaults(func=cmd_info)
    p = sub.add_parser("dis"); p.add_argument("file"); p.add_argument("section", type=int)
    p.add_argument("offset"); p.add_argument("count", type=int, nargs="?", default=32); p.set_defaults(func=cmd_dis)
    p = sub.add_parser("relocs"); p.add_argument("file"); p.add_argument("--to", type=int); p.set_defaults(func=cmd_relocs)
    p = sub.add_parser("dol"); p.add_argument("file"); p.add_argument("addr")
    p.add_argument("count", type=int, nargs="?", default=32); p.set_defaults(func=cmd_dol)
    p = sub.add_parser("find"); p.add_argument("file"); p.add_argument("hex"); p.set_defaults(func=cmd_find)
    p = sub.add_parser("check"); p.add_argument("file")
    p.add_argument("--elf", action="append", metavar="[PART=]FILE.elf",
                   help="the ELF elf2rel made the REL (or its loader/main part) from: names each undefined "
                        "symbol and its user, and also finds pointers to it")
    p.set_defaults(func=cmd_check)
    p = sub.add_parser("diff"); p.add_argument("a"); p.add_argument("b"); p.set_defaults(func=cmd_diff)
    p = sub.add_parser("pack"); p.add_argument("loader"); p.add_argument("plugin"); p.add_argument("out")
    p.set_defaults(func=cmd_pack)
    a = ap.parse_args()
    a.func(a)
    return 0


if __name__ == "__main__":
    sys.exit(main())
