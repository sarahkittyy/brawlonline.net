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
AREA_COSTUME = 0x1BC                # the local area's costume (muSelCharPlayerArea m_charColorNo)


class Game:
    def __init__(self, name: str, art: Path = ART, config_args: tuple[str, ...] = ()):
        self.art = art
        art.mkdir(parents=True, exist_ok=True)
        self.inst = ppboot.make_instance(name, [str(PLUGIN)], boot="offline")
        self.inst.config.config_args = list(config_args)   # -C overrides (e.g. a 4:3 Wii)
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
                (self.art / f"{self.inst.name}-dolphin.log").write_bytes(log.read_bytes())
            remove_tree(d, self.inst.linked_dirs)

    def steps(self, *s: str) -> None:
        drive.run(self.c, list(s), out=str(self.art))

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
        self.c.screenshot(str(self.art / f"{name}.png"))

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

    def banner_shown(self) -> bool:
        """Brawl's READY TO FIGHT banner (muSelCharTask+0x3C4, MenSelchrReady) is visible: its
        ScnMdlSimple's +0xCC flags have 0x60 set while nwSMSetVisibility hides it."""
        task = self.u32(self.u32(self.u32(0x805A0060) + 4) + 0x400)
        return not self.u32(self.u32(self.u32(task + 0x3C4) + 0xC) + 0xCC) & 0x60

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
    assert not g.banner_shown(), "READY TO FIGHT with no character"
    g.shot("03-room-created")

    # A second player joins (the join sound), chooses, then readies with blue Mario.
    sim.add_player(1, "alice")
    g.wait(40)
    g.shot("04-alice-choosing")
    sim.set_ready(1, GMKIND_MARIO, 2)
    g.wait(40)
    g.shot("05-alice-ready")

    # Our character and START: the lock-in for the room's next game.
    # READY TO FIGHT while START would ready us, gone once it has.
    g.hand_to(*FOX)
    g.tap("A")
    g.wait(20)
    assert g.banner_shown(), "no READY TO FIGHT with a character, not ready"
    g.shot("05a-ready-to-fight")
    g.tap("START")
    g.wait(30)
    lock = ppom.read_local(c, sim.b)["lock"]
    assert lock["ready"] == 1 and lock["game"] == sim.room.game and lock["char_kind"] == GMKIND_FOX, lock
    assert g.panel_state(0) == 4, "no Ready on our own panel"   # room_css.cpp ST_READY
    assert not g.banner_shown(), "READY TO FIGHT while ready"
    g.shot("06-both-ready")
    # B takes it back (and READY TO FIGHT is back).
    g.tap("B")
    g.wait(20)
    assert ppom.read_local(c, sim.b)["lock"]["ready"] == 0
    assert g.banner_shown(), "no READY TO FIGHT after unreadying"

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
    # A on our own panel: the next costume, the team stays (any costume on any team, the user's,
    # 2026-10-12; Brawl's team battle CSS cycles the team there). Our lock-in carries it, and
    # while locked in A there changes nothing (as X / Y).
    costume0 = g.u32(g.area(0) + AREA_COSTUME)
    g.hand_to(PANEL_X[0], -10.0)
    g.tap("A")
    g.wait(30)
    costume = g.u32(g.area(0) + AREA_COSTUME)
    assert costume != costume0 and sim.room.members[0].team == team0, (costume0, costume, sim.room.members[0])
    g.shot("08c-costume-on-a-team")
    g.tap("START")
    g.wait(30)
    lock = ppom.read_local(c, sim.b)["lock"]
    assert lock["ready"] == 1 and lock["costume"] == costume, (lock, costume)
    g.tap("A")
    g.wait(30)
    assert g.u32(g.area(0) + AREA_COSTUME) == costume and sim.room.members[0].team == team0
    g.tap("B")
    g.wait(20)
    assert ppom.read_local(c, sim.b)["lock"]["ready"] == 0
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

    # Hold Z leaves nothing (P+'s AltZ pick); hold B and Join Room: START, the keypad in room-code mode.
    g.hold("Z", 60)
    g.wait(20)
    assert not sim.room_ops("leave") and sim.room is not None, "hold Z left the room"
    to_join_room(g)
    assert g.banner_shown(), "no READY TO FIGHT out of a room (START enters a code)"
    g.shot("11-left-the-room")
    # A room with two players: we join in its third slot (P3), and our panel is the third.
    sim.joinable["KFQB"] = roomsim.Room(code="KFQB", host=1, open=[True, True, True, False],
                                        members=[roomsim.Member("carol", team=0),
                                                 roomsim.Member("bob", team=1), None, None])
    g.tap("START")
    g.wait(40)
    assert not g.banner_shown(), "READY TO FIGHT over the keypad"
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
    to_join_room(g)
    g.tap("START")
    g.wait(40)
    keypad_type(g, "BBBB")
    g.tap("START")
    g.wait(60)
    assert sim.error and sim.text == "Room not found."
    g.shot("15-room-not-found")

    # Hold B: back to WITH FRIENDS with the room's button (Join Room, cursor 2) highlighted.
    g.hold("B", 120)
    sim.service(600, until=lambda: g.scene() == "muMenuMain")
    g.wait(90)
    g.shot("16-back-on-with-friends")
    assert friends_cursor(g) == 2
    assert sim.screen() == ppom.SCREENS["menus"]


