"""Watching the room's game from the room CSS (the user's, 2026-10-19; docs/design/rooms.md §1.3):
three Dolphins against our local accounts + mm servers with spectating on, as test_room_sit_out.py.

- C sits out; A and B play game 1. C's game leaves the room CSS by itself and plays game 1 from
  the players' upload (mm sends it on the watch channel), every frame checked against the players'
  checksums; A and B see C's card read WATCHING.
- C runs a few seconds behind (the upload's 10 s hold at a game's start): A and B are back on the
  room CSS first, and C's screen says "Game ended, press B to return to CSS.". Game 2 starts
  without waiting for C, and C is back on the room CSS after game 1, then watches game 2 too.
- B on C's controller ends the watch at once: C is back on the room CSS while A and B play on.
- The players' games are a normal room game: no checksum mismatch, the same session as without a
  watcher (nothing changes on the players' side; the watcher only reads what mm already keeps).

Screenshots: run/artifacts/game-code/rooms/<test>/<player>/.
"""

from __future__ import annotations

import threading
import time
from typing import Any, Callable, Iterator

import pytest

from ppharness.backend import OnlineBackend
from ppharness.instance import DolphinInstance

from test_online import _make_backend, _wait
from test_online_game import Game
from test_rooms_game import (CSS, create_room, online_set, pick, ppom, room_view, to_join_room, type_room_code,
                             until_view)
from test_room_sit_out import _join, _open_slot, _phase, _plate, _players, _short_rules
from ppharness import brawl as B

pytestmark = [pytest.mark.dolphin, pytest.mark.server, pytest.mark.gpu]

SLOT_WATCHING = ppom.SLOT_WATCHING
MATCH = "scMelee"


@pytest.fixture
def watch_backend() -> Iterator[OnlineBackend]:
    be = _make_backend(spectate="on")
    try:
        yield be
    finally:
        be.stop()


def _watch(g: Game) -> dict[str, Any]:
    return g.c.call("rooms_status")["watch"]


def _watching_seen(g: Game, slot: int) -> bool:
    """`slot`'s player watching, as `g`'s Dolphin has the room (SESSION, which the cards show, is
    not written during a match)."""
    v = g.c.call("rooms_status")["view"]
    p = v and v["slots"][slot]["player"]
    return bool(p and p["watching"])


def _play(players: list[Game], game: int, out: dict[str, Any]) -> threading.Thread:
    """Plays the players' game to its end (both play at random: a minute or two, long enough to be
    watched while it goes on) in the background; its report in `out[game]` (online_set.play_game:
    confirmed checksums, rollbacks)."""
    def run() -> None:
        out[game] = online_set.play_game(players, game)
    t = threading.Thread(target=run, daemon=True)
    t.start()
    return t


def _pick_stage_if_asked(picker: Game, others: list[Game]) -> None:
    online_set.wait(lambda: picker.scene() == "scSelStage" or all(_phase(g) == "running" for g in others), 120,
                    f"{picker.name}'s stage select or the game")
    if picker.scene() == "scSelStage":
        picker.steps("wait 40")
        B.sss_pick_stage(picker.c, B.STAGE_KIND[online_set.STAGE_PICK], 0)


