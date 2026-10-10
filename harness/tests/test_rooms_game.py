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
import contextlib
import sys
import time
from pathlib import Path
from typing import Any, Callable

import pytest

from ppharness.backend import OnlineBackend
from ppharness.instance import DolphinInstance

from test_online import _wait
from test_online_game import ROOT, WIFI_INTERFACE_TASK, Game, _boot, backend  # noqa: F401  (the backend fixture)

sys.path.insert(0, str(ROOT / "harness" / "tools"))
sys.path.insert(0, str(ROOT / "tools" / "gamecode"))
import online_set  # noqa: E402
import ppom  # noqa: E402
from ppharness import brawl as B  # noqa: E402

pytestmark = [pytest.mark.dolphin, pytest.mark.server, pytest.mark.gpu]

ART = ROOT / "run" / "artifacts" / "game-code" / "rooms"
CSS = "scSelctCharacter"

# The keypad's letter keys (every letter they show is taken in room-code mode), by (row, column).
ROOM_KEYS = {(1, 1): "ABC", (1, 2): "DEF", (2, 0): "GHI", (2, 1): "JKL", (2, 2): "MNO", (3, 0): "PQRS",
             (3, 1): "TUV", (3, 2): "WXYZ"}


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

    # The room's second game (staging, 2026-10-10: the loser picked a stage and was sent back to
    # the stage select over and over): the winner locks in, the loser's START opens the stage
    # select (in game 2 first left with B: unlocked, START opens it again), the pick brings them
    # back locked in with it, and that stage is played.
    loser = a if pickers[0] == 0 else b
    winner = b if loser is a else a
    winner.steps("tap START 8", "wait 30")
    _wait(lambda: lock_of(winner)["ready"] and lock_of(winner)["game"] == game + 1, 30, "the winner locked in")
    loser.steps("tap START 8", "wait 30")
    _wait(lambda: loser.scene() == "scSelStage", 60, "the loser on the stage select")
    loser.steps("wait 40")
    loser.shot("06a-stage-select")
    # The room has started (everyone ready): it can no longer be unreadied, so B does not leave
    # the stage select (it once left it unlocked, and the room never took START again).
    loser.steps("tap B 8", "wait 60")
    assert loser.scene() == "scSelStage", ("B left the room's stage select", loser.scene())
    stage = online_set.STAGE_PICK
    B.sss_pick_stage(loser.c, B.STAGE_KIND[stage], 0)
    # Back on the CSS locked in with the pick, and the match starts: not the stage select again.
    seen = []

    def started() -> bool:
        seen.append(loser.scene())
        return online_set.gstatus(loser.c)["phase"] in ("running", "error", "ended")
    try:
        _wait(started, 90, "game 2 to start after the pick")
    except AssertionError:
        raise AssertionError(f"no game 2 after the pick; the loser's scenes: {sorted(set(seen))}, "
                             f"lock {lock_of(loser)}, scratch {loser.debug_scratch()}")
    after = seen[seen.index(CSS):] if CSS in seen else []
    assert "scSelStage" not in after, ("the stage select opened again after the pick", seen)
    rep = online_set.play_game([a, b], game + 1)
    assert rep["checksums"]["mismatches"] == 0, rep
    assert rep["stage"] == B.STAGE_KIND[stage], ("the loser's pick was not played", rep)
    _wait(lambda: a.scene() == CSS and b.scene() == CSS, 180, "both back on the room's CSS after game 2")
    for g in (a, b):
        until_view(g, lambda v: v["flags"] & ppom.RF_IN and v["status"] == 0 and v["game"] == game + 2,
                   "the room waiting for its third game", timeout=60)
        g.steps("wait 60")
        g.shot("06b-back-after-game-2")

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


