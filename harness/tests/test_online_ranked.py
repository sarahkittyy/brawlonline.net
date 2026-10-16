"""Ranked from the game's own menus, end to end: Slippi's game setup on P+'s stage select, and a
best-of-three set rated with Elo.

Two instances (each with its own ``user.json`` and its own SD card with our plugin) go main menu
-> PLAY ONLINE -> WITH ANYONE -> Ranked -> a character -> START. ``mm`` pairs the two from its
Ranked queue. Then, as Slippi's ranked game setup (slippi-ssbm-c ``Scenes/Ranked/GameSetup.c``)
on P+'s own stage select with its stage striking (Dolphin ``Online/GameSetup.cpp``, plugin
``stage_legal.cpp``):

- game 1: both games go to the stage select by themselves; only the five starters are selectable;
  player 1 (the host) strikes one, player 2 two, player 1 one, with X, each only on their turn
  (an X out of turn changes nothing); the strikes show on both; START and B do nothing there (the
  stage select used to leave for the CSS mid-strike); the stage left is played;
- games 2+: the winner of the last game bans two stages (X), the loser picks the stage (A); then the
  winner locks in a character first: the loser's START does nothing until then, and the loser's
  line names the winner's character; in game 2 the loser lets the 45 s (and Slippi's 3 s of grace)
  run out and the CSS locks in the character on it;
- until one player has two wins. Both Dolphins report every game, the server rates the set once,
  the connection closes on the CSS after the set, and both CSSs show the rating and its change.

Instances are named ``rk-*``. Screenshots go to ``run/artifacts/game-bridge/ranked-set/``. Needs
what ``test_online_unranked.py`` needs.
"""

from __future__ import annotations

import calendar
import datetime
import re
import struct
import threading
import time
from typing import Any, Callable, Iterator

import pytest

from ppharness import brawl as B
from ppharness import flows as F
from ppharness.backend import OnlineBackend
from ppharness.instance import DolphinInstance

from test_online import _make_backend, _wait
from test_online_game import (Game, _boot, _both, _find_logged, _p1_damage_addr, _peer_shown, online_set,
                              ppom)

pytestmark = [pytest.mark.dolphin, pytest.mark.server, pytest.mark.gpu]

