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


def join_by_code(g: Game, code: str) -> None:
    """From the main menu: WITH FRIENDS > Join Room, a character, the room's code."""
    g.to_online_page()
    to_join_room(g)
    pick(g, right=2)
    type_room_code(g, code)
    until_view(g, lambda v: v["flags"] & ppom.RF_IN, f"{g.name} in the room")


def bo3_room(backend: OnlineBackend, dolphin: Callable[..., DolphinInstance], gpu_backend: str,
             test: str) -> tuple[Game, Game, str]:
    """A and B in a room playing a published Bo3 (created from the launcher's ruleset box): A, B and
    the room code."""
    ua, ub = backend.create_user(f"{test[:4]}ah", "STAH"), backend.create_user(f"{test[:4]}bj", "STBJ")
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
    return a, b, code


@pytest.mark.slow
def test_room_bo3_set(backend: OnlineBackend, dolphin: Callable[..., DolphinInstance],
                      gpu_backend: str) -> None:
    a, b, code = bo3_room(backend, dolphin, gpu_backend, "bo3")
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
        # Everyone stays in the room; the status line has the result until the next set
        # (room-rules.md B).
        winner = (a, b)[st["winner"]]
        _wait(lambda g=g: rooms(g)["view"]["status_text"].startswith(f"{winner.name} wins the set 2-"), 30,
              f"{g.name}: the set's result")
        g.steps("wait 60")
        g.shot("05-set-over")
    _all(lambda: a.steps("wait 1"), lambda: b.steps("wait 1"))


QUIT_NOTICE = 0x2000000   # debug scratch[10]: "<name> quit" on the CSS, the quitter's port in bits 26-27


def to_stage_select(players: list[Game], game: int, timeout: float = 120) -> None:
    """Both on the stage select for the set's `game`: whoever has a character turn (or is not locked
    in on the CSS) presses START."""
    def step() -> bool:
        if all(online_set.scene(g.c) == SSS for g in players):
            return True
        for g in players:
            v = setup_view(g)
            if online_set.scene(g.c) == CSS and v["active"] and v["type"] == 3 and v["my_turn"]:
                g.steps("tap START 8", "wait 30")
        return False
    _wait(step, timeout, f"game {game}: both on the stage select")


def quit_match(g: Game, buttons: str) -> None:
    g.steps("wait 120", "tap START 8", "wait 60", f"hold {buttons} 90")


