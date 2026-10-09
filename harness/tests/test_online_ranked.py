"""Ranked from the game's own menus, end to end: Slippi's game setup on P+'s stage select, and a
best-of-three set rated with Elo.

Two instances (each with its own ``user.json`` and its own SD card with our plugin) go main menu
-> PLAY ONLINE -> WITH ANYONE -> Ranked -> a character -> START. ``mm`` pairs the two from its
Ranked queue. Then, as Slippi's ranked game setup (slippi-ssbm-c ``Scenes/Ranked/GameSetup.c``)
on P+'s own stage select with its stage striking (Dolphin ``Online/GameSetup.cpp``, plugin
``stage_legal.cpp``):

- game 1: both games go to the stage select by themselves; only the five starters are selectable;
  player 1 (the host) strikes one, player 2 two, player 1 one, with X, each only on their turn
  (an X out of turn changes nothing); the strikes show on both; the stage left is played;
- games 2+: the winner of the last game bans one stage (X), the loser picks the stage (A); then the
  winner locks in a character first: the loser's START does nothing until then, and the loser's
  line names the winner's character;
- until one player has two wins. Both Dolphins report every game, the server rates the set once,
  the connection closes on the CSS after the set, and both CSSs show the rating and its change.

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
from test_online_game import Game, _boot, _both, _find_logged, _peer_shown, online_set, ppom

pytestmark = [pytest.mark.dolphin, pytest.mark.server, pytest.mark.gpu]

DEFAULT_RATING = 1400.0   # common::ranked::DEFAULT_RATING
FIRST_SET_SWING = 100.0   # K_START / 2: a first set between two players at the same rating
# server/config/rulesets.json ranked.starters: Battlefield, FD, Smashville, Dream Land, PS2.
STARTERS = [0x01, 0x02, 0x21, 0x2D, 0x2E]
SSS = "scSelStage"


@pytest.fixture(scope="module")
def backend() -> Iterator[OnlineBackend]:
    be = _make_backend()
    try:
        yield be
    finally:
        be.stop()


def _ranked(g: Game) -> dict[str, Any]:
    return g.c.online_status()["ranked"]


def _view(g: Game) -> dict[str, Any]:
    return _ranked(g)["setup"]["view"]


def _search_ranked(g: Game) -> None:
    n = g.finds()
    g.press_until("START", lambda: g.finds() > n, "FIND_OPPONENT")
    _find_logged(g, 0, "")


def _wait_view(g: Game, pred: Callable[[dict[str, Any]], bool], what: str, timeout: float = 30) -> dict[str, Any]:
    return _wait(lambda: (lambda v: v if pred(v) else None)(_view(g)), timeout, f"{g.inst.name}: {what}")


def _strike(g: Game, kind: int, other: Game) -> None:
    """X on `kind` on the stage select; both machines then show it struck."""
    before = len(_view(g)["selectable"])
    B.sss_pick_stage(g.c, kind, 0, button="X")
    for p in (g, other):
        # Struck on both; after the last strike the setup may already be done (no step left).
        _wait_view(p, lambda v: not v["active"] or v["type"] == 4 or
                   (kind not in v["selectable"] and len(v["selectable"]) == before - 1),
                   f"{kind:#x} struck")


def _both_on(players: list[Game], scene: str, what: str) -> None:
    online_set.wait(lambda: all(online_set.scene(p.c) == scene for p in players), 120, what)


def _locks(g: Game) -> int:
    return online_set.local(g.c)["lock"]["seq"]


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
        g.inst.wait_for_log(r"GET_RANK -> GET_RANK state=2 rating=1400\.0 sets=0", timeout=30)
        g.shot("03-rating-before")

    _search_ranked(a)
    _search_ranked(b)
    _peer_shown(a, ub)
    _peer_shown(b, ua)
    match_id = a.c.mm_status()["match"]["match_id"]
    assert match_id.startswith("mode.ranked-"), match_id
    for g in (a, b):
        _wait(lambda g=g: g.c.mm_status()["handoff"] == "started", 30, "the hand-off")
        r = _ranked(g)
        assert r["active"] and r["match_id"] == match_id and r["setup"]["starters"] == STARTERS, r
    players = [a, b]
    host = a if _ranked(a)["local_port"] == 0 else b
    join = b if host is a else a

    # Game 1: both on the stage select by themselves; strikes 1-2-1 over the five starters.
    _both_on(players, SSS, "game 1: both on the stage select")
    for g in players:
        v = _wait_view(g, lambda v: v["type"] == 1 and sorted(v["selectable"]) == STARTERS, "the starters")
        assert v["my_turn"] == (g is host) and v["count"] == 1, v
    host.steps("wait 30")
    host.shot("g1-sss-host-turn")
    join.shot("g1-sss-join-waits")
    assert _view(host)["text"].startswith("Strike 1 stage"), _view(host)
    assert _view(join)["text"].startswith("Opponent is striking"), _view(join)
    # Out of turn: the joiner's X does nothing.
    B.sss_pick_stage(join.c, 0x01, 0, button="X")
    join.steps("wait 30")
    assert sorted(_view(join)["selectable"]) == STARTERS and _view(join)["my_turn"] is False
    _strike(host, 0x02, join)
    v = _wait_view(join, lambda v: v["my_turn"], "the joiner's turn")
    assert v["count"] == 2 and v["text"].startswith("Strike 2 stages"), v
    _strike(join, 0x21, host)
    _strike(join, 0x2E, host)
    join.shot("g1-sss-after-join")
    _strike(host, 0x2D, join)
    # Battlefield is left: both leave for the CSS and the match.
    rep1 = online_set.play_game(players, 1)
    assert rep1["stage"] == 0x01, rep1
    games = [rep1]

    game = 1
    while True:
        wins = [_wait(lambda g=g: (lambda r: r if sum(r["wins"]) >= len(games) else None)(_ranked(g)), 30,
                      f"game {game} counted") for g in players]
        if any(r["set_over"] for r in wins):
            break
        game += 1
        assert game <= 5, "a best of three that does not end"
        winner_port = _ranked(a)["setup"]["last_winner"]
        winner = a if _ranked(a)["local_port"] == winner_port else b
        loser = b if winner is a else a

        # The winner bans one stage, the loser picks one.
        _both_on(players, SSS, f"game {game}: both on the stage select")
        v = _wait_view(winner, lambda v: v["game"] == game and v["type"] == 1 and v["my_turn"], "the ban")
        assert v["text"].startswith("Ban 1 stage"), v
        assert _wait_view(loser, lambda v: v["game"] == game and v["type"] == 1, "waits")["text"].startswith(
            "Opponent is banning")
        sel = v["selectable"]
        ban, pick = sel[0], sel[1]
        _strike(winner, ban, loser)
        v = _wait_view(loser, lambda v: v["type"] == 2 and v["my_turn"], "the pick")
        assert ban not in v["selectable"] and v["text"].startswith("Pick a stage"), v
        loser.shot(f"g{game}-sss-loser-picks")
        B.sss_pick_stage(loser.c, pick, 0)
        _both_on(players, online_set.CSS, f"game {game}: both back on the CSS")

        # Characters: the winner first; the loser's START does nothing until then.
        v = _wait_view(loser, lambda v: v["type"] == 3, "the character step")
        assert not v["may_lock"] and v["text"] == "Opponent is choosing", v
        n = _locks(loser)
        loser.steps("tap START 8", "wait 40")
        assert _locks(loser) == n, "the loser locked in before the winner"
        loser.shot(f"g{game}-css-loser-waits")
        winner.steps("tap START 8", "wait 30")
        v = _wait_view(loser, lambda v: v["may_lock"], "the loser's turn")
        assert v["text"].startswith("Opponent picked "), v
        loser.shot(f"g{game}-css-loser-turn")
        loser.steps("tap START 8", "wait 30")
        rep = online_set.play_game(players, game)
        assert rep["stage"] == pick, (hex(rep["stage"]), hex(pick))
        games.append(rep)

    for rep in games:
        assert rep["checksums"]["mismatches"] == 0 and all(e == "game set" for e in rep["end"]), rep

    # The set is over: back on the CSS the connection closes by itself, and nobody left.
    _both_on(players, online_set.CSS, "both back on the CSS after the set")
    for g in players:
        _wait(lambda g=g: g.c.mm_status()["state"] == "idle" and g.c.mm_status()["handoff"] == "", 30,
              f"{g.inst.name}: the connection closed after the set")
        r = _ranked(g)
        assert r["set_over"] and not r["left_reported"] and not r["peer_gone"], r

    local_port = {g.inst.name: _ranked(g)["local_port"] for g in players}
    w = _ranked(a)["wins"]
    assert w == _ranked(b)["wins"], (w, _ranked(b)["wins"])
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
