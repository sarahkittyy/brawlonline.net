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


def back_to_join_room(g: Game) -> None:
    """Out of the room and onto Join Room's CSS: hold B (ROOM_LEAVE; WITH FRIENDS, on Create
    Room), then Join Room below it. A room has no hold Z to leave (P+ picks AltZ characters with
    Z held; the user's, 2026-10-12)."""
    g.steps("hold B 120", "until muMenuMain 600", "wait 90", "tap DDOWN 4", "wait 30", "tap A", "wait 400",
            f"until {CSS} 600")
    assert menu_state(g) == 0x12, hex(menu_state(g))   # 0x10 | ROOM_ENTRY_JOIN


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


def room_history(backend: OnlineBackend, gs: list[Game], users: list[Any], ports: tuple | list,
                 mode: str) -> dict[str, Any]:
    """The room's game in every player's match history (docs/rooms-protocol.md §3): each game
    reported it once (POST /v1/ranked/report-game, nothing failed or pending), and every player's
    Rooms history has it with the room's mode, each player's slot and the places the reports
    agree on. Returns the history entry (players in slot order)."""
    _wait(lambda: all((lambda r: r["reports_sent"] == 1 and r["pending_jobs"] == 0)(g.c.online_status()["ranked"])
                      for g in gs), 60, "every game's report sent")
    for g in gs:
        r = g.c.online_status()["ranked"]
        assert r["room"] and r["reports_failed"] == 0 and r["last_status"] == "ASSIGNED", (g.name, r)
    pages = [backend._http("GET", "/v1/me/matches?mode=rooms", token=u.session_token) for u in users]
    assert all(len(pg["matches"]) == 1 for pg in pages), pages
    m = pages[0]["matches"][0]
    assert all(pg["matches"][0]["matchId"] == m["matchId"] for pg in pages), pages
    assert m["mode"] == "room" and not m["ranked"] and m["room"]["mode"] == mode, m
    assert [p["uid"] for p in m["players"]] == [u.uid for u in users], m
    assert [p["port"] for p in m["players"]] == [p + 1 for p in ports], m
    assert all(p["place"] is not None for p in m["players"]), m
    assert not backend._http("GET", "/v1/me/matches?mode=unranked", token=users[0].session_token)["matches"]
    print(f"history: {m['room']}, places {[p['place'] for p in m['players']]}, teams {[p['team'] for p in m['players']]}")
    return m


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
    winner.shot("06a-other-picks-the-stage")   # the line: "P<n> choosing stage..."
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

    # B holds Z: stays in the room (P+'s AltZ pick); holds B: leaves it, A sees the slot empty again.
    b.steps("hold Z 90", "wait 30")
    assert room_view(b)["flags"] & ppom.RF_IN, "hold Z left the room"
    b.steps("hold B 120", "until muMenuMain 600", "wait 30")
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

    # A (the host) leaves: on Join Room's CSS; B is the host now.
    back_to_join_room(a)
    until_view(a, lambda v: not v["flags"] & ppom.RF_IN, "A out of the room")
    until_view(b, lambda v: v["host"] == 1 and not v["slots"][0]["bits"] & ppom.SLOT_TAKEN, "B the host")
    pick(a)
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


def css_area(g: Game) -> int:
    """The local player's area (muSelCharPlayerArea): +0x1A8 the hand, +0x1BC the costume."""
    task = _u32(g, _u32(g, _u32(g, 0x805A0060) + 4) + 0x400)
    return _u32(g, task + 0x44)


def hand(g: Game) -> tuple[float, float]:
    import struct
    h = _u32(g, css_area(g) + 0x1A8)
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
    """Three games: the host opens slot 3 with the hand and A, R turns Teams on, picks a costume
    with A on its own panel (its team stays), two players join by code; everyone on red (X / Y) is refused ("Pick different teams", no start although all
    are ready); one player to blue and the room starts a 2-against-1 team battle under the
    gameplay session on all three, everyone in the costume they locked in (not the team's); blue
    walks off, and everyone is back on the room's CSS."""
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
    # Any costume on any team (the user's, 2026-10-12): A on our own panel, twice, the costume
    # after next; the team stays (Brawl's team battle CSS cycles the team there).
    costume0 = _u32(a, css_area(a) + 0x1BC)
    hand_to(a, PANEL_X[0], -10.0)
    a.steps("tap A 8", "wait 20", "tap A 8", "wait 20")
    my_costume = _u32(a, css_area(a) + 0x1BC)
    assert my_costume not in (costume0, 0), (costume0, my_costume)
    assert room_view(a)["slots"][0]["team"] == 0, room_view(a)
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
    # Every player in the costume they locked in, not their team's.
    locked = [pl["costume"] for pl in se["players"][:3]]
    assert locked[0] == my_costume, (locked, my_costume)
    assert [p.color_no for p in setups[0].players[:3]] == locked, (setups[0].players, locked)
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
    # The history: red (A, B) first together, blue second.
    m = room_history(backend, gs, users, (0, 1, 2), "teams")
    assert [(p["team"], p["place"], p["wins"]) for p in m["players"]] == [(0, 1, 1), (0, 1, 1), (1, 2, 0)], m


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