# ---- the RULES button (rules_button.cpp; docs/game-code.md §14 "RULES button") ----------------
RULES_ART = ROOT / "run" / "artifacts" / "game-code" / "rooms" / "rules-button"
BUTTON_RULES = 0x30            # its hand button number (Brawl's are 2-0x26)
TASK_RULES_OPEN = 0x630        # muSelCharTask: P+'s rules menu is open


class RulesButton:
    """g_rulesButton (rules_button.cpp): made, presses, hits, its colL/colR rect, its place."""

    def __init__(self, g: Game):
        self.g = g
        self.addr = ppom_sym(g, "g_rulesButton")

    def read(self) -> dict:
        made, presses, hits, l, r, t, b = struct.unpack(">3I4f", self.g.c.read_mem(self.addr, 0x1C))
        return {"made": made, "presses": presses, "hits": hits, "rect": (l, r, t, b)}

    def centre(self) -> tuple[float, float]:
        l, r, t, b = self.read()["rect"]
        return (l + r) / 2, (t + b) / 2


def css_task(g: Game) -> int:
    return g.u32(g.u32(g.u32(0x805A0060) + 4) + 0x400)


def hand_button(g: Game) -> int:
    return g.u32(g.u32(g.area(0) + 0x1A8) + 0xAC)


def menu_open(g: Game) -> bool:
    return g.u32(css_task(g) + TASK_RULES_OPEN) != 0


def create_room(g: Game) -> None:
    g.steps("until muMenuMain 3000", "wait 60")
    g.sim = sim = roomsim.RoomSim(g.c)
    g.wait(60)
    g.tap("A")
    g.wait(60)
    g.tap("DLEFT", 4)
    g.wait(20)
    g.tap("A")
    sim.service(400, until=lambda: g.scene() == "scSelctCharacter" and sim.room is not None)
    g.wait(60)


def friends_cursor(g: Game) -> int:
    """WITH FRIENDS' highlighted button: 0 Create Room, 1 Direct 1v1, 2 Join Room."""
    return struct.unpack(">H", g.c.read_mem(g.u32(ppom_sym(g, "g_friendsPage")) + 0x42, 2))[0]


def friends_to(g: Game, button: int) -> None:
    """WITH FRIENDS' cursor to `button` (Create Room top left, Direct 1v1 right, Join Room below)."""
    for d in ("DDOWN", "DRIGHT", "DDOWN", "DRIGHT") if button == 2 else ("DUP", "DLEFT", "DUP", "DLEFT"):
        if friends_cursor(g) == button:
            break
        g.tap(d, 4)
        g.wait(15)
    assert friends_cursor(g) == button, friends_cursor(g)


def to_join_room(g: Game, pick: tuple[float, float] = FOX) -> None:
    """Out of the room and onto Join Room's CSS with a character picked, where START enters a room
    code. A room has no hold Z to leave (P+ picks AltZ characters with Z held; the user's,
    2026-10-12), so: hold B (ROOM_LEAVE, WITH FRIENDS), then Join Room."""
    g.hold("B", 120)
    g.sim.service(600, until=lambda: g.scene() == "muMenuMain")
    g.wait(90)
    assert g.sim.room is None and g.sim.room_ops("leave"), "hold B did not leave the room"
    friends_to(g, 2)
    g.tap("A")
    g.sim.service(600, until=lambda: g.scene() == "scSelctCharacter")
    g.wait(60)
    g.hand_to(*pick, tol=0.6)
    g.tap("A")
    g.wait(20)


def test_host_holds_a_on_a_player_to_remove_them(game: Game) -> None:
    """The host removes a player by holding A on their panel (rooms.md #10). Player feedback
    2026-10-12: nothing happened, with three slots open and one other player (the held field has
    A a frame after the press, and the hold was let go on its first frame); a 1v1 room's player
    can be removed too (its slot stays open). A tap removes nobody."""
    g = game
    g.c.game_bridge_config(enabled=False)
    create_room(g)
    sim = g.sim
    kicks = lambda: [tuple(o[1:3]) for o in sim.room_ops("slot")]  # noqa: E731
    sim.add_player(1, "alice")
    sim.room.open[2] = True
    g.wait(40)
    g.hand_to(PANEL_X[1], -10.0)
    g.wait(5)
    # A tap on alice: nothing sent, she stays.
    g.tap("A")
    g.wait(70)
    assert kicks() == [] and sim.room.members[1] is not None
    # Held: removed, her slot closed (three were open).
    g.hold("A", 70)
    g.wait(20)
    g.shot("30-alice-removed")
    assert kicks() == [(1, 0)], (kicks(), g.hand())
    assert sim.room.members[1] is None and sim.room.open == [True, False, True, False]
    # A 1v1 room (slots 1-2 open): removed, the slot stays open.
    sim.room.open = [True, True, False, False]
    sim.add_player(1, "bob")
    g.wait(40)
    g.hold("A", 70)
    g.wait(20)
    g.shot("31-bob-removed-1v1")
    assert kicks() == [(1, 0), (1, 0)], kicks()
    assert sim.room.members[1] is None and sim.room.open == [True, True, False, False] and not sim.error


