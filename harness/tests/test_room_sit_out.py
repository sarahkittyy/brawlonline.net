"""SIT OUT, open slots and the placed coin end to end: the game's room UI against Dolphin's side
and our local accounts + mm servers, as test_rooms_game.py (the user's, 2026-10-18;
docs/design/rooms.md "Sitting out").

- Open slots wait for nobody: two players in a room of three open slots start once both are
  ready; a third who joins during the game waits on the CSS, and the next game waits for them.
- A coin put down shows on the others' panels before START (not ready).
- A player who sits out (A on their own name plate) is left out of the game: no ticket, no
  session, their card grey with SIT OUT; the others play.
- The loser picks: when the last game's loser sits out, one of the next game's players picks
  (drawn by mm), and the stage they pick is played; the one sitting out never picks.

Screenshots: run/artifacts/game-code/rooms/<test>/<player>/.
"""

from __future__ import annotations

import contextlib
import secrets
import threading
import time
from typing import Any, Callable

import pytest

from ppharness.backend import OnlineBackend
from ppharness.instance import DolphinInstance

from test_online import _wait
from test_online_game import Game, _boot, backend  # noqa: F401  (the backend fixture)
from test_rooms_game import (ART, CSS, PANEL_X, _all, create_room, hand_to, lock_of, online_set, pick, ppom,
                             room_view, status_line, to_join_room, type_room_code, until_view)
from ppharness import brawl as B

pytestmark = [pytest.mark.dolphin, pytest.mark.server, pytest.mark.gpu]

PLATE_Y = -20.0
GMKIND_FOX = 0x07


def _players(backend: OnlineBackend, dolphin, gpu_backend: str, test: str, tag: str, n: int) -> list[Game]:
    users = [backend.create_user(f"{tag}{'abc'[k]}", f"{tag.upper()}{'ABC'[k]}",
                                 email=f"{tag}{k}-{secrets.token_hex(4)}@example.test") for k in range(n)]
    gs = [_boot(dolphin, f"{tag}-{'abc'[k]}", backend, u, gpu_backend, test, "gameplay", artifacts=ART)
          for k, u in enumerate(users)]
    _all(*[g.to_main_menu for g in gs])
    _all(*[g.to_online_page for g in gs])
    return gs


def _short_rules(gs: list[Game]) -> None:
    for g in gs:
        B.write_rules(g.c, stocks=1, minutes=2, items_off=True)
        ppom.allow_test_rules(g.c)


def _open_slot(host: Game, slot: int) -> None:
    hand_to(host, PANEL_X[slot], -10.0)
    host.steps("tap A 8", "wait 30")
    until_view(host, lambda v: v["slots"][slot]["bits"] & ppom.SLOT_OPEN, f"slot {slot + 1} open")
    hand_to(host, -23.0, 10.5)


def _join(g: Game, code: str, slot: int) -> None:
    type_room_code(g, code)
    v = until_view(g, lambda v: v["flags"] & ppom.RF_IN, f"{g.name} in the room")
    assert v["local_port"] == slot, v


def _phase(g: Game) -> str:
    return online_set.gstatus(g.c)["phase"]


def _running(gs: list[Game], what: str) -> None:
    online_set.wait(lambda: all(_phase(g) in ("running", "error", "ended") for g in gs), 240, what)
    st = [(g.name, _phase(g), online_set.gstatus(g.c).get("error")) for g in gs]
    assert all(p == "running" for _, p, _ in st), st


def _walk_off(g: Game, port: int, seed: str) -> None:
    from ppharness import flows as F
    with contextlib.suppress(Exception):
        F.fight([F.Seat(g.c, port, 0, g.name)], 60 * 40, mode="selfdestruct", seed=seed,
                stop=lambda: _phase(g) != "running")


def _play_out(players: list[Game], loser: Game, loser_port: int, seed: str) -> None:
    """The loser walks off (one stock): the others win; everyone back on the room's CSS."""
    time.sleep(4)
    t = threading.Thread(target=_walk_off, args=(loser, loser_port, seed))
    t.start()
    t.join(300)
    online_set.wait(lambda: all(_phase(g) != "running" for g in players), 240, "the game to end")
    _wait(lambda: all(g.scene() == CSS for g in players), 180, "the players back on the room's CSS")


def _plate(g: Game, slot: int) -> None:
    """A on that card's name plate (one's own: SIT OUT / PLAY; the host's on anyone's)."""
    hand_to(g, PANEL_X[slot] + 1.5, PLATE_Y, tol=0.6)
    g.steps("tap A 8", "wait 30")
    hand_to(g, -23.0, 10.5)