def test_room_names_follow_the_slots_after_the_host_left(backend: OnlineBackend,
                                                         dolphin: Callable[..., DolphinInstance],
                                                         gpu_backend: str) -> None:
    """Staging, 2026-10-10: the host left their own room, the other player became host, the old
    host joined again (slot 1), and in the next game each had the other's name under their
    damage. A room's game is played on ports = slots with the room host as the decider, and
    GameBridge put a 2-player match's names decider first (Direct's order, where the decider is
    P1). Here: SESSION's name for each port is that slot's player, on both machines, in the
    match."""
    test = "names-after-host-left"
    ua = backend.create_user("rnamea", "RNMA")
    ub = backend.create_user("rnameb", "RNMB")
    a = _boot(dolphin, "room-names-a", backend, ua, gpu_backend, test, "gameplay", artifacts=ART)
    b = _boot(dolphin, "room-names-b", backend, ub, gpu_backend, test, "gameplay", artifacts=ART)
    _all(a.to_main_menu, b.to_main_menu)
    _all(a.to_online_page, b.to_online_page)
    code = create_room(a)
    to_join_room(b)
    pick(a)
    pick(b, right=2)
    for g in (a, b):
        B.write_rules(g.c, stocks=1, minutes=2, items_off=True)
        ppom.allow_test_rules(g.c)
    type_room_code(b, code)
    until_view(b, lambda v: v["flags"] & ppom.RF_IN and v["local_port"] == 1, "B in slot 2")

    # A (the host) holds Z: out of the room, on the CSS; B is the host now.
    a.steps("hold Z 70", "wait 30")
    until_view(a, lambda v: not v["flags"] & ppom.RF_IN, "A out of the room")
    until_view(b, lambda v: v["host"] == 1 and not v["slots"][0]["bits"] & ppom.SLOT_TAKEN, "B the host")
    # A joins again by the code: slot 1, B still the host.
    type_room_code(a, code)
    va = until_view(a, lambda v: v["flags"] & ppom.RF_IN, "A in the room again")
    assert va["local_port"] == 0 and va["host"] == 1, va
    _all(lambda: a.steps("wait 60"), lambda: b.steps("wait 60"))
    a.shot("03-back-in-slot-1")

    game = room_view(a)["game"]
    a.steps("tap START 8", "wait 40")
    until_view(b, lambda v: v["slots"][0]["bits"] & ppom.SLOT_READY, "B sees A ready")
    b.steps("tap START 8", "wait 10")
    _wait(lambda: all(online_set.gstatus(g.c)["phase"] in ("running", "error", "ended") for g in (a, b)), 240,
          "both sessions running")
    st = [online_set.gstatus(g.c) for g in (a, b)]
    assert [s["local_slot"] for s in st] == [0, 1], st
    want = [ua.display_name, ub.display_name]
    for g in (a, b):
        names = [p["name"] for p in ppom.read_session(g.c, ppom.find_block(g.c))["players"][:2]]
        assert names == want, (g.name, "names by port", names, want)
    rep = online_set.play_game([a, b], game)
    assert rep["checksums"]["mismatches"] == 0, rep


def test_room_two_players_with_a_gap(backend: OnlineBackend, dolphin: Callable[..., DolphinInstance],
                                     gpu_backend: str) -> None:
    """Staging, 2026-10-10: the host closed slot 2 and opened slot 3, a player joined (slot 3),
    and the room's game ended at once on both ("the match's players differ from the session's:
    fewer than two players"): GameBridge wrote SESSION `numPlayers` as the number of players (2)
    where the game reads the highest port + 1 (3), so the game built its match from P1 and P2
    only. Here: slot 3 joined through the game's screens, both ready, the match runs under the
    gameplay session with P1 and P3 and both reach game set with the same checksums."""
    test = "two-players-gap"
    ua = backend.create_user("rgaph", "RGPH")
    ub = backend.create_user("rgapj", "RGPJ")
    a = _boot(dolphin, "room-gap-a", backend, ua, gpu_backend, test, "gameplay", artifacts=ART)
    b = _boot(dolphin, "room-gap-b", backend, ub, gpu_backend, test, "gameplay", artifacts=ART)
    _all(a.to_main_menu, b.to_main_menu)
    _all(a.to_online_page, b.to_online_page)
    code = create_room(a)
    # The host's slot controls (as the hand's A on a panel sends them): slot 3 open, then slot 2
    # closed (at least 2 slots stay open).
    a.c.call("rooms_request", op="slot", slot=3, open=True)
    until_view(a, lambda v: v["slots"][2]["bits"] == ppom.SLOT_OPEN, "slot 3 open")
    a.c.call("rooms_request", op="slot", slot=2, open=False)
    until_view(a, lambda v: v["slots"][1]["bits"] == 0 and v["slots"][2]["bits"] == ppom.SLOT_OPEN,
               "slot 2 closed, slot 3 open")
    to_join_room(b)
    pick(a)
    pick(b, right=2)
    for g in (a, b):
        B.write_rules(g.c, stocks=1, minutes=1, items_off=True)
        ppom.allow_test_rules(g.c)
    type_room_code(b, code)
    vb = until_view(b, lambda v: v["flags"] & ppom.RF_IN, "B in the room")
    assert vb["local_port"] == 2 and vb["host"] == 0, vb
    until_view(a, lambda v: v["slots"][2]["bits"] & ppom.SLOT_TAKEN, "A sees B in slot 3")
    _all(lambda: a.steps("wait 60"), lambda: b.steps("wait 60"))
    a.shot("03-b-in-slot-3")
    b.shot("03-in-slot-3")
    game = room_view(a)["game"]
    a.steps("tap START 8", "wait 40")
    until_view(b, lambda v: v["slots"][0]["bits"] & ppom.SLOT_READY, "B sees A ready")
    b.steps("tap START 8", "wait 10")
    _wait(lambda: all(online_set.gstatus(g.c)["phase"] in ("running", "error", "ended") for g in (a, b)), 240,
          "both sessions running")
    st = [online_set.gstatus(g.c) for g in (a, b)]
    assert all(s["phase"] == "running" for s in st), [(s["phase"], s.get("error")) for s in st]
    assert [s["match_ports"] for s in st] == [0b101, 0b101], st
    for g in (a, b):
        assert ppom.read_session(g.c, ppom.find_block(g.c))["num_players"] == 3
        g.shot("04-match")
    # B (P3) walks off the stage (1 stock): game set.
    from ppharness import flows as F
    with contextlib.suppress(Exception):
        F.fight([F.Seat(b.c, 2, 0, "J")], 60 * 60 * 2, mode="selfdestruct",
                stop=lambda: online_set.gstatus(b.c)["phase"] != "running")
    _wait(lambda: all(online_set.gstatus(g.c)["phase"] != "running" for g in (a, b)), 240, "the game to end")
    st = [online_set.gstatus(g.c) for g in (a, b)]
    assert [s["end_reason"] for s in st] == ["game set", "game set"], st
    limit = min(s["current_frame"] for s in st) - 16
    cks = [{r[0]: r[1] for r in g.c.call("gprb_checksums", since=0)["rows"] if r[0] <= limit} for g in (a, b)]
    common = set(cks[0]) & set(cks[1])
    assert len(common) > 30 and all(cks[0][f] == cks[1][f] for f in common), len(common)
    _wait(lambda: a.scene() == CSS and b.scene() == CSS, 180, "both back on the room's CSS")
    for g in (a, b):
        until_view(g, lambda v: v["flags"] & ppom.RF_IN and v["status"] == 0 and v["game"] == game + 1,
                   "the room waiting for its next game", timeout=60)


