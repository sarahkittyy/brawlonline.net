"""Stock control in a room (docs/design/room-rules.md decision 19, 2026-10-15): the rules'
stockControl (P+'s STOCK CONTROL, set in a ruleset) shows P+'s STOCKS box on the room CSS's panels;
each player picks their own stock count on their own panel, their lock-in carries it, the others
see it once they are ready, and the match gives each player their count
(game-code/PPOnline/source/room_css.cpp, online_match.cpp; docs/game-code.md §14).

- test_stock_boxes_on_the_room_css: one instance, tools/gamecode/roomsim.py as Dolphin's side.
- test_stock_control_set_and_desync_restore: two instances, the real server: a published Bo3 with
  stock control, each player's stocks in game 1, and a desync's restart keeps the agreed stocks
  (P+'s handicap code, which makes the count the player's stocks when the fighter is made, must
  not put the picked count back).

Screenshots: run/artifacts/game-code/rooms/stock-control/.
"""
from __future__ import annotations

import struct
from typing import Callable

import pytest

from test_rooms_ui import FOX, PANEL_X, ROOT, Game, create_room, ppom
from test_online_game import backend  # noqa: F401  (the backend fixture)
from ppharness import brawl as B

pytestmark = [pytest.mark.dolphin, pytest.mark.gpu]

ART = ROOT / "run" / "artifacts" / "game-code" / "rooms" / "stock-control"
TASK_RULES = 0x62C
TASK_HANDICAP_MODE = 0x5CC
AREA_STOCK_BOX, AREA_STOCKS = 0xD4, 0x1D0
BUTTON_STOCK_BOX = 0x1E
HAND_MODE_PICKER = 5
RU_STOCK_CONTROL = 0x13
BOX_Y = -17.3                 # the STOCKS box, lower right of the panel (room_css.cpp STOCK_BOX_DY)
BOX_DX = 1.5                  # the box's centre from the panel's (room_css.cpp STOCK_BOX_DX moved it left of P+'s place)


def css_task(g: Game) -> int:
    return g.u32(g.u32(g.u32(0x805A0060) + 4) + 0x400)


def set_rule(g: Game) -> bytes:
    return g.c.read_mem(g.u32(g.u32(B.GAME_GLOBAL_PTR) + B.GG_SET_RULE), 0x10)


def hand(g: Game) -> int:
    return g.u32(g.area(0) + 0x1A8)


def box_in_group(g: Game, i: int) -> bool:
    """Area i's STOCKS box model is in the area's scene group (area+0x404: what showStockBox
    0x80697720 inserts and removes; an nw4r ScnGroup, objects +0xDC, count +0xE4)."""
    a = g.area(i)
    grp = g.u32(a + 0x404)
    mdl = g.u32(g.u32(a + AREA_STOCK_BOX) + 0x10)
    n = g.u32(grp + 0xE4)
    objs = g.u32(grp + 0xDC)
    return any(g.u32(objs + 4 * k) == mdl for k in range(min(n, 64)))


def stocks_shown(g: Game, i: int) -> int:
    return g.u32(g.area(i) + AREA_STOCKS)


def rules_with_stock_control(on: bool) -> bytes:
    r = bytearray(roomsim_rules())
    r[RU_STOCK_CONTROL] = 1 if on else 0
    return bytes(r)


def roomsim_rules() -> bytes:
    import roomsim
    return roomsim.DIRECT_RULES


def pick_stocks(g: Game, n: int, slot: int = 0) -> None:
    """The local panel's STOCKS box: A opens Brawl's picker at the current pick, the D-pad moves
    one entry (10% = 1 stock) a press, A takes it."""
    g.hand_to(PANEL_X[slot] + BOX_DX, BOX_Y, tol=0.5)
    g.wait(5)
    assert g.u32(hand(g) + 0xAC) == BUTTON_STOCK_BOX, hex(g.u32(hand(g) + 0xAC))
    cur = stocks_shown(g, 0)
    g.tap("A")
    g.wait(20)
    assert g.u32(hand(g) + 0xA4) == HAND_MODE_PICKER, "the picker did not open"
    for _ in range(abs(n - cur)):
        g.tap("DDOWN" if n > cur else "DUP")
        g.wait(10)
    g.tap("A")
    g.wait(20)
    assert stocks_shown(g, 0) == n, (stocks_shown(g, 0), n)


@pytest.fixture
def game(dolphin_exe):
    g = Game("stock-control", art=ART)
    yield g
    g.close()