def test_a_sitter_watches_the_rooms_game(watch_backend: OnlineBackend,
                                         dolphin: Callable[..., DolphinInstance],
                                         gpu_backend: str) -> None:
    test = "room-watch-sitter"
    a, b, c = gs = _players(watch_backend, dolphin, gpu_backend, test, "rwa", 3)
    _short_rules(gs)
    code = create_room(a)
    pick(a)
    _open_slot(a, 2)
    for k, g in ((1, b), (2, c)):
        to_join_room(g)
        pick(g, right=2 * k)
        _join(g, code, k)
    until_view(a, lambda v: all(v["slots"][p]["bits"] & ppom.SLOT_TAKEN for p in range(3)), "all players")
    _plate(c, 2)
    until_view(a, lambda v: v["slots"][2]["bits"] & ppom.SLOT_SITTING_OUT, "C sitting out")

    # Game 1: A and B. C watches it without doing anything.
    for g in (a, b):
        g.steps("tap START 8", "wait 10")
    reports: dict[str, Any] = {}
    t1 = _play([a, b], 1, reports)
    _wait(lambda: _watch(c)["armed"] and c.scene() == MATCH, 120, "C watching game 1")
    assert _phase(c) != "running" or online_set.gstatus(c.c).get("mode") == "playback", online_set.gstatus(c.c)
    # C's game is armed from the setup, which comes in the players' countdown, and the upload
    # trickles from the game's start (2026-10-19): C's view runs a few seconds behind the players,
    # not 13 s or more.
    _wait(lambda: c.c.call("replay_status").get("shown", -1) >= 0, 60, "C's view of game 1 running")
    lag = (online_set.gstatus(a.c).get("current_frame", 0) - c.c.call("replay_status")["shown"]) / 60
    print(f"C's view of game 1 starts {lag:.1f} s behind the players", flush=True)
    assert lag < 8, f"C's view started {lag:.1f} s behind the players"
    for g in (a, b):
        _wait(lambda: _watching_seen(g, 2), 30, f"{g.name}: C watching")
    c.steps("wait 240")
    c.shot("01-c-watches-game-1")
    a.shot("01-a-plays-c-watching")
    t1.join(900)
    assert 1 in reports, "game 1 did not finish"
    _wait(lambda: all(g.scene() == CSS for g in (a, b)), 180, "A and B back on the room's CSS")
    behind = c.scene() == MATCH and _watch(c)["armed"]
    if behind:
        # The players are done and C still watches: the line says how to leave, and on A's CSS
        # C's card reads WATCHING (while it lasts).
        _wait(lambda: _watch(c)["toasts"] > 0, 15, "the 'Game ended' line on C's screen")
        c.shot("02-c-game-ended-line")
        _wait(lambda: room_view(a)["slots"][2]["bits"] & SLOT_WATCHING or not _watch(c)["armed"], 30,
              "C's card on A's CSS")
        a.shot("02-a-css-c-watching")

    # Game 2 starts without waiting for C (game 1's loser picks the stage).
    for g in (a, b):
        until_view(g, lambda v: v["status"] == 0, f"{g.name}: the room waiting", timeout=60)
    v = room_view(a)
    picker = b if v["slots"][1]["picks_stage"] else a
    for g in (a, b):
        g.steps("tap START 8", "wait 10")
    _pick_stage_if_asked(picker, [a, b])
    t2 = _play([a, b], 2, reports)
    _wait(lambda: all(_phase(g) == "running" for g in (a, b)), 120, "game 2 running")
    assert _phase(c) != "running" or online_set.gstatus(c.c).get("mode") == "playback"

    # C watched game 1 to its end, every frame as the players had it, and is back on the CSS.
    _wait(lambda: _watch(c)["finished"] >= 1, 180, "C's watch of game 1 to end")
    w = _watch(c)
    r1 = w["results"][0]
    assert r1["started"] and r1["finished"] and not r1["left_early"], w
    played = min(reports[1]["frames"])
    assert r1["desync_frame"] == -1 and r1["frames_checked"] >= played - 30, (played, w)
    if behind:
        assert w["toasts"] > 0, w

    # ... then watches game 2, and B ends that at once.
    _wait(lambda: _watch(c)["watched"] >= 2 and c.scene() == MATCH, 120, "C watching game 2")
    c.steps("wait 120")
    c.shot("03-c-watches-game-2")
    c.steps("tap B 8", "wait 10")
    _wait(lambda: _watch(c)["left_early"] >= 1, 15, "B to end C's watch")
    _wait(lambda: c.scene() == CSS and not _watch(c)["armed"], 60, "C back on the room's CSS")
    c.shot("04-c-left-game-2")
    for g in (a, b):
        _wait(lambda: not _watching_seen(g, 2), 30, f"{g.name}: C no longer watching")
    # Nothing more of game 2 is sent to C (the players' game goes on, or ended by itself: a short
    # game can be over before its upload's 10 s hold).
    time.sleep(5)
    assert not _watch(c)["games"] and _watch(c)["skip"], _watch(c)
    t2.join(900)
    assert 2 in reports, "game 2 did not finish"

    for game, rep in sorted(reports.items()):
        print(f"game {game}: frames {rep['frames']}, rollbacks {rep['rollbacks']}, "
              f"max rollback {rep['max_rollback']}, checksums {rep['checksums']}", flush=True)
        assert rep["checksums"]["mismatches"] == 0 and rep["checksums"]["compared"] > 300, rep
    print("C's watches:", _watch(c), flush=True)
    sa = [g.c.call("spectate_status") for g in (a, b)]
    print("uploads:", sa, flush=True)