# The CSS hand (test_rooms_ui.py): muSelCharHand +0x90 x, +0x94 y; the panels' centres.
HAND_X, HAND_Y = 0x90, 0x94
PANEL_X = [-22.0, -7.0, 8.0, 23.0]


def _u32(g: Game, a: int) -> int:
    return int.from_bytes(g.c.read_mem(a, 4), "big")


def hand(g: Game) -> tuple[float, float]:
    import struct
    task = _u32(g, _u32(g, _u32(g, 0x805A0060) + 4) + 0x400)
    h = _u32(g, _u32(g, task + 0x44) + 0x1A8)
    return struct.unpack(">ff", g.c.read_mem(h + HAND_X, 8))


def hand_to(g: Game, x: float, y: float, tol: float = 1.2) -> None:
    """Closed loop: the stick toward (x, y) until the hand is there."""
    from ppharness.client import PadInput
    for _ in range(80):
        hx, hy = hand(g)
        dx, dy = x - hx, y - hy
        if abs(dx) <= tol and abs(dy) <= tol:
            return

        def axis(d: float) -> int:
            if abs(d) <= tol:
                return 128
            v = max(45, min(127, int(abs(d) * 15)))
            return 128 + (v if d > 0 else -v)
        g.c.pad_script(0, [PadInput(main=(axis(dx), axis(dy)), hold=3), PadInput(hold=2)])
        g.steps("wait 5")
    raise AssertionError(f"{g.name}: the hand did not reach {(x, y)}: {hand(g)}")


def status_line(g: Game) -> str:
    return g.c.call("rooms_status")["text"]