def write_direct(g: Game, connect_code: str) -> str:
    """The launcher's Direct invite (join-room.json kind "direct", docs/rooms-protocol.md §5)."""
    import json
    import os
    import uuid
    from datetime import datetime, timezone
    rid = str(uuid.uuid4())
    created = datetime.now(timezone.utc).isoformat(timespec="milliseconds").replace("+00:00", "Z")
    d = Path(g.inst.user_dir) / "Online"
    tmp = d / "join-room.json.tmp"
    tmp.write_text(json.dumps({"version": 1, "id": rid, "kind": "direct", "connectCode": connect_code,
                               "createdAt": created}))
    os.replace(tmp, d / "join-room.json")
    return rid


def launcher_answer(g: Game, rid: str, timeout: float = 5) -> dict[str, Any]:
    import json

    def got() -> dict[str, Any] | None:
        try:
            lr = json.loads((Path(g.inst.user_dir) / "Online" / "game-status.json").read_text())["lastRequest"]
        except (OSError, ValueError, KeyError):
            return None
        return lr if lr and lr.get("id") == rid else None
    return _wait(got, timeout, f"{g.name}: the answer to {rid}", interval=0.1)


def menu_state(g: Game) -> int:
    """The plugin's DEBUG menuState: PPOM mode + 1 on an online CSS (3 = Direct), 0x10 | entry on
    a room's."""
    block = int(g.bridge()["block"])
    debug_off = int.from_bytes(g.c.read_mem(block + 0x14, 2), "big")
    return _u32(g, block + debug_off + 0x14)


def test_direct_invite_from_the_launcher(backend: OnlineBackend, dolphin: Callable[..., DolphinInstance],
                                         gpu_backend: str) -> None:
    """The launcher's Direct invite, accepted: both launchers hand their game the other player's
    connect code. A idles on the ONLINE menu page, B is in its own room's CSS (Dolphin leaves the
    room; the CSS is built again as Direct's). Both games land on the Direct CSS with the code
    filled in; once a character is picked, START searches that code without the keypad, and mm
    pairs them as a normal Direct match."""
    from test_online_game import _find_logged, _peer_shown
    test = "direct-invite"
    ua = backend.create_user("dimenu", "DIMN")
    ub = backend.create_user("diroom", "DIRM")
    a = _boot(dolphin, "dinv-a", backend, ua, gpu_backend, test, "record", artifacts=ART)
    b = _boot(dolphin, "dinv-b", backend, ub, gpu_backend, test, "record", artifacts=ART)
    _all(a.to_main_menu, b.to_main_menu)
    _all(a.to_online_page, b.to_online_page)
    create_room(b)
    pick(b)
    a.steps("wait 30")
    assert room_view(a)["screen"] == ppom.SCREENS["menus"], room_view(a)
    assert room_view(b)["screen"] == ppom.SCREENS["room"], room_view(b)
    ja, jb = room_view(a)["direct_join"], room_view(b)["direct_join"]

    ra, rb = write_direct(a, ub.connect_code), write_direct(b, ua.connect_code)
    assert launcher_answer(a, ra)["result"] == "accepted"
    assert launcher_answer(b, rb)["result"] == "accepted"
    for g, other, j in ((a, ub, ja), (b, ua, jb)):
        v = until_view(g, lambda v: v["direct_join"] == (j + 1) & 0xFF and v["flags"] == 0 and
                       v["screen"] == ppom.SCREENS["online-css"], "on the Direct CSS", timeout=60)
        assert v["direct_code"] == other.connect_code, v
        _wait(lambda g=g: g.scene() == CSS, 60, f"{g.name} on the CSS")
        _wait(lambda g=g: menu_state(g) == 3, 10, f"{g.name}'s CSS is Direct's")
        assert g.debug_scratch()[9] >> 24 == 1, hex(g.debug_scratch()[9])
        g.steps("wait 90")
        g.shot("01-direct-css-invited")
    assert "left the room for the Direct invite" in " ".join(b.c.call("rooms_status")["log"])

    # A picks a character (B kept the one from the room). START searches the code: no keypad.
    pick(a)
    if b.css()["char"] == 0x28:
        pick(b)
    for g in (a, b):
        g.shot("02-character-picked")
        n = g.finds()
        g.press_until("START", lambda g=g: g.finds() > n, f"{g.name}'s FIND_OPPONENT")
        time.sleep(2.2)   # mm takes one ticket per account per 2 s (both search once anyway)
    _find_logged(a, 2, ub.connect_code)
    _find_logged(b, 2, ua.connect_code)
    for g in (a, b):
        assert g.debug_scratch()[4] == 0, f"{g.name}: the keypad opened ({g.debug_scratch()})"

    ra_, rb_ = _peer_shown(a, ub), _peer_shown(b, ua)
    assert ra_["peer_code"] == ub.connect_code and rb_["peer_code"] == ua.connect_code
    sa, sb = a.c.mm_status(), b.c.mm_status()
    assert sa["state"] == sb["state"] == "connection_success", (sa, sb)
    assert sa["match"]["match_id"] == sb["match"]["match_id"]
    assert sa["mode"] == sb["mode"] == "direct"
    assert sa["opponent_code"] == ub.connect_code and sb["opponent_code"] == ua.connect_code
    for g in (a, b):
        g.steps("wait 30")
        g.shot("03-connected")
        g.c.mm_cancel()


