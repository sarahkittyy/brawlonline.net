"""tools/gamecode/reltool.py `check`: calls to symbols that are neither in the module nor in the
symbol maps fail the build (game-code/build.sh).

elf2rel only warns ("Unresolved external symbol") and drops the relocation of such a symbol, so
with -mlongcall the call becomes `lis r3,0; addi r3,r3,0; mtctr r3; bctrl` and jumps to address 0
(2026-10-11: a memcmp call in the room rules crashed the game as the room CSS loaded, and the old
check, which only knew the short call's `bl .`, passed it). Built from synthetic REL and ELF bytes,
so it needs no toolchain.
"""

from __future__ import annotations

import struct
import subprocess
import sys
from pathlib import Path

from ppharness import paths

ROOT = paths.workspace_root()
sys.path.insert(0, str(ROOT / "tools" / "gamecode"))

import reltool  # noqa: E402

LONGCALL = [0x3C600000, 0x38630000, 0x7C6903A6, 0x4E800421]   # lis r3,0; addi r3,r3,0; mtctr; bctrl
CONSTANT = [0x3C800000, 0x6084FFFF]                             # lis r4,0; ori r4,r4,0xffff (0x0000ffff)
BL_SELF = [0x48000001]                                          # bl .
BLR = [0x4E800020]

# .text: the user of the symbols at 0, then one more function.
#   0x00 memcmp (no relocation in the REL: dropped)   0x10 the constant
#   0x18 strlen (relocated against the DOL)           0x28 blr
#   0x2c other: the address of weak_hook (weak, dropped as it should be), blr
TEXT_WORDS = LONGCALL + CONSTANT + LONGCALL + BLR + [0x3C600000, 0x38630000] + BLR
USER_SIZE = 0x2C
STRLEN = 0x803F0640


def make_rel(text_words: list[int], dol_relocs: list[tuple[int, int, int]], rel_id: int = 20560) -> bytes:
    """A REL with one executable section (1) and relocations (offset, type, addend) against
    module 0 in it."""
    text = struct.pack(f">{len(text_words)}I", *text_words)
    hdr_size, sec_off = 0x4C, 0x4C
    nsec = 2
    text_off = sec_off + 8 * nsec
    imp_off = text_off + len(text)
    rel_off = imp_off + 8
    relocs = struct.pack(">HBBI", 0, 202, 1, 0)   # RVL_SECT 1
    pos = 0
    for off, typ, addend in sorted(dol_relocs):
        relocs += struct.pack(">HBBI", off - pos, typ, 0, addend)
        pos = off
    relocs += struct.pack(">HBBI", 0, 203, 0, 0)
    hdr = struct.pack(">12I", rel_id, 0, 0, nsec, sec_off, 0, 0, 3, 0, rel_off, imp_off, 8)
    hdr += bytes(4) + struct.pack(">3I", 0, 0, 0) + struct.pack(">3I", 4, 4, 0)
    assert len(hdr) == hdr_size
    secs = struct.pack(">II", 0, 0) + struct.pack(">II", text_off | 1, len(text))
    return hdr + secs + text + struct.pack(">II", 0, rel_off) + relocs


def make_elf(text_size: int, symbols: list[tuple[str, int, int, int, int]],
             relocs: list[tuple[int, int, int]]) -> bytes:
    """A relocatable ELF32 BE: [null, .text, .rela.text, .symtab, .strtab, .shstrtab].
    symbols: (name, value, size, bind, shndx) after the null symbol; relocs: (offset, type, symbol)."""
    strtab = b"\0"
    syms = bytes(16)
    for name, value, size, bind, shndx in symbols:
        syms += struct.pack(">3IBBH", len(strtab), value, size, bind << 4 | (2 if shndx else 0), 0, shndx)
        strtab += name.encode() + b"\0"
    rela = b"".join(struct.pack(">IIi", off, sym << 8 | typ, 0) for off, typ, sym in relocs)
    names = [b"", b".text", b".rela.text", b".symtab", b".strtab", b".shstrtab"]
    shstr = b"\0" + b"".join(n + b"\0" for n in names[1:])
    name_off = [0] + [shstr.index(n + b"\0") for n in names[1:]]
    body_off = 0x34
    blobs = [b"", bytes(text_size), rela, syms, strtab, shstr]
    offs, pos = [], body_off
    for blob in blobs:
        offs.append(pos)
        pos += len(blob)
    shoff = (pos + 3) & ~3
    # (type, link, info, entsize) per section
    meta = [(0, 0, 0, 0), (1, 0, 0, 0), (4, 3, 1, 12), (2, 4, 1, 16), (3, 0, 0, 0), (3, 0, 0, 0)]
    shdrs = b"".join(struct.pack(">10I", name_off[i], t, 0, 0, offs[i] if i else 0, len(blobs[i]), link, info, 4, ent)
                     for i, (t, link, info, ent) in enumerate(meta))
    ehdr = b"\x7fELF" + bytes([1, 2, 1]) + bytes(9)
    ehdr += struct.pack(">HHIIIIIHHHHHH", 1, 20, 1, 0, 0, shoff, 0, 0x34, 0, 0, 40, len(meta), 5)
    out = ehdr + b"".join(blobs)
    return out + bytes(shoff - len(out)) + shdrs