def test_room_three_players_teams(backend: OnlineBackend, dolphin: Callable[..., DolphinInstance],
                                  gpu_backend: str) -> None:
    """Three games: the host opens slot 3 with the hand and A, R turns Teams on, two players join
    by code; everyone on red (X / Y) is refused ("Pick different teams", no start although all
    are ready); one player to blue and the room starts a 2-against-1 team battle under the
    gameplay session on all three; blue walks off, and everyone is back on the room's CSS."""
    test = "three-players"
    users = [backend.create_user(n, n[:4].upper()) for n in ("rkira", "rlars", "rmona")]
    a, b, c = gs = [_boot(dolphin, f"room3-{x}", backend, u, gpu_backend, test, "gameplay", artifacts=ART)
                    for x, u in zip("abc", users)]
    _all(*[g.to_main_menu for g in gs])
    _all(*[g.to_online_page for g in gs])

    code = create_room(a)
    pick(a)
    # Slot 3 (port 2): the hand over the third panel, A. R: Teams on.
    hand_to(a, PANEL_X[2], -10.0)
    a.steps("tap A 8", "wait 30")
    until_view(a, lambda v: v["slots"][2]["bits"] & ppom.SLOT_OPEN, "slot 3 open")
    a.steps("tap R 8", "wait 30")
    until_view(a, lambda v: v["flags"] & ppom.RF_TEAMS, "Teams on")
    hand_to(a, -23.0, 10.5)   # back on Fox
    a.steps("wait 30")
    a.shot("03-slot-3-open-teams-on")

    _all(lambda: to_join_room(b), lambda: to_join_room(c))
    pick(b, right=2)
    pick(c, right=4)
    for g in gs:
        B.write_rules(g.c, stocks=1, minutes=1, items_off=True)
        ppom.allow_test_rules(g.c)
    type_room_code(b, code)
    until_view(b, lambda v: v["flags"] & ppom.RF_IN, "B in the room")
    type_room_code(c, code)
    vc = until_view(c, lambda v: v["flags"] & ppom.RF_IN, "C in the room")
    assert vc["local_port"] == 2 and vc["mode"] == 2, vc
    v = until_view(a, lambda v: [s["team"] for s in v["slots"][:3]] == [0, 1, 2] and
                   all(s["bits"] & ppom.SLOT_TAKEN for s in v["slots"][:3]), "three players, red blue green")
    for g in gs:
        assert not lock_of(g)["ready"], (g.name, lock_of(g))
        g.steps("wait 40")
        g.shot("04-three-in-the-room")

    # Everyone on red: B Y (blue -> red), C X (green -> red).
    b.steps("tap Y 8", "wait 20")
    c.steps("tap X 8", "wait 20")
    until_view(a, lambda v: [s["team"] for s in v["slots"][:3]] == [0, 0, 0], "everyone red")
    _wait(lambda: all(status_line(g) == "Pick different teams" for g in gs), 20, "Pick different teams")
    game = room_view(a)["game"]
    for g in gs:
        g.steps("tap START 8", "wait 10")
    until_view(a, lambda v: all(s["bits"] & ppom.SLOT_READY for s in v["slots"][:3]), "all ready")
    for g in gs:
        g.steps("wait 40")
        g.shot("05-all-red-refused")
    time.sleep(3)
    v = room_view(a)
    assert v["status"] == 0 and status_line(a) == "Pick different teams", (v, status_line(a))

    # C: B takes the lock back, X to blue (red -> blue), START: 2 against 1, the room starts.
    c.steps("tap B 8", "wait 20")
    until_view(a, lambda v: not v["slots"][2]["bits"] & ppom.SLOT_READY, "C not ready")
    c.steps("tap X 8", "wait 20")
    until_view(a, lambda v: v["slots"][2]["team"] == 1, "C blue")
    c.steps("wait 20")
    c.shot("06-blue")
    c.steps("tap START 8", "wait 10")
    online_set.wait(lambda: all(online_set.gstatus(g.c)["phase"] in ("running", "error", "ended") for g in gs),
                    240, "the room's game running on all three")
    st = [online_set.gstatus(g.c) for g in gs]
    assert all(s["phase"] == "running" for s in st), [(s["phase"], s.get("error")) for s in st]
    assert [s["local_slot"] for s in st] == [0, 1, 2], st
    for g in gs:
        se = ppom.read_session(g.c, ppom.find_block(g.c))
        assert se["teams"] == 1 and se["game"] == game, se
        assert [pl["team"] for pl in se["players"][:3]] == [0, 0, 1], se
    setups = [B.read_match_setup(g.c.read_mem) for g in gs]
    assert setups[0] == setups[1] == setups[2], setups
    time.sleep(8)
    for g in gs:
        g.shot("07-team-battle")

    # Blue (C, port 2) walks off the stage; red wins on stocks. Then everyone is back on the
    # room's CSS.
    from ppharness import flows as F
    with contextlib.suppress(Exception):
        F.fight([F.Seat(c.c, 2, 0, "C")], 60 * 60, mode="selfdestruct", seed="3p",
                stop=lambda: online_set.gstatus(c.c)["phase"] != "running")
    try:
        online_set.wait(lambda: all(online_set.gstatus(g.c)["phase"] != "running" for g in gs), 240,
                        "the game to end")
    except TimeoutError:
        for g in gs:
            g.shot("07-timeout")
        raise AssertionError(f"the game did not end: {[online_set.gstatus(g.c) for g in gs]}")
    st = [online_set.gstatus(g.c) for g in gs]
    limit = min(s["current_frame"] for s in st) - online_set.CONFIRM_MARGIN
    cks = [{r[0]: r[1] for r in g.c.call("gprb_checksums", since=0)["rows"] if r[0] <= limit} for g in gs]
    common = sorted(set(cks[0]) & set(cks[1]) & set(cks[2]))
    bad = [f for f in common if not cks[0][f] == cks[1][f] == cks[2][f]]
    print(f"3 players: frames {[s['current_frame'] for s in st]}, end {[s['end_reason'] for s in st]}, "
          f"rollbacks {[s['rollbacks'] for s in st]}, checksums compared {len(common)}, mismatches {len(bad)}")
    assert common and not bad, (len(common), bad[:5])
    _wait(lambda: all(g.scene() == CSS for g in gs), 180, "all back on the room's CSS")
    for g in gs:
        until_view(g, lambda v: v["flags"] & ppom.RF_IN and v["status"] == 0 and v["game"] == game + 1,
                   "the room waiting for its next game", timeout=60)
        g.steps("wait 60")
        g.shot("08-back-in-the-room")


def write_join(g: Game, code: str) -> str:
    """The launcher's jump-to-room request (Dolphin's Online/join-room.json, as test_rooms.py)."""
    import json
    import os
    import uuid
    from datetime import datetime, timezone
    rid = str(uuid.uuid4())
    created = datetime.now(timezone.utc).isoformat(timespec="milliseconds").replace("+00:00", "Z")
    d = Path(g.inst.user_dir) / "Online"
    tmp = d / "join-room.json.tmp"
    tmp.write_text(json.dumps({"version": 1, "id": rid, "code": code, "createdAt": created}))
    os.replace(tmp, d / "join-room.json")
    return rid