DEFAULT_RATING = 1400.0   # common::ranked::DEFAULT_RATING
FIRST_SET_SWING = 100.0   # K_START / 2: a first set between two players at the same rating
# server/config/rulesets.json ranked.starters: Battlefield, Luigi's Mansion, Temple of Time,
# Smashville, PS2.
STARTERS = [0x01, 0x04, 0x09, 0x21, 0x2E]
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
        assert r["active"] and r["match_id"] == match_id and sorted(r["setup"]["starters"]) == STARTERS, r
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
    # Out of turn: the joiner's X does nothing, nor does A on the same stage (it takes no stage).
    B.sss_pick_stage(join.c, 0x01, 0, button="X")
    join.steps("wait 30", "tap A 8", "wait 40")
    assert online_set.scene(join.c) == SSS, online_set.scene(join.c)
    assert sorted(_view(join)["selectable"]) == STARTERS and _view(join)["my_turn"] is False
    _strike(host, 0x04, join)
    v = _wait_view(join, lambda v: v["my_turn"], "the joiner's turn")
    assert v["count"] == 2 and v["text"].startswith("Strike 2 stages"), v
    # START (P+'s random pick) and B (back to the CSS) leave no ranked stage select: neither the
    # host who just struck nor the joiner whose turn it is.
    for g in players:
        g.steps("tap START 8", "wait 30", "tap B 8", "wait 40")
    for g in players:
        assert online_set.scene(g.c) == SSS, (g.inst.name, online_set.scene(g.c))
    v = _view(join)
    assert v["my_turn"] and v["count"] == 2 and len(v["selectable"]) == 4, v
    _strike(join, 0x21, host)
    _strike(join, 0x2E, host)
    join.shot("g1-sss-after-join")
    _strike(host, 0x09, join)
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

        # The winner bans two stages, the loser picks one.
        _both_on(players, SSS, f"game {game}: both on the stage select")
        v = _wait_view(winner, lambda v: v["game"] == game and v["type"] == 1 and v["my_turn"], "the ban")
        assert v["text"].startswith("Ban 2 stages") and v["count"] == 2, v
        assert _wait_view(loser, lambda v: v["game"] == game and v["type"] == 1, "waits")["text"].startswith(
            "Opponent is banning")
        sel = v["selectable"]
        bans, pick = sel[:2], sel[2]
        _strike(winner, bans[0], loser)
        v = _wait_view(winner, lambda v: v["type"] == 1 and v["my_turn"] and v["count"] == 1, "the second ban")
        assert v["text"].startswith("Ban 1 stage"), v
        _strike(winner, bans[1], loser)
        v = _wait_view(loser, lambda v: v["type"] == 2 and v["my_turn"], "the pick")
        assert not set(bans) & set(v["selectable"]) and v["text"].startswith("Pick a stage"), v
        loser.shot(f"g{game}-sss-loser-picks")
        # The winner's cursor rests on P+'s page button: the stage select still leaves by itself
        # once the loser picked (staging 2026-10-12: its A turned the page every frame instead).
        page = B.sss_hover_item(winner.c, B.SSS_ITEM_PAGE).page
        B.sss_pick_stage(loser.c, pick, 0)
        def left_or_turned() -> bool:
            v = B.read_sss(winner.c.read_mem)
            return online_set.scene(winner.c) != SSS or (v is not None and v.page != page)
        online_set.wait(left_or_turned, 20, f"game {game}: the winner leaves the stage select")
        assert online_set.scene(winner.c) != SSS, ("the winner's stage select turned the page", page)
        _both_on(players, online_set.CSS, f"game {game}: both back on the CSS")

        # Characters: the winner first; the loser's START does nothing until then.
        v = _wait_view(loser, lambda v: v["type"] == 3, "the character step")
        # The winner by name, and that the loser is next (staging 2026-10-12).
        assert not v["may_lock"] and v["text"].startswith(f"{winner.name} is picking, then you ("), v
        n = _locks(loser)
        loser.steps("tap START 8", "wait 40")
        assert _locks(loser) == n, "the loser locked in before the winner"
        loser.shot(f"g{game}-css-loser-waits")
        # The loser's token stays put during the winner's turn; the winner locks in, and then A
        # and B on the winner's locked-in token do nothing either (staging 2026-10-12).
        loser.steps("tap B 8", "wait 30")
        lt = B.read_css_area(loser.c.read_mem, 0)
        assert lt is not None and lt.placed, lt
        winner.steps("tap START 8", "wait 30")
        for k in ("A", "B"):
            winner.steps(f"tap {k} 8", "wait 30")
            wt = B.read_css_area(winner.c.read_mem, 0)
            assert wt is None or wt.placed, (k, wt)
        v = _wait_view(loser, lambda v: v["may_lock"], "the loser's turn")
        assert v["text"].startswith(f"Your turn: {winner.name} picked "), v
        loser.shot(f"g{game}-css-loser-turn")
        if game == 2:
            # Slippi's character step: 45 s, then 3 s of grace, then the character selected.
            _wait(lambda: _locks(loser) > n, 75, "the loser's time ran out")
        else:
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
    winner = a if local_port[a.inst.name] == winner_port else b
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
    assert log.count(" game reported") == 2 * len(games), log[-3000:]
    assert "ranked set settled" in log and "ranked leave reported" not in log
    for g in players:
        assert g.bridge()["lost"] == 0


# P+'s hold-to-pause timers (u32 at 0x80584080 + 8 * pad; NETPLAY.txt "Hold Start to Pause v3").
PPLUS_PAUSE_HOLD = 0x80584080
# The game's pause state, as P+'s hold code reads it: (*(r13 - 0x4350) + 0x18) -> +0x3C, 1 paused.
PAUSE_STATE_PTR = 0x805A4420 - 0x4350


def _paused(g: Game) -> bool:
    p = g.c.read_u32(PAUSE_STATE_PTR)
    if not 0x80000000 <= p < 0x81800000:
        return False
    q = g.c.read_u32(p + 0x18)
    return 0x80000000 <= q < 0x81800000 and g.c.read_u32(q + 0x3C) == 1