@pytest.mark.slow
def test_room_set_quit_and_runback(backend: OnlineBackend, dolphin: Callable[..., DolphinInstance],
                                   gpu_backend: str) -> None:
    """Quits in a room's set (docs/game-code.md §11, the quit per mode). Game 1 struck, picked and
    started, then A quits (L+R+A+START): it did not happen. Both back on the CSS, which says who
    quit; the server's set still at game 1, 0-0, and game 1 struck again from the starters; played
    to the end, it counts. In game 2 A runs back (L+R+A+Y+START): both go straight into the same
    match on the same stage, nothing counted; that match played to the end counts as game 2.
    Screenshots: run/artifacts/game-code/rooms/sets/set-quit/."""
    a, b, code = bo3_room(backend, dolphin, gpu_backend, "set-quit")
    players = [a, b]
    a.steps("tap START 8", "wait 40")
    b.steps("tap START 8", "wait 10")
    online_set.wait(lambda: all(online_set.scene(g.c) == SSS for g in players), 120, "game 1: both on the stage select")
    play_steps(players, 1)
    online_set.wait(lambda: all(online_set.gstatus(g.c)["phase"] == "running" for g in players), 240,
                    "game 1 running on both")
    quitter = online_set.local(a.c)["local_port"]
    quit_match(a, "L+R+A+START")
    online_set.wait(lambda: all(online_set.scene(g.c) != "scMelee" for g in players), 60, "both out of the match")
    _wait(lambda: all(g.debug_scratch()[10] & QUIT_NOTICE for g in players), 30, "the quit notice on both")
    for g in players:
        assert (g.debug_scratch()[10] >> 26) & 3 == quitter, (g.name, hex(g.debug_scratch()[10]))
        g.shot("01-quit-notice")
    # Not counted: the server's set is still at game 1, 0-0, and game 1 is struck again.
    for g in players:
        _wait(lambda g=g: (lambda st: st if st and st["game"] == 1 and st["score"] == [0, 0] else None)(rooms(g).get("set")),
              30, f"{g.name}: the set still at game 1, 0-0")
    to_stage_select(players, 1)
    for g in players:
        until_setup(g, lambda v: v["active"] and v["type"] == 1 and sorted(v["selectable"]) == STARTERS,
                    "game 1 struck again from the starters")
    a.shot("02-g1-struck-again")
    play_steps(players, 1)
    games = [online_set.play_game(players, 1)]
    _wait(lambda: (lambda st: st if st and sum(st["score"]) == 1 else None)(rooms(a).get("set")), 30,
          "game 1 counted by the server")

    # Game 2: the bans, the characters (the winner first), then a runback.
    to_stage_select(players, 2)
    play_steps(players, 2)
    online_set.wait(lambda: all(online_set.scene(g.c) == CSS for g in players), 120, "game 2: both back on the CSS")
    for _ in range(2):
        mover = _wait(lambda: next((g for g in players if setup_view(g)["type"] == 3 and setup_view(g)["my_turn"]), None),
                      60, "a character turn")
        mover.steps("tap START 8", "wait 30")
        if all(not setup_view(g)["active"] or setup_view(g)["type"] != 3 for g in players):
            break
    online_set.wait(lambda: all(online_set.gstatus(g.c)["phase"] == "running" for g in players), 240,
                    "game 2 running on both")
    before = [online_set.gstatus(g.c)["match_index"] for g in players]
    setup = B.read_match_setup(a.c.read_mem)
    quit_match(a, "L+R+A+Y+START")
    online_set.wait(lambda: all(online_set.gstatus(g.c)["phase"] == "running" and
                                online_set.gstatus(g.c)["match_index"] == before[0] + 1 for g in players),
                    90, "game 2 again on both")
    for g in players:
        assert B.read_match_setup(g.c.read_mem) == setup, (g.name, "not the same match")
        g.shot("03-g2-runback")
    st = rooms(a)["set"]
    assert st["game"] == 2 and sum(st["score"]) == 1, st
    games.append(online_set.play_game(players, 2))
    _wait(lambda: (lambda st: st if st and sum(st["score"]) == 2 else None)(rooms(a).get("set")), 30,
          "game 2 counted by the server")
    for rep in games:
        assert rep["checksums"]["mismatches"] == 0, rep