def test_room_launcher_jump(backend: OnlineBackend, dolphin: Callable[..., DolphinInstance],
                            gpu_backend: str) -> None:
    """The launcher's join (Dolphin bumps LOCAL.roomJoin once it has joined): from the ONLINE menu
    page the game goes to the room's CSS by itself; from an idle Direct CSS the CSS is built again
    as the room's."""
    test = "launcher-jump"
    ua = backend.create_user("rlhost", "RLHS")
    ub = backend.create_user("rljump", "RLJM")
    a = _boot(dolphin, "room-la", backend, ua, gpu_backend, test, "gameplay", artifacts=ART)
    b = _boot(dolphin, "room-lb", backend, ub, gpu_backend, test, "gameplay", artifacts=ART)
    _all(a.to_main_menu, b.to_main_menu)
    _all(a.to_online_page, b.to_online_page)
    code = create_room(a)
    pick(a)

    # B idles on the ONLINE page: the join takes it to the room's CSS.
    b.steps("wait 30")
    assert room_view(b)["screen"] == ppom.SCREENS["menus"], room_view(b)
    write_join(b, code)
    _wait(lambda: b.scene() == CSS, 60, "B on the room's CSS")
    vb = until_view(b, lambda v: v["flags"] & ppom.RF_IN and v["screen"] == ppom.SCREENS["room"], "B in the room")
    assert vb["code"] == code and vb["local_port"] == 1, vb
    until_view(a, lambda v: v["slots"][1]["bits"] & ppom.SLOT_TAKEN, "A sees B")
    b.steps("wait 60")
    b.shot("01-jumped-from-the-online-page")

    # B leaves (hold B: back to WITH FRIENDS on Join Room), goes up to Direct 1v1 and its CSS.
    b.steps("hold B 120", "until muMenuMain 600", "wait 90")
    until_view(a, lambda v: v["slots"][1]["bits"] == ppom.SLOT_OPEN, "A sees B gone")
    b.steps("tap DUP 8", "wait 20")
    b.shot("02-with-friends-direct")
    b.steps("tap A", "wait 400", f"until {CSS} 600", "wait 60")
    assert room_view(b)["screen"] == ppom.SCREENS["online-css"], room_view(b)
    pick(b, right=2)
    b.shot("03-direct-css")
    write_join(b, code)
    vb = until_view(b, lambda v: v["flags"] & ppom.RF_IN and v["screen"] == ppom.SCREENS["room"],
                    "B in the room from the Direct CSS", timeout=60)
    _wait(lambda: b.scene() == CSS, 60, "B on the room's CSS")
    b.steps("wait 90")
    b.shot("04-jumped-from-the-direct-css")
    a.steps("wait 10")
    a.shot("04-b-jumped-in")
    assert room_view(a)["slots"][1]["bits"] & ppom.SLOT_TAKEN


@pytest.mark.parametrize("when", ["before-start", "during-boot"])
def test_room_launcher_join_while_the_game_starts(backend: OnlineBackend,
                                                  dolphin: Callable[..., DolphinInstance],
                                                  gpu_backend: str, tmp_path: Path, when: str) -> None:
    """A room clicked in the launcher with the game closed (Play writes join-room.json, then
    starts Dolphin) or while it is still booting: the real plugin boots (nothing stands in for its
    LOCAL.screen), Dolphin answers the request `accepted` at once and holds it until the game first
    shows its menus, then joins; the game lands on the room's CSS without a second click (staging,
    2026-10-10: the boot's scenes were answered "Finish your current game first.")."""
    import json
    from test_rooms import MmClient
    test = f"launcher-join-{when}"
    uh = backend.create_user("rlbhost", "RLBH")
    uj = backend.create_user("rlbjoin", "RLBJ")
    owner = MmClient(backend, uh, tmp_path, "room", "--create", "--hold-secs", "400")
    try:
        code = owner.code()
        rid: list[str] = []

        def put(user_dir: Path) -> None:
            import uuid
            from datetime import datetime, timezone
            rid.append(str(uuid.uuid4()))
            created = datetime.now(timezone.utc).isoformat(timespec="milliseconds").replace("+00:00", "Z")
            (Path(user_dir) / "Online" / "join-room.json").write_text(
                json.dumps({"version": 1, "id": rid[0], "code": code, "createdAt": created}))

        g = _boot(dolphin, f"room-lj-{when}", backend, uj, gpu_backend, test, "gameplay", artifacts=ART,
                  before_launch=put if when == "before-start" else None)
        if when == "during-boot":
            st = g.c.call("rooms_status")
            assert not st["reached_menus"], st    # still booting
            put(g.inst.user_dir)

        def answer() -> dict | None:
            try:
                lr = json.loads((Path(g.inst.user_dir) / "Online" / "game-status.json").read_text())["lastRequest"]
            except (OSError, ValueError, KeyError):
                return None
            return lr if lr and lr.get("id") == rid[0] else None

        # Accepted at once (within the launcher's 5 s), although the game is still booting.
        lr = _wait(answer, 5, "the answer to the request")
        assert lr["result"] == "accepted", lr
        # Straight into the room's CSS (the answer can come before the emulation runs, and
        # read_mem fails until it does).
        g.c.wait_state("running", timeout=120)
        _wait(lambda: g.scene() == CSS, 120, "the room's CSS")
        v = until_view(g, lambda v: v["flags"] & ppom.RF_IN and v["screen"] == ppom.SCREENS["room"],
                       "in the room", timeout=60)
        assert v["code"] == code and v["local_port"] == 1 and v["join"] >= 1, v
        st = g.c.call("rooms_status")
        assert st["reached_menus"] and not st["screen_harness"], st
        # The boot's ONLINE page opens no connect window, so the online pages make the Wi-Fi
        # CSS's muWifiInterfaceTask (netmenu.cpp ensureWifiTask); without it the first CSS after
        # the boot was built white, with no panels (prod, 2026-10-10).
        assert _u32(g, WIFI_INTERFACE_TASK) != 0
        owner.until(lambda m: m.get("type") == "room-state" and m["slots"][1]["player"] and
                    m["slots"][1]["player"]["displayName"] == "rlbjoin", "the joiner seen by the host")
        g.steps("wait 60")
        g.shot("01-in-the-room-after-the-boot")
        log = (Path(g.inst.user_dir) / "Logs" / "dolphin.log").read_text(errors="replace")
        assert "refused" not in log, [l for l in log.splitlines() if "refused" in l]
    finally:
        owner.stop()


