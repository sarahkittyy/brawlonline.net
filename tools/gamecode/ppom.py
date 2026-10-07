#!/usr/bin/env python3
"""Harness side of the PPOM game<->Dolphin channel (mirror of game-code/PPOnline/include/ppom.h).

Dolphin services the mailbox itself (Source/Core/Core/Online/GameBridge.cpp, at the frame-end
boundary). `serve` here and `drive.py mbx-serve` play Dolphin's part with `read_mem` / `write_mem`
for experiments without a server: turn Dolphin's servicing off first
(`python -m ppharness cmd --port P game_bridge_config enabled=false`).

    python tools/gamecode/ppom.py --port P find
    python tools/gamecode/ppom.py --port P dump               # header, mailbox, debug counters
    python tools/gamecode/ppom.py --port P log                 # MuMsg::printIndex call log
    python tools/gamecode/ppom.py --port P cfg 0x7             # set feature flags
    python tools/gamecode/ppom.py --port P serve [--peer-name N --peer-code C --state 4]
          answer every request once (FIND_OPPONENT -> GET_MATCH_STATE response)
"""

from __future__ import annotations

import argparse
import struct
import sys
import time
from dataclasses import dataclass
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "harness"))
from ppharness.client import HarnessClient  # noqa: E402

MAGIC = b"PPOM"
SYRINGE_HEAP = (0x817BA5A0, 0x10000)   # P+ v3.2 Syringe.asm: heap 60 start, size 0x10000

CMD = {0xB3: "GET_MATCH_STATE", 0xB4: "FIND_OPPONENT", 0xB6: "OPEN_LOGIN", 0xB8: "UPDATE",
       0xB9: "GET_ONLINE_STATUS", 0xBA: "CLEANUP_CONNECTION", 0xBE: "FETCH_CODE_SUGGESTION",
       0xE3: "GET_RANK"}
MODES = {0: "ranked", 1: "unranked", 2: "direct", 3: "teams"}

REQ_SLOTS, REQ_SIZE, REQ_PAYLOAD = 4, 0x80, 0x78
RESP_PAYLOAD = 0x1F8
CODE_LEN, NAME_LEN, ERROR_LEN = 9, 16, 120

# Mailbox offsets (relative to the mailbox start)
MB_REQ_WRITE, MB_REQ_READ, MB_RESP_COUNT, MB_RESP_SEEN = 0x0, 0x4, 0x8, 0xC
MB_REQ = 0x10
MB_RESP = MB_REQ + REQ_SLOTS * REQ_SIZE          # 0x210


@dataclass
class Block:
    addr: int
    version: int
    size: int
    mailbox: int
    mailbox_size: int
    debug: int
    debug_size: int


def u16s(s: str, n: int) -> bytes:
    s = s[: n - 1]
    return b"".join(struct.pack(">H", ord(c)) for c in s).ljust(2 * n, b"\0")


def from_u16s(b: bytes) -> str:
    out = []
    for i in range(0, len(b), 2):
        c = struct.unpack_from(">H", b, i)[0]
        if c == 0:
            break
        out.append("#" if c == 0xFF03 else chr(c))
    return "".join(out)


def find_block(c: HarnessClient) -> Block:
    start, size = SYRINGE_HEAP
    mem = c.read_mem(start, size)
    i = 0
    while True:
        i = mem.find(MAGIC, i)
        if i < 0:
            raise SystemExit("PPOM block not found in the Syringe heap (plugin not loaded?)")
        ver, sz, mbo, mbs, _so, _ss, _lo, _ls, dbo, dbs = struct.unpack_from(">HHHHHHHHHH", mem, i + 4)
        if ver == 1 and 0x100 < sz < 0x4000 and mbo and dbo:
            a = start + i
            return Block(a, ver, sz, a + mbo, mbs, a + dbo, dbs)
        i += 4


def read_requests(c: HarnessClient, b: Block):
    mb = c.read_mem(b.mailbox, MB_RESP)
    wr, rd, rc, rs = struct.unpack_from(">4I", mb, 0)
    reqs = []
    for k in range(REQ_SLOTS):
        o = MB_REQ + k * REQ_SIZE
        seq, cmd = struct.unpack_from(">IB", mb, o)
        reqs.append((seq, cmd, mb[o + 8:o + 8 + REQ_PAYLOAD]))
    return (wr, rd, rc, rs), reqs


def describe_request(cmd: int, payload: bytes) -> str:
    if cmd == 0xB4:
        mode, ch, cos, team = payload[:4]
        code = from_u16s(payload[4:4 + 2 * CODE_LEN])
        return f"FIND_OPPONENT mode={MODES.get(mode, mode)} code={code!r} char={ch:#x} costume={cos} team={team}"
    return f"{CMD.get(cmd, hex(cmd))} {payload[:16].hex()}"


def match_state_payload(mm_state: int, peer_name: str = "", peer_code: str = "", error: str = "",
                        role: int = 0, phase: int = 0, percent: int = 0) -> bytes:
    p = struct.pack(">BBBB", mm_state, role, phase, percent)
    p += u16s(peer_name, NAME_LEN) + u16s(peer_code, CODE_LEN) + b"\0\0" + u16s(error, ERROR_LEN)
    return p


