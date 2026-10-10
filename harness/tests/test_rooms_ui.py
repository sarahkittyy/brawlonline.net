"""The room UI on one instance, with Dolphin's half of the rooms interface played by a script.

docs/design/rooms.md (WITH FRIENDS' three buttons, the room CSS, the keypad's room-code mode),
docs/rooms-game-interface.md (the PPOM v5 room protocol). Dolphin's own servicing is turned off
and tools/gamecode/roomsim.py answers the plugin's requests and writes the room view the way
GameBridge does, so the game's side is tested on its own; harness/tests/test_rooms_game.py plays
real rooms against Dolphin's side and the servers.

Screenshots: run/artifacts/game-code/rooms/ui/.
"""

from __future__ import annotations

import os
import struct
import sys
import time
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools" / "gamecode"))
sys.path.insert(0, str(ROOT / "tools" / "sdcard"))

import drive  # noqa: E402
import ppboot  # noqa: E402
import ppom  # noqa: E402
import roomsim  # noqa: E402
from ppharness.client import HarnessClient, PadInput  # noqa: E402

pytestmark = [pytest.mark.dolphin, pytest.mark.gpu]

PLUGIN = Path(os.environ.get("PPHARNESS_PLUGIN") or ROOT / "game-code" / "PPOnline" / "PPOnline.rel")
ART = ROOT / "run" / "artifacts" / "game-code" / "rooms" / "ui"

# Brawl's CSS hand (muSelCharHand, the local area's +0x1A8): +0x90 x, +0x94 y, in the CSS's own
# units (game-code.md "Rooms"); the character grid and the panels in those units.
HAND_X, HAND_Y = 0x90, 0x94
FOX = (-23.0, 10.5)                 # Fox's portrait in the grid
MODE_BUTTON = (27.5, 17.0)          # the CSS's STAGE button as "FFA" / "TEAMS" (room_css.cpp headerButtons)
PUBLIC_BUTTON = (27.5, 19.5)        # its ITEM button as "PUBLIC" / "PRIVATE"
MY_FLAG = (-26.0, -4.5)             # our own panel's team flag (room_css.cpp handOverMyFlag)
SLOT_POS = 16.0 / 0.99              # a panel's model offset per slot (room_css.cpp SLOT_POS)
PANEL_X = [-22.0, -7.0, 8.0, 23.0]  # the four panels' centres, y about -10
GMKIND_MARIO, GMKIND_FOX = 0x00, 0x07