SLOT_POS = 16.0 / 0.99   # a panel's model offset per slot (room_css.cpp SLOT_POS)


def panel_offset(g: Game, area: int) -> float:
    import struct
    task = _u32(g, _u32(g, _u32(g, 0x805A0060) + 4) + 0x400)
    a = _u32(g, task + 0x44 + 4 * area)
    return struct.unpack(">f", g.c.read_mem(_u32(g, a + 0xB0) + 0x3C, 4))[0]


# The longest names the accounts server takes (15 characters, common/codes.rs): wide letters and
# kana, for the names under the damage (match_hud.cpp: each fits its panel).
LONG_NAMES = ["WWWWWWWWWWWWWWW", "\u30d6\u30ed\u30a6\u30eb\u30aa\u30f3\u30e9\u30a4\u30f3\u306e\u30c6\u30b9\u30c8\u3067\u3059",
              "MMMMMMMMMMMMMMM", "\u3042\u3044\u3046\u3048\u304a\u304b\u304d\u304f\u3051\u3053\u3055\u3057\u3059\u305b\u305d"]


@pytest.mark.parametrize("ports", [(0, 1, 2, 3), (0, 1, 3)], ids=["four-long-names", "three-with-a-gap"])
def test_room_four_players(backend: OnlineBackend, dolphin: Callable[..., DolphinInstance],
                           gpu_backend: str, ports: tuple) -> None:
    """Four games (or three on P1, P2 and P4): the host opens the other slots with the hand,
    the players join by code, each sees its own panel at its slot (panels in slot order on every
    screen), everyone readies, the room starts a free-for-all under the gameplay session (four:
    P1 and P3 picked the same character and costume, P3 plays the next costume, as P+'s Versus;
    the longest names, wide letters and kana, each fitted under its damage panel), all but P1
    walk off, and all are back on the room's CSS for the next game."""
    four = len(ports) == 4
    test = "four-players" if four else "three-players-gap"
    import secrets
    names = LONG_NAMES if four else ["rga", "rgb", "rgd"]
    users = [backend.create_user(n, f"RF{'ABCD'[k]}{'L' if four else 'G'}", email=f"room{k}-{secrets.token_hex(4)}@example.test")
             for k, n in enumerate(names)]
    gs = [_boot(dolphin, f"room{len(ports)}-{x}", backend, u, gpu_backend, test, "gameplay", artifacts=ART)
          for x, u in zip("abcd", users)]
    a = gs[0]
    _all(*[g.to_main_menu for g in gs])
    _all(*[g.to_online_page for g in gs])

    code = create_room(a)
    pick(a)
    for slot in ports[2:]:
        hand_to(a, PANEL_X[slot], -10.0)
        a.steps("tap A 8", "wait 30")
        until_view(a, lambda v, s=slot: v["slots"][s]["bits"] & ppom.SLOT_OPEN, f"slot {slot + 1} open")
    v = room_view(a)
    assert not v["flags"] & ppom.RF_TEAMS, v   # a room starts as FFA
    hand_to(a, -23.0, 10.5)
    if not four:   # slot 2 (P2) stays open, slot 3 closed by default; P4 opened above
        assert not room_view(a)["slots"][2]["bits"] & ppom.SLOT_OPEN
    a.shot("03-slots-open")

    _all(*[lambda g=g: to_join_room(g) for g in gs[1:]])
    # P3 takes the host's character in the same (first) costume: P+'s Versus never shows two
    # alike, so the match gives P3 the next free costume (online_match.cpp distinctCostumes).
    for k, g in enumerate(gs[1:], 1):
        pick(g, right=0 if (four and k == 2) else 2 * k)
    for g in gs:
        B.write_rules(g.c, stocks=1, minutes=2, items_off=True)
        ppom.allow_test_rules(g.c)
    for k, g in enumerate(gs[1:], 1):
        type_room_code(g, code)
        vk = until_view(g, lambda v: v["flags"] & ppom.RF_IN, f"player {k + 1} in the room")
        assert vk["local_port"] == ports[k], vk
    until_view(a, lambda v: all(v["slots"][p]["bits"] & ppom.SLOT_TAKEN for p in ports), "all players")
    for k, g in enumerate(gs):
        g.steps("wait 60")
        # each sees its own panel at its slot
        assert abs(panel_offset(g, 0) - ports[k] * SLOT_POS) < 0.1, (g.name, panel_offset(g, 0))
        g.shot("04-all-in-the-room")

    game = room_view(a)["game"]
    for g in gs:
        g.steps("tap START 8", "wait 10")
    online_set.wait(lambda: all(online_set.gstatus(g.c)["phase"] in ("running", "error", "ended") for g in gs),
                    240, "the room's game running on all")
    st = [online_set.gstatus(g.c) for g in gs]
    assert all(s["phase"] == "running" for s in st), [(s["phase"], s.get("error")) for s in st]
    assert [s["local_slot"] for s in st] == list(ports), st
    setups = [B.read_match_setup(g.c.read_mem) for g in gs]
    assert all(x == setups[0] for x in setups), setups
    if four:
        pl = setups[0].players
        assert pl[0].character == pl[2].character and pl[0].color != pl[2].color, setups[0]
    assert all(sh["init"][:4] == [0, 0, 0, 0] for sh in [B.read_shades(g.c.read_mem) for g in gs])
    time.sleep(8)
    for g in gs:
        g.shot("05-match")

    # Players 2-4 walk off the stage; player 1 wins.
    from ppharness import flows as F
    import threading
    def off(g: Game, port: int) -> None:
        with contextlib.suppress(Exception):
            F.fight([F.Seat(g.c, port, 0, g.name)], 60 * 60, mode="selfdestruct", seed=f"4p{port}",
                    stop=lambda: online_set.gstatus(g.c)["phase"] != "running")
    th = [threading.Thread(target=off, args=(g, ports[k])) for k, g in enumerate(gs) if k > 0]
    for t in th:
        t.start()
    for t in th:
        t.join(300)
    online_set.wait(lambda: all(online_set.gstatus(g.c)["phase"] != "running" for g in gs), 240, "the game to end")
    st = [online_set.gstatus(g.c) for g in gs]
    limit = min(s["current_frame"] for s in st) - online_set.CONFIRM_MARGIN
    cks = [{r[0]: r[1] for r in g.c.call("gprb_checksums", since=0)["rows"] if r[0] <= limit} for g in gs]
    common = sorted(set(cks[0]).intersection(*cks[1:]))
    bad = [f for f in common if len({ck[f] for ck in cks}) != 1]
    print(f"{len(ports)} players: frames {[s['current_frame'] for s in st]}, end {[s['end_reason'] for s in st]}, "
          f"rollbacks {[s['rollbacks'] for s in st]}, checksums compared {len(common)}, mismatches {len(bad)}")
    assert common and not bad, (len(common), bad[:5])
    _wait(lambda: all(g.scene() == CSS for g in gs), 180, "all back on the room's CSS")
    for g in gs:
        until_view(g, lambda v: v["flags"] & ppom.RF_IN and v["status"] == 0 and v["game"] == game + 1,
                   "the room waiting for its next game", timeout=60)
        g.steps("wait 60")
        g.shot("06-back-in-the-room")