def sys_stick(g: Game) -> tuple[int, int]:
    """Port 1's main stick as the pad system has it (gfPadStatus +0x30/+0x31, signed)."""
    ps = g.u32(0x805A0040)
    x, y = struct.unpack(">bb", g.c.read_mem(ps + 0x244 + 0x30, 2))
    return x, y


def test_hold_z_to_pick_an_alt_stays_in_the_room(game: Game) -> None:
    """P+'s "Hold Z for AltZ Characters" picks a character with Z held. The user's, 2026-10-12:
    holding Z and picking a character in a room left the room ("Press START to enter code", as
    Direct's CSS). A room no longer has a hold Z to leave: Z held the whole way to Fox and while A
    picks him leaves nothing, and neither does Z alone."""
    g = game
    g.c.game_bridge_config(enabled=False)
    create_room(g)
    sim = g.sim
    sim.add_player(1, "alice")
    g.wait(40)
    g.hand_to(PANEL_X[3], 10.0)   # away from Fox, so the way there is long
    g.wait(10)
    # Z down, then the hand to Fox with Z held (closed loop, as hand_to): over the hold's 48 frames.
    frames = 0
    sticks = []
    for _ in range(80):
        hx, hy = g.hand()
        dx, dy = FOX[0] - hx, FOX[1] - hy
        if abs(dx) <= 1.2 and abs(dy) <= 1.2:
            break
        def axis(d: float) -> int:   # slow, a little past the dead zone
            return 128 if abs(d) <= 1.2 else 128 + (40 if d > 0 else -40)
        g.c.pad_set(0, buttons=["Z"], main=(axis(dx), axis(dy)))
        g.wait(3)
        sticks.append(sys_stick(g))
        frames += 3
    assert frames > 48, f"the way to Fox took only {frames} frames"
    print("sys stick on the way:", sorted(set(sticks)))
    # A short stop with Z alone (under the hold), then A on Fox with Z held, and Z held on.
    g.c.pad_set(0, buttons=["Z"])
    g.wait(20)
    g.c.pad_set(0, buttons=["Z", "A"])
    g.wait(4)
    g.c.pad_set(0, buttons=["Z"])
    g.wait(90)
    g.c.pad_set(0)
    g.wait(20)
    g.shot("32-alt-picked-in-the-room")
    assert not sim.room_ops("leave"), f"Z held for the alt left the room (sticks {sorted(set(sticks))[:6]})"
    assert sim.room is not None and sim.me() == 0
    # Z alone, still: nothing either (a room has no hold Z; hold B leaves).
    g.hold("Z", 90)
    g.wait(20)
    assert not sim.room_ops("leave") and sim.room is not None, "hold Z alone left the room"


def test_the_pickers_start_line_is_one_line(game: Game) -> None:
    """The stage picker's "Press START to select stage" (a little wider than the room's status
    line at scale 1) is one smaller line, not two (staging feedback 2026-10-12)."""
    g = game
    c = g.c
    c.game_bridge_config(enabled=False)
    g.steps("until muMenuMain 3000", "wait 60")
    g.sim = sim = roomsim.RoomSim(c)
    g.wait(60)
    g.tap("A")
    g.wait(60)
    g.tap("DLEFT", 4)
    g.wait(20)
    g.tap("A")
    sim.service(400, until=lambda: g.scene() == "scSelctCharacter" and sim.room is not None)
    g.wait(60)
    sim.add_player(1, "alice")
    g.wait(40)
    g.hand_to(*FOX)
    g.tap("A")
    g.wait(20)
    # We lost the last game: we pick the next stage (SESSION players[0].picksStage; the sim
    # leaves the byte as it is).
    c.write_mem(sim.b.session + 0x0C + ppom.SP_PICKS_STAGE, b"")
    g.wait(30)
    assert g.scratch(14) >> 24 == 3, hex(g.scratch(14))   # LAYOUT_ONE_SMALL
    g.shot("40-select-stage-one-line")

@pytest.fixture
def rules_game(dolphin_exe):
    g = Game("rules-button", art=RULES_ART)
    yield g
    g.close()