def test_stock_boxes_on_the_room_css(game: Game) -> None:
    """Without stock control no panel has a STOCKS box. With it (the room's rules), our panel and
    every taken slot's has one, open and closed slots none; set rule +5 and the CSS's own mode
    (task+0x5CC) are P+'s "on" (2). We pick 3 on ours; alice's box shows the 2 she picked once she
    is ready. START: the lock-in's port values carry 3. Locked in, A on our box opens nothing and
    the pick stays. Unready, the rules off: every box goes; on again, ours is back at 3."""
    g = game
    g.c.game_bridge_config(enabled=False)
    create_room(g)
    sim = g.sim
    sim.add_player(1, "alice")
    sim.room.open[2] = True
    g.wait(40)
    assert not any(box_in_group(g, i) for i in range(4))
    assert set_rule(g)[5] == 0 and g.u32(css_task(g) + TASK_HANDICAP_MODE) == 0
    g.shot("01-off")

    sim.room.rules = rules_with_stock_control(True)
    g.wait(30)
    assert ppom.read_session(g.c, sim.b)["rules"].get("stockControl") is True
    assert set_rule(g)[5] == 2 and g.u32(css_task(g) + TASK_HANDICAP_MODE) == 2
    # Ours and alice's (choosing); slot 3 open, slot 4 closed: none.
    assert [box_in_group(g, i) for i in range(4)] == [True, True, False, False]
    g.shot("02-on")

    pick_stocks(g, 3)
    sim.set_ready(1, 0x00, 2, stocks=2)   # alice: blue Mario, 2 stocks
    g.wait(30)
    assert stocks_shown(g, 1) == 2, stocks_shown(g, 1)
    g.shot("03-picked-alice-ready")

    g.hand_to(*FOX)
    g.tap("A")
    g.wait(20)
    g.tap("START")
    g.wait(30)
    lo = ppom.read_local(g.c, sim.b)
    assert lo["lock"]["ready"] == 1 and lo["own"]["stocks"] == 3, lo
    # Locked in: Brawl's picker stays shut, the pick stays.
    g.hand_to(PANEL_X[0] + BOX_DX, BOX_Y, tol=0.5)
    g.wait(5)
    g.tap("A")
    g.wait(20)
    assert g.u32(hand(g) + 0xA4) != HAND_MODE_PICKER, "the picker opened while locked in"
    g.tap("DDOWN")
    g.wait(10)
    g.tap("A")
    g.wait(20)
    assert stocks_shown(g, 0) == 3 and ppom.read_local(g.c, sim.b)["own"]["stocks"] == 3
    g.shot("04-locked")
    g.tap("B")
    g.wait(20)
    assert ppom.read_local(g.c, sim.b)["lock"]["ready"] == 0

    # A time match: no boxes (P+ shows none), as with the rules off.
    r = bytearray(sim.room.rules)
    r[1] = 0          # style: time
    sim.room.rules = bytes(r)
    g.wait(30)
    assert not any(box_in_group(g, i) for i in range(4)) and set_rule(g)[5] == 0
    sim.room.rules = rules_with_stock_control(False)
    g.wait(30)
    assert not any(box_in_group(g, i) for i in range(4))
    g.shot("05-off-again")
    sim.room.rules = rules_with_stock_control(True)
    g.wait(30)
    assert box_in_group(g, 0) and stocks_shown(g, 0) == 3
    # The lock-in without stock control carries none.
    sim.room.rules = rules_with_stock_control(False)
    g.wait(30)
    g.tap("START")
    g.wait(30)
    lo = ppom.read_local(g.c, sim.b)
    assert lo["lock"]["ready"] == 1 and lo["own"]["stocks"] == 0, lo


# ---- two instances, the real server ---------------------------------------------------------

STOCK_SET = {"format": 1, "name": "Stock Control Set", "rules": {"stocks": 1, "timeMinutes": 3, "stockControl": True},
             "bestOf": 3, "stageChoice": "loser-picks", "strikes": [1, 2, 1], "bans": 2, "dsr": True}


def _online_hand_to(g, x: float, y: float, tol: float = 0.5) -> None:
    """The local hand (its area's, port 0) to (x, y), closed loop, on a real session's CSS."""
    from ppharness.client import PadInput
    for _ in range(100):
        a = B.read_css_area(g.c.read_mem, 0)
        dx, dy = x - a.hand_x, y - a.hand_y
        if abs(dx) <= tol and abs(dy) <= tol:
            return
        axis = lambda d: 128 if abs(d) <= tol else 128 + (1 if d > 0 else -1) * max(45, min(127, int(abs(d) * 15)))  # noqa: E731
        g.c.pad_script(0, [PadInput(main=(axis(dx), axis(dy)), hold=3), PadInput(hold=2)])
        g.steps("wait 5")
    raise AssertionError(f"{g.name}: the hand did not reach {(x, y)}")


