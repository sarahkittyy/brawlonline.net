"""The 2v2 queue (docs/design/2v2.md): four games search WITH ANYONE > 2V2, the server matches
them, and they play P+'s rainbow team battle under the gameplay session, teams shuffled every game,
a 45 s ready-up between games.

Instances are named ``twos-*`` (``ppharness clean --prefix twos-``). Screenshots go to
``run/artifacts/game-code/twos/<test>/``. Needs what ``test_rooms_game.py`` needs.
"""

from __future__ import annotations

import contextlib
import threading
import time
from typing import Any, Callable

import pytest

from ppharness import flows as F
from ppharness.backend import OnlineBackend
from ppharness.client import PadInput
from ppharness.instance import DolphinInstance

from test_online import _wait
from test_online_game import ROOT, Game, _boot, backend  # noqa: F401  (the backend fixture)
from test_rooms_game import PANEL_X, ROOM_KEYS, hand_to, room_view, until_view
from test_waiting_room import _banner_shown
import online_set  # noqa: E402  (test_online_game put harness/tools on the path)
import ppom  # noqa: E402
from ppharness import brawl as B  # noqa: E402

pytestmark = [pytest.mark.dolphin, pytest.mark.server, pytest.mark.gpu, pytest.mark.slow]

ART = ROOT / "run" / "artifacts" / "game-code" / "twos"
CSS = "scSelctCharacter"
TEAMS_RAINBOW = 2
READY_SECONDS = 45


@pytest.fixture(autouse=True)
def _server_logs(backend: OnlineBackend, request: pytest.FixtureRequest) -> Any:
    """mm's and accounts' logs next to the screenshots (the backend's own go with it)."""
    yield
    out = ART / request.node.name
    out.mkdir(parents=True, exist_ok=True)
    for name in ("mm", "accounts"):
        (out / f"{name}.log").write_text(backend.log_text(name), encoding="utf-8")


def _boot_four(backend: OnlineBackend, dolphin: Callable[..., DolphinInstance], gpu_backend: str,
               test: str, names: list[str]) -> tuple[list[Game], list[Any]]:
    import secrets
    users = [backend.create_user(n, f"TW{'ABCD'[k]}", email=f"twos{k}-{secrets.token_hex(4)}@example.test")
             for k, n in enumerate(names)]
    gs = [_boot(dolphin, f"twos-{x}", backend, u, gpu_backend, test, "gameplay", artifacts=ART)
          for x, u in zip("abcd", users)]
    online_set.both(*[g.to_main_menu for g in gs])
    online_set.both(*[g.to_online_page for g in gs])
    return gs, users


def _to_party_css(g: Game) -> str:
    """WITH ANYONE > 2V2: the player's own party (a 2-slot room of kind twos), a character picked.
    Returns the party's code."""
    g.to_css("twos")
    v = until_view(g, lambda v: v["flags"] & ppom.RF_IN and v["flags"] & ppom.RF_TWOS, "the 2v2 party")
    B.write_rules(g.c, stocks=1, minutes=2, items_off=True)
    ppom.allow_test_rules(g.c)
    return v["code"]


def _search(g: Game) -> None:
    n = g.finds()
    g.press_until("START", lambda: g.finds() > n, "FIND_OPPONENT")


def _teams(g: Game) -> dict[int, int]:
    """SESSION's teams of the next (or running) game: port -> team."""
    se = online_set.session(g.c)
    return {i: p["team"] for i, p in enumerate(se["players"]) if p["present"]}


def _split(teams: dict[int, int]) -> frozenset[frozenset[int]]:
    by: dict[int, set[int]] = {}
    for port, t in teams.items():
        by.setdefault(t, set()).add(port)
    return frozenset(frozenset(s) for s in by.values())


