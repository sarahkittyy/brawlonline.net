"""Ranked from the game's own menus, end to end: a best-of-three set rated with Elo.

Two instances (each with its own ``user.json`` and its own SD card with our plugin) go main menu
-> PLAY ONLINE -> WITH ANYONE -> Ranked -> a character -> START. The plugin posts
``FIND_OPPONENT mode=0``; ``mm`` pairs the two from its Ranked queue (both at the default rating,
so inside the rating band at once). The set runs under the gameplay-only session like Unranked
(every stage random from the server's Ranked list), with random inputs, until one player has two
wins. Then:

- both Dolphins have reported every game to ``accounts`` (``POST /v1/ranked/report-game``) and the
  server has settled the set once (``COMPLETE``, the same winner both Dolphins counted);
- the connection closes by itself once the games are back on the character select (Slippi's
  ranked set end): matchmaking is idle again, nobody reported a leave;
- each Dolphin fetched the result: the winner's rating rose by K/2 and the loser's fell by K/2
  (two first-timers at the default rating, ``server/crates/common/src/ranked.rs``), and the game
  reads it with ``GET_RANK``, which the CSS shows as e.g. "1500 (+100)".

Instances are named ``rk-*``. Screenshots go to ``run/artifacts/game-bridge/ranked-set/``. Needs
what ``test_online_unranked.py`` needs.
"""

from __future__ import annotations

import re
from typing import Any, Callable, Iterator

import pytest

from ppharness import brawl as B
from ppharness.backend import OnlineBackend
from ppharness.instance import DolphinInstance

from test_online import _make_backend, _wait
from test_online_game import CMD_GET_MATCH_STATE, Game, _boot, _both, _find_logged, _peer_shown, online_set, ppom

pytestmark = [pytest.mark.dolphin, pytest.mark.server, pytest.mark.gpu]

DEFAULT_RATING = 1400.0   # common::ranked::DEFAULT_RATING
FIRST_SET_SWING = 100.0   # K_START / 2: a first set between two players at the same rating


@pytest.fixture(scope="module")
def backend() -> Iterator[OnlineBackend]:
    be = _make_backend()
    try:
        yield be
    finally:
        be.stop()


def _ranked(g: Game) -> dict[str, Any]:
    return g.c.online_status()["ranked"]


def _search_ranked(g: Game) -> None:
    n = g.finds()
    g.press_until("START", lambda: g.finds() > n, "FIND_OPPONENT")
    _find_logged(g, 0, "")