def test_room_three_players_second_game_on_the_losers_pick(backend: OnlineBackend,
                                                           dolphin: Callable[..., DolphinInstance],
                                                           gpu_backend: str) -> None:
    """Prod, 2026-10-10: a three-player free-for-all room, game 1 played (P1 first, P2 second,
    P3 last), and the room's game 2 never started: all three ready, the server matched them, the
    host's session waited for P3's stage pick, and P3 never saw the stage select. Here the same
    order: P3 then P2 walk off in game 1; in game 2 P2 readies, then P1, then the loser P3, whose
    START must open the stage select; the pick is played."""
    test = "three-players-second-game"
    import secrets
    users = [backend.create_user(n, f"R2{'ABC'[k]}P", email=f"room2g{k}-{secrets.token_hex(4)}@example.test")
             for k, n in enumerate(["r2ga", "r2gb", "r2gc"])]
    a, b, c = gs = [_boot(dolphin, f"room2g-{x}", backend, u, gpu_backend, test, "gameplay", artifacts=ART)
                    for x, u in zip("abc", users)]
    _all(*[g.to_main_menu for g in gs])
    _all(*[g.to_online_page for g in gs])

    code = create_room(a)
    pick(a)
    hand_to(a, PANEL_X[2], -10.0)
    a.steps("tap A 8", "wait 30")
    until_view(a, lambda v: v["slots"][2]["bits"] & ppom.SLOT_OPEN, "slot 3 open")
    hand_to(a, -23.0, 10.5)
    _all(lambda: to_join_room(b), lambda: to_join_room(c))
    pick(b, right=2)
    pick(c, right=4)
    for g in gs:
        B.write_rules(g.c, stocks=1, minutes=2, items_off=True)
        ppom.allow_test_rules(g.c)
    for k, g in ((1, b), (2, c)):
        type_room_code(g, code)
        vk = until_view(g, lambda v: v["flags"] & ppom.RF_IN, f"player {k + 1} in the room")
        assert vk["local_port"] == k, vk
    until_view(a, lambda v: all(v["slots"][p]["bits"] & ppom.SLOT_TAKEN for p in range(3)), "all players")
    for g in gs:
        g.steps("wait 60")

    def running(game: int) -> list[dict[str, Any]]:
        online_set.wait(lambda: all(online_set.gstatus(g.c)["phase"] in ("running", "error", "ended") for g in gs),
                        240, f"room game {game} running on all")
        st = [online_set.gstatus(g.c) for g in gs]
        assert all(s["phase"] == "running" for s in st), [(s["phase"], s.get("error")) for s in st]
        return st

    def walk_off(g: Game, port: int) -> None:
        from ppharness import flows as F
        with contextlib.suppress(Exception):
            F.fight([F.Seat(g.c, port, 0, g.name)], 60 * 40, mode="selfdestruct", seed=f"2g{port}",
                    stop=lambda: online_set.gstatus(g.c)["phase"] != "running")

    def lobby_dump() -> str:
        out = []
        for g in gs:
            s = online_set.gstatus(g.c)
            out.append(f"{g.name}: phase {s.get('phase')} lobby {s.get('lobby')} "
                       f"peers {[{k: p.get(k) for k in ('slot', 'name', 'seen', 'left', 'confirmed', 'last_heard_ms', 'lock')} for p in s.get('peers', [])]} "
                       f"local {online_set.local(g.c)} lock {lock_of(g)} scene {g.scene()}")
        return "\n".join(out)

    game = room_view(a)["game"]
    for g in gs:
        g.steps("tap START 8", "wait 10")
    running(game)
    time.sleep(6)
    # P3 walks off, then P2: P1 wins, P3 is last (the next game's stage picker).
    import threading
    th = [threading.Thread(target=walk_off, args=(c, 2))]
    th[0].start()
    time.sleep(20)
    th.append(threading.Thread(target=walk_off, args=(b, 1)))
    th[1].start()
    for t in th:
        t.join(300)
    online_set.wait(lambda: all(online_set.gstatus(g.c)["phase"] != "running" for g in gs), 240, "game 1 to end")
    _wait(lambda: all(g.scene() == CSS for g in gs), 180, "all back on the room's CSS")
    for g in gs:
        until_view(g, lambda v: v["flags"] & ppom.RF_IN and v["status"] == 0 and v["game"] == game + 1,
                   "the room waiting for its next game", timeout=60)
    v = room_view(a)
    pickers = [i for i, s in enumerate(v["slots"]) if s["picks_stage"]]
    print("pickers after game 1", pickers, [room_view(g)["slots"][2]["picks_stage"] for g in gs])
    assert pickers == [2], v
    for g in gs:
        g.steps("wait 60")
        g.shot("06-back-in-the-room")

    # Game 2 in prod's order: P2, then P1, then the loser.
    b.steps("tap START 8", "wait 30")
    _wait(lambda: lock_of(b)["ready"], 30, "P2 locked in")
    a.steps("tap START 8", "wait 30")
    _wait(lambda: lock_of(a)["ready"], 30, "P1 locked in")
    c.steps("tap START 8", "wait 10")
    try:
        _wait(lambda: c.scene() == "scSelStage", 60, "the loser on the stage select")
    except AssertionError:
        for g in gs:
            g.shot("07-no-stage-select")
        raise AssertionError("the loser never reached the stage select\n" + lobby_dump())
    c.steps("wait 40")
    c.shot("07-stage-select")
    stage = online_set.STAGE_PICK
    B.sss_pick_stage(c.c, B.STAGE_KIND[stage], 0)
    try:
        st = running(game + 1)
    except AssertionError as e:
        raise AssertionError(f"{e}\n{lobby_dump()}")
    setups = [B.read_match_setup(g.c.read_mem) for g in gs]
    assert all(x == setups[0] for x in setups), setups
    assert setups[0].stage_kind == B.STAGE_KIND[stage], setups[0]
    for g in gs:
        g.shot("08-game-2")