class Game:
    def __init__(self, name: str):
        ART.mkdir(parents=True, exist_ok=True)
        self.inst = ppboot.make_instance(name, [str(PLUGIN)], boot="offline")
        self.inst.launch()
        self.c: HarnessClient = self.inst.connect()
        self.c.wait_state("running", timeout=120)
        self.sim: roomsim.RoomSim | None = None

    def close(self) -> None:
        try:
            self.inst.stop()
        except Exception:  # noqa: BLE001
            self.inst.kill()
        d = self.inst.user_dir
        if d and d.exists():
            from ppharness.instance import remove_tree
            log = d / "Logs" / "dolphin.log"
            if log.exists():
                (ART / f"{self.inst.name}-dolphin.log").write_bytes(log.read_bytes())
            remove_tree(d, self.inst.linked_dirs)

    def steps(self, *s: str) -> None:
        drive.run(self.c, list(s), out=str(ART))

    def scene(self) -> str:
        return drive.scene(self.c).name

    def wait(self, frames: int) -> None:
        self.sim.service(frames)

    def tap(self, btn: str, frames: int = 3) -> None:
        self.c.pad_script(0, [PadInput(buttons=btn.split("+"), hold=frames), PadInput(hold=6)])
        self.sim.service(frames + 10)

    def hold(self, btn: str, frames: int) -> None:
        self.c.pad_script(0, [PadInput(buttons=btn.split("+"), hold=frames), PadInput(hold=6)])
        self.sim.service(frames + 10)

    def shot(self, name: str) -> None:
        self.c.screenshot(str(ART / f"{name}.png"))

    def u32(self, a: int) -> int:
        return struct.unpack(">I", self.c.read_mem(a, 4))[0]

    def scratch(self, i: int) -> int:
        return ppom.read_debug(self.c, self.sim.b)["scratch"][i]

    def status_text(self) -> str:
        """The room status line Dolphin (the sim) last answered, as the game shows it."""
        return self.sim.text if self.sim.error else self.sim.status_line()

    def hand(self) -> tuple[float, float]:
        task = self.u32(self.u32(self.u32(0x805A0060) + 4) + 0x400)
        h = self.u32(self.u32(task + 0x44) + 0x1A8)
        x, y = struct.unpack(">ff", self.c.read_mem(h + HAND_X, 8))
        return x, y

    def area(self, i: int) -> int:
        task = self.u32(self.u32(self.u32(0x805A0060) + 4) + 0x400)
        return self.u32(task + 0x44 + 4 * i)

    def panel_state(self, i: int) -> int:
        """The state shown in area i's box (its state object, area+0x41C, +8)."""
        return self.u32(self.u32(self.area(i) + 0x41C) + 8)

    def panel_offset(self, i: int) -> float:
        """Area i's panel model (+0xB0) offset, MuObject +0x3C."""
        return struct.unpack(">f", self.c.read_mem(self.u32(self.area(i) + 0xB0) + 0x3C, 4))[0]

    def hand_to(self, x: float, y: float, tol: float = 1.2) -> None:
        """Closed loop: the stick toward (x, y) until the hand is there."""
        for _ in range(80):
            hx, hy = self.hand()
            dx, dy = x - hx, y - hy
            if abs(dx) <= tol and abs(dy) <= tol:
                return
            def axis(d: float) -> int:   # past the stick's dead zone, slower near the target
                if abs(d) <= tol:
                    return 128
                v = max(45, min(127, int(abs(d) * 15)))
                return 128 + (v if d > 0 else -v)
            sx, sy = axis(dx), axis(dy)
            self.c.pad_script(0, [PadInput(main=(sx, sy), hold=3), PadInput(hold=2)])
            self.sim.service(5)
        raise AssertionError(f"hand did not reach {(x, y)}: {self.hand()}")


@pytest.fixture
def game(dolphin_exe):
    g = Game("rooms-ui")
    yield g
    g.close()


