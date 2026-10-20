"""A room's stages on the loser's stage select (docs/design/room-rules.md §2, the stage switch).

A published ruleset without a set, its stages Battlefield, Final Destination and Yoshi's Story:
- loser picks from the list: SESSION has the room's stages, game 1 is drawn from them, and in game 2
  the loser's stage select greys every other stage (P+'s strike art), refuses one, and plays a
  pick from the list on its normal layout (L held: no alternate stage);
- loser picks any stage: nothing is struck, the pick is played on its normal layout (L held:
  alternate stages are off unless the ruleset turns them on), and P+'s random (START) draws from
  the room's stages;
- a ruleset with alternate stages on: the loser's pick keeps the layout L selects, from any stage
  or from the list;
- a game on P+'s Training Room (the waiting room's stage) is an online match like any other.

Screenshots: run/artifacts/game-code/rooms/stage-list/<test>/<player>/.
"""
from __future__ import annotations

import struct
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
TRAINING_ROOM = 0x37                        # also the waiting room's stage (training_room.cpp)
STAGES = {"starters": [BF, FD], "counterpicks": [YS]}
CHOICE_LOSER_PICKS, CHOICE_LOSER_PICKS_LIST = 0, 3


def ruleset(choice: str, alternates: bool = False) -> dict:
    r = {"format": 1, "name": f"Stage List {choice[-4:]}{' Alts' if alternates else ''}",
         "rules": {"stocks": 1, "timeMinutes": 2}, "bestOf": 0, "stageChoice": choice, "stages": STAGES}
    if alternates:
        r["alternateStages"] = True
    return r


def session(g: Game) -> dict:
    return online_set.session(g.c)


def room_with(backend: OnlineBackend, dolphin: Callable[..., DolphinInstance], gpu_backend: str, test: str,
              choice: str, alternates: bool = False) -> tuple[Game, Game]:
    """A creates a room with the published ruleset from the launcher, B joins by its code."""
    ua, ub = backend.create_user(f"{test[:4]}ah", "SLAH"), backend.create_user(f"{test[:4]}bj", "SLBJ")
    a = _boot(dolphin, "list-a", backend, ua, gpu_backend, test, "gameplay", artifacts=LIST_ART)
    b = _boot(dolphin, "list-b", backend, ub, gpu_backend, test, "gameplay", artifacts=LIST_ART)
    _all(a.to_main_menu, b.to_main_menu)
    _all(a.to_online_page, b.to_online_page)
    code = backend._http("POST", "/v1/rulesets", {"ruleset": ruleset(choice, alternates)},
                         token=ua.session_token)["code"]
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

    # Game 2: nothing struck; a stage off the list, L held: its normal layout (alternate stages off).
    loser = to_losers_stage_select(a, b, game)
    assert loser.c.read_mem(online_set.STRIKE_TABLE, 30) == bytes(30), "stages struck with any stage"
    loser.shot("02-stage-select")
    B.sss_pick_stage(loser.c, FOD, 0, held=["L"])
    _wait(lambda: online_set.gstatus(loser.c)["phase"] in ("running", "error", "ended"), 90,
          "game 2 to start after the pick")
    rep = online_set.play_game([a, b], game)
    assert rep["checksums"]["mismatches"] == 0, rep
    assert rep["stage"] == FOD, ("the pick was not played", rep)
    assert rep["asl"] == [0, 0] and rep["asl_loaded"] == [0, 0], ("an alternate layout without alternates", rep)
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


@pytest.mark.slow
def test_room_alternate_stages_on(backend: OnlineBackend, dolphin: Callable[..., DolphinInstance],
                                  gpu_backend: str) -> None:
    """A ruleset with alternate stages on (the user's, 2026-10-19: off by default): the loser's pick
    with L held plays the L layout on both machines."""
    a, b = room_with(backend, dolphin, gpu_backend, "alts", "loser-picks", alternates=True)
    game = play_first_game(a, b)
    loser = to_losers_stage_select(a, b, game)
    B.sss_pick_stage(loser.c, FOD, 0, held=["L"])
    _wait(lambda: online_set.gstatus(loser.c)["phase"] in ("running", "error", "ended"), 90,
          "game 2 to start after the pick")
    rep = online_set.play_game([a, b], game)
    assert rep["checksums"]["mismatches"] == 0, rep
    assert rep["stage"] == FOD and rep["asl"] == [0x40, 0x40], ("the pick or its layout was not played", rep)
    assert rep["asl_loaded"][0] == rep["asl_loaded"][1] == 0x40, ("P+ did not load the L layout", rep)