@pytest.mark.slow
@pytest.mark.parametrize("how", ["window", "app"])
def test_ranked_has_no_pause_and_closing_abandons(backend: OnlineBackend, dolphin: Callable[..., DolphinInstance],
                                                  gpu_backend: str, how: str) -> None:
    """Ranked has no pause, as Slippi's (the user's, 2026-10-14: a player could pause a ranked game
    and never unpause, and the opponent's only way out abandoned the set). Game 1 running: A holds
    START far past P+'s 50 frames, then L+R+A+START: nothing pauses on either machine, nothing
    quits, the game runs on. Then A leaves the only way left, closing the game: its window
    (`window`: emulation stops, as the window's X does) or the whole app (`app`: the harness's
    quit, as Cmd-Q). A reports its own leave right away, so the server settles the set as
    abandoned by A at once, not after the opponent's report and the 2-minute grace; A takes the
    set loss (no game was played, so B is credited nothing)."""
    test = f"ranked-no-pause-{how}"
    ua, ub = backend.create_user(f"rnp{how[0]}a", f"RNA{how[0].upper()}"), backend.create_user(f"rnp{how[0]}b", f"RNB{how[0].upper()}")
    a = _boot(dolphin, f"rnp{how[0]}-a", backend, ua, gpu_backend, test, "gameplay")
    b = _boot(dolphin, f"rnp{how[0]}-b", backend, ub, gpu_backend, test, "gameplay")
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
    match_id = a.c.mm_status()["match"]["match_id"]
    host = a if _ranked(a)["local_port"] == 0 else b
    join = b if host is a else a
    _both_on(players, SSS, "game 1: both on the stage select")
    _wait_view(host, lambda v: v["type"] == 1 and v["my_turn"], "the host's strike")
    _strike(host, 0x04, join)
    _wait_view(join, lambda v: v["my_turn"], "the joiner's turn")
    _strike(join, 0x21, host)
    _strike(join, 0x2E, host)
    _strike(host, 0x09, join)
    online_set.wait(lambda: all(online_set.gstatus(g.c)["phase"] == "running" for g in players), 240,
                    "game 1 running on both")

    # START held 2 s (P+ pauses after 50 frames), then the pause screen's quit: neither does anything.
    a.steps("wait 120", "hold START 120")
    for g in players:
        assert not _paused(g), (g.inst.name, "paused")
        timers = [g.c.read_u32(PPLUS_PAUSE_HOLD + 8 * pad) for pad in range(4)]
        assert all(t < 50 for t in timers), (g.inst.name, timers)
    a.shot("01-start-held")
    a.steps("hold L+R+A+START 90", "wait 60")
    for g in players:
        assert not _paused(g), (g.inst.name, "paused")
        assert not g.debug_scratch()[10] & 0x40000, (g.inst.name, "a pause-screen quit")
        st = online_set.gstatus(g.c)
        assert st["phase"] == "running" and st["match_index"] == 0, st
        assert _ranked(g)["wins"] == [0, 0], _ranked(g)
    a.shot("02-still-running")

    # A closes the game mid-set.
    left_at = time.monotonic()
    if how == "window":
        a.c.call("emu_stop")
    else:
        a.inst.stop()
    _wait(lambda: _ranked(b)["peer_gone"], 30, "B sees A gone")
    res = _wait(lambda: (lambda r: r if r["status"] != "ASSIGNED" else None)(
        backend._http("GET", f"/v1/ranked/result?matchId={match_id}&uid={ub.uid}")), 60, "the set settled")
    took = time.monotonic() - left_at
    assert res["status"] == "ABANDONED", res
    # The server's grace for a missing leave is 120 s and its sweep 30 s: settled well before both,
    # so on A's own leave.
    assert took < 25, f"settled after {took:.0f} s: A's leave did not arrive"
    log = re.sub(r"\x1b\[[0-9;]*m", "", backend.log_text("accounts"))
    assert re.search(rf'ranked leave reported.*uid={ua.uid}.*kind="?left"?(\s|$)', log), log[-3000:]
    pub_a = backend._http("GET", f"/user/{ua.uid}?additionalFields=chatMessages,rank")
    assert pub_a["rank"]["ratingUpdateCount"] == 1 and pub_a["rank"]["ratingOrdinal"] < DEFAULT_RATING, pub_a
    pub_b = backend._http("GET", f"/user/{ub.uid}?additionalFields=chatMessages,rank")
    assert (pub_b.get("rank") or {}).get("ratingUpdateCount", 0) == 0, pub_b
    if how == "app":
        assert a.inst.process is None or a.inst.process.poll() is not None
    else:
        a.inst.stop()
    want = "the game is closing" if how == "window" else "Ranked: reports at exit sent"
    text = _log(a)
    assert "Ranked: leaving set" in text and want in text, text[-3000:]


@pytest.mark.slow
def test_ranked_game_1_strikes_after_a_slow_hand_off(backend: OnlineBackend,
                                                      dolphin: Callable[..., DolphinInstance],
                                                      gpu_backend: str) -> None:
    """2026-10-14, under load: game 1 skipped the strikes and played a random stage. The hand-off
    started the session before Ranked began its game setup, and the session set up game 1 in
    between with no stage decider ("game 1 setup: stage 0x2d (random from 9 stages ...)").
    PPR_TEST_HANDOFF_DELAY_MS holds the hand-off 3 s after the session starts, on both machines:
    game 1 must still go to the stage select with the five starters to strike."""
    test = "ranked-slow-hand-off"
    ua, ub = backend.create_user("rsha", "RSHA"), backend.create_user("rshb", "RSHB")
    env = {"PPR_TEST_HANDOFF_DELAY_MS": "3000"}
    a = _boot(dolphin, "rsh-a", backend, ua, gpu_backend, test, "gameplay", env=env)
    b = _boot(dolphin, "rsh-b", backend, ub, gpu_backend, test, "gameplay", env=env)
    _both(a.to_main_menu, b.to_main_menu)
    _both(a.to_online_page, b.to_online_page)
    _both(lambda: a.to_css("ranked"), lambda: b.to_css("ranked"))
    _to_ranked_css(a, b)
    _search_ranked(a)
    _search_ranked(b)
    _peer_shown(a, ub)
    _peer_shown(b, ua)
    players = [a, b]
    _paired(a, b)
    host = a if _ranked(a)["local_port"] == 0 else b
    _both_on(players, SSS, "game 1: both on the stage select")
    for g in players:
        v = _wait_view(g, lambda v: v["type"] == 1 and sorted(v["selectable"]) == STARTERS, "the starters")
        assert v["my_turn"] == (g is host) and v["count"] == 1, v
        g.shot("g1-sss")
        text = _log(g)
        assert "PPR_TEST_HANDOFF_DELAY_MS: 3000 ms" in text, text[-3000:]
        assert not re.search(r"game 1 setup: stage \S+ \(random", text), text[-3000:]


