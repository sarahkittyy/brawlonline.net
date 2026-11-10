"""Ranked with players named in Japanese (bug report #272: "After losing a round in a ranked
match, the game sometimes gets stuck"; always, against あほ).

The loser of a game comes back to the CSS while the winner picks first, and the CSS's first
frame prints the status line with the winner's name first ("あほ is picking, then you (45)"),
through the rule line's override (``Text::printStyled``). That copy kept the rule line's style
prefix only up to its colour's 00 byte (12 02 0C), so the game read the first four bytes of the
text as the colour; in a Japanese name that left Brawl's text walker on the middle byte of a
character, where it loops forever: the loser's game froze on the CSS (no more frames, no more
GP_FETCH_STEP), and the winner's game filed opponent_left 63 s later.

Here: two players with Japanese names, game 1 to the joiner; the host, the loser, waits on the
CSS for the winner's character: its game keeps running (the plugin's frame counter, a
screenshot), its line names the winner, and once the winner locks in it is the loser's turn.
Needs what ``test_online_ranked.py`` needs.
"""

from __future__ import annotations

import time
from typing import Any, Callable

import pytest

from ppharness import brawl as B
from ppharness.instance import DolphinInstance

from test_online import _wait  # noqa: F401  (the backend's helpers)
from test_online_game import _boot, _both, _peer_shown, online_set, ppom
import test_online_ranked as R

pytestmark = [pytest.mark.dolphin, pytest.mark.server, pytest.mark.gpu]

backend = R.backend


def _frames(g: Any) -> int:
    return ppom.read_debug(g.c, ppom.find_block(g.c))["frames"]


def test_loser_waits_for_a_japanese_named_winner(backend: Any, dolphin: Callable[..., DolphinInstance],
                                                  gpu_backend: str) -> None:
    test = "ranked-names"
    ua = backend.create_user("あほ", "AHOX", email="aho-ranked-names@example.test")
    ub = backend.create_user("あいうえお", "AIUE", email="aiueo-ranked-names@example.test")
    no_stream = {"Online": {"StreamGames": False}}
    a = _boot(dolphin, "rkn-a", backend, ua, gpu_backend, test, "gameplay", dolphin_ini=no_stream)
    b = _boot(dolphin, "rkn-b", backend, ub, gpu_backend, test, "gameplay", dolphin_ini=no_stream)
    _both(a.to_main_menu, b.to_main_menu)
    _both(a.to_online_page, b.to_online_page)
    _both(lambda: a.to_css("ranked"), lambda: b.to_css("ranked"))
    for g in (a, b):
        B.write_rules(g.c, stocks=1, minutes=2, items_off=True)
        ppom.allow_test_rules(g.c)
    R._search_ranked(a)
    R._search_ranked(b)
    _peer_shown(a, ub)
    _peer_shown(b, ua)
    players = [a, b]
    host = a if R._ranked(a)["local_port"] == 0 else b
    join = b if host is a else a

    # Game 1: both lock in, strikes 1-2-1, the joiner wins.
    R._both_on(players, online_set.CSS, "game 1: both on the CSS")
    for g in players:
        R._wait_view(g, lambda v: v["game"] == 1 and v["type"] == 3, "game 1's character step")
    join.steps("tap START 8", "wait 30")
    host.steps("tap START 8", "wait 30")
    R._both_on(players, R.SSS, "game 1: both on the stage select")
    R._strike(host, 0x04, join)
    R._strike(join, 0x21, host)
    R._strike(join, 0x2E, host)
    R._strike(host, 0x09, join)
    online_set.play_game(players, 1, winner=join)

    # The host lost: on the CSS it waits for the joiner's character, named in Japanese first on
    # its line. Its game must keep running.
    online_set.wait(lambda: online_set.scene(host.c) == online_set.CSS, 120, "the loser on the CSS")
    f0 = _frames(host)
    time.sleep(5)
    f1 = _frames(host)
    assert f1 - f0 > 120, ("the loser's game stopped on the CSS", f0, f1)
    v = R._wait_view(host, lambda v: v["game"] == 2 and v["type"] == 3, "the character step")
    assert not v["may_lock"] and v["text"].startswith(f"{join.name} is picking, then you ("), v
    host.shot("g2-css-loser-waits")
    join.steps("tap START 8", "wait 30")
    v = R._wait_view(host, lambda v: v["may_lock"], "the loser's turn")
    assert v["text"].startswith(f"Your turn: {join.name} picked "), v
    host.shot("g2-css-loser-turn")