def test_rules_button(rules_game: Game) -> None:
    g = rules_game
    c = g.c
    c.game_bridge_config(enabled=False)
    create_room(g)
    sim = g.sim
    rb = RulesButton(g)

    # Made once, left of PUBLIC / FFA, between them in height.
    st = rb.read()
    l, r, t, b = st["rect"]
    assert st["made"] == 1, st
    assert r < PUBLIC_BUTTON[0] - 2.0 and r - l > 3.0 and MODE_BUTTON[1] < (t + b) / 2 < PUBLIC_BUTTON[1], st
    g.shot("01-room-css")

    # The hand on it: its button in the hand (+0xAC), the highlight playing, as on PUBLIC.
    g.hand_to(*rb.centre(), tol=0.4)
    hits = rb.read()["hits"]
    g.wait(20)
    assert hand_button(g) == BUTTON_RULES and rb.read()["hits"] > hits
    g.shot("02-hover")

    # A: P+'s rules menu over the room CSS (RoomRules::open); PUBLIC / FFA untouched.
    public, teams = sim.room.public, sim.room.teams
    g.tap("A")
    g.wait(40)
    assert rb.read()["presses"] == 1 and menu_open(g)
    assert (sim.room.public, sim.room.teams) == (public, teams)
    assert not sim.room_ops("public") and not sim.room_ops("teams")
    g.shot("03-rules-menu")
    # Under the menu the hand passes over it.
    hits = rb.read()["hits"]
    g.wait(20)
    assert rb.read()["hits"] == hits
    # B closes it; the CSS is ours again.
    g.tap("B")
    g.wait(40)
    assert not menu_open(g)
    g.wait(10)
    assert hand_button(g) == BUTTON_RULES
    g.shot("04-menu-closed")

    # A game starting: greyed, the hand passes over it, A does nothing.
    g.hand_to(20.0, 12.0, tol=0.5)
    sim.room.status = 1
    g.wait(20)
    g.shot("05-greyed")
    g.hand_to(*rb.centre(), tol=0.4)
    g.wait(5)
    assert hand_button(g) != BUTTON_RULES
    g.tap("A")
    g.wait(30)
    assert rb.read()["presses"] == 1 and not menu_open(g)
    sim.room.status = 0
    g.wait(20)

    # The status line ends before it: "Waiting on: Sarah, alice" (397 font units) no longer fits
    # on one line there and takes two (DEBUG scratch[14] >> 24: online_menu.cpp LAYOUT_TWO).
    sim.add_player(1, "alice")
    g.wait(40)
    g.hand_to(20.0, 12.0, tol=0.5)
    g.wait(10)
    assert sim.status_line() == "Waiting on: Sarah, alice" and g.scratch(14) >> 24 == 2, hex(g.scratch(14))
    g.shot("06-status-line")

    # Not the host: A opens the same menu (read-only, RoomRules), B closes it, nothing is sent.
    sim.room.host = 1
    g.wait(30)
    g.hand_to(*rb.centre(), tol=0.4)
    g.tap("A")
    g.wait(40)
    assert rb.read()["presses"] == 2 and menu_open(g)
    g.shot("07-viewer-menu")
    g.tap("B")
    g.wait(40)
    assert not menu_open(g) and not sim.room_ops("rules")
    sim.room.host = 0
    g.wait(20)

    # Out of the room (Join Room's CSS): no RULES; the hand where it was finds nothing.
    to_join_room(g)
    made0 = rb.read()["made"]
    g.hand_to(*rb.centre(), tol=0.4)
    g.wait(10)
    assert hand_button(g) != BUTTON_RULES
    g.shot("08-left-the-room")

    # Back to WITH FRIENDS (the CSS's task and our model deleted) and a room again: made again.
    g.hold("B", 120)
    sim.service(600, until=lambda: g.scene() == "muMenuMain")
    g.wait(60)
    friends_to(g, 0)
    g.tap("A")
    sim.service(400, until=lambda: g.scene() == "scSelctCharacter" and sim.room is not None)
    g.wait(60)
    st = rb.read()
    assert st["made"] == made0 + 1 and abs(st["rect"][0] - l) < 0.05, st
    g.hand_to(*rb.centre(), tol=0.4)
    g.wait(10)
    assert hand_button(g) == BUTTON_RULES
    g.shot("09-room-again")


@pytest.fixture
def rules_game_4_3(dolphin_exe):
    g = Game("rules-button-4x3", art=RULES_ART, config_args=("SYSCONF.IPL.AR=0",))
    yield g
    g.close()


def test_rules_button_at_4_3(rules_game_4_3: Game) -> None:
    """The bar at 4:3 (the Wii set to 4:3): RULES, PUBLIC and FFA all on screen."""
    g = rules_game_4_3
    g.c.game_bridge_config(enabled=False)
    create_room(g)
    rb = RulesButton(g)
    assert rb.read()["made"] == 1
    g.shot("4x3-room-css")
    g.hand_to(*rb.centre(), tol=0.4)
    g.wait(20)
    assert hand_button(g) == BUTTON_RULES
    g.shot("4x3-hover")
    # The ruleset / set line with the longest names, at 4:3.
    long_set_line(g)
    g.shot("4x3-long-ruleset")


# ---- the room's ruleset and set (room_play.cpp; docs/game-code.md §14 "The Ruleset line and the set")
PLAY_ART = ROOT / "run" / "artifacts" / "game-code" / "rooms" / "ruleset-set"
RULESETS = [("NQRBTW", "NorCal Weekly v3"), ("KDFGHJ", "Big House Bo5")]
RULESET_DB = {"NQRBTW": ("NorCal Weekly v3", 3), "KDFGHJ": ("Big House Bo5", 5), "BCDFGH": ("Typed Code", 3)}


class RoomPlayView:
    """g_roomPlay (room_play.cpp) and the room CSS's ruleset / set line (online_menu.cpp)."""

    def __init__(self, g: Game):
        self.g = g
        self.addr = ppom_sym(g, "g_roomPlay")
        self.line_addr = ppom_sym(g, "s_playPrinted")

    def read(self) -> dict:
        d = self.g.c.read_mem(self.addr, 0x64)
        text = lambda b: b.split(b"\0")[0].decode("ascii", "replace")  # noqa: E731
        return {"posts": struct.unpack_from(">I", d, 0)[0], "op": d[4], "arg": d[5], "browse": d[6],
                "choice": d[7], "code": text(d[8:16]), "ruleset": text(d[20:60]), "set": text(d[60:100])}

    def line(self) -> str:
        return self.g.c.read_mem(self.line_addr, 128).split(b"\0")[0].decode("ascii", "replace")