def _play(gs: list[Game], game: int) -> dict[str, Any]:
    """The game running on all four, the same setup on each, a rainbow team battle of two teams of
    two; the team of the lower team number walks off, the other stands. Compares the confirmed
    checksums. Returns the teams (in-game port -> team) and each game's port."""
    online_set.wait(lambda: all(online_set.gstatus(g.c)["phase"] in ("running", "error", "ended") for g in gs),
                    240, f"game {game} running on all")
    st = [online_set.gstatus(g.c) for g in gs]
    assert all(s["phase"] == "running" for s in st), [(s["phase"], s.get("error")) for s in st]
    setups = [B.read_match_setup(g.c.read_mem) for g in gs]
    assert all(x == setups[0] for x in setups), setups
    se = online_set.session(gs[0].c)
    assert se["teams"] == TEAMS_RAINBOW, se["teams"]
    teams = _teams(gs[0])
    assert all(_teams(g) == teams for g in gs), [_teams(g) for g in gs]
    split = _split(teams)
    assert len(split) == 2 and all(len(t) == 2 for t in split), teams
    losers = {p for p, t in teams.items() if t == min(teams.values())}
    time.sleep(6)
    for g in gs:
        g.shot(f"g{game}-match")
    ports = [online_set.local(g.c)["local_port"] for g in gs]

    def off(g: Game, port: int) -> None:
        with contextlib.suppress(Exception):
            F.fight([F.Seat(g.c, port, 0, g.name)], 60 * 60, mode="selfdestruct", seed=f"tw{game}{port}",
                    stop=lambda: online_set.gstatus(g.c)["phase"] != "running")
    th = [threading.Thread(target=off, args=(g, p)) for g, p in zip(gs, ports) if p in losers]
    for t in th:
        t.start()
    for t in th:
        t.join(300)
    online_set.wait(lambda: all(online_set.gstatus(g.c)["phase"] != "running" for g in gs), 240, f"game {game} to end")
    st = [online_set.gstatus(g.c) for g in gs]
    limit = min(s["current_frame"] for s in st) - online_set.CONFIRM_MARGIN
    cks = [{r[0]: r[1] for r in g.c.call("gprb_checksums", since=0)["rows"] if r[0] <= limit} for g in gs]
    common = sorted(set(cks[0]).intersection(*cks[1:]))
    bad = [f for f in common if len({ck[f] for ck in cks}) != 1]
    print(f"game {game}: teams {teams}, frames {[s['current_frame'] for s in st]}, end {[s['end_reason'] for s in st]}, "
          f"rollbacks {[s['rollbacks'] for s in st]}, checksums compared {len(common)}, mismatches {len(bad)}")
    assert common and not bad, (len(common), bad[:5])
    return {"teams": teams, "ports": ports}


def _back_on_css(gs: list[Game], game: int) -> None:
    """After game set: everyone on the plain online CSS, the session kept, the ready-up running."""
    _wait(lambda: all(g.scene() == CSS for g in gs), 180, f"after game {game}: all on the CSS")
    for g in gs:
        lo = online_set.local(g.c)
        assert lo["state"] == 2 and not lo["disconnected"], (g.name, lo)
    secs = [g.c.read_mem(online_set.block(g.c).local + ppom.L_READY_SECONDS, 1)[0] for g in gs]
    assert all(0 < s <= READY_SECONDS for s in secs), secs
    for g in gs:
        g.steps("wait 30")
        g.shot(f"g{game}-back-on-css")


def _ready(g: Game, game: int) -> None:
    # (the lock-in's seq moves with the coin too: the CSS sends it as a lock-in that is not ready)
    def ready() -> bool:
        lock = online_set.local(g.c)["lock"]
        return bool(lock["ready"]) and lock["game"] == game
    g.press_until("START", ready, f"{g.name}: ready for game {game}")