def fixture(*, memcmp_resolved: bool = False) -> tuple[bytes, bytes]:
    dol = [(0x1A, 6, STRLEN), (0x1E, 4, STRLEN)]
    if memcmp_resolved:
        dol += [(0x02, 6, 0x803F0000), (0x06, 4, 0x803F0000)]
    rel = make_rel(TEXT_WORDS, dol)
    symbols = [("msbinLineContains__6OnlineFPCviPCc", 0, USER_SIZE, 1, 1),
               ("other", USER_SIZE, 0x0C, 1, 1),
               ("memcmp", 0, 0, 1, 0), ("strlen", 0, 0, 1, 0), ("weak_hook", 0, 0, 2, 0)]
    relocs = [(0x02, 6, 3), (0x06, 4, 3), (0x1A, 6, 4), (0x1E, 4, 4), (0x2E, 6, 5), (0x32, 4, 5)]
    return rel, make_elf(len(TEXT_WORDS) * 4, symbols, relocs)


def test_rel_alone_finds_the_longcall_left_at_0_and_bl_self():
    rel, _ = fixture()
    # The weak symbol's lis/addi at 0x2c is the same pattern; only the ELF tells them apart.
    assert reltool.unresolved_calls(reltool.Rel(rel)) == [0x00, 0x2C]
    words = LONGCALL + CONSTANT + BL_SELF + BLR
    assert reltool.unresolved_calls(reltool.Rel(make_rel(words, []))) == [0x00, 0x18]
    # Relocated, a constant, or a register set again in between: not flagged.
    assert reltool.unresolved_calls(reltool.Rel(make_rel(LONGCALL, [(0x02, 6, STRLEN), (0x06, 4, STRLEN)]))) == []
    assert reltool.unresolved_calls(reltool.Rel(make_rel(CONSTANT + BLR, []))) == []
    li_between = [0x3C600000, 0x38600005, 0x38630000] + BLR   # lis r3,0; li r3,5; addi r3,r3,0
    assert reltool.unresolved_calls(reltool.Rel(make_rel(li_between, []))) == []


def test_elf_names_the_symbol_and_its_user():
    rel, elf = fixture()
    r, e = reltool.Rel(rel), reltool.Elf(elf)
    assert reltool.elf_matches(e, r)
    assert reltool.dropped_refs(e, r) == [("memcmp", "msbinLineContains__6OnlineFPCviPCc", 1, 0x02),
                                          ("memcmp", "msbinLineContains__6OnlineFPCviPCc", 1, 0x06)]
    rel_ok, _ = fixture(memcmp_resolved=True)
    assert reltool.dropped_refs(e, reltool.Rel(rel_ok)) == []


def _check(tmp_path: Path, rel: bytes, elf: bytes | None) -> subprocess.CompletedProcess:
    (tmp_path / "x.rel").write_bytes(rel)
    args = [sys.executable, str(ROOT / "tools" / "gamecode" / "reltool.py"), "check", str(tmp_path / "x.rel")]
    if elf is not None:
        (tmp_path / "x.elf").write_bytes(elf)
        args += ["--elf", str(tmp_path / "x.elf")]
    return subprocess.run(args, capture_output=True, text=True, timeout=60)


def test_check_fails_the_build_and_says_why(tmp_path):
    rel, elf = fixture()
    p = _check(tmp_path, rel, elf)
    assert p.returncode != 0
    assert "undefined symbol memcmp, used by msbinLineContains__6OnlineFPCviPCc (sec1+0x2, sec1+0x6)" in p.stdout
    assert "1 unresolved reference(s)" in p.stderr
    p = _check(tmp_path, rel, None)
    assert p.returncode != 0
    assert "unresolved call at sec1+0x0" in p.stdout


def test_check_passes_a_clean_module_and_refuses_a_stale_elf(tmp_path):
    rel_ok, elf = fixture(memcmp_resolved=True)
    p = _check(tmp_path, rel_ok, elf)
    assert p.returncode == 0, p.stdout + p.stderr
    assert "no unresolved calls or symbols" in p.stdout
    stale = make_elf(len(TEXT_WORDS) * 4 + 4, [], [])
    p = _check(tmp_path, rel_ok, stale)
    assert p.returncode != 0 and "section sizes differ" in p.stderr