def panel_stars(g: Game) -> list[int]:
    """The stars each panel shows, in slot order with the local player in slot 0 (room_css.cpp:
    s_myStarsShown for the local panel, s_shown[area].stars, Shown +4, 22 bytes each)."""
    me = struct.unpack(">i", g.c.read_mem(ppom_sym(g, "s_myStarsShown__7RoomCss"), 4))[0]
    shown = g.c.read_mem(ppom_sym(g, "s_shown__7RoomCss"), 22 * 4)
    return [me] + [shown[22 * i + 4] for i in range(1, 4)]


def open_rules_line(g: Game, line: int) -> None:
    """The RULES button, then the cursor down to `line` of the RULES page."""
    rb = RulesButton(g)
    g.hand_to(*rb.centre(), tol=0.4)
    g.tap("A")
    g.wait(60)
    assert menu_open(g)
    for _ in range(line):
        g.tap("DDOWN", 4)
        g.wait(8)
    g.wait(10)


def close_rules(g: Game) -> None:
    g.tap("B")
    g.wait(40)
    assert not menu_open(g)


@pytest.fixture
def play_game(dolphin_exe):
    g = Game("ruleset-set", art=PLAY_ART)
    yield g
    g.close()


def test_room_ruleset_and_set(play_game: Game) -> None:
    g = play_game
    g.c.game_bridge_config(enabled=False)
    create_room(g)
    sim = g.sim
    sim.rulesets = list(RULESETS)
    sim.ruleset_db = dict(RULESET_DB)
    sim.add_player(1, "alice")
    g.wait(40)
    v = RoomPlayView(g)
    g.hand_to(20.0, 12.0, tol=0.5)
    g.wait(10)
    assert v.line() == "Ruleset: P+ default   Loser picks the stage", v.line()
    g.shot("01-room-css")

    # RULESET (the STAGE CHOICE row) shows the room's; right: the list (CMD_RULESETS); A picks.
    open_rules_line(g, 4)
    st = v.read()
    assert (st["ruleset"], st["set"], st["browse"]) == ("P+ default", "No set", 0xFF), st
    assert any(cmd == ppom.CMD_RULESETS for _s, cmd, _p in sim.requests), "no CMD_RULESETS"
    g.shot("02-ruleset-line")
    g.tap("DRIGHT", 4)
    g.wait(15)
    assert v.read()["ruleset"] == "NorCal Weekly v3"
    g.shot("03-browse")
    g.tap("A", 4)
    g.wait(40)
    st = v.read()
    assert not menu_open(g) and (st["op"], st["code"]) == (10, "NQRBTW"), st
    assert sim.room.ruleset_name == "NorCal Weekly v3" and sim.room.best_of == 3
    g.hand_to(20.0, 12.0, tol=0.5)
    g.wait(10)
    assert v.line() == "Ruleset: NorCal Weekly v3   Best of 3", v.line()
    g.shot("04-picked")

    # "Enter code" (the last entry, left twice from NorCal's): the keypad's 6-letter mode.
    open_rules_line(g, 4)
    g.tap("DLEFT", 4)
    g.wait(10)
    g.tap("DLEFT", 4)
    g.wait(10)
    assert v.read()["ruleset"] == "Enter code", v.read()
    g.tap("A", 4)
    g.wait(60)
    assert not menu_open(g) and keypad_open(g)
    g.shot("05-keypad")
    keypad_type(g, "BCDFGH")
    g.shot("06-code-typed")
    g.tap("START")
    g.wait(60)
    assert not keypad_open(g)
    assert ppom.from_u16s(sim.room_ops("ruleset")[-1][4:4 + 2 * ppom.CODE_LEN]) == "BCDFGH"
    assert sim.room.ruleset_name == "Typed Code"
    assert ppom.read_local(g.c, sim.b)["lock"]["ready"] == 0, "the keypad's START locked in"
    # A code mm does not know: the room's error.
    open_rules_line(g, 4)
    g.tap("DLEFT", 4)
    g.wait(10)
    g.tap("A", 4)
    g.wait(60)
    keypad_type(g, "BBBBBB")
    g.tap("START")
    g.wait(60)
    assert sim.error and sim.text == "Ruleset not found." and sim.room.ruleset_name == "Typed Code"
    g.shot("07-ruleset-not-found")
    sim.text, sim.error = "", 0

    # A set: the score on the CSS (sides by slot), and the ruleset locked.
    r = sim.room
    r.set_on, r.set_game, r.score, r.sides = True, 2, [1, 0], [0, 1]
    g.wait(30)
    assert v.line() == "Ruleset: Typed Code   Bo3: Sarah 1 - 0 alice", v.line()
    g.shot("08-set-score")
    posts = v.read()["posts"]
    open_rules_line(g, 4)
    g.tap("DRIGHT", 4)
    g.tap("A", 4)
    g.wait(20)
    assert v.read()["browse"] == 0xFF and v.read()["posts"] == posts and menu_open(g)
    close_rules(g)

    # The score on the panels: Brawl's win stars, one a game won (no host star during a set).
    r.score = [0, 2]
    g.wait(30)
    assert panel_stars(g)[:2] == [0, 2], panel_stars(g)
    r.score = [1, 0]
    g.wait(30)
    assert panel_stars(g)[:2] == [1, 0], panel_stars(g)

    # Paused because a player left (rules.md §1.1 A): the set waits for them; the host can only
    # end it (End set, right), A on the score does nothing.
    r.set_flags = 1
    g.wait(30)
    assert v.line().endswith("(paused)"), v.line()
    posts = v.read()["posts"]
    open_rules_line(g, 2)
    assert v.read()["set"] == "Paused 1-0"
    g.tap("A", 4)
    g.wait(10)
    g.tap("DRIGHT", 4)
    g.wait(10)
    assert v.read()["set"] == "End set" and v.read()["posts"] == posts and menu_open(g), v.read()
    g.shot("09-paused-waiting")
    g.tap("DLEFT", 4)
    g.wait(10)
    assert v.read()["set"] == "Paused 1-0"
    # The results differed, everyone there: Replay game first, End set; A sends ROOM_SET.
    r.set_flags = 3
    g.wait(20)
    assert v.read()["set"] == "Replay game", v.read()
    g.tap("DRIGHT", 4)
    g.wait(10)
    assert v.read()["set"] == "End set"
    g.tap("DLEFT", 4)
    g.wait(10)
    assert v.read()["set"] == "Replay game"
    g.shot("10-results-differed")
    g.tap("A", 4)
    g.wait(40)
    st = v.read()
    assert not menu_open(g) and (st["op"], st["arg"]) == (9, 1) and r.set_flags == 0, st
    # End set: differed again, right to End set, A.
    r.set_flags = 3
    g.wait(20)
    open_rules_line(g, 2)
    g.tap("DRIGHT", 4)
    g.wait(10)
    assert v.read()["set"] == "End set"
    g.tap("A", 4)
    g.wait(40)
    st = v.read()
    assert (st["op"], st["arg"]) == (9, 0) and not r.set_on and r.score == [0, 0], st
    g.wait(20)
    assert panel_stars(g)[:2] == [1, 0], panel_stars(g)   # no set: the host's star again

    # Not the host: both lines read-only.
    r.set_on, r.score, r.set_flags = True, [0, 1], 3
    r.host = 1
    g.wait(30)
    posts = v.read()["posts"]
    open_rules_line(g, 2)
    assert v.read()["set"] == "Paused 0-1"
    g.tap("DRIGHT", 4)
    g.tap("A", 4)
    g.wait(20)
    g.tap("DDOWN", 4)
    g.tap("DDOWN", 4)
    g.wait(10)
    g.tap("DRIGHT", 4)
    g.tap("A", 4)
    g.wait(20)
    st = v.read()
    assert st["posts"] == posts and st["browse"] == 0xFF and menu_open(g), st
    g.shot("11-viewer")
    close_rules(g)

    # A 2v2 set: every member of a side shows its wins (sideSlots: slots 1+3 against 2+4).
    r.host = 0
    r.open = [True, True, True, True]
    sim.add_player(2, "bob")
    sim.add_player(3, "carol")
    r.teams = True
    r.set_on, r.set_flags, r.score, r.side_slots = True, 0, [2, 1], [0b0101, 0b1010]
    g.wait(60)
    assert panel_stars(g) == [2, 1, 2, 1], panel_stars(g)
    assert v.line() == "Ruleset: Typed Code   Bo3: Sarah 2 - 1 alice", v.line()
    g.shot("12-2v2-stars")