def _sitting(g: Game, slot: int) -> bool:
    return bool(room_view(g)["slots"][slot]["bits"] & ppom.SLOT_SITTING_OUT)


def test_two_players_start_with_an_open_slot_and_a_joiner_waits(backend: OnlineBackend,
                                                                 dolphin: Callable[..., DolphinInstance],
                                                                 gpu_backend: str) -> None:
    test = "sit-out-open-slots"
    a, b, c = gs = _players(backend, dolphin, gpu_backend, test, "sox", 3)
    _short_rules(gs)
    code = create_room(a)
    pick(a)
    _open_slot(a, 2)
    to_join_room(b)
    pick(b, right=2)
    _join(b, code, 1)
    # The placed coin shows before START: B sees A's Fox, A not ready.
    v = until_view(b, lambda v: v["slots"][0]["char"] == GMKIND_FOX, "A's placed coin on B's panels")
    assert not v["slots"][0]["bits"] & ppom.SLOT_READY, v
    for g in (a, b):
        g.steps("wait 30")
        g.shot("01-two-of-three")
    # Both ready, slot 3 open and empty: the game starts.
    game = room_view(a)["game"]
    for g in (a, b):
        g.steps("tap START 8", "wait 10")
    _running([a, b], "the two players' game")
    # C joins during it: waits on the CSS, readies meanwhile.
    to_join_room(c)
    pick(c, right=4)
    _join(c, code, 2)
    c.steps("wait 60")
    assert c.scene() == CSS and _phase(c) not in ("running",), _phase(c)
    assert online_set.gstatus(a.c)["phase"] == "running"
    c.shot("02-joined-during-the-game")
    _play_out([a, b], b, 1, "sox")
    for g in gs:
        until_view(g, lambda v: v["status"] == 0, "the room waiting", timeout=60)
    assert room_view(a)["game"] == game + 1
    # The next game waits for C too.
    for g in (a, b):
        g.steps("tap START 8", "wait 30")
    time.sleep(5)
    assert room_view(a)["status"] == 0 and _phase(a) != "running", "started without C"
    # (B lost game 1 and picks: its stage select opens once C is ready too.)
    c.steps("tap START 8", "wait 10")
    online_set.wait(lambda: b.scene() == "scSelStage" or all(_phase(g) == "running" for g in gs), 120,
                    "the three players' game or B's stage select")
    if b.scene() == "scSelStage":
        b.steps("wait 40")
        B.sss_pick_stage(b.c, B.STAGE_KIND[online_set.STAGE_PICK], 0)
    _running(gs, "the three players' game")
    for g in gs:
        g.shot("03-three-play")


def test_sitting_out_and_the_stage_pick(backend: OnlineBackend, dolphin: Callable[..., DolphinInstance],
                                        gpu_backend: str) -> None:
    test = "sit-out-stage-pick"
    a, b, c = gs = _players(backend, dolphin, gpu_backend, test, "soy", 3)
    _short_rules(gs)
    code = create_room(a)
    pick(a)
    _open_slot(a, 2)
    for k, g in ((1, b), (2, c)):
        to_join_room(g)
        pick(g, right=2 * k)
        _join(g, code, k)
    until_view(a, lambda v: all(v["slots"][p]["bits"] & ppom.SLOT_TAKEN for p in range(3)), "all players")

    # C sits out (A on their own plate): grey card, SIT OUT, on every screen.
    _plate(c, 2)
    for g in gs:
        until_view(g, lambda v: v["slots"][2]["bits"] & ppom.SLOT_SITTING_OUT, f"{g.name}: C sitting out")
        g.steps("wait 20")
        g.shot("01-c-sits-out")
    # A and B ready: their game starts, C has no session and stays on the CSS.
    for g in (a, b):
        g.steps("tap START 8", "wait 10")
    _running([a, b], "game 1 without C")
    c.steps("wait 120")
    assert c.scene() == CSS and _phase(c) != "running" and not lock_of(c)["ready"], (_phase(c), lock_of(c))
    assert "Waiting for the game to end" in c.c.call("rooms_status")["text"], c.c.call("rooms_status")
    c.shot("02-c-watches-the-css")
    # B loses: B picks the next stage...
    _play_out([a, b], b, 1, "soy")
    for g in gs:
        until_view(g, lambda v: v["status"] == 0 and v["slots"][1]["picks_stage"], f"{g.name}: B picks")
    # ...until B sits out and C plays again: A or C picks (mm's draw), the same on every screen.
    _plate(b, 1)
    _plate(c, 2)
    for g in gs:
        until_view(g, lambda v: v["slots"][1]["bits"] & ppom.SLOT_SITTING_OUT
                   and not v["slots"][2]["bits"] & ppom.SLOT_SITTING_OUT, f"{g.name}: B out, C in")
    pickers = {g.name: [i for i, s in enumerate(room_view(g)["slots"]) if s["picks_stage"]] for g in gs}
    picker = pickers[a.name]
    assert picker in ([0], [2]) and all(p == picker for p in pickers.values()), pickers
    other = c if picker == [0] else a
    pg = a if picker == [0] else c
    for g in gs:
        g.shot("03-b-out-c-in")
    other.steps("tap START 8", "wait 30")
    _wait(lambda: lock_of(other)["ready"], 30, "the other player ready")
    pg.steps("tap START 8", "wait 10")
    _wait(lambda: pg.scene() == "scSelStage", 60, "the picker on the stage select")
    pg.steps("wait 40")
    pg.shot("04-picker-stage-select")
    stage = online_set.STAGE_PICK
    B.sss_pick_stage(pg.c, B.STAGE_KIND[stage], 0)
    _running([a, c], "game 2 without B")
    b.steps("wait 60")
    assert b.scene() == CSS and _phase(b) != "running"
    setups = [B.read_match_setup(g.c.read_mem) for g in (a, c)]
    assert setups[0] == setups[1] and setups[0].stage_kind == B.STAGE_KIND[stage], setups
    for g in gs:
        g.shot("05-game-2")


