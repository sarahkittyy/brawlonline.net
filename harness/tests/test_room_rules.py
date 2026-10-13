"""Room rules end to end (docs/design/room-rules.md, phase 2): the host edits the room's rules in
P+'s own rules menu over the room CSS (game-code/PPOnline/source/room_rules.cpp), everyone in the
room sees them (room-state rules, SESSION Rules), the others can only look, and the room's games
are played with them. Screenshots: run/artifacts/game-code/rooms/rules/<test>/<player>/.
"""
from __future__ import annotations

import struct
from typing import Any, Callable

import pytest

from ppharness.backend import OnlineBackend
from ppharness.instance import DolphinInstance

from test_online import _wait
from test_online_game import Game, _boot, backend  # noqa: F401  (the backend fixture)
from test_rooms_game import ART, CSS, _all, create_room, pick, room_view, to_join_room, type_room_code, until_view
import ppom  # noqa: E402  (on sys.path through test_rooms_game)
from ppharness import brawl as B  # noqa: E402

pytestmark = [pytest.mark.dolphin, pytest.mark.server, pytest.mark.gpu]

RULES_ART = ART / "rules"
TASK_RULES, TASK_RULES_OPEN = 0x62C, 0x630   # muSelCharTask: muRuleTask*, open


def css_task(g: Game) -> int:
    return g.c.read_u32(g.c.read_u32(g.c.read_u32(0x805A0060) + 4) + 0x400)


def menu_open(g: Game) -> bool:
    return g.c.read_u32(css_task(g) + TASK_RULES_OPEN) != 0


def set_rule(g: Game) -> bytes:
    return g.c.read_mem(B.Mem(g.c.read_mem).chain(B.GAME_GLOBAL_PTR, B.GG_SET_RULE), 0x10)


def session_rules(g: Game) -> dict[str, Any] | None:
    return ppom.read_session(g.c, ppom.find_block(g.c))["rules"]


def open_rules(g: Game) -> None:
    ppom.set_cfg_bits(g.c, ppom.CFG_TEST_OPEN_RULES)
    _wait(lambda: menu_open(g), 10, f"{g.name}: the rules menu open")
    g.steps("wait 40")


def two_in_a_room(backend: OnlineBackend, dolphin: Callable[..., DolphinInstance], gpu_backend: str,
                  test: str) -> tuple[Game, Game, str]:
    ua = backend.create_user(f"{test[:6]}h", "RRHS")
    ub = backend.create_user(f"{test[:6]}j", "RRJN")
    a = _boot(dolphin, "rules-a", backend, ua, gpu_backend, test, "gameplay", artifacts=RULES_ART)
    b = _boot(dolphin, "rules-b", backend, ub, gpu_backend, test, "gameplay", artifacts=RULES_ART)
    _all(a.to_main_menu, b.to_main_menu)
    _all(a.to_online_page, b.to_online_page)
    code = create_room(a)
    to_join_room(b)
    pick(a)
    pick(b, right=2)
    type_room_code(b, code)
    until_view(b, lambda v: v["flags"] & ppom.RF_IN, "B in the room")
    until_view(a, lambda v: v["slots"][1]["bits"] & ppom.SLOT_TAKEN, "A sees B")
    _all(lambda: a.steps("wait 60"), lambda: b.steps("wait 60"))
    return a, b, code


def pages(g: Game) -> list[int]:
    rt = g.c.read_u32(css_task(g) + TASK_RULES)
    return [g.c.read_u32(rt + o) for o in (0x268, 0x26C, 0x270, 0x274)]


def cursor(g: Game, page: int) -> int:
    return int.from_bytes(g.c.read_mem(pages(g)[page] + 0x42, 2), "big")


def rules_working(g: Game) -> tuple[bytes, bytes]:
    """RULES' and MORE RULES' working copies (+0x66C): style, target, stock control, damage, stage
    choice, -, -, stocks / time limit, team attack, pause, auto L-cancel, input assist."""
    p = pages(g)
    return g.c.read_mem(p[0] + 0x66C, 8), g.c.read_mem(p[1] + 0x66C, 6)


def tap(g: Game, *keys: str) -> None:
    for k in keys:
        g.steps(f"tap {k} 8", "wait 20")


def close_menu(g: Game) -> None:
    for _ in range(4):
        if not menu_open(g):
            break
        tap(g, "B")
    _wait(lambda: not menu_open(g), 10, f"{g.name}: the rules menu closed")
    g.steps("wait 30")