def _online_pick_stocks(g, slot: int, n: int) -> None:
    """On the room CSS: our panel (drawn at our slot)'s STOCKS box, Brawl's picker, `n`."""
    _online_hand_to(g, PANEL_X[slot] + BOX_DX, BOX_Y)
    g.steps("wait 5")
    area = B.read_css_area(g.c.read_mem, 0).area
    assert struct.unpack(">I", g.c.read_mem(struct.unpack(">I", g.c.read_mem(area + 0x1A8, 4))[0] + 0xAC, 4))[0] \
        == BUTTON_STOCK_BOX, f"{g.name}: the hand is not on the STOCKS box"
    g.steps("tap A 8", "wait 30")
    for _ in range(n):
        g.steps("tap DDOWN 6", "wait 10")
    g.steps("tap A 8", "wait 30")
    got = struct.unpack(">I", g.c.read_mem(area + AREA_STOCKS, 4))[0]
    assert got == n, (g.name, got, n)


def _handicaps(g) -> list[int]:
    """The match players' handicaps (gmGlobalModeMelee player +0x26, tenths of a stock)."""
    mm = B.Mem(g.c.read_mem).chain(B.GAME_GLOBAL_PTR, B.GG_MODE_MELEE)
    return [struct.unpack(">H", g.c.read_mem(mm + 0x98 + 0x5C * i + 0x26, 2))[0] for i in range(2)]


@pytest.mark.server
@pytest.mark.slow
def test_stock_control_set_and_desync_restore(backend, dolphin: Callable, gpu_backend: str) -> None:
    """A Bo3 published with stock control (the launcher's ruleset editor) and 1 stock: A picks 3 on
    their panel, B (in slot 2, their panel drawn there) 2. Game 1 (a blind pick: A's count is
    hidden from B as A's character is) is played with A 3 and B 2 stocks, the same on both
    machines. A walks off once (2 left); then A's damage changes on A's machine only: the desync's
    restart gives A the 2 stocks both machines agreed on, not the 3 picked, and B 2 (P+'s handicap
    code makes the handicap the stocks when the fighter is made: the restart's setup writes the
    agreed stocks there too). The game is then played out with no mismatch."""
    import threading

    from ppharness import flows as F
    from test_online import _wait
    from test_online_game import _p1_damage_addr
    from test_room_rules import session_rules
    from test_room_sets import (CSS, SSS, _all, _boot, _fighters, launcher_answer, pick, play_steps, rooms,
                                setup_view, to_join_room, type_room_code, until_setup, until_view, write_create)
    import online_set

    sc_art = ART / "set"
    ua, ub = backend.create_user("stctlah", "SCAH"), backend.create_user("stctlbj", "SCBJ")
    a = _boot(dolphin, "sc-a", backend, ua, gpu_backend, "stock-control", "gameplay", artifacts=sc_art)
    b = _boot(dolphin, "sc-b", backend, ub, gpu_backend, "stock-control", "gameplay", artifacts=sc_art)
    _all(a.to_main_menu, b.to_main_menu)
    _all(a.to_online_page, b.to_online_page)
    rs_code = backend._http("POST", "/v1/rulesets", {"ruleset": STOCK_SET}, token=ua.session_token)["code"]
    rid = write_create(a, ruleset=rs_code)
    assert launcher_answer(a, rid)["result"] == "accepted"
    _wait(lambda: a.scene() == CSS, 60, "A on the room's CSS")
    code = until_view(a, lambda v: v["flags"] & ppom.RF_IN, "A in its room", timeout=60)["code"]
    to_join_room(b)
    pick(a)
    pick(b, right=2)
    type_room_code(b, code)
    until_view(b, lambda v: v["flags"] & ppom.RF_IN, "B in the room")
    for g in (a, b):
        _wait(lambda g=g: (session_rules(g) or {}).get("stockControl") is True, 30, f"{g.name}: stock control")
    _online_pick_stocks(a, 0, 3)
    _online_pick_stocks(b, 1, 2)
    a.shot("01-picked")
    b.shot("01-picked")

    players = [a, b]
    a.steps("tap START 8", "wait 40")
    _wait(lambda: ppom.read_room(b.c, ppom.find_block(b.c))["slots"][0]["bits"] & ppom.SLOT_READY, 20, "B sees A ready")
    # Game 1 of a set is a blind pick: neither A's character nor A's count reach B.
    assert ppom.read_room(b.c, ppom.find_block(b.c))["slots"][0]["stocks"] == 0
    b.steps("tap START 8", "wait 10")
    online_set.wait(lambda: all(online_set.scene(g.c) == SSS for g in players), 120, "game 1: both on the stage select")
    play_steps(players, 1)
    online_set.wait(lambda: all(online_set.gstatus(g.c)["phase"] == "running" for g in players), 240,
                    "game 1 running on both")
    for g in players:
        se = ppom.read_session(g.c, ppom.find_block(g.c))
        assert [p["pv"]["stocks"] for p in se["players"][:2]] == [3, 2], se["players"]
        assert _handicaps(g) == [30, 20], (g.name, _handicaps(g))
    a.steps("wait 200")
    start = [_fighters(g) for g in players]
    assert start[0] == start[1] and {p: st for p, (st, _) in start[0].items()} == {0: 3, 1: 2}, start
    a.shot("02-game1-3-and-2")

    # A walks off once.
    for _ in range(12):
        if _fighters(a)[0][0] < 3:
            break
        F.fight([F.Seat(a.c, online_set.local(a.c)["local_port"], 0, a.name)], 120, mode="selfdestruct", seed="sc")
    a.steps("wait 300")
    before = [_fighters(g) for g in players]
    print("before the desync", before, flush=True)
    assert before[0] == before[1] and before[0][0][0] == 2 and before[0][1][0] == 2, before

    for _ in range(6):
        a.c.write_mem(_p1_damage_addr(a), struct.pack(">f", 77.0))
        a.steps("wait 5")
    online_set.wait(lambda: all(g.debug_scratch()[10] & 0x100000 for g in players), 30, "DESYNC DETECTED on both")
    online_set.wait(lambda: all(online_set.scene(g.c) == CSS for g in players), 60, "both back on the CSS")
    for g in players:
        until_setup(g, lambda v: v["active"] and v["type"] == 5, "the ready-up after the desync")
    b.steps("tap START 8", "wait 60")
    a.steps("tap START 8")
    online_set.wait(lambda: all(online_set.gstatus(g.c)["phase"] == "running" for g in players), 60,
                    "the restarted game running on both")
    rs = [online_set.gstatus(g.c)["restore"] for g in players]
    assert rs[0] == rs[1] and rs[0]["stocks"][0] == 2 and rs[0]["stocks"][1] == 2, rs
    a.steps("wait 200")
    for g in players:
        got = _fighters(g)
        assert {p: st for p, (st, _) in got.items()} == {0: 2, 1: 2}, (g.name, got, "the picked count came back")
        assert {p: d for p, (_, d) in got.items()} == {p: d for p, (_, d) in before[0].items()}, (g.name, got, before)
        assert _handicaps(g) == [20, 20], (g.name, _handicaps(g))
        g.shot("03-restarted")
    rep = online_set.play_game(players, 2)
    assert rep["checksums"]["mismatches"] == 0, rep