@pytest.mark.slow
def test_room_game_on_the_training_room(backend: OnlineBackend, dolphin: Callable[..., DolphinInstance],
                                        gpu_backend: str) -> None:
    """Bug #35 (2026-10-16): the loser picks P+'s Training Room, the stage kind (0x37) of the
    waiting room Unranked and Ranked search in. Dolphin took every scMelee on it for the waiting
    room, so no session ran in that game: each machine played on alone (the others stood still,
    a pause was not seen). It is a match like any other: the session runs on both and the game is
    played to its end with the same checksums."""
    a, b = room_with(backend, dolphin, gpu_backend, "trng", "loser-picks")
    game = play_first_game(a, b)
    loser = to_losers_stage_select(a, b, game)
    # On P+'s page 2: A on the page button below the icons turns it (sss_pick_stage stays on its page).
    page = B.stage_positions(TRAINING_ROOM, loser.c.read_mem)[0][0]
    B.sss_hover_item(loser.c, B.SSS_ITEM_PAGE)
    for _ in range(4):
        if B.read_sss(loser.c.read_mem).page == page:
            break
        loser.steps("tap A 6", "wait 30")
    assert B.read_sss(loser.c.read_mem).page == page, "the stage select did not turn to the Training Room's page"
    B.sss_pick_stage(loser.c, TRAINING_ROOM, 0)
    _wait(lambda: online_set.gstatus(loser.c)["phase"] in ("running", "error", "ended"), 90,
          "game 2 to start after the pick")
    rep = online_set.play_game([a, b], game)
    assert rep["stage"] == TRAINING_ROOM, ("the Training Room was not played", rep)
    assert rep["checksums"]["compared"] > 0 and rep["checksums"]["mismatches"] == 0, rep
    _wait(lambda: a.scene() == CSS and b.scene() == CSS, 180, "both back on the room's CSS after game 2")


@pytest.mark.slow
def test_host_edits_the_stages_in_game(backend: OnlineBackend, dolphin: Callable[..., DolphinInstance],
                                       gpu_backend: str) -> None:
    """The host's STAGE SWITCH (stage_switch.cpp) with the real Dolphin and mm: Yoshi's Story becomes
    a counterpick and the loser picks from the list; when the menu closes the room is Custom, both
    machines' SESSION has the new stages and choice, and the other player's ready is taken back."""
    from test_room_rules import TASK_RULES, close_menu, css_task, open_rules, two_in_a_room
    a, b, _code = two_in_a_room(backend, dolphin, gpu_backend, "switch-edits")
    b.steps("tap START 8", "wait 40")
    until_view(a, lambda v: v["slots"][1]["bits"] & ppom.SLOT_READY, "A sees B ready")

    def cursor() -> tuple[int, int]:
        page = a.c.read_u32(a.c.read_u32(css_task(a) + TASK_RULES) + 0x274)
        return struct.unpack(">2i", a.c.read_mem(page + 0x670, 8))

    open_rules(a)
    for k in ["DDOWN"] * 5 + ["DRIGHT", "A"] + ["DDOWN"] * 5 + ["A"]:
        a.steps(f"tap {k} 8", "wait 14")
    a.steps("wait 40")
    assert cursor() == (0, 0), cursor()
    a.steps("tap DDOWN 8", "wait 14")             # Yoshi's Story, page 1's first stage
    assert cursor() == (0, 1), cursor()
    a.steps("tap A 8", "wait 14", "tap DUP 8", "wait 14", "tap A 8", "wait 14")   # counterpick; the bar
    a.shot("10-host-switch")
    close_menu(a)
    for g in (a, b):
        _wait(lambda g=g: YS in (session(g).get("stages") or {}).get("counterpicks", []), 20,
              f"{g.name}: Yoshi's Story on the room's list")
        se = session(g)
        assert se["play"]["stage_choice"] == CHOICE_LOSER_PICKS_LIST, se["play"]
        assert se["play"]["name"] == "Custom", se["play"]
    until_view(b, lambda v: not v["slots"][1]["bits"] & ppom.SLOT_READY, "B's ready taken back")
