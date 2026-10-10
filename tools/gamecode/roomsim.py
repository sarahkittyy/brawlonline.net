#!/usr/bin/env python3
"""Dolphin's half of the rooms interface, played by a script (docs/rooms-game-interface.md).

For testing the game's room UI on one instance without servers: Dolphin's own servicing is
turned off (`game_bridge_config enabled=false`) and RoomSim answers the plugin's mailbox
(GET_ONLINE_STATUS, CMD_ROOM) and writes the room view into SESSION and LOCAL the way
GameBridge does. The room itself is a small model of mm's (server/crates/mm/src/rooms.rs): a
code, four slots by in-game port, a host, Teams, public, readiness, the status lines of
docs/rooms-protocol.md. Other members are added and changed by the test (`add_player`,
`set_ready`, ...). A room's start (tickets, the gameplay session) is not modelled: end-to-end
room games need the real Dolphin and servers (harness/tests/test_rooms_game.py).

    sim = RoomSim(client); sim.service(frames=60)       # answer requests for 60 game frames
"""

from __future__ import annotations

import struct
import time
from dataclasses import dataclass, field

import ppom

VOWELS_OK = set("BCDFGHJKLMNPQRSTVWXZ")


@dataclass
class Member:
    name: str
    code: str = ""
    ready: bool = False
    char: int = 0xFF          # gmCharacterKind while ready
    costume: int = 0
    team: int = 0
    in_game: bool = False
    local: bool = False


@dataclass
class Room:
    code: str = "KFQB"
    public: bool = True
    teams: bool = False
    status: int = 0           # 0 waiting, 1 starting, 2 in game
    host: int = 0
    open: list = field(default_factory=lambda: [True, True, False, False])
    members: list = field(default_factory=lambda: [None, None, None, None])
    game: int = 1