def test_host_edits_the_room_rules_everyone_plays_them(backend: OnlineBackend,
                                                       dolphin: Callable[..., DolphinInstance],
                                                       gpu_backend: str) -> None:
    """The host opens P+'s rules menu over the room CSS (CFG_TEST_OPEN_RULES, as the RULES button
    will), sets 3 stocks, a 1-minute time limit and auto L-cancel; stock control (handicap, not a
    room rule) does not move for the host either. When the menu closes the rules reach the other
    player (SESSION Rules from room-state), whose ready is taken back. The other player opens the
    same menu and can change nothing. Both ready: the game is played with the host's rules on both
    machines, in sync."""
    a, b, code = two_in_a_room(backend, dolphin, gpu_backend, "host-edits")
    b.steps("tap START 8", "wait 40")
    until_view(a, lambda v: v["slots"][1]["bits"] & ppom.SLOT_READY, "A sees B ready")

    # B is looking at the rules while the host edits them: B's screens go out of date, so B's menu
    # is closed from outside (muRuleTask+0x288, a byte) once the change arrives.
    open_rules(b)

    # The host edits.
    open_rules(a)
    a.shot("01-host-rules-open")
    tap(a, "DDOWN", "DLEFT")                       # stocks 4 -> 3
    tap(a, "DDOWN", "DRIGHT")                      # stock control: read-only for the host too
    w1, _ = rules_working(a)
    assert w1[7] == 3 and w1[2] == 0, w1.hex()
    tap(a, "DDOWN", "DDOWN", "DDOWN", "DRIGHT")    # the bottom row: MORE RULES
    assert cursor(a, 0) == 6, cursor(a, 0)
    a.shot("02-host-rules-edited")
    tap(a, "A")
    a.steps("wait 20")
    tap(a, *["DLEFT"] * 7)                         # time limit 8 -> 1
    tap(a, "DDOWN", "DDOWN", "DDOWN", "DRIGHT")    # auto L-cancel on
    _, w2 = rules_working(a)
    assert (w2[0], w2[3]) == (1, 1), w2.hex()
    a.shot("03-host-more-rules")
    # The item switch, for the record (its layout is not mapped yet).
    tap(a, "B")
    a.steps("wait 20")
    print("RULES cursor after MORE RULES", cursor(a, 0))
    close_menu(a)
    want = {"style": "stock", "stocks": 3, "timeMinutes": 1, "damageRatio": 1.0, "teamAttack": True,
            "pause": "normal", "itemFrequency": 0, "itemSwitch": [0, 4, 0, 0, 0, 0, 0, 0], "hazards": False,
            "autoLCancel": True, "inputAssist": False}
    for g in (a, b):
        _wait(lambda g=g: session_rules(g) == want, 20, f"{g.name}: the host's rules", interval=0.3)
    _wait(lambda: not menu_open(b), 10, "B's out-of-date rules menu closed by the change")
    v = until_view(b, lambda v: not v["slots"][1]["bits"] & ppom.SLOT_READY, "B's ready taken back")
    b.steps("wait 20")
    b.shot("04-rules-changed")                     # "Host changed the rules"

    # The other player looks: nothing they press changes a rule.
    open_rules(b)
    b.shot("05-viewer-open")
    before = rules_working(b)
    tap(b, "A", "DRIGHT", "DDOWN", "DLEFT", "DRIGHT", "DDOWN", "DDOWN", "DRIGHT", "DDOWN", "DDOWN")
    assert rules_working(b)[0] == before[0], (before[0].hex(), rules_working(b)[0].hex())
    tap(b, "DRIGHT")                               # the bottom row: MORE RULES
    tap(b, "A")
    b.steps("wait 20")
    more = rules_working(b)[1]                     # (the page fills its copy when it opens)
    assert (more[0], more[3]) == (1, 1), more.hex()
    tap(b, "DLEFT", "DDOWN", "DLEFT", "DDOWN", "DLEFT", "DDOWN", "DLEFT")
    assert rules_working(b)[1] == more, (more.hex(), rules_working(b)[1].hex())
    b.shot("06-viewer-more-rules")
    close_menu(b)
    b.steps("wait 60")
    for g in (a, b):
        assert session_rules(g) == want, (g.name, session_rules(g))

    # The game, with the host's rules on both machines.
    game = room_view(a)["game"]
    _all(lambda: a.steps("tap START 8", "wait 30"), lambda: b.steps("tap START 8", "wait 30"))
    import online_set  # noqa: E402
    online_set.wait(lambda: all(online_set.scene(g.c) == "scMelee" for g in (a, b)), 120, "both in the match")
    online_set.wait(lambda: all(online_set.gstatus(g.c)["phase"] == "running" for g in (a, b)), 240,
                    "the match running on both")
    for g in (a, b):
        r = set_rule(g)
        mm = B.Mem(g.c.read_mem).chain(B.GAME_GLOBAL_PTR, B.GG_MODE_MELEE)
        raw = g.c.read_mem(mm, 0x98 + 2 * 0x5C)
        alc = g.c.read_mem(mm + 0xE5, 1)[0] & 3
        assert (r[4], r[8], r[0xB], r[0xC]) == (3, 1, 1, 0), (g.name, r.hex())
        assert [raw[0x98 + 4], raw[0x98 + 0x5C + 4]] == [3, 3] and alc == 1, (g.name, raw[0x98:0x9D].hex(), alc)
    rep = online_set.play_game([a, b], game)
    assert rep["checksums"]["mismatches"] == 0, rep
    _wait(lambda: a.scene() == CSS and b.scene() == CSS, 180, "both back on the room's CSS")
    for g in (a, b):
        g.steps("wait 60")
        assert session_rules(g) == want, (g.name, session_rules(g))
        g.shot("07-back-in-the-room")