def _fighters(g: Game) -> dict[int, tuple[int, float]]:
    """Each player's (stocks, damage) in g's running 1v1, by in-game port."""
    return {p.port: (p.stocks, round(p.damage, 3)) for p in B.read_players(g.c.read_mem, max_entries=2)
            if p.stocks is not None and p.damage is not None}


def _drive(seats: list[tuple[Game, str]], frames: int, stop: Callable[[], bool] | None = None) -> None:
    """Each game drives its own player (local pad 0) with flows.fight's `mode` for `frames`."""
    def run(g: Game, mode: str) -> None:
        port = _ranked(g)["local_port"]
        F.fight([F.Seat(g.c, port, 0, g.name)], frames, mode=mode, seed=f"dsr-{g.name}", stop=stop)
    th = [threading.Thread(target=run, args=s) for s in seats]
    for t in th:
        t.start()
    for t in th:
        t.join(timeout=600)


@pytest.mark.slow
def test_ranked_desync_restarts_the_game(backend: OnlineBackend, dolphin: Callable[..., DolphinInstance],
                                         gpu_backend: str) -> None:
    """A desync in Ranked plays the game again from the state both machines agreed on (the user's,
    2026-10-13; docs/game-code.md, desync recovery). Game 1 on Smashville with 3 stocks: both
    fighters take damage and B's player loses a stock; then A's P1 damage is changed on A only.
    Both games show DESYNC DETECTED and go back to the CSS, where the characters stay locked and
    each player readies up: A with START ("Waiting on <B>"), B by the 45 s timer running out. The
    same game then starts again, as a fresh one (the countdown, the spawn points), with every
    player's stocks and damage of the last frame whose checksums both compared equal (A's damage
    from before the write) and the time left then. It still counts as the set's game 1: B loses
    its last stocks, and both report game 1 to the server, which counts it.
    Screenshots: run/artifacts/game-bridge/ranked-desync/."""
    test = "ranked-desync"
    ua, ub = backend.create_user("dora", "DORA"), backend.create_user("dsyn", "DSYN")
    a = _boot(dolphin, "rkd-a", backend, ua, gpu_backend, test, "gameplay")
    b = _boot(dolphin, "rkd-b", backend, ub, gpu_backend, test, "gameplay")
    players = [a, b]
    _both(a.to_main_menu, b.to_main_menu)
    _both(a.to_online_page, b.to_online_page)
    _both(lambda: a.to_css("ranked"), lambda: b.to_css("ranked"))
    for g in players:
        B.write_rules(g.c, stocks=3, minutes=4, items_off=True)
        ppom.allow_test_rules(g.c)
    _search_ranked(a)
    _search_ranked(b)
    _peer_shown(a, ub)
    _peer_shown(b, ua)
    _paired(a, b)
    host = a if _ranked(a)["local_port"] == 0 else b
    join = b if host is a else a
    port_a, port_b = _ranked(a)["local_port"], _ranked(b)["local_port"]
    _strike_to(host, join, SMASHVILLE)
    online_set.wait(lambda: all(online_set.gstatus(g.c)["phase"] == "running" for g in players), 240,
                    "game 1 running on both")
    setup1 = B.read_match_setup(a.c.read_mem)
    assert setup1.stage_kind == SMASHVILLE and setup1.time_limit_frames == 4 * 3600, setup1

    # Both fight (damage on both), then B's player walks off the stage until it has lost a stock.
    _drive([(a, "chase"), (b, "chase")], 900)
    stocks_b = _fighters(b)[port_b][0]
    _drive([(a, "idle"), (b, "selfdestruct")], 1200, stop=lambda: _fighters(b)[port_b][0] < stocks_b)
    a.steps("wait 300")   # B's player respawns and lands; nobody moves
    before = [_fighters(g) for g in players]
    print("before the desync", before, flush=True)
    assert before[0] == before[1], before
    assert before[0][port_b][0] == 2 and before[0][port_a][0] == 3, before
    assert any(d > 0 for _, d in before[0].values()), ("no damage taken", before)
    for g in players:
        g.shot("01-before-desync")

    # A's P1 takes 77% on A only (a few times: a rollback may load a state from before a write).
    for _ in range(6):
        a.c.write_mem(_p1_damage_addr(a), struct.pack(">f", 77.0))
        a.steps("wait 5")
    online_set.wait(lambda: all(g.debug_scratch()[10] & 0x100000 for g in players), 30,
                    "DESYNC DETECTED on both")
    a.shot("02-desync-detected")
    _both_on(players, online_set.CSS, "both back on the CSS")
    back = time.monotonic()
    for g in players:
        st = online_set.gstatus(g.c)
        assert st["end_reason"] == "desync" and st["phase"] == "connected", (g.name, st["end_reason"], st["phase"])
        v = _wait_view(g, lambda v: v["type"] == 5, "the ready-up after the desync")
        assert v["may_lock"] and v["text"].startswith("Desync: press START ("), v
        # Not counted: no game was won.
        assert _ranked(g)["wins"] == [0, 0], _ranked(g)
    a.steps("wait 60")
    for g in players:
        g.shot("03-css-ready-up")
    # The characters stay locked: A and B on the token change nothing.
    for g in players:
        g.steps("tap B 8", "wait 20", "tap A 8", "wait 20")
        t = B.read_css_area(g.c.read_mem, 0)
        assert t is None or t.placed, (g.name, t)
    a.steps("tap START 8", "wait 40")
    v = _view(a)
    assert v["type"] == 5 and not v["may_lock"] and v["text"].startswith(f"Waiting on {b.name} ("), v
    a.shot("04-a-ready-waits")
    assert all(online_set.gstatus(g.c)["phase"] == "connected" for g in players)
    # B does nothing: its 45 s run out and its game readies it.
    online_set.wait(lambda: all(online_set.gstatus(g.c)["phase"] == "running" for g in players), 90,
                    "the restarted game running on both")
    waited = time.monotonic() - back
    assert waited > 40, ("the game started before B's time ran out", waited)

    # The same game again from the agreed state, on both.
    rs = [online_set.gstatus(g.c)["restore"] for g in players]
    print("restore", rs, flush=True)
    assert rs[0] == rs[1] and rs[0]["game"] == 2, rs
    for port, (stocks, damage) in before[0].items():
        assert rs[0]["stocks"][port] == stocks and abs(rs[0]["damage"][port] - damage) < 0.01, (port, rs[0], before)
    assert 0 < rs[0]["frames_left"] < 4 * 3600, rs[0]
    setup2 = B.read_match_setup(a.c.read_mem)
    assert setup2 == B.read_match_setup(b.c.read_mem), (setup2, B.read_match_setup(b.c.read_mem))
    assert setup2.stage_kind == SMASHVILLE and setup2.time_limit_frames == rs[0]["frames_left"], setup2
    assert [(p.character, p.color_no) for p in setup2.players[:2]] == \
        [(p.character, p.color_no) for p in setup1.players[:2]], (setup1, setup2)
    a.steps("wait 120")
    for g in players:
        assert g.debug_scratch()[10] & 0xC00000 == 0xC00000, (g.name, hex(g.debug_scratch()[10]))
        got = _fighters(g)
        assert got == before[0], (g.name, "restored", got, before[0])
        g.shot("05-restarted")

    # It is the set's game 1: B loses it, both report game 1, the server counts it.
    rep = online_set.play_game(players, 2, winner=host)
    assert rep["checksums"]["mismatches"] == 0 and rep["end"] == ["game set", "game set"], rep
    for g in players:
        _wait(lambda g=g: sum(_ranked(g)["wins"]) == 1, 30, f"{g.name}: the restarted game counted")
        assert _ranked(g)["wins"][_ranked(host)["local_port"]] == 1, _ranked(g)
    _wait(lambda: backend.log_text("accounts").count(" game reported") >= 2, 30, "both reports")
    reports = [re.sub(r"\x1b\[[0-9;]*m", "", l) for l in backend.log_text("accounts").splitlines()
               if " game reported" in l]
    assert len(reports) == 2 and all(re.search(r"\bgame=1\b", l) for l in reports), reports