def test_stock_box_on_a_joiners_panel(game: Game) -> None:
    """A joiner's own panel is drawn at their slot (P2 here) and the host's at P1 (room_css.cpp
    placeAreas). Each STOCKS box hangs from its panel and moves with it: given the slot's offset as
    well, ours was drawn on P3's panel and the host's off the screen's left. Ours is hit at our
    panel (A opens the picker there), and every box's own offset is only its shift left."""
    import roomsim
    from test_rooms_ui import keypad_type, to_join_room

    g = game
    g.c.game_bridge_config(enabled=False)
    create_room(g)
    sim = g.sim
    to_join_room(g)
    sim.joinable["KFQB"] = roomsim.Room(code="KFQB", host=0, open=[True, True, False, False],
                                        members=[roomsim.Member("carol", team=0), None, None, None],
                                        rules=rules_with_stock_control(True))
    g.tap("START")
    g.wait(40)
    keypad_type(g, "KFQB")
    g.tap("START")
    g.wait(80)
    assert sim.me() == 1
    sim.set_ready(0, 0x00, 0, stocks=4)
    g.wait(30)
    for i in (0, 1):
        x = struct.unpack(">f", g.c.read_mem(g.u32(g.area(i) + AREA_STOCK_BOX) + 0x3C, 4))[0]
        assert abs(x - (-3.6)) < 0.01, (i, x)
    assert box_in_group(g, 0) and box_in_group(g, 1) and stocks_shown(g, 1) == 4
    g.shot("09-joiner-before-pick")
    pick_stocks(g, 2, slot=1)
    g.wait(10)
    g.shot("10-joiner-slot-2")