def test_room_ui_with_a_scripted_dolphin(game: Game) -> None:
    g = game
    c = g.c
    c.game_bridge_config(enabled=False)
    g.steps("until muMenuMain 3000", "wait 60")
    g.sim = sim = roomsim.RoomSim(c)
    g.wait(60)

    # WITH FRIENDS: three buttons, labels drawn with the game's font (scratch[6] >> 16: 3 drawn).
    g.tap("A")
    g.wait(60)
    assert (g.scratch(6) >> 16) == 3, hex(g.scratch(6))
    g.shot("01-with-friends-direct")
    g.tap("DLEFT", 4)
    g.wait(20)
    g.shot("02-with-friends-create-room")

    # Create Room: the room CSS, ROOM_CREATE public; the code and Public in the top window.
    g.tap("A")
    sim.service(400, until=lambda: g.scene() == "scSelctCharacter" and sim.room is not None)
    g.wait(60)
    assert sim.room_ops("create") and sim.room_ops("create")[0][1] == 1
    assert sim.screen() == ppom.SCREENS["room"]
    g.shot("03-room-created")

    # A second player joins (the join sound), chooses, then readies with blue Mario.
    sim.add_player(1, "alice")
    g.wait(40)
    g.shot("04-alice-choosing")
    sim.set_ready(1, GMKIND_MARIO, 2)
    g.wait(40)
    g.shot("05-alice-ready")

    # Our character and START: the lock-in for the room's next game.
    g.hand_to(*FOX)
    g.tap("A")
    g.wait(20)
    g.tap("START")
    g.wait(30)
    lock = ppom.read_local(c, sim.b)["lock"]
    assert lock["ready"] == 1 and lock["game"] == sim.room.game and lock["char_kind"] == GMKIND_FOX, lock
    assert g.panel_state(0) == 4, "no Ready on our own panel"   # room_css.cpp ST_READY
    g.shot("06-both-ready")
    # B takes it back.
    g.tap("B")
    g.wait(20)
    assert ppom.read_local(c, sim.b)["lock"]["ready"] == 0

    # Host: A with the hand over the third panel opens slot 3 (port 2).
    g.hand_to(PANEL_X[2], -10.0)
    g.tap("A")
    g.wait(30)
    assert sim.room.open[2], sim.room.open
    g.shot("07-slot-3-opened")
    # The FFA button (the bar's STAGE button; A on it, as R): Teams on (three open slots: a team battle);
    # the panels in team colours. A room starts as FFA.
    assert not sim.room.teams
    g.hand_to(*MODE_BUTTON, tol=0.5)
    g.tap("A")
    g.wait(40)
    assert sim.room.teams
    g.shot("08-teams-on")
    # A on our own flag: the next team colour (Brawl's way).
    team0 = sim.room.members[0].team
    g.hand_to(*MY_FLAG, tol=0.5)
    g.tap("A")
    g.wait(30)
    assert sim.room.members[0].team == (team0 + 1) % 3, sim.room.members[0]
    g.shot("08a-flag-clicked")
    g.tap("Y")
    g.wait(30)
    assert sim.room.members[0].team == team0
    # X: our team colour to the next one (red -> blue), Y back.
    team0 = sim.room.members[0].team
    g.tap("X")
    g.wait(30)
    assert sim.room.members[0].team == (team0 + 1) % 3, sim.room.members[0]
    g.shot("08b-team-changed")
    g.tap("Y")
    g.wait(30)
    assert sim.room.members[0].team == team0
    # The PUBLIC button (the bar's ITEM button; A on it, as L): private.
    g.hand_to(*PUBLIC_BUTTON, tol=0.5)
    g.tap("A")
    g.wait(30)
    assert not sim.room.public
    g.shot("09-private")
    # A on the third panel again closes it ("Closed" on it); R Teams off.
    g.hand_to(PANEL_X[2], -10.0)
    g.tap("A")
    g.wait(30)
    assert not sim.room.open[2]
    g.tap("R")
    g.wait(30)

    # alice leaves (the back sound): her panel searching again.
    sim.remove_player(1)
    g.wait(40)
    g.shot("10-alice-left")

    # Hold Z: leave the room, idle on the CSS; START: the keypad in room-code mode.
    g.hold("Z", 60)
    g.wait(20)
    assert sim.room_ops("leave"), "no ROOM_LEAVE"
    assert sim.room is None
    g.shot("11-left-the-room")
    # A room with two players: we join in its third slot (P3), and our panel is the third.
    sim.joinable["KFQB"] = roomsim.Room(code="KFQB", host=1, open=[True, True, True, False],
                                        members=[roomsim.Member("carol", team=0),
                                                 roomsim.Member("bob", team=1), None, None])
    g.tap("START")
    g.wait(40)
    g.shot("12-keypad-room-code")
    # The symbols key ('#') is refused with the error sound (scratch[15] >> 16 counts refusals).
    refused0 = (g.scratch(15) >> 16) & 0xFF
    keypad_anchor(g)
    g.tap("A", 8)
    g.wait(10)
    assert ((g.scratch(15) >> 16) & 0xFF) == (refused0 + 1) & 0xFF
    typed = keypad_type(g, "KFQB")
    g.shot("13-keypad-typed")
    g.tap("START")
    g.wait(60)
    joins = sim.room_ops("join")
    assert joins and ppom.from_u16s(joins[-1][4:4 + 2 * ppom.CODE_LEN]) == "KFQB", (typed, joins)
    assert sim.room is not None and sim.me() == 2
    g.wait(30)
    # Panels in slot order: our own panel (area 0) drawn two slots right, carol's (area 2) at P1.
    assert abs(g.panel_offset(0) - 2 * SLOT_POS) < 0.1 and abs(g.panel_offset(2) + 2 * SLOT_POS) < 0.1,         (g.panel_offset(0), g.panel_offset(2))
    assert not keypad_open(g), "the keypad came back after the join"
    assert ppom.read_local(c, sim.b)["lock"]["ready"] == 0, "the keypad's START locked in"
    g.shot("14-joined-bob-host")

    # A wrong code: "Room not found." in red.
    g.hold("Z", 60)
    g.wait(20)
    g.tap("START")
    g.wait(40)
    keypad_type(g, "BBBB")
    g.tap("START")
    g.wait(60)
    assert sim.error and sim.text == "Room not found."
    g.shot("15-room-not-found")

    # Hold B: back to WITH FRIENDS with the room's button (Create Room, cursor 0) highlighted.
    g.hold("B", 120)
    sim.service(600, until=lambda: g.scene() == "muMenuMain")
    g.wait(90)
    g.shot("16-back-on-with-friends")
    page = g.u32(ppom_sym(g, "g_friendsPage"))
    assert struct.unpack(">H", c.read_mem(page + 0x42, 2))[0] == 0
    assert sim.screen() == ppom.SCREENS["menus"]


