"""Rooms end to end: the game's room UI (game-code/PPOnline) against Dolphin's side and our local
accounts + mm servers (docs/design/rooms.md, docs/rooms-game-interface.md).

Every step is played through the game's own screens with controller input: WITH FRIENDS >
Create Room on one game, WITH FRIENDS > Join Room and the room code on the keypad on the other,
the panels showing the others, START to get ready, the room's game under the gameplay session,
and back on the room's CSS. Screenshots: run/artifacts/game-code/rooms/<test>/<player>/.

Run with a Dolphin built from rooms-dolphin (PPHARNESS_DOLPHIN_DIR) and this plugin.
"""

from __future__ import annotations

import concurrent.futures
import sys
import time
from pathlib import Path
from typing import Any, Callable

import pytest

from ppharness.backend import OnlineBackend
from ppharness.instance import DolphinInstance

from test_online import _wait
from test_online_game import ROOT, Game, _boot, backend  # noqa: F401  (the backend fixture)

sys.path.insert(0, str(ROOT / "harness" / "tools"))
sys.path.insert(0, str(ROOT / "tools" / "gamecode"))
import online_set  # noqa: E402
import ppom  # noqa: E402
from ppharness import brawl as B  # noqa: E402

pytestmark = [pytest.mark.dolphin, pytest.mark.server, pytest.mark.gpu]

ART = ROOT / "run" / "artifacts" / "game-code" / "rooms"
CSS = "scSelctCharacter"

# The keypad's letter keys in room-code mode (code_entry.cpp ROOM_KEYS), by (row, column).
ROOM_KEYS = {(1, 1): "BC", (1, 2): "DF", (2, 0): "GH", (2, 1): "JKL", (2, 2): "MN", (3, 0): "PQRS",
             (3, 1): "TV", (3, 2): "WXZ"}


def _all(*fns: Callable[[], Any]) -> list[Any]:
    with concurrent.futures.ThreadPoolExecutor(len(fns)) as ex:
        return [f.result() for f in [ex.submit(fn) for fn in fns]]


def room_view(g: Game) -> dict[str, Any]:
    return ppom.read_room(g.c, ppom.find_block(g.c))


def until_view(g: Game, pred: Callable[[dict[str, Any]], Any], what: str, timeout: float = 30) -> dict[str, Any]:
    try:
        return _wait(lambda: (lambda v: v if pred(v) else None)(room_view(g)), timeout, what, interval=0.2)
    except AssertionError:
        raise AssertionError(f"{g.name}: {what}: {room_view(g)} rooms {g.c.call('rooms_status')}")


def lock_of(g: Game) -> dict[str, Any]:
    return ppom.read_local(g.c, ppom.find_block(g.c))["lock"]


def to_with_friends(g: Game) -> None:
    g.steps("tap A", "wait 90")
    assert (g.debug_scratch()[6] >> 16) == 3, "the WITH FRIENDS labels were not drawn"


def create_room(g: Game) -> str:
    """WITH FRIENDS > Create Room (the first button, left of Direct 1v1)."""
    to_with_friends(g)
    g.steps("tap DLEFT 4", "wait 30")
    g.shot("01-with-friends-create-room")
    g.steps("tap A", "wait 400", f"until {CSS} 600")
    v = until_view(g, lambda v: v["flags"] & ppom.RF_IN, "the new room")
    assert v["host"] == v["local_port"] == 0 and v["flags"] & ppom.RF_PUBLIC
    g.steps("wait 60")
    g.shot("02-room-created")
    return v["code"]


def to_join_room(g: Game) -> None:
    """WITH FRIENDS > Join Room (the bottom button)."""
    to_with_friends(g)
    g.steps("tap DDOWN 4", "wait 30")
    g.shot("01-with-friends-join-room")
    g.steps("tap A", "wait 400", f"until {CSS} 600")


def pick(g: Game, right: int = 0) -> None:
    """A character: the hand from the name plate up to the grid's second row (Fox), then `right`
    portraits to the right."""
    g.steps("stick up 30", "wait 5")
    if right:
        g.steps(f"stick right {6 * right}", "wait 5")
    g.steps("tap A 8", "wait 30")


