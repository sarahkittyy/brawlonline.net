"""A B held through a ranked stage select does not take the player out of the set.

Ranked's stage select takes B out (nothing leaves it before the steps are done). The game-2 winner
who holds B while the opponent picks is on the CSS with B still held when the stage select leaves
by itself, and Brawl's hold-B counter used to take them back to the ONLINE page: the set abandoned,
-100 (harness, 2026-10-12). Now a B already down when the online CSS comes up is taken out until
it is released (game-code online_menu.cpp holdBGuard). Both are locked in before the stages (P+ is
character first, 2026-10-19), so the CSS is only passed through: game 2 starts with B still held,
and nobody left. A new hold after the release, on game 3's CSS, still leaves, as Brawl's own way
out.
"""

from __future__ import annotations

import time
from typing import Callable

import pytest

from ppharness import brawl as B
from ppharness.instance import DolphinInstance

from test_online_game import Game, _boot, _both, _peer_shown, online_set, ppom
from test_online_ranked import (SSS, STARTERS, _both_on, _locks, _ranked, _search_ranked, _strike,
                                _wait, _wait_view, backend)  # noqa: F401

pytestmark = [pytest.mark.dolphin, pytest.mark.server, pytest.mark.gpu]


@pytest.mark.slow
def test_b_held_through_the_stage_select_stays_on_the_css(backend, dolphin: Callable[..., DolphinInstance],
                                                          gpu_backend: str) -> None:
    test = "ranked-hold-b"
    ua, ub = backend.create_user("hba", "HBA"), backend.create_user("hbb", "HBB")
    a = _boot(dolphin, "hb-a", backend, ua, gpu_backend, test, "gameplay")
    b = _boot(dolphin, "hb-b", backend, ub, gpu_backend, test, "gameplay")
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
    for g in players:
        _wait(lambda g=g: g.c.mm_status()["handoff"] == "started", 30, "the hand-off")
    host = a if _ranked(a)["local_port"] == 0 else b
    join = b if host is a else a

    _both_on(players, SSS, "game 1: both on the stage select")
    for g in players:
        _wait_view(g, lambda v: v["type"] == 1 and sorted(v["selectable"]) == STARTERS, "the starters")
    _strike(host, 0x04, join)
    _wait_view(join, lambda v: v["my_turn"], "the joiner's turn")
    _strike(join, 0x21, host)
    _strike(join, 0x2E, host)
    _strike(host, 0x09, join)
    online_set.play_game(players, 1, winner=join)   # the guest wins, as in the players' reports
    winner, loser = join, host

    _both_on(players, SSS, "game 2: both on the stage select")
    v = _wait_view(winner, lambda v: v["game"] == 2 and v["type"] == 1 and v["my_turn"], "the ban")
    _strike(winner, v["selectable"][0], loser)
    v = _wait_view(winner, lambda v: v["type"] == 1 and v["my_turn"] and v["count"] == 1, "the second ban")
    _strike(winner, v["selectable"][0], loser)
    v = _wait_view(loser, lambda v: v["type"] == 2 and v["my_turn"], "the pick")

    # The winner holds B from before the pick until well after the CSS is passed (Brawl's hold-B
    # counter leaves after about a second).
    winner.c.pad_set(0, buttons=["B"])
    time.sleep(1.0)
    B.sss_pick_stage(loser.c, v["selectable"][0], 0)
    online_set.wait(lambda: all(online_set.gstatus(g.c)["phase"] == "running" for g in players), 120,
                    "game 2 running on both, B still held")
    winner.steps("wait 120")
    assert _ranked(winner)["active"] and not _ranked(winner)["left_reported"], _ranked(winner)
    winner.c.pad_set(0)
    online_set.play_game(players, 2, winner=loser)   # 1-1

    # On game 3's CSS (the character step) a new hold is Brawl's own way out, as before (the set
    # is abandoned).
    _both_on(players, online_set.CSS, "game 3: both on the CSS")
    winner.steps("wait 30")
    winner.c.pad_set(0, buttons=["B"])
    online_set.wait(lambda: online_set.scene(winner.c) != online_set.CSS, 30, "hold B leaves the CSS")
    winner.c.pad_set(0)