def write_response(c: HarnessClient, b: Block, seq: int, cmd: int, payload: bytes, status: int = 0) -> None:
    body = struct.pack(">IBBxx", seq, cmd, status) + payload.ljust(RESP_PAYLOAD, b"\0")
    c.write_mem(b.mailbox + MB_RESP, body)
    rc = struct.unpack(">I", c.read_mem(b.mailbox + MB_RESP_COUNT, 4))[0]
    c.write_mem(b.mailbox + MB_RESP_COUNT, struct.pack(">I", rc + 1))   # publish last


def consume(c: HarnessClient, b: Block, upto: int) -> None:
    c.write_mem(b.mailbox + MB_REQ_READ, struct.pack(">I", upto))


DEBUG_FIELDS = ("frames", "printCount", "overrides", "lastError", "cfg", "menuState")


def read_debug(c: HarnessClient, b: Block) -> dict:
    d = c.read_mem(b.debug, b.debug_size)
    vals = dict(zip(DEBUG_FIELDS, struct.unpack_from(">6I", d, 0)))
    vals["scratch"] = list(struct.unpack_from(">10I", d, 0x18))
    log = []
    for k in range(32):
        lr, msg, win, line, data = struct.unpack_from(">IIHhI", d, 0x40 + 16 * k)
        log.append((lr, msg, win, line, data))
    vals["log"] = log
    return vals


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--port", type=int, required=True)
    sub = ap.add_subparsers(dest="cmd", required=True)
    sub.add_parser("find")
    sub.add_parser("dump")
    sub.add_parser("log")
    p = sub.add_parser("cfg"); p.add_argument("value")
    p = sub.add_parser("serve")
    p.add_argument("--peer-name", default="Tester")
    p.add_argument("--peer-code", default="TEST#001")
    p.add_argument("--state", type=int, default=4, help="mmState to answer (4 = CONNECTION_SUCCESS)")
    p.add_argument("--error", default="")
    p.add_argument("--timeout", type=float, default=120.0)
    p.add_argument("--once", action="store_true")
    a = ap.parse_args()

    c = HarnessClient.connect(a.port, timeout=30)
    b = find_block(c)
    if a.cmd == "find":
        print(f"PPOM v{b.version} at {b.addr:#010x} size {b.size:#x}: mailbox {b.mailbox:#010x}, debug {b.debug:#010x}")
    elif a.cmd == "dump":
        (wr, rd, rc, rs), reqs = read_requests(c, b)
        print(f"block {b.addr:#010x}  reqWrite={wr} reqRead={rd} respCount={rc} respSeen={rs}")
        for seq, cmd, pl in reqs:
            if seq:
                print(f"  req seq={seq}: {describe_request(cmd, pl)}")
        d = read_debug(c, b)
        print({k: (hex(v) if k == "cfg" else v) for k, v in d.items() if k != "log"})
    elif a.cmd == "log":
        d = read_debug(c, b)
        n = d["printCount"]
        for k in range(min(n, 32)):
            lr, msg, win, line, data = d["log"][(n - 1 - k) % 32]
            if line == -2:
                print(f"#{n - k}: printf  caller={lr:#010x} msg={msg:#010x} win={win}")
            elif line == -3:
                print(f"#{n - k}: create  caller={lr:#010x} msg={msg:#010x} windows={win} font={data}")
            else:
                print(f"#{n - k}: printIndex caller={lr:#010x} msg={msg:#010x} win={win} line={line} msbin={data:#010x}")
    elif a.cmd == "cfg":
        c.write_mem(b.debug + 0x10, struct.pack(">I", int(a.value, 0)))
    elif a.cmd == "serve":
        t0 = time.time()
        answered = 0
        while time.time() - t0 < a.timeout:
            (wr, rd, _rc, _rs), reqs = read_requests(c, b)
            for seq, cmd, pl in sorted(reqs):
                if seq and seq > rd:
                    print(f"request seq={seq}: {describe_request(cmd, pl)}")
                    if cmd == 0xB4:
                        code = from_u16s(pl[4:4 + 2 * CODE_LEN])
                        payload = match_state_payload(a.state, a.peer_name, a.peer_code or code, a.error,
                                                      role=1)
                        write_response(c, b, seq, 0xB3, payload)
                        print(f"  -> GET_MATCH_STATE mmState={a.state} peer={a.peer_name} ({a.peer_code or code})")
                    elif cmd == 0xB9:
                        payload = struct.pack(">BB", 1, 0) + u16s("Sarah", NAME_LEN) + u16s("SARA#001", CODE_LEN)
                        write_response(c, b, seq, 0xB9, payload)
                        print("  -> GET_ONLINE_STATUS ok")
                    consume(c, b, seq)
                    answered += 1
            if a.once and answered:
                break
            time.sleep(0.05)
        print(f"answered {answered} request(s)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
