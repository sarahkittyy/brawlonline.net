"""Small FAT32 reader/writer for disk images such as Dolphin's virtual SD card (``sd.raw``).

Standard library only. Supports partitionless ("superfloppy") images, which is what Dolphin
creates, and MBR images with one FAT32 partition. Long file names (VFAT) are read and written.

Writing covers what the SD patcher needs: create directories, add or replace files, delete
files. Both FAT copies are updated, and the FSInfo free-cluster hint is kept accurate.

The read side mirrors ``launcher/src/brawl_assets/fat32.ts`` (the launcher's read-only reader).
"""

from __future__ import annotations

import os
import struct
from dataclasses import dataclass, field
from typing import BinaryIO, Iterator

ATTR_RO, ATTR_HIDDEN, ATTR_SYSTEM, ATTR_VOLUME, ATTR_DIR, ATTR_ARCHIVE = 1, 2, 4, 8, 0x10, 0x20
ATTR_LFN = 0x0F
EOC = 0x0FFFFFFF
FREE = 0


class FatError(Exception):
    pass


@dataclass
class DirEntry:
    name: str                 # long name if present, else the 8.3 name
    short_name: str           # "NAME.EXT"
    attr: int
    first_cluster: int
    size: int
    # where the 8.3 record lives (absolute byte offset in the image), and the offsets of all
    # 32-byte slots that belong to this entry (LFN slots + the 8.3 slot)
    slot_offset: int = 0
    slot_offsets: list[int] = field(default_factory=list)

    @property
    def is_dir(self) -> bool:
        return bool(self.attr & ATTR_DIR)


def _lfn_checksum(short11: bytes) -> int:
    s = 0
    for b in short11:
        s = (((s & 1) << 7) + (s >> 1) + b) & 0xFF
    return s


_SHORT_OK = set(b"ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789$%'-_@~`!(){}^#&")