def test_direct_invite_before_the_game_starts(backend: OnlineBackend, dolphin: Callable[..., DolphinInstance],
                                              gpu_backend: str) -> None:
    """A Direct invite accepted with the game closed (the launcher writes join-room.json, then
    starts Dolphin): answered `accepted` at once, held through the boot, and the game goes from
    the boot's ONLINE page to the Direct CSS with the code filled in."""
    import json
    import uuid
    from datetime import datetime, timezone
    ua, ub = backend.create_user("diboot", "DIBT"), backend.create_user("diother", "DIOT")
    rid = str(uuid.uuid4())

    def put(user_dir: Path) -> None:
        created = datetime.now(timezone.utc).isoformat(timespec="milliseconds").replace("+00:00", "Z")
        (Path(user_dir) / "Online" / "join-room.json").write_text(json.dumps(
            {"version": 1, "id": rid, "kind": "direct", "connectCode": ub.connect_code, "createdAt": created}))

    g = _boot(dolphin, "dinv-boot", backend, ua, gpu_backend, "direct-invite-boot", "record", artifacts=ART,
              before_launch=put)
    assert launcher_answer(g, rid)["result"] == "accepted"
    g.c.wait_state("running", timeout=120)
    _wait(lambda: g.scene() == CSS, 120, "the Direct CSS")
    v = until_view(g, lambda v: v["screen"] == ppom.SCREENS["online-css"] and v["direct_join"] >= 1,
                   "on the Direct CSS", timeout=60)
    assert v["direct_code"] == ub.connect_code, v
    _wait(lambda: menu_state(g) == 3, 10, "the CSS is Direct's")
    assert g.debug_scratch()[9] >> 24 == 1, hex(g.debug_scratch()[9])
    g.steps("wait 60")
    g.shot("01-direct-css-after-the-boot")


def write_queue(g: Game, queue: str) -> str:
    """The launcher's Play unranked / Play ranked (join-room.json kind "queue", docs/rooms-protocol.md §5)."""
    import json
    import os
    import uuid
    from datetime import datetime, timezone
    rid = str(uuid.uuid4())
    created = datetime.now(timezone.utc).isoformat(timespec="milliseconds").replace("+00:00", "Z")
    d = Path(g.inst.user_dir) / "Online"
    tmp = d / "join-room.json.tmp"
    tmp.write_text(json.dumps({"version": 1, "id": rid, "kind": "queue", "queue": queue, "createdAt": created}))
    os.replace(tmp, d / "join-room.json")
    return rid


def test_play_buttons_from_the_launcher(backend: OnlineBackend, dolphin: Callable[..., DolphinInstance],
                                        gpu_backend: str) -> None:
    """Home's Play ranked / Play unranked: from a room's CSS Dolphin leaves the room and the game
    goes to the Ranked CSS; from that idle CSS Play unranked builds it again as Unranked's; from the
    ONLINE menu page Play ranked goes to the Ranked CSS. Nothing is searched until START, which then
    searches that queue, and `GET /v1/rooms` counts the player in it (the launcher's "X in queue")."""
    from test_online_game import _find_logged
    ua = backend.create_user("playbtn", "PLBT")
    g = _boot(dolphin, "play-a", backend, ua, gpu_backend, "play-buttons", "record", artifacts=ART)
    g.to_main_menu()
    g.to_online_page()
    create_room(g)

    def goes_to(queue: str, mode_state: int, shot: str) -> None:
        j = room_view(g)["direct_join"]
        rid = write_queue(g, queue)
        assert launcher_answer(g, rid)["result"] == "accepted"
        v = until_view(g, lambda v: v["direct_join"] == (j + 1) & 0xFF and v["flags"] == 0 and
                       v["screen"] == ppom.SCREENS["online-css"], f"on the {queue} CSS", timeout=60)
        assert v["direct_code"] == "" and v["direct_queue"] == (2 if queue == "ranked" else 1), v
        _wait(lambda: g.scene() == CSS, 60, "on the CSS")
        _wait(lambda: menu_state(g) == mode_state, 10, f"the CSS is {queue}'s")
        g.steps("wait 60")
        g.shot(shot)

    # MODE_RANKED + 1, MODE_UNRANKED + 1 (ppom.h Mode). From the room's CSS: out of the room.
    goes_to("ranked", 1, "01-ranked-css-from-a-room")
    assert "left the room for the launcher's Play" in " ".join(g.c.call("rooms_status")["log"])
    goes_to("unranked", 2, "02-unranked-css-from-ranked")
    assert g.debug_scratch()[9] >> 24 == 2, hex(g.debug_scratch()[9])
    assert g.finds() == 0, "a Play only opens the CSS"

    # From the ONLINE menu page (Unranked's CSS goes back there).
    g.steps("hold B 120", "until muMenuMain 600", "wait 90")
    assert room_view(g)["screen"] == ppom.SCREENS["menus"], room_view(g)
    goes_to("ranked", 1, "03-ranked-css-from-the-menus")
    assert g.debug_scratch()[9] >> 24 == 3, hex(g.debug_scratch()[9])

    # START searches Ranked (FIND_OPPONENT mode 0, no code), and the server counts it.
    pick(g)
    n = g.finds()
    g.press_until("START", lambda: g.finds() > n, "FIND_OPPONENT")
    _find_logged(g, 0, "")
    rooms = _wait(lambda: (lambda r: r if r["queues"]["ranked"] == 1 else None)(
        backend._http("GET", "/v1/rooms", token=ua.session_token)), 15, "/v1/rooms counts the ranked ticket")
    assert rooms["queues"]["unranked"] == 0, rooms
    g.steps("wait 30")
    g.shot("04-searching-ranked")
    g.c.mm_cancel()