@pytest.mark.slow
def test_room_set_waits_for_a_player(backend: OnlineBackend, dolphin: Callable[..., DolphinInstance],
                                     gpu_backend: str) -> None:
    """Decision A (room-rules.md §1.1) against the real server: a player whose Dolphin goes away
    during game 1's strikes pauses the set (the room turns private, their slot kept); when they
    join again it goes on by itself from the strike made, the room public again. Going away in the
    match loses that game."""
    a, b, code = bo3_room(backend, dolphin, gpu_backend, "waits")
    ub = b.user
    players = [a, b]
    a.steps("tap START 8", "wait 40")
    b.steps("tap START 8", "wait 10")
    online_set.wait(lambda: all(online_set.scene(g.c) == SSS for g in players), 120, "both on the stage select")
    for g in players:
        until_setup(g, lambda v: v["active"] and v["type"] == 1 and sorted(v["selectable"]) == STARTERS, "the starters")
    # The first striker strikes one stage; then B's Dolphin is gone.
    mover = _wait(lambda: next((g for g in players if setup_view(g)["my_turn"]), None), 30, "the first striker")
    struck = setup_view(mover)["selectable"][0]
    strike(mover, struck)
    for g in players:
        until_setup(g, lambda v: struck not in v["selectable"], "the strike seen")
    a.steps("wait 60")   # room-step reaches the server
    b.inst.kill()
    st = _wait(lambda: (lambda s: s if s and s["paused"] else None)(rooms(a).get("set")), 120, "A: the set waits")
    assert st["waiting_for"] == ub.display_name and st["score"] == [0, 0], st
    assert rooms(a)["view"]["public"] is False
    _wait(lambda: rooms(a)["view"]["status_text"] == f"Set paused: {ub.display_name} left", 30, "A: the pause text")
    _wait(lambda: a.scene() == CSS, 120, "A back on the room's CSS")
    a.shot("01-paused")

    # B comes back: the set goes on by itself, the room public again; game 1 goes on from the strike.
    b = _boot(dolphin, "set-b2", backend, ub, gpu_backend, "waits", "gameplay", artifacts=SETS_ART)
    b.to_main_menu()
    join_by_code(b, code)
    assert rooms(b)["view"]["you"] == 2
    _wait(lambda: not rooms(a)["set"]["paused"] and rooms(a)["view"]["public"], 30, "A: the set goes on, public")
    players = [a, b]
    a.steps("tap START 8", "wait 40")
    b.steps("tap START 8", "wait 10")
    online_set.wait(lambda: all(online_set.scene(g.c) == SSS for g in players), 120, "again on the stage select")
    for g in players:
        v = until_setup(g, lambda v: v["active"] and v["type"] == 1, "the strikes")
        assert v["game"] == 1 and struck not in v["selectable"] and len(v["selectable"]) == 4, v
    b.shot("02-resumed-strikes")
    play_steps(players, 1)
    # In the match, B goes away again: game 1 is A's (side 0, the host's slot).
    online_set.wait(lambda: all(online_set.gstatus(g.c)["phase"] == "running" for g in players), 240,
                    "game 1 running")
    a.steps("wait 120")
    b.inst.kill()
    st = _wait(lambda: (lambda s: s if s and s["paused"] and s["score"] == [1, 0] else None)(rooms(a).get("set")),
               120, "A: game 1 counted for A, the set waits")
    assert st["game"] == 2, st
    _wait(lambda: a.scene() == CSS, 120, "A back on the room's CSS")
    b = _boot(dolphin, "set-b3", backend, ub, gpu_backend, "waits", "gameplay", artifacts=SETS_ART)
    b.to_main_menu()
    join_by_code(b, code)
    for g in (a, b):
        st = _wait(lambda g=g: (lambda s: s if s and not s["paused"] else None)(rooms(g).get("set")), 30,
                   f"{g.name}: the set goes on at game 2")
        assert st["score"] == [1, 0] and st["game"] == 2, st
        g.steps("wait 60")
        g.shot("03-game-2")


def keypad_letters(g: Game, code: str) -> None:
    """Type letters on Brawl's keypad in its room-code letter layout (the ruleset mode has the
    same keys), from the top-left letter key (as test_rooms_game.type_room_code moves there)."""
    from test_rooms_game import ROOM_KEYS
    steps: list[str] = []
    move = lambda d: steps.extend([f"tap {d} 8", "wait 6"])  # noqa: E731
    for _ in range(6):
        move("DUP")
    move("DDOWN")
    move("DLEFT")
    move("DLEFT")
    pos, last = (1, 0), None
    for ch in code:
        key = next(k for k, v in ROOM_KEYS.items() if ch in v)
        if key == last:
            move("DRIGHT" if pos[1] < 2 else "DLEFT")
            move("DLEFT" if pos[1] < 2 else "DRIGHT")
        r, c = pos
        while r < key[0]:
            move("DDOWN"); r += 1
        while r > key[0]:
            move("DUP"); r -= 1
        while c < key[1]:
            move("DRIGHT"); c += 1
        while c > key[1]:
            move("DLEFT"); c -= 1
        pos = (r, c)
        steps.extend(["tap A 8", "wait 10"] * (ROOM_KEYS[key].index(ch) + 1))
        last = key
    g.steps(*steps)


