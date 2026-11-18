"""The room's STAGE SWITCH on one instance (game-code/PPOnline/source/stage_switch.cpp, docs/design/
room-rules.md §2): P+'s RANDOM STAGE SWITCH page of the rules menu, opened in a room, shows the room's
stages (SESSION RoomStages) and the host edits them; tools/gamecode/roomsim.py plays Dolphin's side.

- The host: the page shows the room's stages (P+'s switch is the room's while the menu is open, the
  player's own after), starters labelled STR; A cycles a stage Off -> Counterpick -> Starter, A on
  the top bar switches the loser to the list, Z on the top row the default hazards and Z on a stage
  that stage's own (the rules' hazardStages, 2026-10-26); closing the menu sends ROOM_STAGES and
  the hazards with the rules.
- Another player: the same page, read-only; nothing is sent.
- The cursor's frame is on its own tile on every frame of a move (it showed for a frame over
  another stage, staging feedback 2026-10-12).
- A set: the bar is ON (a set's loser always picks from the list) and A on it changes nothing.
- The footer's two lines (the stage, and the legend: ON / STR / OFF and the hazards) over the
  CSS's READY TO FIGHT banner (screenshots).

Screenshots: run/artifacts/game-code/rooms/stage-switch/.
"""
from __future__ import annotations

import struct

import pytest

from ppharness import brawl as B

from test_rooms_ui import FOX, ROOT, Game, PadInput, ppom, roomsim

pytestmark = [pytest.mark.dolphin, pytest.mark.gpu]

ART = ROOT / "run" / "artifacts" / "game-code" / "rooms" / "stage-switch"
TASK_RULES, TASK_RULES_OPEN, RULES_SWITCH = 0x62C, 0x630, 0x274
PAGE_COL, PAGE_ROW, PAGE_TILES, PAGE_TILES_2 = 0x670, 0x674, 0x704, 0x730
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


def tile(g: Game) -> int:
    """The tile under the cursor, -1 on the top row. The game's cursor counts the page's first 31
    tiles in rows of 5 (columns 0-4) and the rest in rows of 2 right of them (columns 5-6)."""
    c, r = cursor(g)
    if r < 1:
        return -1
    return (r - 1) * 5 + c if c < 5 else 31 + (r - 1) * 2 + (c - 5)


def to_bar(g: Game) -> None:
    for _ in range(8):
        c, r = cursor(g)
        if r == 0:
            break
        g.tap("DUP")
        g.wait(10)
    if cursor(g)[0] >= 5:   # the page button
        g.tap("DLEFT")
        g.wait(10)
    assert cursor(g) == (0, 0), cursor(g)


def page1(g: Game) -> list[int]:
    """The build's first stage page, as stage kinds by position (P+'s: 21, POS; a mod's own)."""
    return B.read_stage_pages(g.c.read_mem)[0]


def tiles(g: Game) -> int:
    """How many tiles the page shows: its two groups', 31 and 10 at the most (P+'s pages have 21
    stages at the most, REX's first 37)."""
    return g.u32(page(g) + PAGE_TILES) + g.u32(page(g) + PAGE_TILES_2)


def by_role(g: Game, starters: list[int], counterpicks: list[int]) -> tuple[int, int, int]:
    """A starter, a counterpick and a stage that is off, of those the build's first page shows:
    P+'s FD, Fountain of Dreams and Yoshi's Story where the page has them so (a build with its own
    stage kinds: REX's Fountain of Dreams is 0x07), else the first starter, the last counterpick
    (on a page with more than 31 stages one of its second group of tiles) and the first one off."""
    shown = page1(g)[:41]

    def pick(want: int, pool: list[int], last: bool = False) -> int:
        return want if want in pool else pool[-1 if last else 0]
    return (pick(FD, [k for k in shown if k in starters]), pick(FOD, [k for k in shown if k in counterpicks], last=True),
            pick(YS, [k for k in shown if k not in starters and k not in counterpicks]))