def write_create(g: Game, ruleset: str | None = None) -> str:
    """The launcher's Create room (join-room.json kind "create", docs/rooms-protocol.md §5), with
    the launcher's ruleset box (`ruleset`, a code) when given."""
    import json
    import os
    import uuid
    from datetime import datetime, timezone
    rid = str(uuid.uuid4())
    created = datetime.now(timezone.utc).isoformat(timespec="milliseconds").replace("+00:00", "Z")
    d = Path(g.inst.user_dir) / "Online"
    tmp = d / "join-room.json.tmp"
    req = {"version": 1, "id": rid, "kind": "create", "createdAt": created}
    if ruleset:
        req["ruleset"] = ruleset
    tmp.write_text(json.dumps(req))
    os.replace(tmp, d / "join-room.json")
    return rid


def test_room_launcher_create(backend: OnlineBackend, dolphin: Callable[..., DolphinInstance],
                              gpu_backend: str) -> None:
    """The launcher's Create room: from the ONLINE menu page the game lands on its new room's CSS
    as Create Room leaves it (host, public, slots 1-2 open; one room-create, Dolphin's); from an
    idle Direct CSS the CSS is built again as the new room's; another player joins it by code."""
    test = "launcher-create"
    ua = backend.create_user("rchost", "RCHS")
    ub = backend.create_user("rcjoin", "RCJN")
    a = _boot(dolphin, "room-ca", backend, ua, gpu_backend, test, "gameplay", artifacts=ART)
    b = _boot(dolphin, "room-cb", backend, ub, gpu_backend, test, "gameplay", artifacts=ART)
    _all(a.to_main_menu, b.to_main_menu)
    _all(a.to_online_page, b.to_online_page)
    a.steps("wait 30")
    assert room_view(a)["screen"] == ppom.SCREENS["menus"], room_view(a)
    c0 = room_view(a)["create"]

    rid = write_create(a)
    assert launcher_answer(a, rid)["result"] == "accepted"
    _wait(lambda: a.scene() == CSS, 60, "A on the room's CSS")
    v = until_view(a, lambda v: v["flags"] & ppom.RF_IN and v["screen"] == ppom.SCREENS["room"], "A in its room",
                   timeout=60)
    assert v["host"] == v["local_port"] == 0 and v["flags"] & ppom.RF_PUBLIC and v["create"] == (c0 + 1) & 0xFF, v
    assert v["slots"][1]["bits"] == ppom.SLOT_OPEN and v["slots"][2]["bits"] == 0, v
    assert menu_state(a) == 0x10 | 1, hex(menu_state(a))   # the room CSS entered as Create Room's
    assert a.debug_scratch()[9] >> 8 & 0xFF == 1, hex(a.debug_scratch()[9])
    code = v["code"]
    a.steps("wait 120")   # the game must not send a create of its own
    assert room_view(a)["code"] == code
    log = " ".join(a.c.call("rooms_status")["log"])
    assert log.count("sent create") == 1 and "game asks to create" not in log, log
    a.shot("01-created-from-the-online-page")

    # B joins it with the code (the keypad, as Join Room).
    to_join_room(b)
    pick(b)
    type_room_code(b, code)
    until_view(a, lambda v: v["slots"][1]["bits"] & ppom.SLOT_TAKEN, "A sees B")

    # A leaves (hold B: back to WITH FRIENDS on Create Room), up to Direct 1v1 and its CSS; a
    # second launcher create builds that CSS again as the new room's.
    a.steps("hold B 120", "until muMenuMain 600", "wait 90")
    a.shot("02-back-on-create-room")
    a.steps("tap DRIGHT 8", "wait 20", "tap A", "wait 400", f"until {CSS} 600", "wait 60")
    assert room_view(a)["screen"] == ppom.SCREENS["online-css"] and menu_state(a) == 3, room_view(a)
    pick(a, right=2)
    rid = write_create(a)
    assert launcher_answer(a, rid)["result"] == "accepted"
    v = until_view(a, lambda v: v["flags"] & ppom.RF_IN and v["code"] != code and
                   v["screen"] == ppom.SCREENS["room"], "A in a new room from the Direct CSS", timeout=60)
    assert v["host"] == 0 and v["create"] == (c0 + 2) & 0xFF, v
    _wait(lambda: a.scene() == CSS, 60, "A on the room's CSS")
    a.steps("wait 90")
    a.shot("03-created-from-the-direct-css")