class RoomSim:
    def __init__(self, c, name: str = "Sarah", code: str = "SARA#001"):
        self.c = c
        self.b = ppom.find_block(c)
        self.name, self.code = name, code
        self.room: Room | None = None
        self.phase = 0            # LOCAL.room: 0 none, 1 joining, 2 in
        self.text, self.error, self.serial = "", 0, 1
        self.joinable: dict[str, Room] = {}   # code -> a room that a ROOM_JOIN finds
        self.requests: list[tuple[int, int, bytes]] = []   # (seq, cmd, payload) seen
        self.room_join_counter = 0
        self.pending: list[tuple[int, int, bytes]] = []    # responses waiting for respSeen

    # ---- the room model -------------------------------------------------------------------
    def me(self) -> int:
        if not self.room:
            return 0xFF
        for i, m in enumerate(self.room.members):
            if m and m.local:
                return i
        return 0xFF

    def mode(self) -> int:
        r = self.room
        n_open = sum(1 for o in r.open if o)
        if n_open <= 2:
            return 0
        return 2 if r.teams else 1

    def status_line(self) -> str:
        r = self.room
        if not r:
            return self.text if self.error else ""
        if r.status == 1:
            return "Starting the game"
        players = [m for m in r.members if m]
        n_open = sum(1 for o in r.open if o)
        if len(players) == 1:
            return "Waiting for players"
        empty = sum(1 for i in range(4) if r.open[i] and not r.members[i])
        if empty:
            return f"Waiting for players ({len(players)}/{n_open})"
        if r.teams and len(players) >= 3 and len({m.team for m in players}) == 1:
            return "Pick different teams"
        waiting = [m.name for m in players if not m.ready]
        if waiting:
            return "Waiting on: " + ", ".join(waiting)
        return ""

    def set_error(self, text: str) -> None:
        self.text, self.error = text, 1
        self.serial += 1

    def create(self, public: bool = True, code: str = "KFQB") -> None:
        self.room = Room(code=code, public=public)
        self.room.members[0] = Member(self.name, self.code, local=True, team=0)
        self.phase = 2
        self.text, self.error = "", 0

    def join(self, code: str) -> None:
        code = code.upper()
        r = self.joinable.get(code)
        if not r or len(code) != 4 or not set(code) <= VOWELS_OK:
            self.phase, self.room = 0, None
            self.set_error("Room not found.")
            return
        slot = next((i for i in range(4) if r.open[i] and not r.members[i]), None)
        if slot is None:
            self.phase, self.room = 0, None
            self.set_error("This room is full.")
            return
        r.members[slot] = Member(self.name, self.code, local=True, team=[0, 1, 2, 0][slot])
        self.room, self.phase = r, 2
        self.text, self.error = "", 0

    def leave(self) -> None:
        if self.room:
            i = self.me()
            if i != 0xFF:
                self.room.members[i] = None
        self.room, self.phase = None, 0
        self.text, self.error = "", 0

    def add_player(self, slot: int, name: str, team: int | None = None) -> None:
        self.room.open[slot] = True
        self.room.members[slot] = Member(name, f"{name[:4].upper()}#1", team=[0, 1, 2, 0][slot] if team is None else team)

    def remove_player(self, slot: int) -> None:
        self.room.members[slot] = None

    def set_ready(self, slot: int, char: int | None, costume: int = 0) -> None:
        m = self.room.members[slot]
        m.ready = char is not None
        m.char = char if char is not None else 0xFF
        m.costume = costume

    # ---- requests ---------------------------------------------------------------------------
    def handle_room(self, seq: int, p: bytes) -> None:
        op, arg, arg2 = p[0], p[1], p[2]
        code = ppom.from_u16s(p[4:4 + 2 * ppom.CODE_LEN])
        r = self.room
        host = r is not None and self.me() == r.host
        if op == 1:
            self.create(public=bool(arg))
        elif op == 2:
            self.join(code)
        elif op == 3:
            self.leave()
        elif op == 4 and r:
            if not host:
                self.set_error("Only the host can do that.")
            elif arg == self.me() and not arg2:
                self.set_error("You can't close your own slot.")
            elif not arg2 and sum(1 for o in r.open if o) <= 2:
                self.set_error("At least 2 slots stay open.")
            elif arg < 4:
                r.open[arg] = bool(arg2)
                if not arg2:
                    r.members[arg] = None
                self.text, self.error = "", 0
        elif op == 5 and r:
            if host:
                r.teams = bool(arg)
                for m in r.members:
                    if m:
                        m.ready = False
            else:
                self.set_error("Only the host can do that.")
        elif op == 6 and r:
            if host:
                r.public = bool(arg)
            else:
                self.set_error("Only the host can do that.")
        elif op == 7 and r and arg < 3:
            r.members[self.me()].team = arg
        self.respond_room(seq)

    def respond_room(self, seq: int) -> None:
        text = self.text if self.error else self.status_line()
        body = struct.pack(">BBBBI", self.phase, self.error, self.me(), 0, self.serial) + ppom.u16s(text, 64)
        self.pending.append((seq, ppom.CMD_ROOM, body))

    def lock_in(self) -> None:
        """The game's lock-in (LOCAL) -> this member's readiness, as Dolphin's room-ready."""
        if not self.room:
            return
        lo = ppom.read_local(self.c, self.b)["lock"]
        m = self.room.members[self.me()]
        ready = bool(lo["ready"]) and lo["game"] == self.room.game
        m.ready = ready
        m.char = lo["char_kind"] if ready else 0xFF
        m.costume = lo["costume"] if ready else 0

    # ---- the view -----------------------------------------------------------------------------
    def write_view(self) -> None:
        c, b = self.c, self.b
        se = bytearray(c.read_mem(b.session, b.session_size))
        lo = bytearray(c.read_mem(b.local, b.local_size))
        r = self.room
        flags = 0
        if r and self.phase == 2:
            flags = ppom.RF_IN | (ppom.RF_PUBLIC if r.public else 0) | (ppom.RF_TEAMS if r.teams else 0)
        se[ppom.S_ROOM_FLAGS] = flags
        se[ppom.S_ROOM_CODE:ppom.S_ROOM_CODE + 4] = (r.code if r else "\0\0\0\0").encode("ascii")[:4].ljust(4, b"\0")
        se[ppom.S_ROOM_HOST] = r.host if r else 0xFF
        se[ppom.S_ROOM_STATUS] = r.status if r else 0
        se[ppom.S_ROOM_MODE] = self.mode() if r else 0
        se[6] = r.game if r else se[6]
        for i in range(4):
            o = 0x0C + 0x80 * i
            m = r.members[i] if r else None
            bits = 0
            if r and r.open[i]:
                bits |= ppom.SLOT_OPEN
            if m:
                bits |= ppom.SLOT_TAKEN | (ppom.SLOT_READY if m.ready else 0) | (ppom.SLOT_IN_GAME if m.in_game else 0)
                if i == r.host:
                    bits |= ppom.SLOT_HOST
            se[o + ppom.SP_ROOM_SLOT] = bits
            se[o + ppom.SP_ROOM_TEAM] = m.team if m else 0xFF
            se[o + ppom.SP_ROOM_CHAR] = m.char if (m and m.ready) else 0xFF
            se[o + ppom.SP_ROOM_COSTUME] = m.costume if (m and m.ready) else 0
            se[o + 4:o + 4 + 2 * ppom.NAME_LEN] = ppom.u16s(m.name if m else "", ppom.NAME_LEN)
            se[o + 0x24:o + 0x24 + 2 * ppom.CODE_LEN] = ppom.u16s(m.code if m else "", ppom.CODE_LEN)
        lo[ppom.L_ROOM] = self.phase
        lo[5] = self.me() if (r and self.phase == 2) else 0xFF
        lo[ppom.L_ROOM_JOIN] = self.room_join_counter & 0xFF
        # SESSION's seq last (the game redraws on it), LOCAL's room bytes only.
        c.write_mem(b.session + 4, bytes(se[4:]))
        seq = struct.unpack_from(">I", se, 0)[0]
        c.write_mem(b.session, struct.pack(">I", seq + 1))
        c.write_mem(b.local + 5, bytes(lo[5:6]))
        c.write_mem(b.local + ppom.L_ROOM_JOIN, bytes(lo[ppom.L_ROOM_JOIN:ppom.L_ROOM + 1]))

    def screen(self) -> int:
        return self.c.read_mem(self.b.local + ppom.L_SCREEN, 1)[0]

    def service(self, frames: int = 30, until=None) -> None:
        """Answer the mailbox and keep the view for `frames` game frames (or until `until()`)."""
        start = self.c.status().input_polls
        last_view = None
        while True:
            (wr, rd, rc, rs), reqs = ppom.read_requests(self.c, self.b)
            for seq, cmd, pl in sorted(reqs):
                if not seq or seq <= rd:
                    continue
                self.requests.append((seq, cmd, bytes(pl)))
                if cmd == 0xB9:
                    self.pending.append((seq, 0xB9, struct.pack(">BB", 1, 0) + ppom.u16s(self.name, ppom.NAME_LEN)
                                         + ppom.u16s(self.code, ppom.CODE_LEN)))
                elif cmd == ppom.CMD_ROOM:
                    self.handle_room(seq, pl)
                elif cmd == 0xBA:
                    pass
                else:
                    self.pending.append((seq, cmd, b""))
                ppom.consume(self.c, self.b, seq)
            if self.pending and rc == rs:
                seq, cmd, body = self.pending.pop(0)
                status = 0xFF if (cmd not in (0xB9, ppom.CMD_ROOM)) else 0
                ppom.write_response(self.c, self.b, seq, cmd, body, status)
            self.lock_in()
            view = (self.phase, repr(self.room), self.room_join_counter)
            if view != last_view:
                self.write_view()
                last_view = view
            now = self.c.status().input_polls
            if until and until():
                return
            if now - start >= frames:
                return
            time.sleep(0.012)

    def room_ops(self, op: str) -> list[bytes]:
        """Payloads of the CMD_ROOM requests with this op seen so far."""
        return [p for _s, cmd, p in self.requests if cmd == ppom.CMD_ROOM and p[0] == ppom.ROOM_OPS[op]]