def _strike_to(host: Game, join: Game, left: int) -> None:
    """Game 1's strikes (1-2-1) that leave the starter `left`."""
    order = [k for k in STARTERS if k != left]
    _both_on([host, join], SSS, "game 1: both on the stage select")
    _wait_view(host, lambda v: v["type"] == 1 and v["my_turn"], "the host's strike")
    _strike(host, order[0], join)
    _wait_view(join, lambda v: v["my_turn"], "the joiner's turn")
    _strike(join, order[1], host)
    _strike(join, order[2], host)
    _strike(host, order[3], join)


def _rtc(when: str) -> dict[str, dict[str, Any]]:
    """Dolphin.ini for an emulated clock at local time `when` (the Wii's clock is local time)."""
    t = calendar.timegm(datetime.datetime.fromisoformat(when).timetuple())
    return {"Core": {"EnableCustomRTC": True, "CustomRTCValue": f"{t:#x}"}}


def _log(g: Game) -> str:
    return (g.inst.user_dir / "Logs" / "dolphin.log").read_text(errors="replace")


def _to_ranked_css(*players: Game) -> None:
    for g in players:
        B.write_rules(g.c, stocks=1, minutes=2, items_off=True)
        ppom.allow_test_rules(g.c)


def _paired(*players: Game) -> None:
    for g in players:
        _wait(lambda g=g: g.c.mm_status()["handoff"] == "started", 30, "the hand-off")


