"""The waiting room: Unranked and Ranked search from Brawl's online waiting room (the Online
Training stage, a Sandbag in the middle, the search line in the room's scrolling banner), as
Brawl's own "With Anyone" and Brawlback's Quickplay do (docs/game-code.md, "The waiting room").

Instances are named ``wr-*`` (``ppharness clean --prefix wr-``). Screenshots go to
``run/artifacts/game-bridge/<test>/``. Needs what ``test_online_unranked.py`` needs.
"""

from __future__ import annotations

import json
from typing import Callable, Iterator

import pytest

from ppharness import brawl as B
from ppharness import paths
from ppharness.backend import OnlineBackend
from ppharness.instance import DolphinInstance

from test_online import _make_backend, _wait
from test_online_game import Game, _boot, _both, _find_logged, _peer_shown, online_set, ppom
from test_online_ranked import STARTERS, _ranked, _strike_to, _wait_view

pytestmark = [pytest.mark.dolphin, pytest.mark.server, pytest.mark.gpu]

ROOT = paths.workspace_root()
STAGE_ONLINE_TRAINING = 0x37
UNRANKED_STAGES = [0x21, 0x2D]   # Smashville, Dream Land (a two-stage test list)
WIFIPR_HUD = 0x805A0300          # ifWifiprTask's HUD: the banner and the player panels
SCREEN_ONLINE_BUSY = 4           # PPOM: an online search (the launcher shows no "In game")
# training_room.cpp's debug word (scratch[5]): 0xF the step (3 in the room, 7 after it), 0xF0
# the room's exit code, 0xF00 how it ended (1 the match, 2 the CSS), 0x10000 the banner seen, 0x20000 a Sandbag made.
SCR_ROOM = 5
ROOM_RUNNING, ROOM_ENDED = 3, 7
ROOM_BANNER, ROOM_SANDBAG = 0x10000, 0x20000


@pytest.fixture(scope="module")
def backend(tmp_path_factory: pytest.TempPathFactory) -> Iterator[OnlineBackend]:
    rules = json.loads((ROOT / "server" / "config" / "rulesets.json").read_text(encoding="utf-8"))
    rules["unranked"]["stages"] = UNRANKED_STAGES
    path = tmp_path_factory.mktemp("rulesets") / "rulesets.json"
    path.write_text(json.dumps(rules), encoding="utf-8")
    be = _make_backend(rulesets_file=path, ticket_ttl_secs=120)
    try:
        yield be
    finally:
        be.stop()


def _room(g: Game) -> int:
    return g.debug_scratch()[SCR_ROOM]


def _stage_kind(g: Game) -> int:
    mm = g.c.read_u32(g.c.read_u32(0x805A00E0) + 8)   # GameGlobal -> gmGlobalModeMelee
    return int.from_bytes(g.c.read_mem(mm + 0x1A, 2), "big")


def _fighter_x(g: Game) -> float | None:
    ps = B.read_players(g.c.read_mem, max_entries=1)
    return ps[0].x if ps else None


def _in_room(g: Game) -> bool:
    return g.scene() == "scMelee" and (_room(g) & 0xF) == ROOM_RUNNING


def _search(g: Game, mode: int) -> None:
    """START on the CSS locks in and searches (FIND_OPPONENT); the CSS leaves for the room."""
    n = g.finds()
    g.press_until("START", lambda: g.finds() > n, "FIND_OPPONENT")
    _find_logged(g, mode, "")
    # (A room whose match is already set up ends as soon as it runs.)
    _wait(lambda: _in_room(g) or (_room(g) & 0xF) == ROOM_ENDED, 30, f"{g.inst.name} in the waiting room")


def _check_room(g: Game, shot: str) -> None:
    """The room is Brawl's Online Training stage with the player's fighter, the Sandbag and the
    search line in the banner, and Dolphin runs no match in it."""
    assert _stage_kind(g) == STAGE_ONLINE_TRAINING, hex(_stage_kind(g))
    _wait(lambda: _room(g) & ROOM_BANNER, 10, "the banner line")
    _wait(lambda: _room(g) & ROOM_SANDBAG, 10, "the Sandbag")
    x0 = _fighter_x(g)
    g.steps("stick left 30", "wait 10")
    x1 = _fighter_x(g)
    assert x0 is not None and x1 is not None and x1 < x0 - 5, ("the player's controller moves the fighter", x0, x1)
    g.steps("tap A 4", "wait 20")
    # Brawl's "Seeking..." player panels are hidden (the user's, 2026-10-16): every panel of the
    # room's HUD (0x805A0300, +0x1C..) is in state 0, none.
    hud = g.c.read_u32(WIFIPR_HUD)
    assert [g.c.read_u32(g.c.read_u32(hud + 0x1C + 4 * k) + 8) for k in range(7)] == [0] * 7
    g.shot(shot)
    st = g.c.mm_status()
    assert st["state"] == "matchmaking", st
    assert ppom.read_room(g.c, ppom.find_block(g.c))["screen"] == SCREEN_ONLINE_BUSY


def _logs_clean(g: Game) -> None:
    """Dolphin never took the room for the match (GameplaySession OnStageCreate, IsWaitingRoom)."""
    text = (g.inst.user_dir / "Logs" / "dolphin.log").read_text(errors="replace")
    assert "differs beyond the stage variant" not in text