def test_host_changes_the_set_length(play_game: Game) -> None:
    """The SET line (staging feedback 2026-10-12: left / right did nothing): the host's left / right
    pick No set / Best of 3 / 5 / 7, sent as ROOM_BEST_OF when the menu closes; the room's rules
    become Custom; a viewer can't change it, nor the host while a set is on."""
    g = play_game
    g.c.game_bridge_config(enabled=False)
    create_room(g)
    sim = g.sim
    sim.add_player(1, "alice")
    g.wait(40)
    v = RoomPlayView(g)
    open_rules_line(g, 2)
    assert v.read()["set"] == "No set"
    g.tap("DRIGHT", 4)
    g.wait(10)
    assert v.read()["set"] == "Best of 3", v.read()
    g.tap("DRIGHT", 4)
    g.wait(10)
    assert v.read()["set"] == "Best of 5"
    g.shot("13-set-length")
    g.tap("DLEFT", 4)
    g.wait(10)
    assert v.read()["set"] == "Best of 3"
    assert not sim.room_ops("best_of"), "sent before the menu closed"
    close_rules(g)
    g.wait(20)
    ops = sim.room_ops("best_of")
    assert len(ops) == 1 and ops[0][1] == 3, ops
    assert sim.room.best_of == 3 and sim.room.custom
    g.hand_to(20.0, 12.0, tol=0.5)
    g.wait(10)
    assert v.line() == "Ruleset: Custom   Best of 3", v.line()
    g.shot("14-best-of-3")
    # Around to No set (3 -> 5 -> 7 -> none).
    open_rules_line(g, 2)
    assert v.read()["set"] == "Best of 3"
    for want in ("Best of 5", "Best of 7", "No set"):
        g.tap("DRIGHT", 4)
        g.wait(10)
        assert v.read()["set"] == want, v.read()
    close_rules(g)
    g.wait(20)
    assert sim.room_ops("best_of")[-1][1] == 0 and sim.room.best_of == 0
    # Unchanged: nothing is sent.
    open_rules_line(g, 2)
    g.tap("DRIGHT", 4)
    g.tap("DLEFT", 4)
    g.wait(10)
    close_rules(g)
    g.wait(20)
    assert len(sim.room_ops("best_of")) == 2
    # During a set the length is locked; the host can end the set (End set, the user's of
    # 2026-10-12): ROOM_SET 0, the set gone, the length free again.
    r = sim.room
    r.best_of, r.set_on, r.score = 3, True, [1, 0]
    g.wait(30)
    open_rules_line(g, 2)
    assert v.read()["set"] == "Bo3 1-0"
    g.tap("DRIGHT", 4)
    g.wait(10)
    assert v.read()["set"] == "End set", v.read()
    g.tap("DRIGHT", 4)
    g.wait(10)
    assert v.read()["set"] == "Bo3 1-0"
    g.tap("DLEFT", 4)
    g.wait(10)
    g.shot("16-end-set")
    g.tap("A", 4)
    g.wait(40)
    st = v.read()
    assert not menu_open(g) and (st["op"], st["arg"]) == (9, 0) and not r.set_on, st
    assert len(sim.room_ops("best_of")) == 2
    open_rules_line(g, 2)
    assert v.read()["set"] == "Best of 3"
    g.tap("DRIGHT", 4)
    g.wait(10)
    assert v.read()["set"] == "Best of 5"
    g.tap("DLEFT", 4)
    g.wait(10)
    close_rules(g)
    g.wait(20)
    assert len(sim.room_ops("best_of")) == 2
    # Not the host: read-only.
    r.host = 1
    g.wait(30)
    open_rules_line(g, 2)
    g.tap("DRIGHT", 4)
    g.wait(10)
    assert v.read()["set"] == "Best of 3"
    g.shot("15-viewer-set-length")
    close_rules(g)
    g.wait(20)
    assert len(sim.room_ops("best_of")) == 2