# The two players' clocks on 2026-10-12 (Pacific and Eastern time). Smashville's variant comes
# from the clock: it was 03 on the host's machine and 06 on the joiner's.
HOST_CLOCK, JOIN_CLOCK = "2026-10-11T22:05:00", "2026-10-12T01:05:00"
SMASHVILLE = 0x21


@pytest.mark.slow
def test_ranked_after_a_set_cut_short(backend: OnlineBackend, dolphin: Callable[..., DolphinInstance],
                                      gpu_backend: str) -> None:
    """2026-10-12 (GALA vs POCH). The host's last set had ended in its first game (the opponent's
    connection dropped), and the host kept that session's match init block, tagged match 0. In the
    next set the joiner reached match 0's start first and took that stale block as this match's.
    It was for another stage, so the joiner kept its own clock-derived stage variant, the setups
    differed at the barrier and the session gave up.

    Here set 1 ends in game 1 when the joiner's window closes. The same host then meets the same
    player again, from a new game whose clock is 3 hours later. The host reaches the match start
    1.5 s late (PPR_GPRB_TEST_HOST_START_DELAY_MS). Game 1 of set 2, on Smashville, must take the
    host's fresh init block and run with equal setups. Screenshots:
    run/artifacts/game-bridge/ranked-stale-init/."""
    test = "ranked-stale-init"
    env = {"PPR_GPRB_TEST_HOST_START_DELAY_MS": "1500"}   # only the host waits
    ua, ub = backend.create_user("stina", "STIN"), backend.create_user("stig", "STIG")
    a = _boot(dolphin, "rks-a", backend, ua, gpu_backend, test, "gameplay", dolphin_ini=_rtc(HOST_CLOCK), env=env)
    b = _boot(dolphin, "rks-b", backend, ub, gpu_backend, test, "gameplay", dolphin_ini=_rtc(HOST_CLOCK), env=env)
    _both(a.to_main_menu, b.to_main_menu)
    _both(a.to_online_page, b.to_online_page)
    _both(lambda: a.to_css("ranked"), lambda: b.to_css("ranked"))
    _to_ranked_css(a, b)
    _search_ranked(a)
    _search_ranked(b)
    _paired(a, b)
    host = a if _ranked(a)["local_port"] == 0 else b
    join = b if host is a else a
    host_user, join_user = (ua, ub) if host is a else (ub, ua)

    # Set 1, game 1 on Battlefield; the joiner's window closes in the middle of it.
    _strike_to(host, join, 0x01)
    online_set.wait(lambda: all(online_set.gstatus(g.c)["phase"] == "running" for g in (host, join)), 240,
                    "set 1 game 1 running on both")
    assert online_set.gstatus(host.c)["match_index"] == 0
    host.steps("wait 120")
    join.c.call("emu_stop")
    _wait(lambda: online_set.gstatus(host.c)["disconnected"], 30, "the host sees the drop")
    _wait(lambda: online_set.scene(host.c) == online_set.CSS, 60, "the host back on the CSS")
    _wait(lambda: host.c.mm_status()["state"] == "idle", 30, "the host idle")
    assert _ranked(host)["peer_gone"], _ranked(host)
    join.inst.stop()

    # Set 2: the same player again, from a new game whose clock is 3 hours later.
    j2 = _boot(dolphin, "rks-j2", backend, join_user, gpu_backend, test, "gameplay",
               dolphin_ini=_rtc(JOIN_CLOCK), env=env)
    j2.to_main_menu()
    j2.to_online_page()
    j2.to_css("ranked")
    _to_ranked_css(host, j2)
    _search_ranked(host)
    _search_ranked(j2)
    _peer_shown(host, join_user)
    _peer_shown(j2, host_user)
    _paired(host, j2)
    assert _ranked(host)["local_port"] == 0, "mm's decider changed between the two sets"
    _strike_to(host, j2, SMASHVILLE)
    rep = online_set.play_game([host, j2], 1)
    assert rep["stage"] == SMASHVILLE, rep
    assert rep["checksums"]["mismatches"] == 0 and all(e == "game set" for e in rep["end"]), rep
    log = _log(j2)
    assert "differs beyond the stage variant" not in log, "the joiner was handed another match's init block"
    # The clocks differ, so the joiner's own variant differed and it took the host's.
    assert "joiner took the host's match init block" in log
    assert "PPR_GPRB_TEST_HOST_START_DELAY_MS" in _log(host)


