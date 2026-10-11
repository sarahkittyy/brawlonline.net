"""A room's set end to end (docs/design/room-rules.md §5): a published Bo3 ruleset, the room
created with it from the launcher's ruleset box, game 1's strikes on P+'s stage select, the winner's bans and
the loser's pick for the games after, the winner's character first, the server keeping the score
from both players' room-result reports, and the session ending with the set.

Screenshots: run/artifacts/game-code/rooms/sets/<test>/<player>/.
"""
from __future__ import annotations

from typing import Any, Callable

import pytest

from ppharness import brawl as B
from ppharness.backend import OnlineBackend
from ppharness.instance import DolphinInstance

from test_online import _wait
from test_online_game import Game, _boot, backend  # noqa: F401  (the backend fixture)
from test_room_rules import session_rules
from test_rooms_game import (ART, CSS, _all, launcher_answer, pick, room_view, to_join_room, type_room_code,
                             until_view, write_create)
import online_set  # noqa: E402
import ppom  # noqa: E402

pytestmark = [pytest.mark.dolphin, pytest.mark.server, pytest.mark.gpu]

SETS_ART = ART / "sets"
SSS = "scSelStage"
STARTERS = [0x01, 0x02, 0x21, 0x2D, 0x2E]   # the default ruleset's (Ranked's)

BO3 = {"format": 1, "name": "Room Set Test", "rules": {"stocks": 1, "timeMinutes": 2},
       "players": "1v1", "bestOf": 3, "stageChoice": "loser-picks", "strikes": [1, 2, 1], "bans": 2,
       "dsr": True}


def setup_view(g: Game) -> dict[str, Any]:
    return g.c.online_status()["ranked"]["setup"]["view"]


def rooms(g: Game) -> dict[str, Any]:
    return g.c.call("rooms_status")


def until_setup(g: Game, pred: Callable[[dict[str, Any]], bool], what: str, timeout: float = 60) -> dict[str, Any]:
    return _wait(lambda: (lambda v: v if pred(v) else None)(setup_view(g)), timeout, f"{g.name}: {what}")


def strike(g: Game, kind: int) -> None:
    B.sss_pick_stage(g.c, kind, 0, button="X")


def play_steps(players: list[Game], game: int) -> None:
    """Whoever's turn it is strikes or bans the first selectable stage, or picks one, until the
    stage is decided (both machines see every step)."""
    for _ in range(12):
        views = {g.name: setup_view(g) for g in players}
        mover = next((g for g in players if views[g.name]["active"] and views[g.name]["my_turn"] and
                      views[g.name]["type"] in (1, 2)), None)
        if mover is None:
            if all(not v["active"] or v["type"] >= 3 for v in views.values()):
                return
            players[0].steps("wait 20")
            continue
        v = views[mover.name]
        assert v["game"] >= 1 and v["selectable"], v
        kind = v["selectable"][0]
        if v["type"] == 2:
            B.sss_pick_stage(mover.c, kind, 0)
        else:
            strike(mover, kind)
        mover.steps("wait 30")
    raise AssertionError(f"game {game}: the steps did not end: {[setup_view(g) for g in players]}")


@pytest.mark.slow
def test_room_bo3_set(backend: OnlineBackend, dolphin: Callable[..., DolphinInstance],
                      gpu_backend: str) -> None:
    test = "bo3"
    ua, ub = backend.create_user("setah", "STAH"), backend.create_user("setbj", "STBJ")
    a = _boot(dolphin, "set-a", backend, ua, gpu_backend, test, "gameplay", artifacts=SETS_ART)
    b = _boot(dolphin, "set-b", backend, ub, gpu_backend, test, "gameplay", artifacts=SETS_ART)
    _all(a.to_main_menu, b.to_main_menu)
    _all(a.to_online_page, b.to_online_page)
    # The host publishes the Bo3 and creates a room with it from the launcher (its ruleset box).
    view = backend._http("POST", "/v1/rulesets", {"ruleset": BO3}, token=ua.session_token)
    rs_code = view["code"]
    rid = write_create(a, ruleset=rs_code)
    assert launcher_answer(a, rid)["result"] == "accepted"
    _wait(lambda: a.scene() == CSS, 60, "A on the room's CSS")
    v = until_view(a, lambda v: v["flags"] & ppom.RF_IN, "A in its room", timeout=60)
    code = v["code"]
    to_join_room(b)
    pick(a)
    pick(b, right=2)
    type_room_code(b, code)
    until_view(b, lambda v: v["flags"] & ppom.RF_IN, "B in the room")
    for g in (a, b):
        _wait(lambda g=g: rooms(g).get("ruleset", {}).get("code") == rs_code, 30, f"{g.name}: the room's ruleset")
        _wait(lambda g=g: (session_rules(g) or {}).get("stocks") == 1, 20, f"{g.name}: the ruleset's rules")
    assert room_view(a)["code"] == code
    a.shot("01-ruleset")

    # Ready: the set starts; game 1 is a blind pick (the other's character stays hidden).
    a.steps("tap START 8", "wait 40")
    _wait(lambda: ppom.read_room(b.c, ppom.find_block(b.c))["slots"][0]["bits"] & ppom.SLOT_READY, 20, "B sees A ready")
    assert ppom.read_room(b.c, ppom.find_block(b.c))["slots"][0]["char"] == 0xFF, "game 1 is not a blind pick"
    b.shot("02-blind")
    b.steps("tap START 8", "wait 10")
    players = [a, b]
    online_set.wait(lambda: all(online_set.scene(g.c) == SSS for g in players), 120, "game 1: both on the stage select")
    for g in players:
        v = until_setup(g, lambda v: v["active"] and v["type"] == 1 and sorted(v["selectable"]) == STARTERS, "the starters")
    a.shot("03-g1-strikes")
    play_steps(players, 1)
    games = [online_set.play_game(players, 1)]

    game = 1
    while True:
        set_ = _wait(lambda: (lambda st: st if st and sum(st["score"]) >= len(games) else None)(rooms(a).get("set")),
                     30, f"game {game} counted by the server")
        if set_["finished"]:
            break
        game += 1
        assert game <= 3, set_
        online_set.wait(lambda: all(online_set.scene(g.c) == SSS for g in players), 120,
                        f"game {game}: both on the stage select")
        v = until_setup(a, lambda v: v["game"] == game and v["type"] in (1, 2), "the bans")
        a.shot(f"04-g{game}-bans")
        play_steps(players, game)
        online_set.wait(lambda: all(online_set.scene(g.c) == CSS for g in players), 120,
                        f"game {game}: both back on the CSS")
        # Characters: the winner first, then the loser.
        for _ in range(2):
            mover = _wait(lambda: next((g for g in players if setup_view(g)["type"] == 3 and
                                        setup_view(g)["my_turn"]), None), 60, "a character turn")
            mover.steps("tap START 8", "wait 30")
            if all(not setup_view(g)["active"] or setup_view(g)["type"] != 3 for g in players):
                break
        games.append(online_set.play_game(players, game))

    for rep in games:
        assert rep["checksums"]["mismatches"] == 0, rep
    # The set is over: the session ends with it and both are back in the room, its score kept.
    _wait(lambda: not rooms(a)["room_game"]["active"] and not rooms(b)["room_game"]["active"], 120,
          "the set's session over")
    for g in (a, b):
        st = rooms(g)["set"]
        assert st["finished"] and max(st["score"]) == 2 and sum(st["score"]) == len(games), st
        g.steps("wait 60")
        g.shot("05-set-over")
    _all(lambda: a.steps("wait 1"), lambda: b.steps("wait 1"))