class Fat32:
    def __init__(self, f: BinaryIO, writable: bool = False):
        self.f = f
        self.writable = writable
        self.base = self._find_volume()
        bs = self._read(self.base, 512)
        (self.bps,) = struct.unpack_from("<H", bs, 0x0B)
        self.spc = bs[0x0D]
        (self.reserved,) = struct.unpack_from("<H", bs, 0x0E)
        self.nfats = bs[0x10]
        (tot16,) = struct.unpack_from("<H", bs, 0x13)
        (tot32,) = struct.unpack_from("<I", bs, 0x20)
        (fatsz16,) = struct.unpack_from("<H", bs, 0x16)
        (fatsz32,) = struct.unpack_from("<I", bs, 0x24)
        if fatsz16 != 0 or fatsz32 == 0:
            raise FatError("not a FAT32 volume")
        self.total_sectors = tot32 or tot16
        self.fat_sectors = fatsz32
        (self.root_cluster,) = struct.unpack_from("<I", bs, 0x2C)
        (self.fsinfo_sector,) = struct.unpack_from("<H", bs, 0x30)
        self.cluster_size = self.bps * self.spc
        self.fat_offset = self.base + self.reserved * self.bps
        self.data_offset = self.fat_offset + self.nfats * self.fat_sectors * self.bps
        data_sectors = self.total_sectors - (self.reserved + self.nfats * self.fat_sectors)
        self.cluster_count = data_sectors // self.spc
        fat_bytes = self._read(self.fat_offset, self.fat_sectors * self.bps)
        n = min(self.cluster_count + 2, len(fat_bytes) // 4)
        self.fat = list(struct.unpack_from(f"<{n}I", fat_bytes, 0))
        self._dirty_fat: set[int] = set()

    # ------------------------------------------------------------------ low level

    def _read(self, off: int, n: int) -> bytes:
        self.f.seek(off)
        b = self.f.read(n)
        if len(b) != n:
            raise FatError(f"short read at {off:#x}")
        return b

    def _write(self, off: int, data: bytes) -> None:
        if not self.writable:
            raise FatError("image opened read-only")
        self.f.seek(off)
        self.f.write(data)

    def _find_volume(self) -> int:
        s0 = self._read(0, 512)
        if s0[0x52:0x5A] == b"FAT32   " and s0[0x1FE:0x200] == b"\x55\xaa":
            return 0
        if s0[0x1FE:0x200] == b"\x55\xaa":
            for i in range(4):
                e = s0[0x1BE + 16 * i: 0x1CE + 16 * i]
                if e[4] in (0x0B, 0x0C):
                    (lba,) = struct.unpack_from("<I", e, 8)
                    return lba * 512
        raise FatError("no FAT32 volume found")

    def cluster_offset(self, c: int) -> int:
        return self.data_offset + (c - 2) * self.cluster_size

    def chain(self, first: int) -> list[int]:
        out = []
        c = first
        seen = set()
        while 2 <= c < 0x0FFFFFF8:
            if c in seen or c >= len(self.fat):
                raise FatError(f"bad cluster chain at {c:#x}")
            seen.add(c)
            out.append(c)
            c = self.fat[c] & 0x0FFFFFFF
        return out

    def read_chain(self, first: int, size: int | None = None) -> bytes:
        parts = [self._read(self.cluster_offset(c), self.cluster_size) for c in self.chain(first)]
        data = b"".join(parts)
        return data if size is None else data[:size]

    # ------------------------------------------------------------------ directories

    def _dir_slots(self, first: int) -> Iterator[tuple[int, bytes]]:
        for c in self.chain(first):
            base = self.cluster_offset(c)
            buf = self._read(base, self.cluster_size)
            for i in range(0, self.cluster_size, 32):
                yield base + i, buf[i:i + 32]

    def list_dir(self, first: int) -> list[DirEntry]:
        out: list[DirEntry] = []
        lfn_parts: dict[int, str] = {}
        lfn_offs: list[int] = []
        lfn_sum = None
        for off, e in self._dir_slots(first):
            if e[0] == 0:
                break
            if e[0] == 0xE5:
                lfn_parts, lfn_offs, lfn_sum = {}, [], None
                continue
            attr = e[11]
            if attr == ATTR_LFN:
                seq = e[0] & 0x1F
                if e[0] & 0x40:
                    lfn_parts, lfn_offs = {}, []
                    lfn_sum = e[13]
                raw = e[1:11] + e[14:26] + e[28:32]
                s = raw.decode("utf-16-le", errors="replace")
                s = s.split("\x00", 1)[0].replace("￿", "")
                lfn_parts[seq] = s
                lfn_offs.append(off)
                continue
            if attr & ATTR_VOLUME:
                lfn_parts, lfn_offs, lfn_sum = {}, [], None
                continue
            nm = e[0:8].decode("latin-1").rstrip()
            ext = e[8:11].decode("latin-1").rstrip()
            if e[0] == 0x05:
                nm = "\xe5" + nm[1:]
            short = nm + ("." + ext if ext else "")
            # NT lowercase flags
            if e[12] & 0x08:
                nm = nm.lower()
            if e[12] & 0x10:
                ext = ext.lower()
            disp = nm + ("." + ext if ext else "")
            long = None
            if lfn_parts and lfn_sum == _lfn_checksum(e[0:11]):
                long = "".join(lfn_parts[k] for k in sorted(lfn_parts))
            hi, = struct.unpack_from("<H", e, 20)
            lo, = struct.unpack_from("<H", e, 26)
            size, = struct.unpack_from("<I", e, 28)
            ent = DirEntry(long or disp, short, attr, (hi << 16) | lo, size, off,
                           (lfn_offs if long else []) + [off])
            lfn_parts, lfn_offs, lfn_sum = {}, [], None
            if short in (".", ".."):
                continue
            out.append(ent)
        return out

    def lookup(self, path: str) -> DirEntry | None:
        parts = [p for p in path.replace("\\", "/").split("/") if p]
        cur = DirEntry("/", "/", ATTR_DIR, self.root_cluster, 0)
        for p in parts:
            if not cur.is_dir:
                return None
            hit = None
            for e in self.list_dir(cur.first_cluster):
                if e.name.lower() == p.lower() or e.short_name.lower() == p.lower():
                    hit = e
                    break
            if hit is None:
                return None
            cur = hit
        return cur

    def read_file(self, path: str) -> bytes:
        e = self.lookup(path)
        if e is None or e.is_dir:
            raise FileNotFoundError(path)
        if e.size == 0:
            return b""
        return self.read_chain(e.first_cluster, e.size)

    def walk(self, path: str = "/") -> Iterator[tuple[str, DirEntry]]:
        e = self.lookup(path)
        if e is None or not e.is_dir:
            raise FileNotFoundError(path)
        stack = [(path.rstrip("/"), e.first_cluster)]
        while stack:
            p, c = stack.pop()
            for ent in self.list_dir(c):
                full = f"{p}/{ent.name}"
                yield full, ent
                if ent.is_dir:
                    stack.append((full, ent.first_cluster))

    # ------------------------------------------------------------------ allocation

    def _set_fat(self, c: int, v: int) -> None:
        self.fat[c] = (self.fat[c] & 0xF0000000) | (v & 0x0FFFFFFF)
        self._dirty_fat.add(c)

    def _alloc(self, n: int) -> list[int]:
        out = []
        c = 2
        limit = min(len(self.fat), self.cluster_count + 2)
        while len(out) < n:
            while c < limit and (self.fat[c] & 0x0FFFFFFF) != FREE:
                c += 1
            if c >= limit:
                raise FatError("SD image is full")
            out.append(c)
            c += 1
        for a, b in zip(out, out[1:]):
            self._set_fat(a, b)
        self._set_fat(out[-1], EOC)
        return out

    def _free_chain(self, first: int) -> None:
        if first < 2:
            return
        for c in self.chain(first):
            self._set_fat(c, FREE)

    def _extend(self, last: int, n: int) -> list[int]:
        new = self._alloc(n)
        self._set_fat(last, new[0])
        return new

    def flush(self) -> None:
        if not self._dirty_fat:
            return
        # write dirty sectors of every FAT copy
        per_sector = self.bps // 4
        sectors = sorted({c // per_sector for c in self._dirty_fat})
        for s in sectors:
            lo = s * per_sector
            vals = self.fat[lo:lo + per_sector]
            vals += [0] * (per_sector - len(vals))
            data = struct.pack(f"<{per_sector}I", *vals)
            for k in range(self.nfats):
                self._write(self.fat_offset + k * self.fat_sectors * self.bps + s * self.bps, data)
        self._dirty_fat.clear()
        # FSInfo: exact free count, next-free hint
        if self.fsinfo_sector:
            off = self.base + self.fsinfo_sector * self.bps
            fs = bytearray(self._read(off, 512))
            if fs[0:4] == b"RRaA" and fs[0x1E4:0x1E8] == b"rrAa":
                limit = min(len(self.fat), self.cluster_count + 2)
                free = sum(1 for c in range(2, limit) if (self.fat[c] & 0x0FFFFFFF) == FREE)
                nxt = next((c for c in range(2, limit) if (self.fat[c] & 0x0FFFFFFF) == FREE),
                           0xFFFFFFFF)
                struct.pack_into("<II", fs, 0x1E8, free, nxt)
                self._write(off, bytes(fs))
        self.f.flush()

    # ------------------------------------------------------------------ writing

    def _write_chain(self, clusters: list[int], data: bytes) -> None:
        cs = self.cluster_size
        for i, c in enumerate(clusters):
            chunk = data[i * cs:(i + 1) * cs]
            if len(chunk) < cs:
                chunk += b"\x00" * (cs - len(chunk))
            self._write(self.cluster_offset(c), chunk)

    def _short_name(self, dir_cluster: int, name: str) -> tuple[bytes, bool]:
        """Return an 11-byte 8.3 name, and whether an LFN is needed."""
        existing = {e.short_name.upper() for e in self.list_dir(dir_cluster)}
        base, _, ext = name.rpartition(".") if "." in name[1:] else (name, "", "")
        if not base:
            base, ext = name, ""

        def clean(s: str) -> str:
            return "".join(ch for ch in s.upper() if ord(ch) < 128 and ord(ch) in _SHORT_OK)

        exact = (len(base) <= 8 and len(ext) <= 3 and clean(base) == base and clean(ext) == ext)
        if exact:
            sn = base.ljust(8) + ext.ljust(3)
            if (base + ("." + ext if ext else "")) not in existing:
                return sn.encode("ascii"), False
        cb, ce = clean(base) or "FILE", clean(ext)[:3]
        for i in range(1, 100000):
            tail = f"~{i}"
            stem = cb[:8 - len(tail)] + tail
            cand = stem + ("." + ce if ce else "")
            if cand not in existing:
                return (stem.ljust(8) + ce.ljust(3)).encode("ascii"), True
        raise FatError("no free short name")

    def _free_slots(self, dir_cluster: int, need: int) -> list[int]:
        """Find `need` consecutive free 32-byte slots in a directory, growing it if needed."""
        run: list[int] = []
        last_cluster = self.chain(dir_cluster)[-1]
        for off, e in self._dir_slots(dir_cluster):
            if e[0] in (0x00, 0xE5):
                run.append(off)
                if len(run) == need:
                    return run
            else:
                run = []
        # grow by enough clusters, zero-filled (0x00 = end marker)
        per = self.cluster_size // 32
        n = (need - len(run) + per - 1) // per
        new = self._extend(last_cluster, n)
        self._write_chain(new, b"")
        for c in new:
            base = self.cluster_offset(c)
            for i in range(0, self.cluster_size, 32):
                run.append(base + i)
                if len(run) == need:
                    return run
        return run

    def _add_entry(self, dir_cluster: int, name: str, attr: int, first: int, size: int) -> None:
        short, need_lfn = self._short_name(dir_cluster, name)
        slots_lfn: list[bytes] = []
        if need_lfn:
            u = name.encode("utf-16-le")
            chars = [u[i:i + 2] for i in range(0, len(u), 2)]
            if len(chars) % 13:          # a name of exactly 13*k units has no terminator
                chars.append(b"\x00\x00")
                while len(chars) % 13:
                    chars.append(b"\xff\xff")
            pieces = [chars[i:i + 13] for i in range(0, len(chars), 13)]
            cs = _lfn_checksum(short)
            n = len(pieces)
            for idx in range(n, 0, -1):
                p = b"".join(pieces[idx - 1])
                seq = idx | (0x40 if idx == n else 0)
                e = bytes([seq]) + p[0:10] + bytes([ATTR_LFN, 0, cs]) + p[10:22] + b"\x00\x00" + p[22:26]
                slots_lfn.append(e)
        # time/date: 2026-01-01 00:00:00
        date = ((2026 - 1980) << 9) | (1 << 5) | 1
        e = bytearray(32)
        e[0:11] = short
        e[11] = attr
        struct.pack_into("<HHHH", e, 14, 0, date, date, (first >> 16) & 0xFFFF)
        struct.pack_into("<HHHI", e, 22, 0, date, first & 0xFFFF, size)
        slots = self._free_slots(dir_cluster, len(slots_lfn) + 1)
        for off, data in zip(slots, slots_lfn + [bytes(e)]):
            self._write(off, data)

    def _parent_of(self, path: str) -> tuple[int, str]:
        parts = [p for p in path.replace("\\", "/").split("/") if p]
        if not parts:
            raise FatError("empty path")
        parent = "/" + "/".join(parts[:-1])
        pe = self.lookup(parent)
        if pe is None or not pe.is_dir:
            raise FileNotFoundError(parent)
        return pe.first_cluster, parts[-1]

    def mkdir(self, path: str, parents: bool = True) -> int:
        e = self.lookup(path)
        if e is not None:
            if not e.is_dir:
                raise FatError(f"{path} exists and is a file")
            return e.first_cluster
        parts = [p for p in path.replace("\\", "/").split("/") if p]
        if parents and len(parts) > 1:
            self.mkdir("/" + "/".join(parts[:-1]), parents=True)
        parent_cluster, name = self._parent_of(path)
        (c,) = self._alloc(1)
        date = ((2026 - 1980) << 9) | (1 << 5) | 1
        buf = bytearray(self.cluster_size)
        for i, (nm, cl) in enumerate(((b".          ", c),
                                      (b"..         ", 0 if parent_cluster == self.root_cluster
                                       else parent_cluster))):
            o = i * 32
            buf[o:o + 11] = nm
            buf[o + 11] = ATTR_DIR
            struct.pack_into("<HHHH", buf, o + 14, 0, date, date, (cl >> 16) & 0xFFFF)
            struct.pack_into("<HHHI", buf, o + 22, 0, date, cl & 0xFFFF, 0)
        self._write(self.cluster_offset(c), bytes(buf))
        self._add_entry(parent_cluster, name, ATTR_DIR, c, 0)
        return c

    def delete(self, path: str) -> bool:
        e = self.lookup(path)
        if e is None:
            return False
        if e.is_dir:
            raise FatError("delete of directories is not supported")
        self._free_chain(e.first_cluster)
        for off in e.slot_offsets:
            self._write(off, b"\xe5")
        return True

    def write_file(self, path: str, data: bytes, mkdirs: bool = True) -> None:
        parts = [p for p in path.replace("\\", "/").split("/") if p]
        if mkdirs and len(parts) > 1:
            self.mkdir("/" + "/".join(parts[:-1]))
        self.delete(path)
        parent_cluster, name = self._parent_of(path)
        first = 0
        if data:
            n = (len(data) + self.cluster_size - 1) // self.cluster_size
            clusters = self._alloc(n)
            self._write_chain(clusters, data)
            first = clusters[0]
        self._add_entry(parent_cluster, name, ATTR_ARCHIVE, first, len(data))

    # ------------------------------------------------------------------ checks

    def check(self) -> list[str]:
        """Consistency check: every reachable chain is valid and no cluster is shared.
        Returns a list of problems (empty = clean)."""
        problems = []
        owner: dict[int, str] = {}
        for c in self.chain(self.root_cluster):
            owner[c] = "/"
        for path, e in self.walk("/"):
            if e.first_cluster < 2:
                continue
            try:
                ch = self.chain(e.first_cluster)
            except FatError as ex:
                problems.append(f"{path}: {ex}")
                continue
            if not e.is_dir:
                need = (e.size + self.cluster_size - 1) // self.cluster_size
                if len(ch) != need:
                    problems.append(f"{path}: chain {len(ch)} clusters, size needs {need}")
            for c in ch:
                if c in owner:
                    problems.append(f"{path}: cluster {c:#x} shared with {owner[c]}")
                owner[c] = path
        limit = min(len(self.fat), self.cluster_count + 2)
        used = sum(1 for c in range(2, limit) if (self.fat[c] & 0x0FFFFFFF) != FREE)
        if used != len(owner):
            problems.append(f"{used - len(owner)} allocated clusters are not reachable (lost chains)")
        return problems


def open_image(path: str | os.PathLike, writable: bool = False) -> Fat32:
    f = open(path, "r+b" if writable else "rb")
    return Fat32(f, writable)