@pytest.mark.slow
@pytest.mark.parametrize("giver", [1, 2], ids=["host-gives-up", "joiner-gives-up"])
def test_ranked_failed_start_is_a_disconnect(backend: OnlineBackend, dolphin: Callable[..., DolphinInstance],
                                             gpu_backend: str, giver: int) -> None:
    """A session can give up on the way into a match when a barrier's check fails. Here
    PPR_GPRB_TEST_GIVE_UP=<port> forces it; on 2026-10-12 the setups differed. Both machines must
    then end the match as a disconnect: DISCONNECTED in the HUD, back on the CSS, idle. Both report
    the opponent gone, so the set is TERMINATED and nobody is rated.

    Before, the session that gave up did not tell its game. That game played the match on alone and
    unsynced (on the joiner the local pad drove the host's fighter), and its CSS waited for a game 2
    that never came, until the player backed out and lost the set as its leaver. The other player's
    game reacts to the leave (as a peer that left, or, if it waits at the barrier, by giving up too:
    "the host left before the match started"). Run with each player giving up: the one that gives
    up is the new path. Screenshots: run/artifacts/game-bridge/ranked-failed-start-<port>/."""
    test = f"ranked-failed-start-{giver}"
    env = {"PPR_GPRB_TEST_GIVE_UP": str(giver)}   # that port's barrier gives up
    ua = backend.create_user(f"frida{giver}", "FRI" + "AB"[giver - 1])
    ub = backend.create_user(f"folke{giver}", "FOL" + "AB"[giver - 1])
    a = _boot(dolphin, "rkf-a", backend, ua, gpu_backend, test, "gameplay", env=env)
    b = _boot(dolphin, "rkf-b", backend, ub, gpu_backend, test, "gameplay", env=env)
    _both(a.to_main_menu, b.to_main_menu)
    _both(a.to_online_page, b.to_online_page)
    _both(lambda: a.to_css("ranked"), lambda: b.to_css("ranked"))
    _to_ranked_css(a, b)
    _search_ranked(a)
    _search_ranked(b)
    players = [a, b]
    _paired(a, b)
    match_id = a.c.mm_status()["match"]["match_id"]
    host = a if _ranked(a)["local_port"] == 0 else b
    join = b if host is a else a
    quitter, other = (host, join) if giver == 1 else (join, host)
    _strike_to(host, join, 0x01)

    # Both games are told. GameBridge cleans the session up right after, so it is read when it
    # says so.
    seen = {}
    for g in players:
        seen[g.inst.name] = _wait(lambda g=g: (lambda st: st if st["disconnected"] else None)(
            online_set.gstatus(g.c)), 120, f"{g.inst.name}: told of the drop", interval=0.02)
    st = seen[quitter.inst.name]
    assert st["phase"] == "error" and st["error"] == "PPR_GPRB_TEST_GIVE_UP" and st["frames"] == 0, st
    st = seen[other.inst.name]
    assert st["frames"] == 0 and (
        (st["phase"] == "ended" and st["error"] == "peer gave up: PPR_GPRB_TEST_GIVE_UP") or
        (st["phase"] == "error" and st["error"] == "the host left before the match started")), st
    for g in players:
        _wait(lambda g=g: online_set.local(g.c)["hud_disconnected"] == 1, 10, f"{g.inst.name}: DISCONNECTED",
              interval=0.05)
        g.shot("01-disconnected")
    for g in players:
        _wait(lambda g=g: online_set.scene(g.c) == online_set.CSS, 60, f"{g.inst.name}: back on the CSS")
        _wait(lambda g=g: g.c.mm_status()["state"] == "idle", 30, f"{g.inst.name}: idle")
        g.steps("wait 60")
        g.shot("02-back-on-css")
        r = _ranked(g)
        assert r["peer_gone"] and not r["left_reported"], r
        log = _log(g)
        assert "GameBridge: the opponent disconnected" in log and "session running" not in log

    res = _wait(lambda: (lambda r: r if r["status"] != "ASSIGNED" else None)(
        backend._http("GET", f"/v1/ranked/result?matchId={match_id}&uid={ua.uid}")), 60, "the set settled")
    assert res["status"] == "TERMINATED" and res["rating"] is None, res
    for u in (ua, ub):
        pub = backend._http("GET", f"/user/{u.uid}?additionalFields=chatMessages,rank")
        assert pub["rank"]["ratingUpdateCount"] == 0, pub


G_MTRAND_SEED = 0x805A00BC   # g_mtRand's seed: randi, the match's RNG (GameplayRollback.h)
CSS_RANDOM = 0x29