def test_viewer_opens_rules_panels_stay(play_game: Game) -> None:
    """A player who is not the host (in slot 3: their panel drawn two slots right) opens RULES to
    look and closes it: the panels stay where they are the whole time and nothing about the room
    changes (staging feedback 2026-10-12: the portraits swapped places while the menu was open and
    swapped back, as if the player had entered the room again)."""
    g = play_game
    g.c.game_bridge_config(enabled=False)
    create_room(g)
    sim = g.sim
    to_join_room(g)                                 # a character: START then enters a code
    sim.joinable["KFQB"] = roomsim.Room(code="KFQB", host=1, open=[True, True, True, False],
                                        members=[roomsim.Member("carol", team=0),
                                                 roomsim.Member("bob", team=1), None, None])
    g.tap("START")
    g.wait(40)
    keypad_type(g, "KFQB")
    g.tap("START")
    g.wait(90)
    assert sim.room is not None and sim.me() == 2
    want = [round(g.panel_offset(i), 2) for i in range(4)]
    assert abs(want[0] - 2 * SLOT_POS) < 0.1 and abs(want[2] + 2 * SLOT_POS) < 0.1, want
    joins, leaves = len(sim.room_ops("join")), len(sim.room_ops("leave"))
    g.shot("17-viewer-before")
    rb = RulesButton(g)
    g.hand_to(*rb.centre(), tol=0.4)
    g.tap("A")
    seen = []
    for _ in range(8):
        g.wait(10)
        seen.append([round(g.panel_offset(i), 2) for i in range(4)])
    assert menu_open(g)
    g.shot("18-viewer-rules-open")
    g.tap("B")
    for _ in range(6):
        g.wait(10)
        seen.append([round(g.panel_offset(i), 2) for i in range(4)])
    assert not menu_open(g)
    g.shot("19-viewer-rules-closed")
    assert all(o == want for o in seen), (want, seen)
    assert (len(sim.room_ops("join")), len(sim.room_ops("leave"))) == (joins, leaves)
    assert sim.room is not None and sim.me() == 2 and sim.room.members[2] is not None


LONG_RULESET = "NorCal Weekly Bo3 Tournament v1"          # 31 characters: all RoomPlay.name holds
LONG_NAMES = ("Sarahkittyalpha", "Bobthebuilderxy")   # 15 characters each


def long_set_line(g: Game) -> None:
    """A room with the longest ruleset name and two long player names, in a set: the line under the
    grid shows all of it (staging feedback 2026-10-12: it was cut), on two lines."""
    sim = g.sim
    sim.name = LONG_NAMES[0]
    r = sim.room
    r.members[0].name = LONG_NAMES[0]
    sim.add_player(1, LONG_NAMES[1])
    r.ruleset_name, r.ruleset_code, r.best_of = LONG_RULESET, "NQRBTW", 3
    r.set_on, r.score, r.sides = True, [1, 0], [0, 1]
    g.wait(40)
    g.hand_to(20.0, 12.0, tol=0.5)
    g.wait(10)
    want = "Ruleset: " + LONG_RULESET + "\nBo3: %s 1 - 0 %s" % LONG_NAMES
    assert RoomPlayView(g).line() == want, RoomPlayView(g).line()
    # a short one stays on one line
    r.ruleset_name = "Short"
    g.wait(30)
    assert RoomPlayView(g).line() == "Ruleset: Short   Bo3: %s 1 - 0 %s" % LONG_NAMES, RoomPlayView(g).line()
    r.ruleset_name = LONG_RULESET
    g.wait(30)