def test_room_launcher_create_from_the_top_page(backend: OnlineBackend, dolphin: Callable[..., DolphinInstance],
                                                gpu_backend: str) -> None:
    """The launcher's Create room from the main menu's top page (GROUP, SOLO, PLAY ONLINE, ...):
    the page reports itself as menus and the game goes from it to its new room's CSS (prod,
    2026-10-12: backed out of the ONLINE page after a Ranked set, the launcher said "Finish your
    current game first.")."""
    test = "launcher-create-top"
    ua = backend.create_user("rctop", "RCTP")
    a = _boot(dolphin, "room-ct", backend, ua, gpu_backend, test, "gameplay", artifacts=ART)
    a.to_main_menu()
    a.steps("tap B", "wait 60")
    a.shot("00-top-page")
    assert a.scene() == "muMenuMain"
    assert room_view(a)["screen"] == ppom.SCREENS["menus"], room_view(a)
    c0 = room_view(a)["create"]

    rid = write_create(a)
    assert launcher_answer(a, rid)["result"] == "accepted"
    _wait(lambda: a.scene() == CSS, 60, "A on the room's CSS")
    v = until_view(a, lambda v: v["flags"] & ppom.RF_IN and v["screen"] == ppom.SCREENS["room"], "A in its room",
                   timeout=60)
    assert v["host"] == v["local_port"] == 0 and v["flags"] & ppom.RF_PUBLIC and v["create"] == (c0 + 1) & 0xFF, v
    assert menu_state(a) == 0x10 | 1, hex(menu_state(a))   # the room CSS entered as Create Room's
    a.steps("wait 90")
    a.shot("01-created-from-the-top-page")
    # The CSS is whole (the Wi-Fi task was made on the way): a character can be picked.
    pick(a)
    a.shot("02-picked")

    # Back out (hold B): WITH FRIENDS on Create Room, as after WITH FRIENDS > Create Room.
    a.steps("hold B 120", "until muMenuMain 600", "wait 90")
    a.shot("03-back-on-create-room")
    _wait(lambda: not room_view(a)["flags"] & ppom.RF_IN, 30, "A out of the room")


def test_room_launcher_create_before_the_game_starts(backend: OnlineBackend,
                                                     dolphin: Callable[..., DolphinInstance],
                                                     gpu_backend: str) -> None:
    """Create room clicked with the game closed: accepted at once, held through the boot, and the
    game goes from the boot's ONLINE page to its new room's CSS."""
    import json
    import uuid
    from datetime import datetime, timezone
    ua = backend.create_user("rcboot", "RCBT")
    rid = str(uuid.uuid4())

    def put(user_dir: Path) -> None:
        created = datetime.now(timezone.utc).isoformat(timespec="milliseconds").replace("+00:00", "Z")
        (Path(user_dir) / "Online" / "join-room.json").write_text(json.dumps(
            {"version": 1, "id": rid, "kind": "create", "createdAt": created}))

    g = _boot(dolphin, "room-cboot", backend, ua, gpu_backend, "launcher-create-boot", "gameplay",
              artifacts=ART, before_launch=put)
    assert launcher_answer(g, rid)["result"] == "accepted"
    g.c.wait_state("running", timeout=120)
    _wait(lambda: g.scene() == CSS, 120, "the room's CSS")
    v = until_view(g, lambda v: v["flags"] & ppom.RF_IN and v["screen"] == ppom.SCREENS["room"], "in its room",
                   timeout=60)
    assert v["host"] == v["local_port"] == 0 and v["create"] >= 1, v
    assert menu_state(g) == 0x10 | 1, hex(menu_state(g))
    g.steps("wait 90")
    log = " ".join(g.c.call("rooms_status")["log"])
    assert log.count("sent create") == 1 and "game asks to create" not in log, log
    g.shot("01-room-created-after-the-boot")


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
    # The history: P1 first, the others placed by when they went out.
    m = room_history(backend, gs, users, ports, "ffa")
    places = [p["place"] for p in m["players"]]
    assert places[0] == 1 and all(2 <= x <= len(ports) for x in places[1:]), m
    assert [p["wins"] for p in m["players"]] == [1] + [0] * (len(ports) - 1) and all(p["team"] is None for p in m["players"])


@pytest.mark.parametrize("ports", [(0, 1, 2), (0, 1, 2, 3)], ids=["three", "four"])
def test_room_player_quits_while_the_match_loads(backend: OnlineBackend,
                                                 dolphin: Callable[..., DolphinInstance],
                                                 gpu_backend: str, ports: tuple) -> None:
    """A room's game of 3-4 starts and the last player closes their Dolphin while the match
    loads (the setup decided, before the barrier): the others start the match without them."""
    test = f"quit-while-loading-{len(ports)}"
    import secrets
    users = [backend.create_user(f"rq{'abcd'[k]}", f"RQ{'ABCD'[k]}{'TF'[len(ports) - 3]}",
                                 email=f"roomq{k}-{secrets.token_hex(4)}@example.test")
             for k in range(len(ports))]
    gs = [_boot(dolphin, f"roomq{len(ports)}-{x}", backend, u, gpu_backend, test, "gameplay", artifacts=ART)
          for x, u in zip("abcd", users)]
    a, q = gs[0], gs[-1]
    _all(*[g.to_main_menu for g in gs])
    _all(*[g.to_online_page for g in gs])
    code = create_room(a)
    pick(a)
    for slot in ports[2:]:
        hand_to(a, PANEL_X[slot], -10.0)
        a.steps("tap A 8", "wait 30")
        until_view(a, lambda v, s=slot: v["slots"][s]["bits"] & ppom.SLOT_OPEN, f"slot {slot + 1} open")
    hand_to(a, -23.0, 10.5)
    _all(*[lambda g=g: to_join_room(g) for g in gs[1:]])
    for k, g in enumerate(gs[1:], 1):
        pick(g, right=2 * k)
    for g in gs:
        B.write_rules(g.c, stocks=1, minutes=2, items_off=True)
        ppom.allow_test_rules(g.c)
    for k, g in enumerate(gs[1:], 1):
        type_room_code(g, code)
        until_view(g, lambda v: v["flags"] & ppom.RF_IN, f"player {k + 1} in the room")
    until_view(a, lambda v: all(v["slots"][p]["bits"] & ppom.SLOT_TAKEN for p in ports), "all players")
    for g in gs:
        g.steps("wait 60")
        g.steps("tap START 8", "wait 10")
    # The quitter's game leaves the CSS for the match's loading, then its Dolphin is closed.
    _wait(lambda: q.scene() != CSS, 240, "the quitter's match loading")
    time.sleep(1.0)
    q.inst.stop()
    rest = gs[:-1]
    online_set.wait(lambda: all(online_set.gstatus(g.c)["phase"] in ("running", "error", "ended") for g in rest),
                    150, "the others' match running (or ended)")
    st = [online_set.gstatus(g.c) for g in rest]
    print("after the quit:", [(s["phase"], s.get("error"), s.get("match_ports")) for s in st])
    assert all(s["phase"] == "running" for s in st), [(s["phase"], s.get("error")) for s in st]
    time.sleep(5)
    for g in rest:
        g.shot("05-match-without-the-quitter")
    st = [online_set.gstatus(g.c) for g in rest]
    assert all(s["phase"] == "running" for s in st), [(s["phase"], s.get("error")) for s in st]
    assert len({s["current_frame"] > 120 for s in st}) == 1 and st[0]["current_frame"] > 120, st


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


