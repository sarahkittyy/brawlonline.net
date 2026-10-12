"""A room's stages on the loser's stage select (docs/design/room-rules.md §2, the stage switch).

A published ruleset without a set, its stages Battlefield, Final Destination and Yoshi's Story:
- loser picks from the list: SESSION has the room's stages, game 1 is drawn from them, and in game 2
  the loser's stage select greys every other stage (P+'s strike art), refuses one, and plays a
  pick from the list on its normal layout (L held: no alternate stage);
- loser picks any stage: nothing is struck, the pick keeps its alternate layout, and P+'s random
  (START) draws from the room's stages.

Screenshots: run/artifacts/game-code/rooms/stage-list/<test>/<player>/.
"""
from __future__ import annotations

from typing import Callable

import pytest

from ppharness import brawl as B
from ppharness.backend import OnlineBackend
from ppharness.instance import DolphinInstance

from test_online import _wait
from test_online_game import Game, _boot, backend  # noqa: F401  (the backend fixture)
from test_room_rules import session_rules
from test_rooms_game import (ART, CSS, _all, launcher_answer, lock_of, pick, room_view, to_join_room,
                             type_room_code, until_view, write_create)
import online_set  # noqa: E402
import ppom  # noqa: E402

pytestmark = [pytest.mark.dolphin, pytest.mark.server, pytest.mark.gpu]

LIST_ART = ART / "stage-list"
SSS = "scSelStage"
BF, FD, FOD, YS = 0x01, 0x02, 0x1F, 0x2A   # Yoshi's Story: page 1, off in P+ default
STAGES = {"starters": [BF, FD], "counterpicks": [YS]}
CHOICE_LOSER_PICKS, CHOICE_LOSER_PICKS_LIST = 0, 3


def ruleset(choice: str) -> dict:
    return {"format": 1, "name": f"Stage List {choice[-4:]}", "rules": {"stocks": 1, "timeMinutes": 2},
            "bestOf": 0, "stageChoice": choice, "stages": STAGES}


def session(g: Game) -> dict:
    return online_set.session(g.c)


def room_with(backend: OnlineBackend, dolphin: Callable[..., DolphinInstance], gpu_backend: str, test: str,
              choice: str) -> tuple[Game, Game]:
    """A creates a room with the published ruleset from the launcher, B joins by its code."""
    ua, ub = backend.create_user(f"{test[:4]}ah", "SLAH"), backend.create_user(f"{test[:4]}bj", "SLBJ")
    a = _boot(dolphin, "list-a", backend, ua, gpu_backend, test, "gameplay", artifacts=LIST_ART)
    b = _boot(dolphin, "list-b", backend, ub, gpu_backend, test, "gameplay", artifacts=LIST_ART)
    _all(a.to_main_menu, b.to_main_menu)
    _all(a.to_online_page, b.to_online_page)
    code = backend._http("POST", "/v1/rulesets", {"ruleset": ruleset(choice)}, token=ua.session_token)["code"]
    rid = write_create(a, ruleset=code)
    assert launcher_answer(a, rid)["result"] == "accepted"
    _wait(lambda: a.scene() == CSS, 60, "A on the room's CSS")
    room = until_view(a, lambda v: v["flags"] & ppom.RF_IN, "A in its room", timeout=60)["code"]
    to_join_room(b)
    pick(a)
    pick(b, right=2)
    type_room_code(b, room)
    until_view(b, lambda v: v["flags"] & ppom.RF_IN, "B in the room")
    for g in (a, b):
        _wait(lambda g=g: (session_rules(g) or {}).get("stocks") == 1, 30, f"{g.name}: the ruleset's rules")
    return a, b


def to_losers_stage_select(a: Game, b: Game, game: int) -> Game:
    """Game `game` (2+): the winner locks in, the loser's START opens the stage select."""
    v = room_view(a)
    pickers = [i for i, s in enumerate(v["slots"]) if s["picks_stage"]]
    assert len(pickers) == 1, v
    loser = a if pickers[0] == 0 else b
    winner = b if loser is a else a
    winner.steps("tap START 8", "wait 30")
    _wait(lambda: lock_of(winner)["ready"] and lock_of(winner)["game"] == game, 30, "the winner locked in")
    loser.steps("tap START 8", "wait 30")
    _wait(lambda: loser.scene() == SSS, 60, "the loser on the stage select")
    loser.steps("wait 40")
    return loser


def play_first_game(a: Game, b: Game) -> int:
    game = room_view(a)["game"]
    a.steps("tap START 8", "wait 40")
    until_view(b, lambda v: v["slots"][0]["bits"] & ppom.SLOT_READY, "B sees A ready")
    b.steps("tap START 8", "wait 10")
    rep = online_set.play_game([a, b], game)
    assert rep["checksums"]["mismatches"] == 0, rep
    assert rep["stage"] in (BF, FD, YS), ("game 1 not drawn from the room's stages", rep)
    _wait(lambda: a.scene() == CSS and b.scene() == CSS, 180, "both back on the room's CSS")
    for g in (a, b):
        until_view(g, lambda v: v["flags"] & ppom.RF_IN and v["status"] == 0 and v["game"] == game + 1,
                   "the room waiting for its next game", timeout=60)
    return game + 1