def test_p2_and_p3_that_cannot_connect_while_p1_sits_out(backend: OnlineBackend,
                                                         dolphin: Callable[..., DolphinInstance],
                                                         gpu_backend: str) -> None:
    """Bug report #202 (2026-10-31): a room's game on P2 and P3 alone (P1 empty or sitting out).
    Their connect failed, and P2's log named P2 itself ("No connection to P2 kzHouston" at P3's
    address): Matchmaking took P2's place among the match's players from its port, so P2 also
    checked P3's connect token against its own, did not punch P3's low ports and told the room it
    could not reach slot 2. Here the host sits out and B (P2) drops C's packets
    (PPR_TEST_P2P_BLOCK, mapped the same way: before the fix it named B and blocked nothing).
    C never tells the room (PPR_TEST_ROOM_NO_UNREACHABLE), so the pair on the line is B's word."""
    test = "sit-out-p2-p3-unreachable"
    users = [backend.create_user(f"sgz{'abc'[k]}", f"SGZ{'ABC'[k]}",
                                 email=f"sgz{k}-{secrets.token_hex(4)}@example.test") for k in range(3)]
    envs = [{}, {"PPR_TEST_P2P_BLOCK": users[2].connect_code},
            {"PPR_TEST_P2P_BLOCK": users[1].connect_code, "PPR_TEST_ROOM_NO_UNREACHABLE": "1"}]
    a, b, c = gs = [_boot(dolphin, f"sgz-{'abc'[k]}", backend, u, gpu_backend, test, "gameplay", artifacts=ART,
                          env=e) for k, (u, e) in enumerate(zip(users, envs))]
    _all(*[g.to_main_menu for g in gs])
    _all(*[g.to_online_page for g in gs])
    code = create_room(a)
    pick(a)
    _open_slot(a, 2)
    for k, g in ((1, b), (2, c)):
        to_join_room(g)
        pick(g, right=2 * k)
        _join(g, code, k)
    until_view(a, lambda v: all(v["slots"][p]["bits"] & ppom.SLOT_TAKEN for p in range(3)), "all players")
    _plate(a, 0)
    for g in gs:
        until_view(g, lambda v: v["slots"][0]["bits"] & ppom.SLOT_SITTING_OUT, f"{g.name}: A sitting out")
    for g in (b, c):
        g.steps("tap START 8", "wait 10")
    _wait(lambda: all(g.c.call("rooms_status")["room_game"]["active"] for g in (b, c)), 60,
          "the room's game started")
    want = "No connection between sgzb and sgzc."
    _wait(lambda: all(want in status_line(g) for g in (b, c)), 40, f"B's and C's line: {want}", interval=0.2)
    for g in gs:
        g.steps("wait 20")
        g.shot("01-told")
    ends = [g.c.call("rooms_status")["room_game"]["last_end"] for g in (b, c)]
    assert ends[0] in (want, "Could not connect to players: sgzc"), ends
    # B dropped C's packets and named C, not itself, in its connect log.
    log = b.inst.log_path.read_text(encoding="utf-8", errors="replace")
    assert f"Test: no packets from {users[2].connect_code}" in log, "B blocked nobody"
    assert f"No connection to P3 sgzc ({users[2].connect_code})" in log, "B did not name C"
    assert "No connection to P2" not in log, "B named itself"