def goto_stage(g: Game, kind: int) -> None:
    """Down from the bar to the stage's row, then along it: the page is drawn in rows of 7 (page 1's
    21 stages: three full rows), and the cursor moves so (down from the bar keeps the column)."""
    p = page1(g).index(kind)
    to_bar(g)
    g.tap("DDOWN")
    g.wait(10)
    while tile(g) // 7 < p // 7:
        g.tap("DDOWN")
        g.wait(10)
    for _ in range(7):
        if tile(g) == p:
            break
        g.tap("DRIGHT" if tile(g) < p else "DLEFT")
        g.wait(10)
    assert tile(g) == p, (tile(g), p)


def cursor_xy(g: Game) -> tuple[float, float]:
    """The cursor frame's place (its ScnMdl's world matrix), world units."""
    obj = g.u32(g.u32(page(g) + 0x64C) + 0x81 * 4)
    m = struct.unpack(">12f", g.c.read_mem(g.u32(obj + 0xC) + 0x34, 48))
    return round(m[5], 1), round(m[9], 1)


def tile_xy(t: int, n: int = 21) -> tuple[float, float]:
    """Tile t's place in rows of 7 (a short last row centred), from tile 0 at (-25.3, 10.9)."""
    last = (n - 1) // 7
    col = t % 7 + ((7 - (n - last * 7)) * 0.5 if t // 7 == last else 0)
    return round(-25.3 + col * 8.4, 1), round(10.9 - (t // 7) * 4.4, 1)


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
    """P+'s switch of page 1: {u16 positions 31.., u32 positions 0..30} (a build's page may be
    longer than 31: REX's has 37)."""
    ext, main = struct.unpack(">HI", g.c.read_mem(B.stage_random_switch(g.c.read_mem), 6))
    return ext << 31 | main


def page1_bits(g: Game, kinds: list[int]) -> int:
    """The switch of page 1 with `kinds` on, by the build's own page (P+'s: Yoshi's Story 0 ...
    Luigi's Mansion 20, POS)."""
    pos = {k: i for i, k in enumerate(page1(g))}
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
    own = g.c.read_mem(B.stage_random_switch(g.c.read_mem), 30)
    room = sim.room.starters + sim.room.counterpicks
    room_starters = list(sim.room.starters)
    # The stages edited, by what they are in the room (P+'s: FD, Fountain of Dreams, Yoshi's
    # Story), and the room's one stage with hazards of its own: the counterpick (P+'s room has
    # Fountain of Dreams so already).
    starter, counterpick, off = by_role(g, sim.room.starters, sim.room.counterpicks)
    sim.room.rules = bytes(sim.room.rules[:0x18]) + bytes([counterpick]).ljust(8, b"\0") + bytes(sim.room.rules[0x20:])
    g.wait(40)
    open_switch(g)
    assert switch_bits(g) == page1_bits(g, room), (hex(switch_bits(g)), hex(page1_bits(g, room)))
    g.shot("01-open")

    # The starter -> off; the counterpick -> starter; the stage that is off -> counterpick; the
    # bar: the loser picks from the list; Z: hazards on.
    for kind in (starter, counterpick, off):
        goto_stage(g, kind)
        g.tap("A")
        g.wait(10)
    to_bar(g)
    g.tap("A")
    g.wait(10)
    g.c.pad_script(0, [PadInput(buttons=["Z"], hold=3), PadInput(hold=6)])
    g.wait(15)
    g.shot("02-edited")
    # Z on a stage: its own hazards. The counterpick (the room's one stage the other way) goes
    # back to the default (now on); the stage that was off twice: its own, then not; the starter
    # its own (off).
    for kind, times in ((counterpick, 1), (off, 2), (starter, 1)):
        goto_stage(g, kind)
        for _ in range(times):
            g.c.pad_script(0, [PadInput(buttons=["Z"], hold=3), PadInput(hold=6)])
            g.wait(15)
    g.shot("02b-hazards-off-on-fd")   # the footer: "Hazards here (Z): OFF"
    to_bar(g)
    g.wait(10)
    g.shot("02c-hazards-default")     # "Hazards (Z): ON, OFF on 1 stage"
    # Rows of 7: from the first stage, down is the 8th (Yoshi's Island), then the 15th (Temple of
    # Time), then the bar; down again keeps the column the cursor came up in: the last row's first
    # tile's, which a short, centred last row draws further right (half a column rounds up); left
    # from there goes round the row.
    # (P+'s page 1 has three full rows; a mod's as many as the stages it shows fill.)
    n = tiles(g)
    rows = (n + 6) // 7
    goto_stage(g, page1(g)[0])
    to_bar(g)
    seen = []
    for k in ["DDOWN"] * (rows + 2) + ["DLEFT"]:
        g.tap(k)
        g.wait(10)
        seen.append(tile(g))
    col = int((7 - (n - (rows - 1) * 7)) * 0.5 + 0.5)
    assert seen[:rows + 1] == [7 * r for r in range(rows)] + [-1], seen
    assert seen[rows + 1] == col and seen[-1] == (col - 1) % 7, (seen, col)
    # The cursor's frame on its tile on every frame of a move, across rows of 5 and of 7.
    wrong = []
    for k in ["DRIGHT"] * 3 + ["DDOWN", "DDOWN", "DUP", "DLEFT", "DDOWN"]:
        g.c.pad_script(0, [PadInput(buttons=[k], hold=3), PadInput(hold=6)])
        g.c.pause()   # (frame by frame: the reads all from one frame)
        try:
            for f in range(12):
                g.c.frame_advance(1)
                t, xy = tile(g), cursor_xy(g)
                if t >= 0 and xy != tile_xy(t, n):
                    wrong.append((k, f, t, xy))
        finally:
            g.c.resume()
        g.wait(5)
    assert not wrong, wrong
    # L / R (P+'s presets) change nothing.
    g.tap("R")
    g.wait(15)
    assert switch_bits(g) == page1_bits(g, room), "a preset was loaded"
    close_menu(g)
    g.wait(30)

    assert g.c.read_mem(B.stage_random_switch(g.c.read_mem), 30) == own, "the player's own switch was not put back"
    assert len(sim.stages_requests) == 1, sim.stages_requests
    choice, starters, counterpicks = sim.stages_requests[0]
    assert choice == 3
    assert starters == sorted(set(room_starters) - {starter} | {counterpick}), starters   # (P+: BF, FoD, SV, DL, PS2)
    assert off in counterpicks and starter not in counterpicks and starter not in starters, counterpicks
    assert sim.room.rules[0x10] == 1, "the hazards did not go with the rules"
    assert sim.room.rules[0x18:0x20] == bytes([starter]).ljust(8, b"\0"), sim.room.rules[0x18:0x20].hex()
    # The page shows the room's new stages when it opens again.
    open_switch(g)
    assert switch_bits(g) == page1_bits(g, starters + counterpicks)
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
    to_bar(g)
    g.tap("A")
    g.c.pad_script(0, [PadInput(buttons=["Z"], hold=3), PadInput(hold=6)])
    g.wait(15)
    assert switch_bits(g) == before
    g.shot("01-viewer")
    close_menu(g)
    assert not sim.stages_requests, sim.stages_requests


def test_a_sets_bar_is_the_list(game: Game) -> None:
    g = game
    sim = in_room(g, host=True)
    sim.room.best_of = 3
    g.wait(40)
    open_switch(g)
    g.tap("A")          # on the bar: refused
    g.wait(15)
    g.shot("01-set")
    close_menu(g)
    assert not sim.stages_requests, sim.stages_requests


def test_the_footer_over_the_ready_banner(game: Game) -> None:
    g = game
    in_room(g, host=True)
    g.hand_to(*FOX)
    g.tap("A")
    g.wait(20)
    assert g.banner_shown(), "no READY TO FIGHT with a character"
    open_switch(g)
    g.shot("20-bar-over-banner")
    goto_stage(g, FD)
    g.wait(15)
    g.shot("21-stage-over-banner")
    close_menu(g)
    assert g.banner_shown(), "READY TO FIGHT gone after the menu"
