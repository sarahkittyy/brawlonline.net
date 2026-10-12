"""The room's STAGE SWITCH on one instance (game-code/PPOnline/source/stage_switch.cpp, docs/design/
room-rules.md §2): P+'s RANDOM STAGE SWITCH page of the rules menu, opened in a room, shows the room's
stages (SESSION RoomStages) and the host edits them; tools/gamecode/roomsim.py plays Dolphin's side.

- The host: the page shows the room's stages (P+'s switch is the room's while the menu is open, the
  player's own after), starters labelled STR; A cycles a stage Off -> Counterpick -> Starter, A on
  the top bar switches the loser to the list, Z the hazards; closing the menu sends ROOM_STAGES
  and the hazards with the rules.
- Another player: the same page, read-only; nothing is sent.

Screenshots: run/artifacts/game-code/rooms/stage-switch/.
"""
from __future__ import annotations

import struct

import pytest

from test_rooms_ui import ROOT, Game, PadInput, ppom, roomsim

pytestmark = [pytest.mark.dolphin, pytest.mark.gpu]

ART = ROOT / "run" / "artifacts" / "game-code" / "rooms" / "stage-switch"
RSS_EXDATA = 0x8042C4E8
TASK_RULES, TASK_RULES_OPEN, RULES_SWITCH = 0x62C, 0x630, 0x274
PAGE_COL, PAGE_ROW = 0x670, 0x674
# Page 1's positions (launcher/src/common/stages.ts): Yoshi's Story 0, Fountain of Dreams 9, FD 10, BF 17.
YS, FOD, FD, BF, SV, DL, PS2 = 0x2A, 0x1F, 0x02, 0x01, 0x21, 0x2D, 0x2E
POS = {YS: 0, FOD: 9, FD: 10, BF: 17}


@pytest.fixture
def game(dolphin_exe):
    g = Game("stage-switch", art=ART)
    yield g
    g.close()


def task(g: Game) -> int:
    return g.u32(g.u32(g.u32(0x805A0060) + 4) + 0x400)


def menu_open(g: Game) -> bool:
    return g.u32(task(g) + TASK_RULES_OPEN) != 0


def page(g: Game) -> int:
    return g.u32(g.u32(task(g) + TASK_RULES) + RULES_SWITCH)


def cursor(g: Game) -> tuple[int, int]:
    return struct.unpack(">2i", g.c.read_mem(page(g) + PAGE_COL, 8))


def goto(g: Game, col: int, row: int) -> None:
    for _ in range(30):
        c, r = cursor(g)
        if (c, r) == (col, row):
            return
        g.tap("DDOWN" if r < row else "DUP" if r > row else "DRIGHT" if c < col else "DLEFT")
        g.wait(10)
    raise AssertionError(f"cursor at {cursor(g)}, not {(col, row)}")


def goto_stage(g: Game, kind: int) -> None:
    p = POS[kind]
    goto(g, p % 5, p // 5 + 1)


def open_switch(g: Game) -> None:
    """RULES (CFG_TEST_OPEN_RULES, as the RULES button), MORE RULES, its STAGE SWITCH line."""
    ppom.set_cfg_bits(g.c, ppom.CFG_TEST_OPEN_RULES)
    g.wait(60)
    assert menu_open(g), "the rules menu did not open"
    for k in ["DDOWN"] * 5 + ["DRIGHT", "A"] + ["DDOWN"] * 5 + ["A"]:
        g.tap(k)
        g.wait(12)
    g.wait(60)
    assert cursor(g) == (0, 0), cursor(g)


def close_menu(g: Game) -> None:
    for _ in range(5):
        if not menu_open(g):
            break
        g.tap("B")
        g.wait(20)
    assert not menu_open(g)
    g.wait(30)


def switch_bits(g: Game) -> int:
    """P+'s switch, page 1's positions 0..30."""
    return struct.unpack(">I", g.c.read_mem(RSS_EXDATA + 2, 4))[0]


def page1_bits(kinds: list[int]) -> int:
    pos = {YS: 0, 0x06: 1, 0x19: 2, 0x43: 3, 0x47: 4, 0x49: 5, 0x1D: 6, 0x0D: 7, 0x1C: 8, FOD: 9, FD: 10,
           0x05: 11, DL: 12, 0x0C: 13, 0x09: 14, 0x23: 15, SV: 16, BF: 17, PS2: 18, 0x03: 19, 0x04: 20}
    return sum(1 << pos[k] for k in kinds if k in pos)


def in_room(g: Game, host: bool) -> roomsim.RoomSim:
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
    if not host:
        sim.room.host = 1
    sim.room.rules = bytes(sim.room.rules[:0x10]) + b"\0" + bytes(sim.room.rules[0x11:])   # hazards off
    g.wait(40)
    return sim


def test_host_edits_the_rooms_stages(game: Game) -> None:
    g = game
    sim = in_room(g, host=True)
    own = g.c.read_mem(RSS_EXDATA, 30)
    room = sim.room.starters + sim.room.counterpicks
    open_switch(g)
    assert switch_bits(g) == page1_bits(room), (hex(switch_bits(g)), hex(page1_bits(room)))
    g.shot("01-open")

    # FD: starter -> off; Fountain of Dreams: counterpick -> starter; Yoshi's Story: off ->
    # counterpick; the bar: the loser picks from the list; Z: hazards on.
    for kind in (FD, FOD, YS):
        goto_stage(g, kind)
        g.tap("A")
        g.wait(10)
    goto(g, 0, 0)
    g.tap("A")
    g.wait(10)
    g.c.pad_script(0, [PadInput(buttons=["Z"], hold=3), PadInput(hold=6)])
    g.wait(15)
    g.shot("02-edited")
    # L / R (P+'s presets) change nothing.
    g.tap("R")
    g.wait(15)
    assert switch_bits(g) == page1_bits(room), "a preset was loaded"
    close_menu(g)
    g.wait(30)

    assert g.c.read_mem(RSS_EXDATA, 30) == own, "the player's own switch was not put back"
    assert len(sim.stages_requests) == 1, sim.stages_requests
    choice, starters, counterpicks = sim.stages_requests[0]
    assert choice == 3
    assert starters == sorted([BF, FOD, SV, DL, PS2]), starters
    assert YS in counterpicks and FD not in counterpicks and FD not in starters, counterpicks
    assert sim.room.rules[0x10] == 1, "the hazards did not go with the rules"
    # The page shows the room's new stages when it opens again.
    open_switch(g)
    assert switch_bits(g) == page1_bits(starters + counterpicks)
    g.shot("03-reopened")
    close_menu(g)


def test_another_player_only_looks(game: Game) -> None:
    g = game
    sim = in_room(g, host=False)
    open_switch(g)
    before = switch_bits(g)
    for kind in (FD, YS):
        goto_stage(g, kind)
        g.tap("A")
        g.wait(10)
    goto(g, 0, 0)
    g.tap("A")
    g.c.pad_script(0, [PadInput(buttons=["Z"], hold=3), PadInput(hold=6)])
    g.wait(15)
    assert switch_bits(g) == before
    g.shot("01-viewer")
    close_menu(g)
    assert not sim.stages_requests, sim.stages_requests