def test_ruleset_picked_in_game(backend: OnlineBackend, dolphin: Callable[..., DolphinInstance],
                                gpu_backend: str) -> None:
    """The game's Ruleset line (room_play.cpp, decision 12) against the real server: the host's own
    published ruleset comes in the list (CMD_RULESETS <- mm room-rulesets), right and A play it in
    the room; "Enter code" types another one's code on the keypad. Each becomes the room's
    ruleset on both machines and the host's remembered one."""
    from test_room_rules import menu_open, open_rules, two_in_a_room
    a, b, _code = two_in_a_room(backend, dolphin, gpu_backend, "picked")
    for g in (a, b):
        g.out = SETS_ART / "picked" / g.name
        g.out.mkdir(parents=True, exist_ok=True)
    first = backend._http("POST", "/v1/rulesets", {"ruleset": BO3}, token=a.user.session_token)["code"]
    other = dict(BO3, name="Typed Ruleset", bestOf=0, bans=0, dsr=False, stageChoice="random", strikes=[])
    second = backend._http("POST", "/v1/rulesets", {"ruleset": other}, token=a.user.session_token)["code"]

    # The list: P+ default (the room's), then the host's rulesets; one right is the newest own one.
    open_rules(a)
    a.steps("tap DDOWN 8", "wait 8", "tap DDOWN 8", "wait 8", "tap DDOWN 8", "wait 8", "tap DDOWN 8", "wait 40")
    a.shot("01-ruleset-line")
    a.steps("tap DRIGHT 8", "wait 30")
    a.shot("02-browse")
    a.steps("tap A 8", "wait 40")
    _wait(lambda: not menu_open(a), 10, "the menu closed by the pick")
    for g in (a, b):
        _wait(lambda g=g: rooms(g).get("ruleset", {}).get("code") in (first, second), 30,
              f"{g.name}: a picked ruleset")
    picked = rooms(a)["ruleset"]["code"]
    assert backend._http("GET", "/v1/me/ruleset", token=a.user.session_token)["ruleset"]["code"] == picked
    a.steps("wait 30")
    a.shot("03-picked")

    # Enter code: left from the room's ruleset to the end of the list, A, the keypad, START.
    want = first if picked == second else second
    open_rules(a)
    a.steps("tap DDOWN 8", "wait 8", "tap DDOWN 8", "wait 8", "tap DDOWN 8", "wait 8", "tap DDOWN 8", "wait 30")
    # (the list wraps: the room's ruleset, P+ default, then "Enter code" at the end)
    a.steps("tap DLEFT 8", "wait 15", "tap DLEFT 8", "wait 15")
    a.shot("04-enter-code")
    a.steps("tap A 8", "wait 60")
    assert not menu_open(a), "Enter code did not open the keypad"
    keypad_letters(a, want)
    a.shot("05-code-typed")
    a.steps("tap START 8", "wait 60")
    for g in (a, b):
        _wait(lambda g=g: rooms(g).get("ruleset", {}).get("code") == want, 30, f"{g.name}: the typed ruleset")
    assert backend._http("GET", "/v1/me/ruleset", token=a.user.session_token)["ruleset"]["code"] == want
    assert not ppom.read_local(a.c, ppom.find_block(a.c))["lock"]["ready"], "the keypad's START locked in"
    a.steps("wait 30")
    a.shot("06-typed")


BO3_2V2 = dict(BO3, name="Doubles Set Test", players="2v2")