def test_room_play_line_long_names(play_game: Game) -> None:
    g = play_game
    g.c.game_bridge_config(enabled=False)
    create_room(g)
    long_set_line(g)
    g.shot("20-long-ruleset-16x9")


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


TOKEN_ART = ROOT / "run" / "artifacts" / "game-code" / "rooms" / "token"
# Portraits (CSS ids, hand positions in the CSS's units: centroids seen while scanning).
TOKEN_CHARS = [(0x13, (-15.3, 10.4)), (0x24, (3.6, 10.7)), (0x07, (-21.3, 10.7)), (0x2B, (-5.8, 5.9))]


def css_area(g: Game):
    from ppharness import brawl as B
    return B.read_css_area(g.c.read_mem, 0)


def token_round(g: Game, css: int, pos: tuple[float, float], gap: int, label: str, log: list) -> None:
    """Place the token on `css`, A on it again `gap` frames after the placing press (the hand
    still on it: it goes back into the hand), then over another portrait and placed there."""
    from ppharness import brawl as B
    g.hand_to(*pos, tol=0.6)
    g.wait(4)
    a = css_area(g)
    assert a.in_hand and a.hand_target == B.CSS_HAND_GRID_HOLDING and a.character == css, (label, a)
    g.c.pad_script(0, [PadInput(buttons=["A"], hold=2), PadInput(hold=gap), PadInput(buttons=["A"], hold=2),
                       PadInput(hold=4)])
    seen = []
    for _ in range(gap + 30):
        g.sim.service(1)
        a = css_area(g)
        seen.append((a.hand_target, a.character, a.in_hand, a.flying))
    a = css_area(g)
    log.append((label, gap, seen[:6], seen[-1]))
    g.shot(f"{label}-gap{gap:02d}")
    # Taken back: in the hand, and the CSS shows the portrait under it as the hand moves.
    assert a.in_hand and not a.flying, (label, gap, a, seen)
    nxt_css, nxt_pos = next((c, p) for c, p in TOKEN_CHARS if c != css)
    g.hand_to(*nxt_pos, tol=0.6)
    g.wait(4)
    a = css_area(g)
    assert a.in_hand and a.hand_target == B.CSS_HAND_GRID_HOLDING and a.character == nxt_css, (label, gap, a, seen)
    g.tap("A")
    g.wait(20)
    a = css_area(g)
    assert a.placed and a.character == nxt_css, (label, gap, a)
    # and up again with A on it once still, for the next round
    g.tap("A")
    g.wait(20)
    a = css_area(g)
    assert a.in_hand and not a.placed, (label, gap, a)


def token_rounds(g: Game, label: str) -> None:
    log: list = []
    try:
        for i, gap in enumerate((1, 2, 3, 5, 8, 12, 20, 40)):
            css, pos = TOKEN_CHARS[i % len(TOKEN_CHARS)]
            token_round(g, css, pos, gap, label, log)
    except AssertionError:
        for row in log:   # (label, gap, the first frames' (target, character, in hand, +0x1F9), the last)
            print(row, flush=True)
        raise


@pytest.fixture
def token_game(dolphin_exe):
    g = Game("token", art=TOKEN_ART)
    yield g
    g.close()


def test_token_picked_up_and_placed_again(token_game: Game) -> None:
    """A on one's own placed token takes it back into the hand, and it is placed again, however
    soon after it went down (staging feedback 2026-10-12: picked up with A it sometimes stayed in
    the hand invisible, the CSS showing no character under it, and could not be placed again):
    as the host in slot 1, then as a player in slot 3 (the local panel's models drawn two slots
    right)."""
    g = token_game
    g.c.game_bridge_config(enabled=False)
    create_room(g)
    sim = g.sim
    sim.add_player(1, "alice")
    g.wait(30)
    g.hand_to(*TOKEN_CHARS[0][1], tol=0.6)
    g.tap("A")
    g.wait(20)
    g.tap("A")                                      # up again: the rounds start with it in hand
    g.wait(20)
    token_rounds(g, "host")
    # Leave, join a room in slot 3.
    to_join_room(g, TOKEN_CHARS[0][1])
    sim.joinable["KFQB"] = roomsim.Room(code="KFQB", host=1, open=[True, True, True, False],
                                        members=[roomsim.Member("carol", team=0),
                                                 roomsim.Member("bob", team=1), None, None])
    g.tap("START")
    g.wait(40)
    keypad_type(g, "KFQB")
    g.tap("START")
    g.wait(90)
    assert sim.room is not None and sim.me() == 2
    g.tap("A")                                      # the token put back on its portrait: up
    g.wait(20)
    if not css_area(g).in_hand:
        g.hand_to(*TOKEN_CHARS[0][1], tol=0.6)
        g.tap("A")
        g.wait(20)
    token_rounds(g, "slot3")
