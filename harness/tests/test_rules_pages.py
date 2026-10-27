"""The room rules menu's L / R pages (docs/design/special-rules.md): MATCH SETUP (the ruleset's set and
stage rows in P+'s MORE RULES look) and SPECIAL (P+'s own Special Versus box), on the room CSS with a
scripted Dolphin (tools/gamecode/roomsim.py, which also answers CMD_SPECIAL_ASSETS from the template
SD card as Dolphin does).

Screenshots: run/artifacts/game-code/rooms/rules-pages/.
"""
from __future__ import annotations

import struct

import pytest

from test_rooms_ui import ROOT, Game, RulesButton, create_room, keypad_type, menu_open, ppom_sym, to_join_room
import ppom  # noqa: E402  (test_rooms_ui put tools/gamecode on the path)
import roomsim  # noqa: E402

pytestmark = [pytest.mark.dolphin, pytest.mark.gpu]

ART = ROOT / "run" / "artifacts" / "game-code" / "rooms" / "rules-pages"
V_PPLUS, V_SETUP, V_SPECIAL = 0, 1, 2
SA_READY = 2


def pages(g: Game) -> dict:
    """rules_pages.cpp's g_rulesPages: the page shown and MATCH SETUP's values."""
    d = g.c.read_mem(ppom_sym(g, "g_rulesPages"), 0x20)
    view, turns, drawn, posts, failed = struct.unpack_from(">5I", d, 0)
    return {"view": view, "turns": turns, "posts": posts, "failed": failed, "bans": d[0x14], "dsr": d[0x15],
            "choice": d[0x16], "alternates": d[0x17], "stock_control": d[0x18]}


def box(g: Game) -> dict:
    """special_box.cpp's g_specialBox: the box's files, objects and the rows it shows."""
    d = g.c.read_mem(ppom_sym(g, "g_specialBox"), 0x28)
    state, asks, made, objs, buf = struct.unpack_from(">5I", d, 0)
    return {"state": state, "made": made, "objs": objs, "rows": list(d[0x14:0x1C]), "row": d[0x1C],
            "failed": struct.unpack_from(">I", d, 0x20)[0]}


