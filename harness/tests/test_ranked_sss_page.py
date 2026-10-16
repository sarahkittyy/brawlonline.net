"""Ranked's stage select shows only the set's stages (game-code/PPOnline/source/stage_legal.cpp;
docs/game-code.md §11, "Ranked's stage select page").

Two instances are paired from the Ranked queue. On each stage select the page P+ shows is the
set's: the counterpicks that are not starters, then the starters, in the server's order (P+ lays
nine out as a row of four over a row of five). Game 1 strikes the starters, so the counterpicks'
icons are hidden; game 2 shows all nine. Once the stage select is gone P+'s own pages are back.

Screenshots go to run/artifacts/game-bridge/ranked-sss-page/. Needs what test_online_ranked.py
needs.
"""

from __future__ import annotations

import struct
from typing import Callable, Iterator

import pytest

from ppharness import brawl as B
from ppharness import paths
from ppharness.backend import OnlineBackend
from ppharness.instance import DolphinInstance

from test_online import _make_backend
from test_online_game import Game, _boot, _both, _peer_shown, online_set
from test_ranked_sss_portraits import _sym
from test_online_ranked import (SSS, STARTERS, _both_on, _paired, _ranked, _search_ranked, _strike,
                                _strike_to, _to_ranked_css, _wait, _wait_view)

pytestmark = [pytest.mark.dolphin, pytest.mark.server, pytest.mark.gpu]

# server/config/rulesets.json "ranked": its stages and starters, in order (the stage select's rows,
# the user's, 2026-10-16: Green Hill Zone, Fountain of Dreams, Frigate Husk, Dream Land over Temple
# of Time, Smashville, Battlefield, PS2, Luigi's Mansion).
RANKED_STAGES = [35, 31, 12, 45, 9, 33, 1, 46, 4]
RANKED_STARTERS = [9, 33, 1, 46, 4]


@pytest.fixture(scope="module")
def backend() -> Iterator[OnlineBackend]:
    """mm with the repository's rulesets.json (not the copy built into the binary, which can be
    older than the file)."""
    be = _make_backend(rulesets_file=paths.workspace_root() / "server" / "config" / "rulesets.json")
    try:
        yield be
    finally:
        be.stop()


def _debug(g: Game) -> dict[str, int]:
    """stage_legal.cpp g_sssPage: page setups, set pages, the last setup's outcome, restores."""
    v = struct.unpack(">4I", g.c.read_mem(_sym(g, "g_sssPage"), 16))
    return {"setups": v[0], "set": v[1], "outcome": v[2], "restored": v[3]}


def _page(g: Game) -> list[int]:
    """The stage select's page (P+'s CURRENT_PAGE) as stage kinds, once its task is made (the scene
    is scSelStage a few frames before the stage select sets its pages up)."""
    _wait(lambda: B.read_sss(g.c.read_mem), 30, f"{g.inst.name}: the stage select's task")
    return B.read_stage_pages(g.c.read_mem)[g.c.read_mem(B.SSS_CURRENT_PAGE, 1)[0]]


@pytest.mark.slow
def test_ranked_stage_select_shows_only_the_sets_stages(backend, dolphin: Callable[..., DolphinInstance],
                                                        gpu_backend: str) -> None:
    test = "ranked-sss-page"
    ua, ub = backend.create_user("rpga", "RPGA"), backend.create_user("rpgb", "RPGB")
    a = _boot(dolphin, "rspg-a", backend, ua, gpu_backend, test, "gameplay")
    b = _boot(dolphin, "rspg-b", backend, ub, gpu_backend, test, "gameplay")
    _both(a.to_main_menu, b.to_main_menu)
    _both(a.to_online_page, b.to_online_page)
    _both(lambda: a.to_css("ranked"), lambda: b.to_css("ranked"))
    pplus_page = B.read_stage_pages(a.c.read_mem)[0]
    assert len(pplus_page) > len(RANKED_STAGES), pplus_page
    _to_ranked_css(a, b)
    _search_ranked(a)
    _search_ranked(b)
    _peer_shown(a, ub)
    _peer_shown(b, ua)
    _paired(a, b)
    players = [a, b]
    host = a if _ranked(a)["local_port"] == 0 else b
    join = b if host is a else a
    assert sorted(RANKED_STARTERS) == STARTERS
    others = [k for k in RANKED_STAGES if k not in STARTERS]
    page = others + RANKED_STARTERS

    _both_on(players, SSS, "game 1: both on the stage select")
    for g in players:
        _wait_view(g, lambda v: v["type"] == 1 and sorted(v["selectable"]) == STARTERS, "the starters")
        assert _page(g) == page, (g.inst.name, _page(g), _debug(g))
    host.steps("wait 30")
    host.shot("g1-sss-host")
    join.shot("g1-sss-join")
    _strike_to(host, join, 0x01)
    rep1 = online_set.play_game(players, 1)
    assert rep1["stage"] == 0x01, rep1
    for g in players:
        assert B.read_stage_pages(g.c.read_mem)[0] == pplus_page   # P+'s pages back for the match

    winner_port = _wait(lambda: (lambda r: r if sum(r["wins"]) >= 1 else None)(_ranked(a)), 30,
                        "game 1 counted")["setup"]["last_winner"]
    winner = a if _ranked(a)["local_port"] == winner_port else b
    loser = b if winner is a else a
    _both_on(players, SSS, "game 2: both on the stage select")
    v = _wait_view(winner, lambda v: v["game"] == 2 and v["type"] == 1 and v["my_turn"], "the ban")
    for g in players:
        assert _page(g) == page, (g.inst.name, _page(g), _debug(g))
    winner.steps("wait 30")
    winner.shot("g2-sss-winner")
    loser.shot("g2-sss-loser")
    # A top-row stage is banned and a starter picked, through the new layout.
    _strike(winner, others[0], loser)
    _wait_view(winner, lambda v: v["type"] == 1 and v["my_turn"] and v["count"] == 1, "the second ban")
    _strike(winner, RANKED_STARTERS[0], loser)
    v = _wait_view(loser, lambda v: v["type"] == 2 and v["my_turn"], "the pick")
    loser.shot("g2-sss-loser-picks")
    pick = others[1]
    assert pick in v["selectable"], v
    B.sss_pick_stage(loser.c, pick, 0)
    online_set.wait(lambda: all(online_set.scene(g.c) != SSS for g in players), 120, "game 2's stage select left")
    for g in players:
        _wait(lambda g=g: B.read_stage_pages(g.c.read_mem)[0] == pplus_page or None, 30, "P+'s pages back")