def _back_on_css(g: Game, what: str) -> None:
    _wait(lambda: g.scene() == "scSelctCharacter" and (_room(g) & 0xF) == ROOM_ENDED, 30, what)


@pytest.mark.slow
def test_unranked_set_from_the_waiting_room(backend: OnlineBackend, dolphin: Callable[..., DolphinInstance],
                                            gpu_backend: str) -> None:
    """A searches alone and waits in the room; B searches, the server pairs them, both rooms end
    and game 1 starts by itself; the set plays as it does from the CSS (no desync)."""
    test = "waiting-room-unranked"
    ua, ub = backend.create_user("nora", "NORA"), backend.create_user("otto", "OTTO")
    a = _boot(dolphin, "wr-a", backend, ua, gpu_backend, test, "gameplay")
    b = _boot(dolphin, "wr-b", backend, ub, gpu_backend, test, "gameplay")
    _both(a.to_main_menu, b.to_main_menu)
    _both(a.to_online_page, b.to_online_page)
    _both(lambda: a.to_css("unranked"), lambda: b.to_css("unranked"))
    for g in (a, b):
        B.write_rules(g.c, stocks=2, minutes=2, items_off=True)
        ppom.allow_test_rules(g.c)

    _search(a, 1)
    assert _in_room(a)
    _check_room(a, "01-room-searching")
    _search(b, 1)
    _peer_shown(a, ub)
    _peer_shown(b, ua)
    rep = online_set.play_set([a, b], games=2, mode="unranked")
    stages = [g["stage"] for g in rep["games"]]
    assert sorted(stages) == sorted(UNRANKED_STAGES), [hex(s) for s in stages]
    for g in (a, b):
        assert _room(g) & 0xF0F == 0x100 | ROOM_ENDED, hex(_room(g))   # ended into the match
        assert g.bridge()["lost"] == 0
        _logs_clean(g)
        g.c.mm_cancel()


@pytest.mark.slow
def test_waiting_room_cancels(backend: OnlineBackend, dolphin: Callable[..., DolphinInstance],
                              gpu_backend: str) -> None:
    """START in the room cancels the search and goes back to the CSS, the character still
    selected; so does holding B, while a short B is an attack. The player plays from controller
    port 4, the one that pressed START on the CSS."""
    test = "waiting-room-cancel"
    ua = backend.create_user("pia", "PIA")
    a = _boot(dolphin, "wr-c", backend, ua, gpu_backend, test, "gameplay", standard_controllers=(3,))
    a.pad = 3
    a.to_main_menu()
    a.to_online_page()
    a.to_css("unranked")
    picked = a.css()["char"]

    _search(a, 1)
    assert _in_room(a)
    _check_room(a, "00-room-port-4")
    a.steps("wait 30", "tap START 4")
    _back_on_css(a, "back on the CSS after START")
    _wait(lambda: a.c.mm_status()["state"] == "idle", 10, "the search cancelled")
    a.steps("wait 40")
    a.shot("01-css-after-start")
    assert a.css()["char"] == picked, (a.css(), picked)

    _search(a, 1)
    assert _in_room(a)
    a.steps("wait 30", "tap B 6", "wait 30")
    assert _in_room(a), "a short B is an attack, not a cancel"
    a.steps("tap B 75")
    _back_on_css(a, "back on the CSS after holding B")
    _wait(lambda: a.c.mm_status()["state"] == "idle", 10, "the search cancelled")
    a.steps("wait 40")
    a.shot("02-css-after-hold-b")
    assert a.scene() == "scSelctCharacter", "the held B must not also leave the CSS"
    assert a.css()["char"] == picked, (a.css(), picked)


@pytest.mark.slow
def test_ranked_game_1_from_the_waiting_room(backend: OnlineBackend, dolphin: Callable[..., DolphinInstance],
                                             gpu_backend: str) -> None:
    """Ranked waits in the room too; once connected both rooms end on the CSS, the game setup
    begins (the strikes on the stage select) and game 1 plays."""
    test = "waiting-room-ranked"
    ua, ub = backend.create_user("rhea", "RHEA"), backend.create_user("sven", "SVEN")
    a = _boot(dolphin, "wr-ra", backend, ua, gpu_backend, test, "gameplay")
    b = _boot(dolphin, "wr-rb", backend, ub, gpu_backend, test, "gameplay")
    _both(a.to_main_menu, b.to_main_menu)
    _both(a.to_online_page, b.to_online_page)
    _both(lambda: a.to_css("ranked"), lambda: b.to_css("ranked"))
    for g in (a, b):
        B.write_rules(g.c, stocks=1, minutes=2, items_off=True)
        ppom.allow_test_rules(g.c)

    _search(a, 0)
    assert _in_room(a)
    _check_room(a, "01-room-searching")
    _search(b, 0)
    _peer_shown(a, ub)
    _peer_shown(b, ua)
    players = [a, b]
    host = a if _ranked(a)["local_port"] == 0 else b
    join = b if host is a else a
    for g in players:
        _wait_view(g, lambda v: v["type"] == 1 and sorted(v["selectable"]) == STARTERS, "the starters")
        assert _room(g) & 0xF0F == 0x200 | ROOM_ENDED, hex(_room(g))   # ended on the CSS
    _strike_to(host, join, 0x01)
    rep = online_set.play_game(players, 1)
    assert rep["stage"] == 0x01, rep
    assert rep["checksums"]["mismatches"] == 0, rep
    for g in players:
        _logs_clean(g)
