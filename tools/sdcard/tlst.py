#!/usr/bin/env python3
"""Write a one-song P+ tracklist (``/Project+/pf/sound/tracklist/<Stage>.tlst``), to force a song.

Format (as P+ v3.2's own one-song ``Battlefield_64.tlst``): ``TLST``, u32 count, u16 file size,
u16 string table offset, then 16-byte entries {u32 song id (0xF000+ for SD songs), u16 0,
u8 volume, u8 frequency, u16 file name offset, u16 title offset, u32 0} and the string table.
The file name is relative to ``/Project+/pf/sound/strm/``, without ``.brstm``.

    python tools/sdcard/tlst.py "Melee/Final Destination" "FD (Melee)" bf-fd.tlst
"""

from __future__ import annotations

import struct
import sys


def one_song(path: str, title: str, song_id: int = 0xF000, volume: int = 0x50, frequency: int = 0x28) -> bytes:
    strs = path.encode() + b"\0" + title.encode() + b"\0"
    table = 12 + 16
    entry = struct.pack(">IHBBHHI", song_id, 0, volume, frequency, 0, len(path) + 1, 0)
    return b"TLST" + struct.pack(">IHH", 1, table + len(strs), table) + entry + strs


if __name__ == "__main__":
    if len(sys.argv) != 4:
        raise SystemExit(__doc__)
    with open(sys.argv[3], "wb") as f:
        f.write(one_song(sys.argv[1], sys.argv[2]))