def _lobby(gs: list[Game], game: int) -> dict[int, int]:
    """Before `game` (game 1 too, as Ranked: the search goes back to the CSS once connected): every
    game is on the CSS, connected, and shows the four players' panels (Dolphin's PutTwosLobby:
    RF_TWOS without RF_IN): each player's name, the character on their CSS (not ready yet: their
    coin) and the teams of the next game, the same on every machine and two of two. The ready-up
    counts down. Returns the teams shown (in-game port -> team)."""
    _wait(lambda: all(g.scene() == CSS and online_set.local(g.c)["state"] == 2 for g in gs), 180,
          f"before game {game}: all on the CSS, connected")

    def shown(g: Game) -> Any:
        v = room_view(g)
        if not v["flags"] & ppom.RF_TWOS or v["flags"] & ppom.RF_IN:
            return None
        sl = v["slots"]
        if not all(sl[i]["bits"] & ppom.SLOT_TAKEN and sl[i]["name"] and sl[i]["char"] != 0xFF and sl[i]["team"] < 3
                   for i in range(4)):
            return None
        return v
    views = [_wait(lambda g=g: shown(g), 30, f"{g.name}: the four panels before game {game}") for g in gs]
    teams = {i: views[0]["slots"][i]["team"] for i in range(4)}
    assert all({i: v["slots"][i]["team"] for i in range(4)} == teams for v in views), [v["slots"] for v in views]
    split = _split(teams)
    assert len(split) == 2 and all(len(t) == 2 for t in split), teams
    names = [sl["name"] for sl in views[0]["slots"]]
    assert len(set(names)) == 4, names
    assert not any(v["slots"][i]["bits"] & ppom.SLOT_READY for v in views for i in range(4)), "ready before START"
    secs = [g.c.read_mem(online_set.block(g.c).local + ppom.L_READY_SECONDS, 1)[0] for g in gs]
    assert all(0 < x <= READY_SECONDS for x in secs), secs
    for g in gs:
        g.steps("wait 30")
        g.shot(f"g{game}-lobby")
    print(f"before game {game}: {names}, teams {teams}")
    return teams


def test_twos_four_alone(backend: OnlineBackend, dolphin: Callable[..., DolphinInstance], gpu_backend: str) -> None:
    """Four players alone: each searches from its own party's CSS (the banner says 2V2), the
    server matches them, game 1 is a rainbow team battle the same on all four; back on the CSS the
    ready-up counts down, everyone readies and game 2 has other teams; after game 2 one player
    does not ready: when the 45 s are up that player is back on its party's CSS, the other three
    search again."""
    gs, _users = _boot_four(backend, dolphin, gpu_backend, "four-alone", ["twa", "twb", "twc", "twd"])
    for g in gs:
        _to_party_css(g)
        g.shot("01-party-css")
    for g in gs[:3]:
        _search(g)
    _wait(lambda: _banner_shown(gs[0], "2V2 - Searching for players - Hold START to cancel"), 60, "the 2V2 banner")
    gs[0].shot("02-waiting-room")
    _search(gs[3])

    l1 = _lobby(gs, 1)
    for g in gs:
        _ready(g, 1)
    r1 = _play(gs, 1)
    assert _split(r1["teams"]) == _split(l1), ("game 1's teams are not the ones shown", l1, r1)
    _back_on_css(gs, 1)
    l2 = _lobby(gs, 2)
    for g in gs:
        _ready(g, 2)
    r2 = _play(gs, 2)
    assert _split(r2["teams"]) == _split(l2), ("game 2's teams are not the ones shown", l2, r2)
    assert _split(r2["teams"]) != _split(r1["teams"]), ("game 2 kept game 1's teams", r1, r2)
    _back_on_css(gs, 2)
    # D does not ready: when the time is up D is back on its party's CSS, the others search.
    finds = [g.finds() for g in gs[:3]]
    for g in gs[:3]:
        _ready(g, 3)
    _wait(lambda: all(g.finds() > n for g, n in zip(gs[:3], finds)), READY_SECONDS + 30, "A-C searching again")
    d = gs[3]
    until_view(d, lambda v: v["flags"] & ppom.RF_IN and v["flags"] & ppom.RF_TWOS, "D back in its own party", timeout=60)
    _wait(lambda: d.scene() == CSS, 60, "D on its party's CSS")   # (built again: scMemoryChange first)
    d.steps("wait 60")
    d.shot("g3-removed")
    for g in gs[:3]:
        g.shot("g3-searching-again")