@pytest.mark.slow
def test_room_2v2_set(backend: OnlineBackend, dolphin: Callable[..., DolphinInstance],
                      gpu_backend: str) -> None:
    """Decision D against the real server: a published 2v2 Bo3 in a four-slot Teams room. Red
    (slots 1 and 3) is side 0 and blue (2 and 4) side 1; either teammate strikes, bans and picks,
    and the session host's machine decides which presses count. Both teammates press within a
    few milliseconds of each other (different stages for a one-stage step, then the same stage),
    and every machine ends on the host's lists. Red wins game 1 and the server counts it from all
    four reports; game 2 starts with red banning, blue picking, then both red players choosing
    characters."""
    import contextlib
    import threading

    from ppharness import flows as F

    from test_rooms_game import PANEL_X, hand_to
    test = "doubles"
    users = [backend.create_user(n, f"DB{'ABCD'[k]}T") for k, n in enumerate(("dbred1", "dbblu1", "dbred2", "dbblu2"))]
    gs = [_boot(dolphin, f"dbl-{x}", backend, u, gpu_backend, test, "gameplay", artifacts=SETS_ART)
          for x, u in zip("abcd", users)]
    a, b, c, d = gs
    _all(*[g.to_main_menu for g in gs])
    _all(*[g.to_online_page for g in gs])
    rs_code = backend._http("POST", "/v1/rulesets", {"ruleset": BO3_2V2}, token=users[0].session_token)["code"]
    rid = write_create(a, ruleset=rs_code)
    assert launcher_answer(a, rid)["result"] == "accepted"
    _wait(lambda: a.scene() == CSS, 60, "A on the room's CSS")
    code = until_view(a, lambda v: v["flags"] & ppom.RF_IN, "A in its room", timeout=60)["code"]
    pick(a)
    for slot in (2, 3):
        hand_to(a, PANEL_X[slot], -10.0)
        a.steps("tap A 8", "wait 30")
        until_view(a, lambda v, s=slot: v["slots"][s]["bits"] & ppom.SLOT_OPEN, f"slot {slot + 1} open")
    a.steps("tap R 8", "wait 30")
    until_view(a, lambda v: v["flags"] & ppom.RF_TEAMS, "Teams on")
    hand_to(a, -23.0, 10.5)
    _all(*[lambda g=g: to_join_room(g) for g in gs[1:]])
    for k, g in enumerate(gs[1:], 1):
        pick(g, right=2 * k)
    for k, g in enumerate(gs[1:], 1):
        type_room_code(g, code)
        until_view(g, lambda v: v["flags"] & ppom.RF_IN, f"player {k + 1} in the room")
    # Slot 3 green -> red, slot 4 red -> blue (X): red 1 + 3, blue 2 + 4.
    c.steps("tap X 8", "wait 20")
    d.steps("tap X 8", "wait 20")
    until_view(a, lambda v: [s["team"] for s in v["slots"]] == [0, 1, 0, 1], "two teams of two")
    for g in gs:
        _wait(lambda g=g: rooms(g).get("ruleset", {}).get("code") == rs_code, 30, f"{g.name}: the room's ruleset")
        g.steps("wait 40")
        g.shot("01-two-teams")

    for g in gs:
        g.steps("tap START 8", "wait 10")
    online_set.wait(lambda: all(online_set.scene(g.c) == SSS for g in gs), 120, "game 1: all on the stage select")
    st = _wait(lambda: rooms(a).get("set"), 30, "the set")
    assert st["side_slots"] == [0b0101, 0b1010], st
    for g in gs:
        until_setup(g, lambda v: v["active"] and v["type"] == 1 and sorted(v["selectable"]) == STARTERS, "the starters")
    teams = {0: (a, c), 1: (b, d)}
    first = 0 if setup_view(a)["my_turn"] else 1
    x, y = teams[first]
    assert setup_view(x)["my_turn"] and setup_view(y)["my_turn"], (setup_view(x), setup_view(y))
    assert not any(setup_view(g)["my_turn"] for g in teams[1 - first])

    def press_together(pairs: list[tuple[Game, int]]) -> list[bool]:
        """Each (player, stage) presses at once on its own thread: GameSetup::Act, as the game's
        GP_COMPLETE_STEP, within a few ms of each other."""
        out: list[bool] = [False] * len(pairs)
        go = threading.Event()

        def one(i: int, g: Game, kind: int) -> None:
            go.wait()
            out[i] = g.c.call("game_setup_act", kind=kind)["ok"]
        th = [threading.Thread(target=one, args=(i, g, k)) for i, (g, k) in enumerate(pairs)]
        for t in th:
            t.start()
        go.set()
        for t in th:
            t.join(30)
        return out

    def converged(n: int, what: str) -> list[int]:
        """Every machine shows the same `n` selectable stages and step (the host's lists)."""
        def same() -> list[int] | None:
            views = [setup_view(g) for g in gs]
            sel = [sorted(v["selectable"]) for v in views]
            ok = all(s_ == sel[0] for s_ in sel) and len(sel[0]) == n and len({v["type"] for v in views}) == 1
            return sel[0] if ok else None
        return _wait(same, 20, what)

    # One stage to strike: both teammates press a different stage at once. Both show theirs at
    # first; the host takes one, and every machine ends on it.
    ok = press_together([(x, STARTERS[0]), (y, STARTERS[1])])
    assert all(ok), ok
    # The teammate whose press the host dropped is told (its stage comes back).
    _wait(lambda: any(setup_view(g)["text"].startswith("Partner was first") for g in (x, y)), 5,
          "Partner was first")
    left = converged(4, "the first strike on every machine")
    assert len(left) == 4 and (STARTERS[0] in left) != (STARTERS[1] in left), left
    assert not any(setup_view(h)["my_turn"] for h in teams[first]), [setup_view(h) for h in teams[first]]
    x.shot("02-one-strike-counted")
    # The other team strikes two: both press the same stage at once; one counts, one is dropped,
    # and the team still has one to strike.
    x2, y2 = teams[1 - first]
    _wait(lambda: setup_view(x2)["my_turn"] and setup_view(y2)["my_turn"], 20, "the other team's turn")
    press_together([(x2, left[0]), (y2, left[0])])
    left2 = converged(3, "the same stage pressed twice")
    assert left2 == [k for k in left if k != left[0]], (left, left2)
    v = setup_view(x2)
    assert v["my_turn"] and v["count"] == 1, v
    play_steps(gs, 1)

    # Red wins game 1: blue walks off. Every player's report counts it.
    online_set.wait(lambda: all(online_set.gstatus(g.c)["phase"] == "running" for g in gs), 240, "game 1 running")
    def off(g: Game, port: int) -> None:
        with contextlib.suppress(Exception):
            F.fight([F.Seat(g.c, port, 0, g.name)], 60 * 60 * 3, mode="selfdestruct", seed=f"dbl{port}",
                    stop=lambda: online_set.gstatus(g.c)["phase"] != "running" or online_set.scene(g.c) != "scMelee")
    th = [threading.Thread(target=off, args=(g, port)) for g, port in ((b, 1), (d, 3))]
    for t in th:
        t.start()
    for t in th:
        t.join(300)
    st = _wait(lambda: (lambda s: s if s and s["score"] == [1, 0] else None)(rooms(a).get("set")), 240,
               "game 1 counted for red")
    assert st["game"] == 2 and not st["paused"], st

    # Game 2: red's captain bans, blue's captain picks, then both red players choose characters.
    online_set.wait(lambda: all(online_set.scene(g.c) == SSS for g in gs), 180, "game 2: all on the stage select")
    v = until_setup(a, lambda v: v["active"] and v["game"] >= 2 and v["type"] == 1, "red bans")
    assert v["my_turn"] and setup_view(c)["my_turn"], (v, setup_view(c))
    assert setup_view(b)["text"].startswith("Opponents are banning"), setup_view(b)
    a.shot("03-g2-red-bans")
    play_steps(gs, 2)
    online_set.wait(lambda: all(online_set.scene(g.c) == CSS for g in gs), 120, "game 2: all back on the CSS")
    movers = _wait(lambda: (lambda m: m if len(m) == 2 else None)(
        [g.name for g in gs if setup_view(g)["type"] == 3 and setup_view(g)["my_turn"]]), 60, "red's character turn")
    assert sorted(movers) == sorted([a.name, c.name]), movers
    for g in gs:
        g.steps("wait 40")
        g.shot("04-g2-red-characters")