@pytest.mark.slow
def test_room_loser_picks_from_the_list(backend: OnlineBackend, dolphin: Callable[..., DolphinInstance],
                                        gpu_backend: str) -> None:
    a, b = room_with(backend, dolphin, gpu_backend, "list", "loser-picks-list")
    for g in (a, b):
        se = session(g)
        assert se["play"]["stage_choice"] == CHOICE_LOSER_PICKS_LIST, se["play"]
        assert se["stages"] == STAGES, se["stages"]
    a.shot("01-room")
    game = play_first_game(a, b)

    loser = to_losers_stage_select(a, b, game)
    loser.shot("02-stage-select")
    # Every stage of page 1 but the room's three is struck (P+'s strike art, P+'s striking on);
    # pages 2-3 all of it. Page 1's positions: Yoshi's Story 0, Fountain of Dreams 9, Final
    # Destination 10, Battlefield 17 (src/common/stages.ts).
    struck = loser.c.read_mem(online_set.STRIKE_TABLE, 30)
    page1 = int.from_bytes(struck[2:6], "big")
    assert [page1 >> pos & 1 for pos in (0, 9, 10, 17)] == [0, 1, 0, 0], f"page 1 struck {page1:#x}"
    assert struck[6:18] == b"\xff" * 12, f"pages 2-3 not struck: {struck.hex()}"
    # A struck stage cannot even be highlighted (P+ drops the selection on it): the pick fails.
    with pytest.raises(B.RecipeError):
        B.sss_pick_stage(loser.c, FOD, 0, timeout_frames=400)
    assert loser.scene() == SSS, ("an off-list stage was taken", loser.scene())
    loser.shot("03-refused")
    # Yoshi's Story with L held: played, on its normal layout.
    B.sss_pick_stage(loser.c, YS, 0, held=["L"])
    _wait(lambda: online_set.gstatus(loser.c)["phase"] in ("running", "error", "ended"), 90,
          "game 2 to start after the pick")
    rep = online_set.play_game([a, b], game)
    assert rep["checksums"]["mismatches"] == 0, rep
    assert rep["stage"] == YS, ("the loser's pick was not played", rep)
    assert rep["asl"] == [0, 0] and rep["asl_loaded"] == [0, 0], ("an alternate layout from the list", rep)


@pytest.mark.slow
def test_room_loser_picks_any_stage(backend: OnlineBackend, dolphin: Callable[..., DolphinInstance],
                                    gpu_backend: str) -> None:
    a, b = room_with(backend, dolphin, gpu_backend, "anys", "loser-picks")
    for g in (a, b):
        se = session(g)
        assert se["play"]["stage_choice"] == CHOICE_LOSER_PICKS, se["play"]
        assert se["stages"] == STAGES, se["stages"]
    game = play_first_game(a, b)

    # Game 2: nothing struck; a stage off the list keeps its alternate layout (L).
    loser = to_losers_stage_select(a, b, game)
    assert loser.c.read_mem(online_set.STRIKE_TABLE, 30) == bytes(30), "stages struck with any stage"
    loser.shot("02-stage-select")
    B.sss_pick_stage(loser.c, FOD, 0, held=["L"])
    _wait(lambda: online_set.gstatus(loser.c)["phase"] in ("running", "error", "ended"), 90,
          "game 2 to start after the pick")
    rep = online_set.play_game([a, b], game)
    assert rep["checksums"]["mismatches"] == 0, rep
    assert rep["stage"] == FOD and rep["asl"] == [0x40, 0x40], ("the pick or its layout was not played", rep)
    _wait(lambda: a.scene() == CSS and b.scene() == CSS, 180, "both back on the room's CSS after game 2")
    for g in (a, b):
        until_view(g, lambda v: v["flags"] & ppom.RF_IN and v["status"] == 0 and v["game"] == game + 1,
                   "the room waiting for its third game", timeout=60)

    # Game 3: P+'s random (START) on the loser's stage select draws from the room's stages.
    loser = to_losers_stage_select(a, b, game + 1)
    loser.steps("tap START 8")
    _wait(lambda: online_set.gstatus(loser.c)["phase"] in ("running", "error", "ended"), 90,
          "game 3 to start after the random pick")
    rep = online_set.play_game([a, b], game + 1)
    assert rep["checksums"]["mismatches"] == 0, rep
    assert rep["stage"] in (BF, FD, YS), ("P+'s random left the room's stages", rep)