def test_a_sitter_leaves_a_watch_that_buffers(watch_backend: OnlineBackend,
                                              dolphin: Callable[..., DolphinInstance],
                                              gpu_backend: str) -> None:
    """Bug report #171: the players' upload paused (players first: their connection was busy), the
    sitter's view ran out of frames and stood still, and START / B did nothing (the game's pad
    thread stands still with it), so the sitter closed Dolphin. Here C's frames stop coming
    (`watch_hold`, as a paused upload): C's screen says it is buffering and how to leave, and B
    on C's controller ends the watch while A and B play on."""
    test = "room-watch-buffering"
    a, b, c = gs = _players(watch_backend, dolphin, gpu_backend, test, "rwb", 3)
    for g in gs:  # a game long enough to still be on when C leaves it
        B.write_rules(g.c, stocks=4, minutes=2, items_off=True)
        ppom.allow_test_rules(g.c)
    code = create_room(a)
    pick(a)
    _open_slot(a, 2)
    for k, g in ((1, b), (2, c)):
        to_join_room(g)
        pick(g, right=2 * k)
        _join(g, code, k)
    until_view(a, lambda v: all(v["slots"][p]["bits"] & ppom.SLOT_TAKEN for p in range(3)), "all players")
    _plate(c, 2)
    until_view(a, lambda v: v["slots"][2]["bits"] & ppom.SLOT_SITTING_OUT, "C sitting out")

    for g in (a, b):
        g.steps("tap START 8", "wait 10")
    reports: dict[str, Any] = {}
    t1 = _play([a, b], 1, reports)
    _wait(lambda: _watch(c)["armed"] and c.scene() == MATCH, 120, "C watching game 1")
    _wait(lambda: c.c.call("replay_status").get("shown", -1) >= 300, 90, "C's view 5 s into game 1")
    # The wait for a buffer's frames at the start says nothing.
    assert _watch(c)["buffering_toasts"] == 0, _watch(c)
    c.c.call("rooms_request", op="watch_hold", hold=True)
    try:
        _wait(lambda: _watch(c)["buffering_toasts"] > 0, 30, "C buffering, with the line saying so")
        shown = c.c.call("replay_status")["shown"]
        time.sleep(3)
        assert c.c.call("replay_status")["shown"] == shown, "C's view did not stand still"
        c.shot("01-c-buffering")
        # B on C's controller while the game stands still.
        c.c.pad_set(0, buttons=["B"])
        try:
            _wait(lambda: _watch(c)["left_early"] >= 1, 10, "B to end C's buffering watch")
        finally:
            c.c.pad_clear(0)
    finally:
        c.c.call("rooms_request", op="watch_hold", hold=False)
    _wait(lambda: c.scene() == CSS and not _watch(c)["armed"], 60, "C back on the room's CSS")
    c.shot("02-c-left-the-buffering-watch")
    assert _phase(a) == "running" or 1 in reports, "A's game stopped"
    t1.join(900)
    assert 1 in reports, "game 1 did not finish"
    rep = reports[1]
    print("game 1:", rep, flush=True)
    print("C's watch:", _watch(c), flush=True)
    assert rep["checksums"]["mismatches"] == 0 and rep["checksums"]["compared"] > 300, rep


def test_a_player_who_joins_during_the_game_watches_it(watch_backend: OnlineBackend,
                                                      dolphin: Callable[..., DolphinInstance],
                                                      gpu_backend: str) -> None:
    """C joins while A and B play: C's game catches up to the live edge of their game and watches
    it, then is back on the room CSS, where the next game waits for C (a joiner plays it)."""
    test = "room-watch-joiner"
    a, b, c = gs = _players(watch_backend, dolphin, gpu_backend, test, "rwj", 3)
    for g in gs:
        B.write_rules(g.c, stocks=4, minutes=2, items_off=True)
        ppom.allow_test_rules(g.c)
    code = create_room(a)
    pick(a)
    _open_slot(a, 2)
    to_join_room(b)
    pick(b, right=2)
    _join(b, code, 1)
    for g in (a, b):
        g.steps("tap START 8", "wait 10")
    reports: dict[str, Any] = {}
    t1 = _play([a, b], 1, reports)
    # Well into the game (past the 20 s that a watcher catches up from).
    _wait(lambda: online_set.gstatus(a.c).get("current_frame", 0) > 30 * 60 or 1 in reports, 240,
          "game 1 30 s in")
    assert 1 not in reports, "game 1 ended too soon"
    to_join_room(c)
    pick(c, right=4)
    # (Not _join: C's game may already be on its way to the watched match, SESSION the match's.)
    type_room_code(c, code)
    _wait(lambda: (c.c.call("rooms_status")["view"] or {}).get("you") == 3, 30, "C in the room's slot 3")
    _wait(lambda: _watch(c)["armed"] and c.scene() == MATCH, 120, "C watching game 1")
    c.steps("wait 240")
    c.shot("01-c-joined-and-watches")
    t1.join(900)
    assert 1 in reports, "game 1 did not finish"
    _wait(lambda: _watch(c)["finished"] >= 1, 180, "C's watch of game 1 to end")
    w = _watch(c)
    r1 = w["results"][0]
    assert r1["catch_up"] and r1["finished"] and r1["desync_frame"] == -1, w
    _wait(lambda: c.scene() == CSS and not _watch(c)["armed"], 60, "C back on the room's CSS")
    # Back: the next game waits for C.
    for g in gs:
        until_view(g, lambda v: v["status"] == 0, f"{g.name}: the room waiting", timeout=60)
    for g in (a, b):
        g.steps("tap START 8", "wait 30")
    time.sleep(5)
    assert room_view(a)["status"] == 0 and _phase(a) != "running", "started without C"
    c.shot("02-c-back-the-room-waits")
    c.steps("tap START 8", "wait 10")
    v = room_view(a)
    picker = next((g for g, s in zip(gs, v["slots"]) if s["picks_stage"]), None)
    if picker:
        _pick_stage_if_asked(picker, gs)
    _wait(lambda: all(_phase(g) == "running" for g in gs), 180, "the three players' game 2")
    rep = reports[1]
    assert rep["checksums"]["mismatches"] == 0 and rep["checksums"]["compared"] > 300, rep
    print("C's watch:", w, flush=True)