def test_twos_pair_found_at_once(backend: OnlineBackend, dolphin: Callable[..., DolphinInstance],
                                 gpu_backend: str) -> None:
    """C and D search first; then the pair: its match is made the moment both of its tickets are
    in, while its pair room (docs/design/2v2.md, "The pair room") is only starting to connect. The
    pair room ends wherever it is, and the four play game 1, the pair on one team."""
    gs, _users = _boot_four(backend, dolphin, gpu_backend, "pair-at-once", ["twa", "twb", "twc", "twd"])
    a, b, c, d = gs
    _make_pair(a, b)
    _to_party_css(c)
    _to_party_css(d)
    _search(c)
    _search(d)
    for g in (a, b):
        g.press_until("START", lambda g=g: online_set.local(g.c)["lock"]["ready"], f"{g.name} ready")
    _lobby(gs, 1)
    for g in gs:
        _ready(g, 1)
    r1 = _play(gs, 1)
    pa, pb = r1["ports"][0], r1["ports"][1]
    assert r1["teams"][pa] == r1["teams"][pb], ("the pair split up", r1)
    for g in (a, b):
        log = (g.inst.user_dir / "Logs" / "dolphin.log").read_text(errors="replace")
        assert "the 2v2 search ends with" not in log, g.name


def test_twos_pair_found_while_its_room_starts(backend: OnlineBackend, dolphin: Callable[..., DolphinInstance],
                                              gpu_backend: str) -> None:
    """The pair's games are connected for their pair room, its hand-off held back
    (PPR_TEST_PAIR_HANDOFF_DELAY_MS), when C and D make the match: the pair room ends there (its
    late hand-off is dropped, no session of it starts) and the four play game 1."""
    import secrets
    users = [backend.create_user(n, f"TW{'ABCD'[k]}", email=f"twos{k}-{secrets.token_hex(4)}@example.test")
             for k, n in enumerate(["twa", "twb", "twc", "twd"])]
    env = {"PPR_TEST_PAIR_HANDOFF_DELAY_MS": "15000"}
    gs = [_boot(dolphin, f"twos-{x}", backend, u, gpu_backend, "pair-mid-start", "gameplay", artifacts=ART,
                env=env if x in "ab" else None)
          for x, u in zip("abcd", users)]
    online_set.both(*[g.to_main_menu for g in gs])
    online_set.both(*[g.to_online_page for g in gs])
    a, b, c, d = gs
    _make_pair(a, b)
    _to_party_css(c)
    _to_party_css(d)
    for g in (a, b):
        g.press_until("START", lambda g=g: online_set.local(g.c)["lock"]["ready"], f"{g.name} ready")
    for g in (a, b):
        _wait(lambda g=g: g.c.mm_status()["pair_room"]["state"] == "connection_success", 60,
              f"{g.name}: the pair room connected, its hand-off waiting")
    _search(c)
    _search(d)
    _lobby(gs, 1)
    for g in gs:
        _ready(g, 1)
    r1 = _play(gs, 1)
    pa, pb = r1["ports"][0], r1["ports"][1]
    assert r1["teams"][pa] == r1["teams"][pb], ("the pair split up", r1)
    for g in (a, b):
        log = (g.inst.user_dir / "Logs" / "dolphin.log").read_text(errors="replace")
        assert "the pair room ended before its hand-off" in log, g.name
        assert "the pair room's session started" not in log, g.name


def _make_pair(a: Game, b: Game) -> str:
    """B joins A's party: A on its party's CSS, B presses A on its open panel ("A: Enter Code") and
    types A's code. Returns the code."""
    code = _to_party_css(a)
    _to_party_css(b)
    b.shot("01-own-party")
    hand_to(b, PANEL_X[1], -10.0)
    b.steps("tap A 8", "wait 40")
    _wait(lambda: b.debug_scratch()[4] == 1 and b.debug_scratch()[5] == 8, 5, "the keypad on the open panel")
    _type_code(b, code)
    vb = until_view(b, lambda v: v["flags"] & ppom.RF_TWOS and v["code"] == code, "B in A's party")
    assert vb["local_port"] == 1, vb
    until_view(a, lambda v: all(v["slots"][k]["bits"] & ppom.SLOT_TAKEN for k in (0, 1)), "A sees B")
    for g in (a, b):
        g.steps("wait 60")
        g.shot("02-pair")
    return code