@pytest.mark.slow
def test_ranked_set_is_rated(backend: OnlineBackend, dolphin: Callable[..., DolphinInstance],
                             gpu_backend: str) -> None:
    test = "ranked-set"
    ua, ub = backend.create_user("rhea", "RHEA"), backend.create_user("sven", "SVEN")
    a = _boot(dolphin, "rk-a", backend, ua, gpu_backend, test, "gameplay")
    b = _boot(dolphin, "rk-b", backend, ub, gpu_backend, test, "gameplay")
    _both(a.to_main_menu, b.to_main_menu)
    _both(a.to_online_page, b.to_online_page)
    _both(lambda: a.to_css("ranked"), lambda: b.to_css("ranked"))
    for g in (a, b):
        assert ppom.read_debug(g.c, ppom.find_block(g.c))["menuState"] - 1 == 0   # Ranked's CSS
        B.write_rules(g.c, stocks=1, minutes=2, items_off=True)
        ppom.allow_test_rules(g.c)
        # The CSS asks for the rating (GET_RANK) and has it before any set.
        g.inst.wait_for_log(r"GET_RANK -> GET_RANK state=2 rating=1400\.0 sets=0", timeout=30)
        g.shot("03-rating-before")

    _search_ranked(a)
    _search_ranked(b)
    ra, rb = _peer_shown(a, ub), _peer_shown(b, ua)
    assert {ra["role"], rb["role"]} == {1, 2}, (ra, rb)
    sa, sb = a.c.mm_status(), b.c.mm_status()
    match_id = sa["match"]["match_id"]
    assert match_id == sb["match"]["match_id"] and match_id.startswith("mode.ranked-"), (sa, sb)
    assert sa["mode"] == "ranked", sa
    for g in (a, b):
        _wait(lambda g=g: g.c.mm_status()["handoff"] == "started", 30, "the hand-off")
        r = _ranked(g)
        assert r["active"] and r["match_id"] == match_id and r["wins"] == [0, 0], r
    assert "ranked ticket waiting" in backend.log_text("mm")

    # Best of three: games until someone has two wins.
    players = [a, b]
    games = []
    game = 1
    while True:
        if game > 1:
            online_set.lock_in_next(players, game, mode="unranked")
        rep = online_set.play_game(players, game)
        assert rep["checksums"]["mismatches"] == 0 and all(e == "game set" for e in rep["end"]), rep
        games.append(rep)
        wins = [_wait(lambda g=g: (lambda r: r if sum(r["wins"]) >= len(games) or r["set_over"] else None)(
            _ranked(g)), 30, f"game {game} counted") for g in players]
        if any(r["set_over"] for r in wins):
            assert all(r["set_over"] for r in wins), ("both Dolphins must end the set", wins)
            break
        assert game < 9, "a best of three that does not end"
        online_set.back_on_css(players, game)
        game += 1

    # The set is over: back on the CSS the connection closes by itself, and nobody left.
    online_set.wait(lambda: all(online_set.scene(g.c) == online_set.CSS for g in players), 120,
                    "both back on the CSS after the set")
    for g in players:
        _wait(lambda g=g: g.c.mm_status()["state"] == "idle" and g.c.mm_status()["handoff"] == "", 30,
              f"{g.inst.name}: the connection closed after the set")
        g.inst.wait_for_log(r"the ranked set is over; closing the connection", timeout=5)
        r = _ranked(g)
        assert not r["left_reported"] and not r["peer_gone"], r

    # Both Dolphins counted the same winner; the server rated the set with it, once.
    local_port = {g.inst.name: _ranked(g)["local_port"] for g in players}
    wins_by = {g.inst.name: _ranked(g)["wins"] for g in players}
    assert wins_by["rk-a"] == wins_by["rk-b"], wins_by
    w = wins_by["rk-a"]
    winner_port = 0 if w[0] >= 2 else 1
    winner = a if local_port["rk-a"] == winner_port else b
    winner_user, loser_user = (ua, ub) if winner is a else (ub, ua)

    for g in players:
        r = _wait(lambda g=g: (lambda r: r if r["change"] is not None else None)(_ranked(g)), 60,
                  f"{g.inst.name}: the set's result")
        assert r["last_status"] == "COMPLETE", r
        want = FIRST_SET_SWING if g is winner else -FIRST_SET_SWING
        assert abs(r["change"] - want) < 0.01 and abs(r["rating"] - (DEFAULT_RATING + want)) < 0.01, r
        assert r["reports_failed"] == 0 and r["reports_sent"] == len(games), r
        sign = "+" if want > 0 else "-"
        g.inst.wait_for_log(rf"GET_RANK state=2 rating={DEFAULT_RATING + want:.1f} sets=1 "
                            rf"change={re.escape(sign)}{abs(want):.1f}", timeout=30)
        g.steps("wait 60")
        g.shot("09-rating-after")

    res = backend._http("GET", f"/v1/ranked/result?matchId={match_id}&uid={winner_user.uid}")
    assert res["status"] == "COMPLETE" and res["winner"] == winner_user.uid, res
    for u, want in ((winner_user, DEFAULT_RATING + FIRST_SET_SWING), (loser_user, DEFAULT_RATING - FIRST_SET_SWING)):
        pub = backend._http("GET", f"/user/{u.uid}?additionalFields=chatMessages,rank")
        assert abs(pub["rank"]["ratingOrdinal"] - want) < 0.01 and pub["rank"]["ratingUpdateCount"] == 1, pub
    log = backend.log_text("accounts")
    assert log.count("ranked game reported") == 2 * len(games), log[-3000:]
    assert "ranked set settled" in log and "ranked leave reported" not in log
    for g in players:
        assert g.bridge()["lost"] == 0