def until(g: Game, cond, frames: int, what: str) -> None:
    for _ in range(frames // 10):
        if cond():
            return
        g.wait(10)
    assert cond(), what


@pytest.fixture
def game(dolphin_exe):
    g = Game("rules-pages", art=ART)
    yield g
    g.close()


def test_host_edits_match_setup_and_special(game: Game) -> None:
    """The host: R turns RULES -> MORE RULES -> MATCH SETUP -> SPECIAL and L back. MATCH SETUP's
    STAGE CHOICE and SPECIAL's rows are edited; closing the menu sends room-format and the room's
    rules with the special rows (the room is Custom), and the menu shows them again when it opens."""
    g = game
    g.c.game_bridge_config(enabled=False)
    create_room(g)
    sim = g.sim
    sim.add_player(1, "alice")
    g.wait(40)
    ppom.set_cfg_bits(g.c, ppom.CFG_TEST_OPEN_RULES)
    until(g, lambda: menu_open(g), 200, "the rules menu opens")
    g.wait(60)
    assert pages(g)["view"] == V_PPLUS
    g.tap("R", 4)
    g.wait(60)
    g.tap("R", 4)
    g.wait(30)
    p = pages(g)
    assert p["view"] == V_SETUP and p["failed"] == 0, p
    g.shot("01-match-setup")
    choice0 = p["choice"]
    g.tap("DDOWN", 4)
    g.wait(8)
    g.tap("DDOWN", 4)
    g.wait(8)
    g.tap("DRIGHT", 4)
    g.wait(20)
    assert pages(g)["choice"] != choice0, pages(g)
    g.shot("02-stage-choice")
    g.tap("R", 4)
    until(g, lambda: box(g)["state"] == SA_READY and box(g)["made"] >= 1, 400, "the Special Versus box")
    b = box(g)
    assert pages(g)["view"] == V_SPECIAL and b["objs"] >= 10 and b["failed"] == 0, b
    assert sim.special_writes >= 1
    g.shot("03-special")
    g.tap("DRIGHT", 4)        # STAMINA: 300%
    g.wait(10)
    g.tap("DDOWN", 4)
    g.wait(8)
    g.tap("DRIGHT", 4)        # SIZE: MEGA
    g.wait(8)
    g.tap("DRIGHT", 4)        # SIZE: MINI
    g.wait(20)
    assert box(g)["rows"][:2] == [1, 2], box(g)
    g.shot("04-special-edited")
    g.tap("L", 4)             # back to MATCH SETUP, then MORE RULES
    g.wait(30)
    assert pages(g)["view"] == V_SETUP
    g.tap("L", 4)
    g.wait(30)
    assert pages(g)["view"] == V_PPLUS
    for _ in range(3):
        if not menu_open(g):
            break
        g.tap("B", 4)
        g.wait(40)
    until(g, lambda: not menu_open(g), 200, "the menu closes")
    g.wait(30)
    g.shot("05-closed")
    fmt = sim.room_ops("format")
    rules = sim.room_ops("rules")
    assert fmt and fmt[-1][1] == pages(g)["choice"], [p[:8].hex() for p in fmt]
    assert rules and list(rules[-1][0x24:0x2C]) == [1, 2, 0, 0, 0, 0, 0, 0], [p.hex() for p in rules]
    assert sim.room.special[:2] == [1, 2] and sim.room.custom
    # Opened again: the room's rows (SESSION Special) on the box.
    sim.service(30)
    ppom.set_cfg_bits(g.c, ppom.CFG_TEST_OPEN_RULES)
    until(g, lambda: menu_open(g), 200, "the rules menu opens again")
    g.wait(40)
    for _ in range(3):
        g.tap("R", 4)
        g.wait(40)
    until(g, lambda: box(g)["made"] >= 2, 300, "the box again")
    assert box(g)["rows"][:2] == [1, 2], box(g)
    g.shot("06-special-again")


def test_viewer_sees_the_pages_read_only(game: Game) -> None:
    """A player who is not the host turns the same pages and sees the room's values; their presses
    change nothing and closing sends nothing."""
    g = game
    g.c.game_bridge_config(enabled=False)
    create_room(g)
    sim = g.sim
    to_join_room(g)
    sim.joinable["KFQB"] = roomsim.Room(code="KFQB", host=1, open=[True, True, True, False],
                                        members=[roomsim.Member("carol", team=0),
                                                 roomsim.Member("bob", team=1), None, None],
                                        special=[0, 0, 0, 2, 0, 0, 0, 0])
    g.tap("START")
    g.wait(40)
    keypad_type(g, "KFQB")
    g.tap("START")
    g.wait(90)
    assert sim.room is not None and sim.me() == 2
    rb = RulesButton(g)
    g.hand_to(*rb.centre(), tol=0.4)
    g.tap("A")
    until(g, lambda: menu_open(g), 200, "the rules menu opens")
    g.wait(40)
    for _ in range(3):
        g.tap("R", 4)
        g.wait(40)
    until(g, lambda: box(g)["state"] == SA_READY and box(g)["made"] >= 1, 400, "the Special Versus box")
    assert pages(g)["view"] == V_SPECIAL
    assert box(g)["rows"] == [0, 0, 0, 2, 0, 0, 0, 0], box(g)      # BODY: CLEAR, the room's
    g.tap("DRIGHT", 4)
    g.wait(20)
    assert box(g)["rows"] == [0, 0, 0, 2, 0, 0, 0, 0], box(g)
    g.shot("07-viewer-special")
    for _ in range(4):
        if not menu_open(g):
            break
        g.tap("B", 4)
        g.wait(40)
    until(g, lambda: not menu_open(g), 200, "the menu closes")
    g.wait(30)
    assert not sim.room_ops("format") and not sim.room_ops("rules"), (sim.room_ops("format"), sim.room_ops("rules"))