@pytest.mark.parametrize("quits", [(1,), (1, 2)], ids=["one-quits", "the-last-quit-ends-it"])
def test_room_player_quits_with_lras(backend: OnlineBackend, dolphin: Callable[..., DolphinInstance],
                                     gpu_backend: str, quits: tuple) -> None:
    """A free-for-all of three: P2 pauses (a held START: rooms have pause off, P+ pauses on a held
    START) and quits with L+R+A+START while P1 and P3 move. Only P2 leaves the match: the game
    resumes for P1 and P3 at once with P2's fighter out, P2's Dolphin leaves the session once the
    quit's frame is final and its game goes back to the room's CSS ("Waiting for the game to
    end"), and P1 and P3 play on without it (the same game on both, no mismatch). The game ends
    when P1 walks off: P3 wins, P2 is in the history as a player who left.
    the-last-quit-ends-it: then P3 quits too, which leaves only P1: P1 wins by Brawl's own game set
    and P3 stays in the session until it (its quit ended the game: quitStays), as P1 does."""
    test = "lras-" + ("one" if len(quits) == 1 else "two")
    import secrets
    import threading
    from ppharness import flows as F
    from ppharness.client import PadInput
    users = [backend.create_user(n, f"RL{'ABC'[k]}{'OT'[len(quits) - 1]}", email=f"roomlras{k}-{secrets.token_hex(4)}@example.test")
             for k, n in enumerate(["rla", "rlb", "rlc"])]
    gs = [_boot(dolphin, f"roomlras{len(quits)}-{x}", backend, u, gpu_backend, test, "gameplay", artifacts=ART)
          for x, u in zip("abc", users)]
    a = gs[0]
    _all(*[g.to_main_menu for g in gs])
    _all(*[g.to_online_page for g in gs])
    code = create_room(a)
    pick(a)
    hand_to(a, PANEL_X[2], -10.0)
    a.steps("tap A 8", "wait 30")
    until_view(a, lambda v: v["slots"][2]["bits"] & ppom.SLOT_OPEN, "slot 3 open")
    hand_to(a, -23.0, 10.5)
    _all(*[lambda g=g: to_join_room(g) for g in gs[1:]])
    for k, g in enumerate(gs[1:], 1):
        pick(g, right=2 * k)
    for g in gs:
        B.write_rules(g.c, stocks=2, minutes=4, items_off=True)
        ppom.allow_test_rules(g.c)
    for k, g in enumerate(gs[1:], 1):
        type_room_code(g, code)
        until_view(g, lambda v: v["flags"] & ppom.RF_IN, f"player {k + 1} in the room")
    until_view(a, lambda v: all(v["slots"][p]["bits"] & ppom.SLOT_TAKEN for p in range(3)), "all players")
    for g in gs:
        g.steps("wait 60")
    game = room_view(a)["game"]
    for g in gs:
        g.steps("tap START 8", "wait 10")
    online_set.wait(lambda: all(online_set.gstatus(g.c)["phase"] in ("running", "error", "ended") for g in gs),
                    240, "the room's game running on all")
    st = [online_set.gstatus(g.c) for g in gs]
    assert all(s["phase"] == "running" for s in st), [(s["phase"], s.get("error")) for s in st]
    time.sleep(4)

    def trail_of(players: list[Game]) -> list[tuple]:
        row = []
        for g in players:
            try:
                s = online_set.gstatus(g.c)
                row.append((g.name, online_set.scene(g.c), s["phase"], s.get("end_reason"),
                            s.get("current_frame"), s.get("local_quit_frame")))
            except Exception as e:  # noqa: BLE001
                row.append((g.name, "error", repr(e)))
        return row

    def stocks(g: Game) -> dict[int, int]:
        return {p.port: p.stocks for p in B.read_players(g.c.read_mem, max_entries=3)}

    walk = [PadInput(main=(40, 128), hold=7), PadInput(main=(216, 128), hold=7)]
    left: list[Game] = []
    for port in quits:
        q = gs[port]
        stay = [g for g in gs if g is not q and g not in left]
        for g in stay:
            g.c.pad_script(0, walk * 60)
        q.steps("hold START 90", "wait 20")
        q.shot(f"0{port}-paused")
        assert online_set.gstatus(q.c)["phase"] == "running"
        q.steps("hold L+R+A+START 20")
        if len(stay) > 1:
            # Only the quitter leaves: its game goes back to the room, the others play on.
            t0 = time.monotonic()
            while time.monotonic() - t0 < 30 and online_set.scene(q.c) != CSS:
                time.sleep(0.25)
            print(f"P{port + 1} quit:", trail_of(gs), [hex(g.debug_scratch()[10]) for g in gs])
            assert online_set.scene(q.c) == CSS, trail_of(gs)
            sq = online_set.gstatus(q.c)
            assert sq["end_reason"] == "quit left", sq["end_reason"]
            assert q.debug_scratch()[10] & 0x600000 == 0x600000, hex(q.debug_scratch()[10])
            frames = [online_set.gstatus(g.c)["current_frame"] for g in stay]
            time.sleep(4)
            st = [online_set.gstatus(g.c) for g in stay]
            assert all(s["phase"] == "running" for s in st), [(s["phase"], s.get("end_reason")) for s in st]
            assert all(s["current_frame"] > f + 120 for s, f in zip(st, frames)), (frames, st)
            for g, s in zip(stay, st):
                peer = next(p for p in s["peers"] if p["slot"] == port)
                assert peer["left"], (g.name, peer)
                assert online_set.session(g.c)["quit_ports"] & (1 << port), g.name
                assert stocks(g).get(port) == 0, (g.name, stocks(g))
                assert g.debug_scratch()[10] & 0x400000, hex(g.debug_scratch()[10])
                g.shot(f"0{port}-playing-on-without-P{port + 1}")
            # The quitter waits on the room's CSS for the others' game.
            v = q.c.call("rooms_status")["view"]
            in_game = [(sl["player"] or {}).get("in_game") for sl in v["slots"][:3]]
            print("quitter's room:", v["status"], repr(v["status_text"]), in_game)
            assert v["status_text"] == "Waiting for the game to end", v
            assert in_game == [g not in left and g is not q for g in gs], v
            q.shot(f"0{port}-quitter-back-in-the-room")
        else:
            # The last quit leaves one player in: Brawl's game set ends the game for everyone.
            online_set.wait(lambda: all(online_set.gstatus(g.c)["phase"] != "running" for g in stay + [q]), 60,
                            "the game set after the last quit")
            ends = [online_set.gstatus(g.c) for g in stay + [q]]
            print("last quit:", [(s["end_reason"], s["current_frame"]) for s in ends])
            assert [s["end_reason"] for s in ends] == ["game set", "game set"], ends
        left.append(q)

    stay = [g for g in gs if g not in left]
    if len(stay) == 2:
        # P1 walks off: P3 wins.
        th = threading.Thread(target=lambda: F.fight([F.Seat(a.c, 0, 0, a.name)], 60 * 60, mode="selfdestruct",
                                                     seed="lras", stop=lambda: online_set.gstatus(a.c)["phase"] != "running"))
        th.start()
        th.join(300)
        online_set.wait(lambda: all(online_set.gstatus(g.c)["phase"] != "running" for g in stay), 240, "the game to end")
    # The players still in the session at the end (the last quitter's too) compare their game.
    ended = stay + ([gs[quits[-1]]] if len(quits) == 2 else [])
    ends = [online_set.gstatus(g.c) for g in ended]
    assert all(s["end_reason"] == "game set" for s in ends), [s["end_reason"] for s in ends]
    limit = min(s["current_frame"] for s in ends) - online_set.CONFIRM_MARGIN
    cks = [{r[0]: r[1] for r in g.c.call("gprb_checksums", since=0)["rows"] if r[0] <= limit} for g in ended]
    common = sorted(set(cks[0]).intersection(*cks[1:]))
    bad = [f for f in common if len({ck[f] for ck in cks}) != 1]
    print(f"ended together: {[g.name for g in ended]}, frames {[s['current_frame'] for s in ends]}, "
          f"rollbacks {[s['rollbacks'] for s in ends]}, checksums compared {len(common)}, mismatches {len(bad)}")
    assert common and not bad, (len(common), bad[:5])
    _wait(lambda: all(g.scene() == CSS for g in gs), 180, "all back on the room's CSS")
    for g in gs:
        until_view(g, lambda v: v["flags"] & ppom.RF_IN and v["status"] == 0 and v["game"] == game + 1,
                   "the room waiting for its next game", timeout=60)
        g.steps("wait 60")
        g.shot("09-back-in-the-room")
    # The history: the winner first; every player who quit has left the game (not placed). Only
    # the players who saw the game end report it.
    winner = 0 if len(quits) == 2 else 2
    _wait(lambda: all((lambda r: r["reports_sent"] == 1 and r["pending_jobs"] == 0)(g.c.online_status()["ranked"])
                      for g in ended), 60, "the reports sent")
    m = backend._http("GET", "/v1/me/matches?mode=rooms", token=users[0].session_token)["matches"][0]
    print("history:", [(p["port"], p.get("place"), p.get("left")) for p in m["players"]])
    by_port = {p["port"] - 1: p for p in m["players"]}
    assert by_port[winner]["place"] == 1, m
    for port in quits:
        assert by_port[port]["place"] is None, m
    if len(quits) == 1:
        assert by_port[0]["place"] == 2, m


