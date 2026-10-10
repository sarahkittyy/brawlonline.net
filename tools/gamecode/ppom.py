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
          [--codes ABCD#123,EFGH#456]
          answer every request once (FIND_OPPONENT -> GET_MATCH_STATE response;
          FETCH_CODE_SUGGESTION from --codes, newest first, with Dolphin's search)
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
       0xE3: "GET_RANK", 0xD0: "ROOM"}
MODES = {0: "ranked", 1: "unranked", 2: "direct", 3: "teams"}

SCROLL = {0: "none", 1: "older", 2: "newer", 3: "reset"}   # Slippi AutoComplete.s

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
    session: int = 0
    session_size: int = 0
    local: int = 0
    local_size: int = 0


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


def _parse_block(c: HarnessClient, a: int):
    hdr = c.read_mem(a, 0x24)
    if hdr[:4] != MAGIC:
        return None
    ver, sz, mbo, mbs, so, ss, lo, ls, dbo, dbs = struct.unpack_from(">HHHHHHHHHH", hdr, 4)
    if ver in (1, 2, 3, 4, 5) and 0x100 < sz < 0x4000 and mbo and dbo:
        return Block(a, ver, sz, a + mbo, mbs, a + dbo, dbs, a + so if ss else 0, ss,
                     a + lo if ls else 0, ls)
    return None


def find_block(c: HarnessClient) -> Block:
    # Where Dolphin's GameBridge found it (the plugin may live outside the Syringe heap, e.g. a
    # loader REL linking it into MEM2), else a scan of the Syringe heap.
    try:
        st = c.call("game_bridge_status")
        if st.get("found"):
            b = _parse_block(c, int(st["block"]))
            if b:
                return b
    except Exception:  # noqa: BLE001 - an older Dolphin, or GameBridge not there yet
        pass
    start, size = SYRINGE_HEAP
    mem = c.read_mem(start, size)
    i = 0
    while True:
        i = mem.find(MAGIC, i)
        if i < 0:
            raise SystemExit("PPOM block not found in the Syringe heap (plugin not loaded?)")
        ver, sz, mbo, mbs, so, ss, lo, ls, dbo, dbs = struct.unpack_from(">HHHHHHHHHH", mem, i + 4)
        if ver in (1, 2, 3, 4, 5) and 0x100 < sz < 0x4000 and mbo and dbo:
            a = start + i
            return Block(a, ver, sz, a + mbo, mbs, a + dbo, dbs, a + so if ss else 0, ss,
                         a + lo if ls else 0, ls)
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


def code_suggestion_request(payload: bytes) -> dict:
    """0xBE request: {mode u8, scroll u8, inputLen u8, pad, index u32, input u16[9]}."""
    mode, scroll, n = payload[0], payload[1], payload[2]
    index = struct.unpack_from(">I", payload, 4)[0]
    text = from_u16s(payload[8:8 + 2 * CODE_LEN])
    text = "".join(chr(ord(ch) - 0xFEE0) if 0xFF01 <= ord(ch) <= 0xFF5E else ch for ch in text)
    return {"mode": mode, "scroll": scroll, "input": text[:n], "index": index}


def code_suggestion_payload(found: bool, code: str, index: int) -> bytes:
    """0xBE response: {found u8, len u8, pad[2], index u32, code u16[9]}; not found: the input."""
    return struct.pack(">BBxxI", 1 if found else 0, len(code), index) + u16s(code, CODE_LEN)


def decode_code_suggestion(payload: bytes) -> dict:
    found, n = payload[0], payload[1]
    index = struct.unpack_from(">I", payload, 4)[0]
    return {"found": bool(found), "len": n, "index": index,
            "code": from_u16s(payload[8:8 + 2 * CODE_LEN])}


def suggest(codes: list[str], prefix: str, index: int, scroll: int) -> tuple[bool, int, str]:
    """Slippi's handleNameEntryLoad (and Dolphin's RecentCodes::SuggestFrom) over `codes`, newest
    first: (found, index, code)."""
    prefix = prefix.upper()

    def match(i: int) -> bool:
        return 0 <= i < len(codes) and codes[i].upper().startswith(prefix)

    cur = {1: index + 1, 2: max(index - 1, 0) if index > 0 else index, 3: 0}.get(scroll, index)
    step = -1 if scroll == 2 else 1
    while 0 <= cur < len(codes) and not match(cur):
        cur += step
    if match(cur):
        return True, cur, codes[cur].upper()
    if match(index):
        return True, index, codes[index].upper()
    return False, index, prefix


def describe_request(cmd: int, payload: bytes) -> str:
    if cmd == 0xBE:
        r = code_suggestion_request(payload)
        return (f"FETCH_CODE_SUGGESTION mode={MODES.get(r['mode'], r['mode'])} "
                f"scroll={SCROLL.get(r['scroll'], r['scroll'])} input={r['input']!r} index={r['index']}")
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


def post(c: HarnessClient, b: Block, cmd: int, payload: bytes = b"") -> int:
    """Tests: post a request as the game does (ppom.cpp post): the slot first, its seq word, then
    reqWrite. Only while the plugin itself posts nothing (its own counter would reuse the seq)."""
    wr = struct.unpack(">I", c.read_mem(b.mailbox + MB_REQ_WRITE, 4))[0]
    seq = wr + 1
    slot = b.mailbox + MB_REQ + ((seq - 1) % REQ_SLOTS) * REQ_SIZE
    c.write_mem(slot + 4, bytes([cmd, 0, 0, 0]) + payload[:REQ_PAYLOAD].ljust(REQ_PAYLOAD, b"\0"))
    c.write_mem(slot, struct.pack(">I", seq))
    c.write_mem(b.mailbox + MB_REQ_WRITE, struct.pack(">I", seq))
    return seq


def read_response(c: HarnessClient, b: Block) -> dict:
    """The response in the mailbox now: {seq, cmd, status, payload}."""
    raw = c.read_mem(b.mailbox + MB_RESP, 8 + RESP_PAYLOAD)
    seq, cmd, status = struct.unpack_from(">IBB", raw, 0)
    return {"seq": seq, "cmd": cmd, "status": status, "payload": raw[8:]}


def consume(c: HarnessClient, b: Block, upto: int) -> None:
    c.write_mem(b.mailbox + MB_REQ_READ, struct.pack(">I", upto))


DEBUG_LOG = 16   # ppom.h DEBUG_LOG: entries in the Debug print ring
DEBUG_FIELDS = ("frames", "printCount", "overrides", "lastError", "cfg", "menuState")

CFG_TEST_RULES = 1 << 6   # ppom.h Cfg: the online ruleset keeps the set rule's stocks and times


def set_cfg_bits(c: HarnessClient, bits: int, on: bool = True) -> int:
    """Set (or clear) DEBUG cfg feature bits; returns the new cfg."""
    b = find_block(c)
    cfg = struct.unpack(">I", c.read_mem(b.debug + 0x10, 4))[0]
    cfg = (cfg | bits) if on else (cfg & ~bits)
    c.write_mem(b.debug + 0x10, struct.pack(">I", cfg))
    return cfg


def allow_test_rules(c: HarnessClient) -> None:
    """Tests that write short rules (brawl.write_rules) into an online CSS: the plugin forces the
    online ruleset at every match setup, and with this flag keeps the written stocks and times
    (both machines must write the same). Items, pause and hazards are still forced."""
    set_cfg_bits(c, CFG_TEST_RULES)


def read_debug(c: HarnessClient, b: Block) -> dict:
    d = c.read_mem(b.debug, b.debug_size)
    vals = dict(zip(DEBUG_FIELDS, struct.unpack_from(">6I", d, 0)))
    nscratch = 16 if b.version >= 2 else 10
    vals["scratch"] = list(struct.unpack_from(f">{nscratch}I", d, 0x18))
    # scratch[0] 0xBE requests sent; [1] CSS lock | phase << 4; [2] Z accepts << 24 |
    # found << 16 | suggestion index; [3] rules applied; [4] keypad 1 open / 2 OK / 3 closed;
    # [5] hand mode (8 = keypad); [6], [8], [9] menu hooks (netmenu.cpp); [7] the ONLINE page lock:
    # GET_ONLINE_STATUS state + 1 (0 = no answer yet) | A presses refused << 8
    log = []
    for k in range(DEBUG_LOG):
        lr, msg, win, line, data = struct.unpack_from(">IIHhI", d, 0x18 + 4 * nscratch + 16 * k)
        log.append((lr, msg, win, line, data))
    vals["log"] = log
    return vals


PORT_VALUES_SIZE = 0x3C
LAYOUT_SIZE = 0x2D


def port_values(d: bytes) -> dict:
    """PortValues (v3): a player's name tag and its controls (ppom.h)."""
    flags, rumble = d[0], d[1]
    return {"tag": bool(flags & 1), "rumble": rumble, "tag_name": from_u16s(d[2:12]),
            "layout": d[12:12 + LAYOUT_SIZE].hex(), "tap_jump": bool(d[12 + 11] & 0x80)}


def read_local(c: HarnessClient, b: Block) -> dict:
    """LOCAL (v3): Dolphin's view of this player's session and the game's lock-in (with the
    player's port values)."""
    d = c.read_mem(b.local, b.local_size or 0x40)
    seq, state, port, rready, disc = struct.unpack_from(">IBBBB", d, 0)
    lseq, ready, css, kind, costume, stage, asl, game = struct.unpack_from(">IBBBBHBB", d, 0x28)
    return {"seq": seq, "state": state, "local_port": port, "remote_ready": rready,
            "disconnected": disc, "peer_name": from_u16s(d[8:8 + 2 * NAME_LEN]),
            "lock": {"seq": lseq, "ready": ready, "css": css, "char_kind": kind, "costume": costume,
                     "stage_pick": stage, "asl": asl, "game": game,
                     **({"team": d[0x34]} if b.version >= 4 else {})},
            **({"own": port_values(d[0x40:0x40 + PORT_VALUES_SIZE]),
                "hud_disconnected": d[0x7C], "desynced": d[0x7D]} if len(d) >= 0x80 else {})}


def read_session(c: HarnessClient, b: Block) -> dict:
    """SESSION (v2/v3): the lobby and the next game's setup, the same on both machines."""
    d = c.read_mem(b.session, b.session_size or 0x110)
    stride = 0x80 if len(d) >= 0x210 else 0x40
    seq, state, mode, game, winner, stage, asl, n = struct.unpack_from(">IBBBBHBB", d, 0)
    players = []
    for i in range(4):
        o = 0x0C + stride * i
        present, kind, costume = d[o], d[o + 1], d[o + 2]
        pl = {"present": present, "char_kind": kind, "costume": costume,
              "name": from_u16s(d[o + 4:o + 4 + 2 * NAME_LEN]),
              "code": from_u16s(d[o + 0x24:o + 0x24 + 2 * CODE_LEN])}
        if stride == 0x80:
            pl["pv"] = port_values(d[o + 0x40:o + 0x40 + PORT_VALUES_SIZE])
        if len(d) >= 0x220:   # v4: 3-4 player matches
            pl.update(team=d[o + 3], picks_stage=d[o + 0x36], out=d[o + 0x37])
        players.append(pl)
    se = {"seq": seq, "state": state, "mode": mode, "game": game, "last_winner": winner,
          "stage": stage, "asl": asl, "num_players": n, "players": players[:max(n, 0)]}
    if len(d) >= 0x220:
        se.update(gone=list(d[0x20C:0x210]), teams=d[0x210], setup_error=d[0x211],
                  out_count=d[0x212])
    return se


# ---- v4: 3-4 player matches (docs/nplayer/setup.md) ----
S_GONE, S_TEAMS, S_SETUP_ERROR, S_OUT_COUNT = 0x20C, 0x210, 0x211, 0x212
SP_TEAM, SP_PICKS_STAGE, SP_OUT = 0x03, 0x36, 0x37
CFG_TEST_GONE = 1 << 7   # ppom.h Cfg: Debug.testGone* set gone flags; removal in local matches too
DEBUG_TEST_GONE = 0x18 + 4 * 16 + 16 * DEBUG_LOG   # Debug.testGoneFrame, then testGonePorts


def write_session(c: HarnessClient, b: Block, *, game: int, stage: int, players: list,
                  teams: bool = False, mode: int = 2, last_winner: int = 0xFF, asl: int = 0,
                  state: int = 2, pickers: int = 0) -> None:
    """Tests (Dolphin's GameBridge off): write a SESSION as Dolphin would, with `players` by port:
    None for an empty port, else {"char_kind", "costume", "team" (0-2 / 0xFF)}. Bumps the seq."""
    d = bytearray(c.read_mem(b.session, b.session_size))
    seq = struct.unpack_from(">I", d, 0)[0]
    d[4:] = bytes(len(d) - 4)
    n = max((i + 1 for i, p in enumerate(players) if p), default=0)
    struct.pack_into(">BBBBHBB", d, 4, state, mode, game, last_winner, stage, asl, n)
    for i in range(4):
        o = 0x0C + 0x80 * i
        p = players[i] if i < len(players) else None
        d[o + SP_TEAM] = 0xFF
        d[o + 1] = 0xFF
        if p:
            d[o] = 1
            d[o + 1] = p["char_kind"]
            d[o + 2] = p.get("costume", 0)
            d[o + SP_TEAM] = p.get("team", 0xFF) if teams else 0xFF
            d[o + 4:o + 4 + 2 * NAME_LEN] = u16s(p.get("name", f"P{i + 1}"), NAME_LEN)
        d[o + SP_PICKS_STAGE] = (pickers >> i) & 1
    if len(d) > S_TEAMS:
        d[S_TEAMS] = 1 if teams else 0
    c.write_mem(b.session + 4, bytes(d[4:]))
    c.write_mem(b.session, struct.pack(">I", seq + 1))


# ---- v5: rooms (docs/rooms-game-interface.md) ----
CMD_ROOM = 0xD0
ROOM_OPS = {"poll": 0, "create": 1, "join": 2, "leave": 3, "slot": 4, "teams": 5, "public": 6,
            "team": 7}
L_SCREEN, L_ROOM_JOIN, L_ROOM = 0x36, 0x37, 0x38
SCREENS = {"unknown": 0, "menus": 1, "online-css": 2, "room": 3, "online-busy": 4, "match": 5,
           "offline": 6, "other": 7}
S_ROOM_FLAGS, S_ROOM_CODE, S_ROOM_HOST, S_ROOM_STATUS, S_ROOM_MODE = 0x213, 0x214, 0x218, 0x219, 0x21A
SP_ROOM_SLOT, SP_ROOM_TEAM, SP_ROOM_CHAR, SP_ROOM_COSTUME = 0x38, 0x39, 0x3A, 0x3B
SLOT_OPEN, SLOT_TAKEN, SLOT_READY, SLOT_HOST, SLOT_IN_GAME = 1, 2, 4, 8, 0x10
RF_IN, RF_PUBLIC, RF_TEAMS = 1, 2, 4


def room_request(op: str, arg: int = 0, arg2: int = 0, code: str = "") -> bytes:
    """A CMD_ROOM request payload (RoomRequest, 0x18 bytes)."""
    return bytes([ROOM_OPS[op], arg, arg2, 0]) + u16s(code, CODE_LEN) + bytes(2)


def decode_room_status(payload: bytes) -> dict:
    """A CMD_ROOM response payload (RoomStatus)."""
    phase, error, port = payload[0], payload[1], payload[2]
    serial = struct.unpack_from(">I", payload, 4)[0]
    return {"phase": phase, "error": error, "local_port": port, "serial": serial,
            "text": from_u16s(payload[8:8 + 128])}


def read_room(c: HarnessClient, b: Block) -> dict:
    """The room view (v5): SESSION's room bytes and LOCAL's room phase, screen and join counter."""
    se = c.read_mem(b.session, b.session_size)
    lo = c.read_mem(b.local, b.local_size)
    slots = []
    for i in range(4):
        o = 0x0C + 0x80 * i
        slots.append({"bits": se[o + SP_ROOM_SLOT], "team": se[o + SP_ROOM_TEAM],
                      "char": se[o + SP_ROOM_CHAR], "costume": se[o + SP_ROOM_COSTUME],
                      "name": from_u16s(se[o + 4:o + 4 + 2 * NAME_LEN]),
                      "picks_stage": se[o + SP_PICKS_STAGE]})
    return {"flags": se[S_ROOM_FLAGS], "code": se[S_ROOM_CODE:S_ROOM_CODE + 4].decode("ascii", "replace"),
            "host": se[S_ROOM_HOST], "status": se[S_ROOM_STATUS], "mode": se[S_ROOM_MODE],
            "game": se[6], "session_state": se[4], "session_seq": struct.unpack_from(">I", se, 0)[0],
            "slots": slots, "phase": lo[L_ROOM], "join": lo[L_ROOM_JOIN], "screen": lo[L_SCREEN],
            "local_port": lo[5], "local_state": lo[4]}


def set_test_gone(c: HarnessClient, b: Block, frame: int, ports: int, later_ports: int = 0,
                  later_by: int = 0) -> None:
    """CFG_TEST_GONE: from game frame `frame` on (g_GameFrame +4), the ports in the bit mask
    `ports` are gone, and `later_by` frames after that also `later_ports`. Before the match only:
    the plugin's data is rolled back with it."""
    c.write_mem(b.debug + DEBUG_TEST_GONE,
                struct.pack(">II", frame, (ports & 0xF) | (later_ports & 0xF) << 8 | later_by << 16))
    set_cfg_bits(c, CFG_TEST_GONE, True)


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
    p.add_argument("--codes", default="", help="recent codes for FETCH_CODE_SUGGESTION, newest first")
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
        resp = c.read_mem(b.mailbox + MB_RESP, 8 + 0x40)
        rseq, rcmd, rstatus = struct.unpack_from(">IBB", resp, 0)
        if rseq:
            extra = ""
            if rcmd == 0xBE and rstatus == 0:
                extra = f" {decode_code_suggestion(resp[8:])}"
            print(f"  last response seq={rseq} {CMD.get(rcmd, hex(rcmd))} status={rstatus:#x}{extra}")
        d = read_debug(c, b)
        print({k: (hex(v) if k == "cfg" else v) for k, v in d.items() if k != "log"})
        if b.local:
            print("local", read_local(c, b))
        if b.session:
            print("session", read_session(c, b))
    elif a.cmd == "log":
        d = read_debug(c, b)
        n = d["printCount"]
        for k in range(min(n, DEBUG_LOG)):
            lr, msg, win, line, data = d["log"][(n - 1 - k) % DEBUG_LOG]
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
                    elif cmd == 0xBE:
                        r = code_suggestion_request(pl)
                        codes = [x for x in a.codes.split(",") if x]
                        found, idx, code = suggest(codes, r["input"], r["index"], r["scroll"])
                        write_response(c, b, seq, 0xBE, code_suggestion_payload(found, code, idx))
                        print(f"  -> FETCH_CODE_SUGGESTION found={found} {code!r} index={idx}")
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