def room_play(g: Game) -> dict[str, Any] | None:
    return ppom.read_session(g.c, ppom.find_block(g.c)).get("play")


def test_host_sets_the_set_length(backend: OnlineBackend, dolphin: Callable[..., DolphinInstance],
                                  gpu_backend: str) -> None:
    """The SET line of P+'s RULES page (room_play.cpp; staging feedback 2026-10-12: left / right did
    nothing): the host's left / right pick the set's length, sent as room-best-of when the menu
    closes. The room's ruleset becomes Custom with that bestOf on both machines and the other
    player's ready is taken back; the other player's left / right change nothing; both ready, a
    Bo3 set starts; the other player drops out, the set pauses and the host ends it on the same line
    (End set). (The whole cycle, No set / 3 / 5 / 7: test_rooms_ui.py
    test_host_changes_the_set_length.)"""
    a, b, code = two_in_a_room(backend, dolphin, gpu_backend, "set-length")
    for g in (a, b):
        assert room_play(g)["best_of"] == 0, (g.name, room_play(g))
    b.steps("tap START 8", "wait 40")
    until_view(a, lambda v: v["slots"][1]["bits"] & ppom.SLOT_READY, "A sees B ready")

    open_rules(a)
    tap(a, "DDOWN", "DDOWN", "DRIGHT")             # SET: No set -> Best of 3
    a.shot("08-host-set-length")
    close_menu(a)
    for g in (a, b):
        _wait(lambda g=g: (room_play(g) or {}).get("best_of") == 3, 20, f"{g.name}: Best of 3", interval=0.3)
        assert room_play(g)["custom"], (g.name, room_play(g))
    until_view(b, lambda v: not v["slots"][1]["bits"] & ppom.SLOT_READY, "B's ready taken back")
    a.steps("wait 30")
    a.shot("09-best-of-3")

    # The other player: left / right on SET change nothing.
    open_rules(b)
    tap(b, "DDOWN", "DDOWN", "DRIGHT", "DRIGHT", "DLEFT")
    b.shot("10-viewer-set-length")
    close_menu(b)
    b.steps("wait 90")
    for g in (a, b):
        assert room_play(g)["best_of"] == 3, (g.name, room_play(g))

    # Both ready: the set starts, with that length (the next game is its game 1).
    _all(lambda: a.steps("tap START 8", "wait 30"), lambda: b.steps("tap START 8", "wait 30"))
    for g in (a, b):
        _wait(lambda g=g: ((room_play(g) or {}).get("set") or {}).get("game") == 1 and room_play(g)["best_of"] == 3,
              60, f"{g.name}: a Bo3 set on", interval=0.5)
    a.shot("11-bo3-set-on")

    # B drops out of the set's session (its Dolphin gone): the set pauses, A back on the CSS, and
    # the host ends it (End set, the user's of 2026-10-12): no result, the set gone, the length as
    # it was. (Between a running set's games the session goes on: mm answers "Wait for the game to
    # end." there.)
    import online_set  # noqa: E402
    from test_room_sets import SSS  # noqa: E402
    online_set.wait(lambda: all(online_set.scene(g.c) == SSS for g in (a, b)), 120, "game 1: both on the stage select")
    b.inst.kill()
    _wait(lambda: a.scene() == CSS, 120, "A back on the room's CSS")
    _wait(lambda: ((room_play(a) or {}).get("set") or {}).get("paused"), 60, "A: the set paused", interval=0.5)
    a.steps("wait 60")
    a.shot("12-set-paused")
    open_rules(a)
    tap(a, "DDOWN", "DDOWN", "DRIGHT")              # SET: Paused 0-0 -> End set
    a.shot("13-end-set")
    tap(a, "A")
    _wait(lambda: not menu_open(a), 10, "A's menu closed by End set")
    _wait(lambda: (room_play(a) or {}).get("set") is None, 20, "A: the set ended", interval=0.3)
    assert room_play(a)["best_of"] == 3, room_play(a)
    a.steps("wait 30")
    a.shot("14-set-ended")
