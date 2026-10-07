"""Brawl ARC (.pac) reader with LZ10/LZ11 decompression (stdlib only).

Format notes ported from BrawlLib (BrawlCrate, GPL-3.0) via launcher/src/brawl_assets/formats/arc.ts.
"""
from __future__ import annotations
import struct


def lz_decompress(b: bytes) -> bytes:
    t = b[0]
    n = b[1] | b[2] << 8 | b[3] << 16
    p = 4
    if n == 0:
        n = struct.unpack_from("<I", b, 4)[0]
        p = 8
    out = bytearray()
    while len(out) < n:
        flags = b[p]; p += 1
        for _ in range(8):
            if len(out) >= n:
                break
            if flags & 0x80:
                if t == 0x10:
                    x = b[p] << 8 | b[p + 1]; p += 2
                    ln, disp = (x >> 12) + 3, (x & 0xFFF) + 1
                else:  # LZ11
                    ind = b[p] >> 4
                    if ind == 0:
                        ln = ((b[p] & 0xF) << 4 | b[p + 1] >> 4) + 0x11
                        disp = ((b[p + 1] & 0xF) << 8 | b[p + 2]) + 1; p += 3
                    elif ind == 1:
                        ln = ((b[p] & 0xF) << 12 | b[p + 1] << 4 | b[p + 2] >> 4) + 0x111
                        disp = ((b[p + 2] & 0xF) << 8 | b[p + 3]) + 1; p += 4
                    else:
                        ln = ind + 1
                        disp = ((b[p] & 0xF) << 8 | b[p + 1]) + 1; p += 2
                for _ in range(ln):
                    out.append(out[-disp])
            else:
                out.append(b[p]); p += 1
            flags = (flags << 1) & 0xFF
    return bytes(out)


def maybe_decompress(b: bytes) -> bytes:
    if b[:4] == b"ARC\0" or len(b) < 8:
        return b
    if b[0] in (0x10, 0x11):
        try:
            d = lz_decompress(b)
            return d
        except Exception:
            return b
    return b


def parse_arc(b: bytes):
    """Yield (index, type, fileIndex, groupIndex, data) for each entry."""
    b = maybe_decompress(b)
    if b[:4] != b"ARC\0":
        raise ValueError("not an ARC")
    n = struct.unpack_from(">H", b, 6)[0]
    off = 0x40
    for i in range(n):
        t, idx, sz = struct.unpack_from(">hhi", b, off)
        g = b[off + 8]
        d = b[off + 0x20:off + 0x20 + sz]
        yield i, t, idx, g, d
        off = (off + 0x20 + sz + 0x1F) & ~0x1F
