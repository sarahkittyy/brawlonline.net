"""Constants and small helpers shared by the client and the mock server.

The source of truth is ``docs/harness-protocol.md`` (protocol v1).
"""

from __future__ import annotations

import json
from typing import Any, Iterable, Sequence

PROTOCOL_VERSION = 1

BUTTONS: tuple[str, ...] = (
    "A", "B", "X", "Y", "Z", "L", "R", "START", "DUP", "DDOWN", "DLEFT", "DRIGHT",
)
BUTTON_SET = frozenset(BUTTONS)

STATES = ("uninitialized", "starting", "running", "paused", "stopping")

# (start, size, name) of the effective-address windows the server accepts. The
# documented windows are the cached ones (0x8/0x9); the Dolphin server also accepts the
# uncached mirrors (0xC/0xD).
MEM1 = (0x80000000, 0x01800000, "MEM1")
MEM2 = (0x90000000, 0x04000000, "MEM2")
MEM1_UNCACHED = (0xC0000000, 0x01800000, "MEM1 (uncached)")
MEM2_UNCACHED = (0xD0000000, 0x04000000, "MEM2 (uncached)")
MEM_REGIONS = (MEM1, MEM2, MEM1_UNCACHED, MEM2_UNCACHED)

MAX_MEM_LEN = 16 * 1024 * 1024
NUM_PORTS = 4
DEFAULT_WAIT_TIMEOUT_MS = 10000

# A single line may carry a 16 MiB read as hex (32 MiB) plus framing.
MAX_LINE_BYTES = 2 * MAX_MEM_LEN + 64 * 1024


def parse_addr(value: int | str) -> int:
    """Accept an int, a decimal string or a ``0x`` hex string."""
    if isinstance(value, bool):
        raise TypeError("address must be an int or a string, not bool")
    if isinstance(value, int):
        addr = value
    elif isinstance(value, str):
        text = value.strip().replace("_", "")
        addr = int(text, 0)
    else:
        raise TypeError(f"address must be an int or a string, not {type(value).__name__}")
    if addr < 0 or addr > 0xFFFFFFFF:
        raise ValueError(f"address out of 32-bit range: {value!r}")
    return addr


def region_of(addr: int, length: int = 1) -> str | None:
    """Name of the memory region fully containing [addr, addr+length), or None."""
    for start, size, name in MEM_REGIONS:
        if start <= addr and addr + length <= start + size:
            return name
    return None


def check_range(addr: int, length: int, max_len: int | None = MAX_MEM_LEN) -> None:
    """Raise ValueError if the range is not readable through the protocol.

    ``max_len`` is the per-request limit of read_mem/write_mem; hash_mem has none.
    """
    if length < 0:
        raise ValueError(f"negative length {length}")
    if max_len is not None and length > max_len:
        raise ValueError(f"length {length} exceeds the 16 MiB limit")
    if region_of(addr, max(length, 1)) is None:
        raise ValueError(
            f"range 0x{addr:08x}+0x{length:x} is not inside MEM1 "
            f"(0x80000000-0x817FFFFF) or MEM2 (0x90000000-0x93FFFFFF)"
        )


def normalize_ranges(ranges: Iterable[Sequence[int | str]]) -> list[list[int]]:
    out: list[list[int]] = []
    for item in ranges:
        if len(item) != 2:
            raise ValueError(f"range must be [addr, len], got {item!r}")
        addr = parse_addr(item[0])
        length = int(item[1])
        out.append([addr, length])
    return out


def normalize_buttons(buttons: Iterable[str] | None) -> list[str]:
    if buttons is None:
        return []
    if isinstance(buttons, str):
        buttons = [buttons]
    out = []
    for b in buttons:
        name = str(b).upper()
        if name not in BUTTON_SET:
            raise ValueError(f"unknown button {b!r}; expected one of {', '.join(BUTTONS)}")
        if name not in out:
            out.append(name)
    return out


def encode(obj: dict[str, Any]) -> bytes:
    return json.dumps(obj, separators=(",", ":")).encode("utf-8") + b"\n"


def decode(line: bytes | str) -> dict[str, Any]:
    if isinstance(line, bytes):
        line = line.decode("utf-8")
    obj = json.loads(line)
    if not isinstance(obj, dict):
        raise ValueError("message is not a JSON object")
    return obj
