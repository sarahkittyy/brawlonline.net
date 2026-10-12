"""A B held through a ranked stage select does not take the player out of the set.

Ranked's stage select takes B out (nothing leaves it before the steps are done). The game-2 winner
who holds B while the opponent picks is on the CSS with B still held when the stage select leaves
by itself, and Brawl's hold-B counter used to take them back to the ONLINE page: the set abandoned,
-100 (harness, 2026-10-12). Now a B already down when the online CSS comes up is taken out until
it is released (game-code online_menu.cpp holdBGuard): the winner stays on the CSS and locks in;
a new hold after the release still leaves, as Brawl's own way out.
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
    _strike(host, 0x02, join)
    _wait_view(join, lambda v: v["my_turn"], "the joiner's turn")
    _strike(join, 0x21, host)
    _strike(join, 0x2E, host)
    _strike(host, 0x2D, join)
    online_set.play_game(players, 1, winner=join)   # the guest wins, as in the players' reports
    winner, loser = join, host

    _both_on(players, SSS, "game 2: both on the stage select")
    _wait_view(winner, lambda v: v["game"] == 2 and v["type"] == 1 and v["my_turn"], "the ban")
    _strike(winner, 0x06, loser)
    _wait_view(winner, lambda v: v["type"] == 1 and v["my_turn"] and v["count"] == 1, "the second ban")
    _strike(winner, 0x02, loser)
    v = _wait_view(loser, lambda v: v["type"] == 2 and v["my_turn"], "the pick")
    n = _locks(winner)

    # The winner holds B from before the pick until well after the CSS is up (Brawl's hold-B
    # counter leaves after about a second).
    winner.c.pad_set(0, buttons=["B"])
    time.sleep(1.0)
    B.sss_pick_stage(loser.c, v["selectable"][0], 0)
    _both_on(players, online_set.CSS, "game 2: both on the CSS")
    winner.steps("wait 180")
    assert online_set.scene(winner.c) == online_set.CSS, online_set.scene(winner.c)
    assert _ranked(winner)["active"] and not _ranked(winner)["left_reported"], _ranked(winner)

    # Released, the winner locks in as usual.
    winner.c.pad_set(0)
    winner.steps("wait 10", "tap START 8", "wait 40")
    assert _locks(winner) > n, "the winner could not lock in"

    # A new hold is Brawl's own way out, as before (the set is abandoned).
    winner.c.pad_set(0, buttons=["B"])
    online_set.wait(lambda: online_set.scene(winner.c) != online_set.CSS, 30, "hold B leaves the CSS")
    winner.c.pad_set(0)
