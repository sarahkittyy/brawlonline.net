"""Scratch repro (not to commit): a ranked set forced to 1-1 so that game 3's winner is the
player who lost game 1; on the stage select that winner moves and presses buttons while the
loser picks, as a person would, and back on the CSS the hand must move and START lock in."""

from __future__ import annotations

import random
import threading
from typing import Any, Callable

import pytest

from ppharness import brawl as B
from ppharness.backend import OnlineBackend
from ppharness.instance import DolphinInstance

from test_online import _wait
from test_online_game import Game, _boot, _both, _peer_shown, online_set, ppom
from test_online_ranked import (SSS, STARTERS, _both_on, _locks, _ranked, _search_ranked, _strike,  # noqa: F401
                                _view, _wait_view, backend)

pytestmark = [pytest.mark.dolphin, pytest.mark.server, pytest.mark.gpu]

BUTTONS = ["A", "B", "X", "Y", "Z", "L", "R", "START"]


def _wander(g: Game, stop: threading.Event, seed: int, log: list) -> None:
    rng = random.Random(seed)
    c = g.c
    while not stop.is_set():
        main = (rng.choice([1, 64, 128, 192, 255]), rng.choice([1, 64, 128, 192, 255]))
        buttons = [rng.choice(BUTTONS)] if rng.random() < 0.35 else []
        if "START" in buttons and online_set.scene(c) != SSS:
            buttons = []
        try:
            c.pad_set(0, buttons=buttons, main=main)
            B.step(c, rng.randint(2, 12))
            c.pad_set(0, main=main)
            B.step(c, rng.randint(1, 6))
        except Exception as e:  # noqa: BLE001
            log.append(repr(e))
            return
        log.append((online_set.scene(c), main, buttons))
    c.pad_set(0)


def _hand(g: Game) -> tuple:
    a = B.read_css_area(g.c.read_mem, 0)
    return (a.hand_x, a.hand_y, a.hand_target) if a else None


def _check_css(winner: Game, loser: Game, game: int) -> None:
    v = _wait_view(winner, lambda v: v["type"] == 3 and v["game"] == game, "the character step", 60)
    print(f"game {game}: winner view {v}")
    winner.c.pad_set(0)
    B.step(winner.c, 20)
    st = online_set.local(winner.c)
    print(f"game {game}: winner local {st}")
    print(f"game {game}: debug {ppom.read_debug(winner.c, ppom.find_block(winner.c))}")
    h0 = _hand(winner)
    winner.steps("stick right 30")
    h1 = _hand(winner)
    winner.steps("stick up 30")
    h2 = _hand(winner)
    winner.shot(f"g{game}-css-winner")
    print(f"game {game}: hand {h0} -> {h1} -> {h2}")
    assert h0 and h1 and h2 and (h0[:2] != h1[:2] or h1[:2] != h2[:2]), ("the hand does not move", h0, h1, h2)
    n = _locks(winner)
    winner.steps("tap START 8", "wait 30")
    assert _locks(winner) > n, ("START did not lock in", _view(winner))


@pytest.mark.slow
def test_ranked_game3_winner_after_pick(backend: OnlineBackend, dolphin: Callable[..., DolphinInstance],
                                        gpu_backend: str) -> None:
    test = "ranked-g3-repro"
    ua, ub = backend.create_user("rhea", "RHEA"), backend.create_user("sven", "SVEN")
    a = _boot(dolphin, "r3-a", backend, ua, gpu_backend, test, "gameplay")
    b = _boot(dolphin, "r3-b", backend, ub, gpu_backend, test, "gameplay")
    _both(a.to_main_menu, b.to_main_menu)
    _both(a.to_online_page, b.to_online_page)
    _both(lambda: a.to_css("ranked"), lambda: b.to_css("ranked"))
    for g in (a, b):
        B.write_rules(g.c, stocks=1, minutes=2, items_off=True)
        ppom.allow_test_rules(g.c)
    _search_ranked(a)
    _search_ranked(b)
    _peer_shown(a, ub)
    _peer_shown(b, ua)
    players = [a, b]
    host = a if _ranked(a)["local_port"] == 0 else b
    join = b if host is a else a

    _both_on(players, SSS, "game 1: both on the stage select")
    _wait_view(host, lambda v: v["type"] == 1 and v["my_turn"], "host strikes")
    _strike(host, 0x02, join)
    _wait_view(join, lambda v: v["my_turn"], "join strikes")
    _strike(join, 0x21, host)
    _strike(join, 0x2E, host)
    _strike(host, 0x2D, join)
    online_set.play_game(players, 1, winner=a)

    for game, winner, loser in ((2, a, b), (3, b, a)):
        _wait(lambda: all(sum(_ranked(g)["wins"]) >= game - 1 for g in players), 30, f"game {game - 1} counted")
        assert _ranked(a)["setup"]["last_winner"] == _ranked(winner)["local_port"], _ranked(a)
        _both_on(players, SSS, f"game {game}: both on the stage select")
        while True:
            v = _wait_view(winner, lambda v: v["game"] == game and v["type"] in (1, 2), "a stage step")
            if v["type"] != 1:
                break
            _strike(winner, v["selectable"][0], loser)
        v = _wait_view(loser, lambda v: v["type"] == 2 and v["my_turn"], "the pick")
        stop = threading.Event()
        log: list = []
        th = threading.Thread(target=_wander, args=(winner, stop, game, log))
        th.start()
        try:
            B.sss_pick_stage(loser.c, v["selectable"][0], 0)
            # The winner keeps going for a moment on the CSS too.
            online_set.wait(lambda: online_set.scene(winner.c) == online_set.CSS, 60, "winner on the CSS")
            B.step(winner.c, 40)
        finally:
            stop.set()
            th.join()
        print(f"game {game}: wander log tail {log[-8:]}")
        _both_on(players, online_set.CSS, f"game {game}: both back on the CSS")
        _check_css(winner, loser, game)
        _wait_view(loser, lambda v: v["may_lock"], "the loser's turn")
        loser.steps("tap START 8", "wait 30")
        online_set.play_game(players, game, winner=loser if game == 2 else None)