# The keypad's letter keys by (row, column), as test_online_game.ALPHA_KEYS (room-code mode takes
# every letter they show); (1, 0) is the symbols key.
ROOM_KEYS = {(1, 1): "ABC", (1, 2): "DEF", (2, 0): "GHI", (2, 1): "JKL", (2, 2): "MNO", (3, 0): "PQRS",
             (3, 1): "TUV", (3, 2): "WXYZ"}


def keypad_anchor(g: Game) -> tuple[int, int]:
    """Up to the erase key, down into row 1, left to column 0: the symbols key (1, 0)."""
    for _ in range(6):
        g.tap("DUP", 8)
    g.tap("DDOWN", 8)
    g.tap("DLEFT", 8)
    g.tap("DLEFT", 8)
    return (1, 0)


def keypad_type(g: Game, code: str) -> str:
    """Type a room code from wherever the cursor is (the same key twice in a row needs a move off
    and back: there is no multi-tap timeout)."""
    pos = keypad_anchor(g)
    last = None
    for ch in code:
        key = next(k for k, v in ROOM_KEYS.items() if ch in v)
        if key == last:
            g.tap("DRIGHT" if pos[1] < 2 else "DLEFT", 8)
            g.tap("DLEFT" if pos[1] < 2 else "DRIGHT", 8)
        r, c = pos
        while r < key[0]:
            g.tap("DDOWN", 8); r += 1
        while r > key[0]:
            g.tap("DUP", 8); r -= 1
        while c < key[1]:
            g.tap("DRIGHT", 8); c += 1
        while c > key[1]:
            g.tap("DLEFT", 8); c -= 1
        pos = (r, c)
        for _ in range(ROOM_KEYS[key].index(ch) + 1):
            g.tap("A", 8)
            g.wait(4)
        last = key
    return code


def keypad_open(g: Game) -> bool:
    return (g.scratch(4) & 0xFF) == 1   # code_entry.cpp: 1 open, 2 OK, 3 closed


def ppom_sym(g: Game, name: str) -> int:
    """A plugin symbol's address: its offset in PPOnlineMain.map plus its section's address."""
    secidx = {".text": 1, ".ctors": 2, ".dtors": 3, ".rodata": 4, ".data": 5, ".bss": 6}
    m = g.u32(0x800030C8)
    addrs = None
    while m:
        i, nxt, _p, nsec, so = struct.unpack(">5I", g.c.read_mem(m, 0x14))
        if i == 20560:
            secs = g.c.read_mem(so, 8 * nsec)
            addrs = [struct.unpack_from(">I", secs, 8 * k)[0] & ~1 for k in range(nsec)]
            break
        m = nxt
    cur = None
    for line in (PLUGIN.parent / "PPOnlineMain.map").read_text().splitlines():
        parts = line.split()
        if len(parts) == 5 and parts[4].startswith("."):
            cur = parts[4]
        elif len(parts) == 5 and (parts[4] == name or parts[4].startswith(name + "__")):
            return addrs[secidx[cur]] + int(parts[0], 16)
    raise KeyError(name)