def test_twos_pair_and_two_alone(backend: OnlineBackend, dolphin: Callable[..., DolphinInstance],
                                 gpu_backend: str) -> None:
    """A and B queue as a pair: B presses A on A's party's open panel ("A: Enter Code") and types
    A's code; both ready, the pair searches together and plays the pair room until C and D, alone,
    make the match. The pair is one team in game 1 and game 2."""
    gs, _users = _boot_four(backend, dolphin, gpu_backend, "pair", ["twa", "twb", "twc", "twd"])
    a, b, c, d = gs
    _make_pair(a, b)
    _to_party_css(c)
    _to_party_css(d)
    for g in (a, b):
        g.press_until("START", lambda g=g: online_set.local(g.c)["lock"]["ready"], f"{g.name} ready")
    _wait(lambda: a.finds() and b.finds(), 30, "the pair searching")
    # Searching together, the pair plays the waiting room together (docs/design/2v2.md, "The pair
    # room"); the match found ends it, and the four go to game 1's CSS.
    for g in (a, b):
        _wait(lambda g=g: _in_pair_room(g), 120, f"{g.name}: in the pair room")
    time.sleep(3)
    for g in (a, b):
        g.shot("02b-pair-room")
    _search(c)
    _search(d)
    _lobby(gs, 1)
    for g in gs:
        _ready(g, 1)
    r1 = _play(gs, 1)
    pa, pb = r1["ports"][0], r1["ports"][1]
    assert r1["teams"][pa] == r1["teams"][pb], ("the pair split up", r1)
    _back_on_css(gs, 1)
    _lobby(gs, 2)
    for g in gs:
        _ready(g, 2)
    r2 = _play(gs, 2)
    assert r2["teams"][pa] == r2["teams"][pb], ("the pair split up", r2)


def _in_pair_room(g: Game) -> bool:
    """The pair room's match runs on this game: the session's, SESSION's mode MODE_PAIR."""
    return (online_set.gstatus(g.c)["phase"] == "running" and g.scene() == "scMelee"
            and online_set.session(g.c)["mode"] == ppom.MODE_PAIR)


def _type_code(g: Game, code: str) -> None:
    """The keypad (room-code mode, open): the code from its letter keys; START joins
    (test_rooms_game.type_room_code without the START that opens it)."""
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
    g.shot("03-code-typed")
    g.steps("tap START 8", "wait 30")


def test_twos_cancel_and_search_again_at_once(backend: OnlineBackend, dolphin: Callable[..., DolphinInstance],
                                              gpu_backend: str) -> None:
    """Alone in a party: START searches, B cancels and START searches again a few frames later
    (the user's, 2026-11-03: a cancel and a new search 70 ms apart left the player READY with
    nothing searching, mm having refused the second ticket, "Searching too often. Wait a
    moment."). Dolphin now waits for mm's ticket interval: the second search goes on."""
    u = backend.create_user("twa", "TWAA")
    g = _boot(dolphin, "twos-a", backend, u, gpu_backend, "search-again", "gameplay", artifacts=ART)
    g.to_main_menu()
    g.to_online_page()
    _to_party_css(g)
    g.steps("wait 30")
    n = g.finds()
    # (one script: the CSS leaves for the waiting room a few frames after the search starts)
    g.c.pad_script(g.pad, [PadInput(buttons=["START"], hold=3), PadInput(hold=2),
                           PadInput(buttons=["B"], hold=3), PadInput(hold=2),
                           PadInput(buttons=["START"], hold=3), PadInput(hold=2)])
    _wait(lambda: g.finds() >= n + 2, 10, "the search, then the second one")
    time.sleep(5)
    st = g.c.mm_status()
    text = (g.inst.user_dir / "Logs" / "dolphin.log").read_text(errors="replace")
    assert "Searching too often" not in text
    assert "waiting for mm's interval" in text
    assert st["state"] == "matchmaking", st
    g.shot("01-searching-again")