def type_room_code(g: Game, code: str) -> None:
    """START opens the keypad in room-code mode; the code from its letter keys; START joins."""
    for _ in range(6):
        g.steps("tap START 8", "wait 40")
        s = g.debug_scratch()
        if s[4] == 1 and s[5] == 8:
            break
        g.steps("wait 40")
    else:
        raise AssertionError(f"the keypad did not open ({g.debug_scratch()})")
    steps = []
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
    g.shot("03-room-code-typed")
    g.steps("tap START 8", "wait 30")


def test_room_two_players_through_the_game(backend: OnlineBackend, dolphin: Callable[..., DolphinInstance],
                                           gpu_backend: str) -> None:
    test = "two-players"
    out = ART / test
    ua = backend.create_user("rhost", "RHST")
    ub = backend.create_user("rjoin", "RJIN")
    a = _boot(dolphin, "room-a", backend, ua, gpu_backend, test, "gameplay", artifacts=ART)
    b = _boot(dolphin, "room-b", backend, ub, gpu_backend, test, "gameplay", artifacts=ART)
    _all(a.to_main_menu, b.to_main_menu)
    _all(a.to_online_page, b.to_online_page)

    # A creates the room; B joins it by its code.
    code = create_room(a)
    assert len(code) == 4 and set(code) <= set("BCDFGHJKLMNPQRSTVWXZ"), code
    to_join_room(b)
    pick(a)
    pick(b, right=2)
    for g in (a, b):   # short games (both must have the same: part of the setup)
        B.write_rules(g.c, stocks=1, minutes=2, items_off=True)
        ppom.allow_test_rules(g.c)
    b.shot("02-join-room-css")
    print("lock before the code", lock_of(b))
    type_room_code(b, code)
    print("lock after the code", lock_of(b))
    vb = until_view(b, lambda v: v["flags"] & ppom.RF_IN, "B in the room")
    print("lock in the room", lock_of(b))
    assert vb["code"] == code and vb["local_port"] == 1 and vb["host"] == 0
    until_view(a, lambda v: v["slots"][1]["bits"] & ppom.SLOT_TAKEN, "A sees B")
    _all(lambda: a.steps("wait 60"), lambda: b.steps("wait 60"))
    a.shot("03-b-joined")
    b.shot("04-joined")
    assert b.debug_scratch()[4] != 1, "the keypad is still open"
    assert not lock_of(b)["ready"], f"B is locked in before START: {lock_of(b)}"

    # Ready: START on both; the panels show the other's character; the room starts by itself.
    game = room_view(a)["game"]
    a.steps("tap START 8", "wait 40")
    until_view(b, lambda v: v["slots"][0]["bits"] & ppom.SLOT_READY, "B sees A ready")
    b.steps("wait 30")
    b.shot("05-host-ready")
    a.shot("05-ready")
    b.steps("tap START 8", "wait 10")
    rep = online_set.play_game([a, b], game)
    assert rep["checksums"]["mismatches"] == 0, rep

    # Back on the room's CSS: still in the room, ready cleared, the next game.
    _wait(lambda: a.scene() == CSS and b.scene() == CSS, 180, "both back on the room's CSS")
    for g in (a, b):
        v = until_view(g, lambda v: v["flags"] & ppom.RF_IN and v["status"] == 0 and v["game"] == game + 1,
                       "the room waiting for its next game", timeout=60)
        assert all(not (s["bits"] & ppom.SLOT_READY) for s in v["slots"][:2]), v
        g.steps("wait 60")
        g.shot("06-back-in-the-room")
    # The loser picks the stage (rooms.md #14): their line says so.
    v = room_view(a)
    pickers = [i for i, s in enumerate(v["slots"]) if s["picks_stage"]]
    assert len(pickers) == 1, v

    # B holds Z: leaves the room and stays on the CSS; A sees the slot empty again.
    b.steps("hold Z 70", "wait 30")
    until_view(b, lambda v: not v["flags"] & ppom.RF_IN, "B out of the room")
    until_view(a, lambda v: v["slots"][1]["bits"] == ppom.SLOT_OPEN, "A sees B gone")
    a.steps("wait 40")
    a.shot("07-b-left")
    b.shot("07-left")
    # A holds B: back to WITH FRIENDS (the room left).
    a.steps("hold B 120", "until muMenuMain 600", "wait 90")
    a.shot("08-back-on-with-friends")
    _wait(lambda: not room_view(a)["flags"] & ppom.RF_IN, 20, "A out of the room")
