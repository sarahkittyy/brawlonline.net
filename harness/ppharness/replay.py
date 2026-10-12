"""Brawl Online replay files (.brep; docs/design/replays.md §4): a reader for tests and tools.

The container: a 32-byte header (magic, major, minor, header size, game_uid), then chunks
(FourCC type, flags, stored length, raw length, CRC-32, then the stored bytes, zlib-compressed
with flag 1), then a 16-byte trailer once the game has ended. Mirrors
dolphin/Source/Core/Core/Rollback/ReplayFile.cpp; the launcher has its own reader in TypeScript.
"""

from __future__ import annotations

import json
import struct
import zlib
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any

MAGIC = b"\x89BRP\r\n\x1a\n"
FLAG_DEFLATE = 1
FLAG_CRITICAL = 2
PAD_SIZE = 0x40
PORTS = 4


@dataclass
class Replay:
    path: Path
    uid: bytes
    minor: int
    info: dict[str, Any]
    setup: dict[str, Any] = field(default_factory=dict)
    fighter_spec: dict[str, Any] = field(default_factory=dict)
    endg: dict[str, Any] | None = None
    first_frame: int = 0
    ports: int = 0
    slots: list[bytes] = field(default_factory=list)       # per frame: 4 * 0x40 pad bytes
    checksums: list[int] = field(default_factory=list)
    parts: list[tuple[int, int, int, int, int]] = field(default_factory=list)
    fighter_block: int = 0
    fighter_ports: int = 0
    fighters: list[bytes] = field(default_factory=list)    # per port: frames * block bytes
    events: list[dict[str, Any]] = field(default_factory=list)
    trailer: bool = False
    truncated: str = ""
    chunk_types: list[str] = field(default_factory=list)

    @property
    def last_frame(self) -> int:
        return self.first_frame + len(self.slots) - 1

    def pad(self, frame: int, port: int) -> bytes:
        s = self.slots[frame - self.first_frame]
        return s[port * PAD_SIZE:(port + 1) * PAD_SIZE]


def _frames(raw: bytes) -> tuple[int, int, list[bytes], list[int], list[tuple[int, ...]]]:
    first, count, ports, zero = struct.unpack_from("<IHBB", raw, 0)
    off = 8
    slots = [bytearray(PORTS * PAD_SIZE) for _ in range(count)]
    for port in range(PORTS):
        if not (ports >> port) & 1:
            for s in slots:
                s[port * PAD_SIZE + 0x38] = 0xFF
            continue
        prev = bytes(PAD_SIZE)
        for f in range(count):
            b = raw[off:off + PAD_SIZE]
            off += PAD_SIZE
            cur = b if f == 0 else bytes(x ^ y for x, y in zip(b, prev))
            slots[f][port * PAD_SIZE:(port + 1) * PAD_SIZE] = cur
            prev = cur
    sums = list(struct.unpack_from(f"<{count}I", raw, off))
    off += 4 * count
    (nparts,) = struct.unpack_from("<H", raw, off)
    off += 2
    parts = [struct.unpack_from("<5I", raw, off + 20 * i) for i in range(nparts)]
    assert off + 20 * nparts == len(raw), "bad frame chunk size"
    return first, ports, [bytes(s) for s in slots], sums, parts


def _fighters(raw: bytes) -> tuple[int, int, int, int, list[bytes]]:
    first, count, ports = struct.unpack_from("<IIB", raw, 0)
    (block,) = struct.unpack_from("<I", raw, 12)
    off = 16
    out = []
    for _ in range(bin(ports).count("1")):
        d = bytearray(raw[off:off + count * block])
        for k in range(block, len(d)):
            d[k] ^= d[k - block]
        out.append(bytes(d))
        off += count * block
    return first, count, ports, block, out


def read(path: str | Path) -> Replay:
    path = Path(path)
    data = path.read_bytes()
    if data[:8] != MAGIC:
        raise ValueError(f"{path}: not a replay")
    major, minor, header_size = struct.unpack_from("<HHI", data, 8)
    if major != 1:
        raise ValueError(f"{path}: format {major}")
    uid = data[16:32]
    end = len(data)
    trailer_endg = None
    if len(data) >= header_size + 16:
        t = data[-16:]
        magic, endg_off, last, crc = struct.unpack("<4sIII", t)
        if magic == b"BRPT" and crc == zlib.crc32(t[:12]):
            end = len(data) - 16
            trailer_endg = endg_off
    rep = Replay(path=path, uid=uid, minor=minor, info={})
    off = header_size
    endg_at = None
    fighter_frames = 0
    while off < end:
        if end - off < 20:
            rep.truncated = "torn chunk header"
            break
        typ, flags, stored_len, raw_len, crc = struct.unpack_from("<4sIIII", data, off)
        at = off
        off += 20
        stored = data[off:off + stored_len]
        off += stored_len
        if len(stored) != stored_len or zlib.crc32(stored) != crc:
            rep.truncated = f"{typ!r} at {at} damaged"
            break
        raw = zlib.decompress(stored) if flags & FLAG_DEFLATE else stored
        assert len(raw) == raw_len, (typ, len(raw), raw_len)
        name = typ.decode("ascii", "replace")
        rep.chunk_types.append(name)
        if name == "INFO":
            rep.info = json.loads(raw)
        elif name == "SETP":
            rep.setup = json.loads(raw)
        elif name == "FTSP":
            rep.fighter_spec = json.loads(raw)
        elif name == "FRMS":
            first, ports, slots, sums, parts = _frames(raw)
            if not rep.slots:
                rep.first_frame, rep.ports = first, ports
            assert first == rep.first_frame + len(rep.slots), "frames do not follow each other"
            rep.slots += slots
            rep.checksums += sums
            rep.parts += parts
        elif name == "FTRM":
            first, count, ports, block, data_ports = _fighters(raw)
            if not rep.fighters:
                rep.fighter_block, rep.fighter_ports = block, ports
                rep.fighters = [b""] * len(data_ports)
            assert first == rep.first_frame + fighter_frames
            rep.fighters = [a + b for a, b in zip(rep.fighters, data_ports)]
            fighter_frames += count
        elif name == "EVNT":
            rep.events += json.loads(raw).get("events", [])
        elif name == "ENDG":
            rep.endg = json.loads(raw)
            endg_at = at
        elif flags & FLAG_CRITICAL:
            raise ValueError(f"{path}: unknown critical chunk {name}")
    rep.trailer = trailer_endg is not None and endg_at == trailer_endg
    return rep


def fighter_block(rep: Replay, port_index: int, frame: int, name: str) -> bytes:
    """The raw bytes of the named FTSP block for a port (its index among the recorded ports)
    after `frame` ran."""
    spec = {b["name"]: b for b in rep.fighter_spec["blocks"]}[name]
    base = (frame - rep.first_frame) * rep.fighter_block + spec["offset"]
    return rep.fighters[port_index][base:base + spec["size"]]