def test_room_runback_restarts_for_everyone(backend: OnlineBackend, dolphin: Callable[..., DolphinInstance],
                                            gpu_backend: str) -> None:
    """A free-for-all of three without a set: P2 pauses and holds L+R+A+Y+START (P+'s salty
    runback). The same match starts again for all three (the user's, 2026-10-13), not only P2
    leaving as after a plain quit: the session sets it up on every machine and the room's game
    goes on in it. The runback's match is the same game on all three (no checksum differs)."""
    import secrets
    from ppharness.client import PadInput
    test = "room-runback-three"
    users = [backend.create_user(n, f"RT{'ABC'[k]}H", email=f"roomrb{k}-{secrets.token_hex(4)}@example.test")
             for k, n in enumerate(["rra", "rrb", "rrc"])]
    gs = [_boot(dolphin, f"roomrb3-{x}", backend, u, gpu_backend, test, "gameplay", artifacts=ART)
          for x, u in zip("abc", users)]
    a = gs[0]
    _all(*[g.to_main_menu for g in gs])
    _all(*[g.to_online_page for g in gs])
    code = create_room(a)
    pick(a)
    hand_to(a, PANEL_X[2], -10.0)
    a.steps("tap A 8", "wait 30")
    until_view(a, lambda v: v["slots"][2]["bits"] & ppom.SLOT_OPEN, "slot 3 open")
    hand_to(a, -23.0, 10.5)
    _all(*[lambda g=g: to_join_room(g) for g in gs[1:]])
    for k, g in enumerate(gs[1:], 1):
        pick(g, right=2 * k)
    for g in gs:
        B.write_rules(g.c, stocks=4, minutes=8, items_off=True)
        ppom.allow_test_rules(g.c)
    for k, g in enumerate(gs[1:], 1):
        type_room_code(g, code)
        until_view(g, lambda v: v["flags"] & ppom.RF_IN, f"player {k + 1} in the room")
    until_view(a, lambda v: all(v["slots"][p]["bits"] & ppom.SLOT_TAKEN for p in range(3)), "all players")
    for g in gs:
        g.steps("wait 60")
    for g in gs:
        g.steps("tap START 8", "wait 10")
    online_set.wait(lambda: all(online_set.gstatus(g.c)["phase"] in ("running", "error", "ended") for g in gs),
                    240, "the room's game running on all")
    st = [online_set.gstatus(g.c) for g in gs]
    assert all(s["phase"] == "running" for s in st), [(s["phase"], s.get("error")) for s in st]
    first = st[0]["match_index"]
    setup = B.read_match_setup(a.c.read_mem)
    time.sleep(4)

    walk = [PadInput(main=(40, 128), hold=7), PadInput(main=(216, 128), hold=7)]
    q = gs[1]
    for g in (gs[0], gs[2]):
        g.c.pad_script(0, walk * 60)
    q.steps("hold START 90", "wait 20")
    assert online_set.gstatus(q.c)["phase"] == "running"
    # Held past the session's end (the game stays paused until then), not into the next match.
    q.steps("hold L+R+A+Y+START 90")
    online_set.wait(lambda: all(online_set.gstatus(g.c)["phase"] == "running" and
                                online_set.gstatus(g.c)["match_index"] == first + 1 for g in gs),
                    90, "the same match again on all three")
    for g in gs:
        g.c.pad_script(0, walk * 60)
    time.sleep(6)
    for g in gs:
        g.shot("01-runback-for-everyone")
        sc = g.debug_scratch()[10]
        assert sc & 0x1000000 and sc & 0xFF == 0x1B, (g.name, hex(sc))   # marked a runback, straight in
        assert not sc & 0x400000, (g.name, "taken as a player's own quit", hex(sc))
        assert B.read_match_setup(g.c.read_mem) == setup, (g.name, "a different match")
        log = g.inst.log.read()
        assert log.count("the last game again (runback)") == 1, g.name
        assert "game run back: the session plays it again" in log, g.name
        assert online_set.session(g.c)["quit_ports"] == 0, g.name
        s = online_set.gstatus(g.c)
        assert s["phase"] == "running" and not any(p["left"] for p in s["peers"]), (g.name, s["peers"])
    ends = [online_set.gstatus(g.c) for g in gs]
    limit = min(s["current_frame"] for s in ends) - online_set.CONFIRM_MARGIN
    cks = [{r[0]: r[1] for r in g.c.call("gprb_checksums", since=0)["rows"] if r[0] <= limit} for g in gs]
    common = sorted(set(cks[0]).intersection(*cks[1:]))
    bad = [f for f in common if len({ck[f] for ck in cks}) != 1]
    print(f"runback match: frames {[s['current_frame'] for s in ends]}, checksums compared {len(common)}, "
          f"mismatches {len(bad)}")
    assert common and not bad, (len(common), bad[:5])