@pytest.mark.slow
def test_ranked_random_draws_every_game(backend: OnlineBackend, dolphin: Callable[..., DolphinInstance],
                                        gpu_backend: str) -> None:
    """Both players on Random for a whole set (the user's, 2026-10-12: a player on Random was
    reported to be the same character in all three games of a Ranked set). Every lock-in is a
    real character drawn for that game and played, and the coin stays on Random between games.
    The draw is the game's randi (g_mtRand, the match's RNG, which the session seeds for every
    match); its state at each character step is logged for both machines. Pass: neither player's
    lock-ins are all one character (1 in 1,764 by chance), and the two players did not draw the
    same character in every game (1 in 74,088). Screenshots: run/artifacts/game-bridge/ranked-random/."""
    test = "ranked-random"
    ua, ub = backend.create_user("rory", "RORY"), backend.create_user("rita", "RITA")
    a = _boot(dolphin, "rr-a", backend, ua, gpu_backend, test, "gameplay")
    b = _boot(dolphin, "rr-b", backend, ub, gpu_backend, test, "gameplay")
    players = [a, b]
    _both(a.to_main_menu, b.to_main_menu)
    _both(a.to_online_page, b.to_online_page)
    _both(lambda: a.to_css("ranked"), lambda: b.to_css("ranked"))
    for g in players:
        B.css_pick_character(g.c, 0, CSS_RANDOM)
        g.steps("wait 30")
        assert online_set.css_panel(g.c)["char"] == CSS_RANDOM
        B.write_rules(g.c, stocks=1, minutes=2, items_off=True)
        ppom.allow_test_rules(g.c)
        g.shot("01-css-random")
    _search_ranked(a)
    _search_ranked(b)
    _peer_shown(a, ub)
    _peer_shown(b, ua)
    _paired(a, b)
    host = a if _ranked(a)["local_port"] == 0 else b
    join = b if host is a else a

    drawn: dict[str, list[int]] = {g.inst.name: [] for g in players}

    def note_setup(game: int, rep: dict[str, Any]) -> None:
        for g in players:
            lock = online_set.local(g.c)["lock"]
            assert lock["game"] == game and lock["css"] != CSS_RANDOM, (g.inst.name, lock)
            # The match played the character drawn for it.
            assert f"port={_ranked(g)['local_port']}, character={lock['char_kind']}," in rep["setup"], (
                g.inst.name, lock, rep["setup"])
            drawn[g.inst.name].append(lock["char_kind"])
        print(f"game {game}: drawn {drawn}", flush=True)

    _strike_to(host, join, 0x01)
    winners = {1: b, 2: a, 3: b}   # 1-1, so game 3 decides
    note_setup(1, online_set.play_game(players, 1, winner=winners[1]))
    game = 1
    while True:
        _wait(lambda: all(sum(_ranked(g)["wins"]) >= game for g in players), 30, f"game {game} counted")
        if any(_ranked(g)["set_over"] for g in players):
            break
        game += 1
        winner_port = _ranked(a)["setup"]["last_winner"]
        winner = a if _ranked(a)["local_port"] == winner_port else b
        loser = b if winner is a else a
        _both_on(players, SSS, f"game {game}: both on the stage select")
        v = _wait_view(winner, lambda v: v["game"] == game and v["type"] == 1 and v["my_turn"], "the ban")
        bans, pick = v["selectable"][:2], v["selectable"][2]
        _strike(winner, bans[0], loser)
        _wait_view(winner, lambda v: v["type"] == 1 and v["my_turn"] and v["count"] == 1, "the second ban")
        _strike(winner, bans[1], loser)
        _wait_view(loser, lambda v: v["type"] == 2 and v["my_turn"], "the pick")
        B.sss_pick_stage(loser.c, pick, 0)
        _both_on(players, online_set.CSS, f"game {game}: both back on the CSS")
        _wait_view(loser, lambda v: v["type"] == 3, "the character step")
        for g in players:
            g.steps("wait 30")
            assert online_set.css_panel(g.c)["char"] == CSS_RANDOM, (g.inst.name, "Random kept")
        print(f"game {game}: g_mtRand at the character step "
              f"{ {g.inst.name: hex(g.c.read_u32(G_MTRAND_SEED)) for g in players} }", flush=True)
        winner.shot(f"g{game}-css-random")
        winner.steps("tap START 8", "wait 30")
        _wait_view(loser, lambda v: v["may_lock"], "the loser's turn")
        loser.steps("tap START 8", "wait 30")
        note_setup(game, online_set.play_game(players, game, winner=winners[game]))

    assert game == 3, drawn
    for name, chars in drawn.items():
        assert len(set(chars)) > 1, (name, "the same character every game", drawn)
    ca, cb = drawn[a.inst.name], drawn[b.inst.name]
    assert any(x != y for x, y in zip(ca, cb)), ("both drew the same character every game", drawn)
