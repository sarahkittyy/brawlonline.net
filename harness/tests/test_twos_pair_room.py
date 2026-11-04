"""A 2v2 party's pair room (docs/design/2v2.md, "The pair room"): while both players of a party
search, their games connect to each other and play the waiting room together, an online match on
the Training Room with no stock or time limit, no pause and no quit prompt; either player holding
START ends it for both, back on the CSS and out of the queue. Partners who cannot connect get the
room's connection error and are out of the queue too.

Instances are named ``twos-*`` (``ppharness clean --prefix twos-``). Screenshots go to
``run/artifacts/game-code/twos-pair-room/<test>/``. Needs what ``test_twos.py`` needs.
"""

from __future__ import annotations

import threading
import time
from typing import Any, Callable

import pytest

from ppharness import flows as F
from ppharness.backend import OnlineBackend
from ppharness.instance import DolphinInstance

from test_online import _wait
from test_online_game import ROOT, Game, _boot, backend  # noqa: F401  (the backend fixture)
from test_rooms_game import PANEL_X, hand_to, until_view
from test_twos import _type_code
import online_set  # noqa: E402  (test_online_game put harness/tools on the path)
import ppom  # noqa: E402
from ppharness import brawl as B  # noqa: E402

pytestmark = [pytest.mark.dolphin, pytest.mark.server, pytest.mark.gpu, pytest.mark.slow]

ART = ROOT / "run" / "artifacts" / "game-code" / "twos-pair-room"
CSS = "scSelctCharacter"
TRAINING_ROOM = 0x37
HOLD_START_FRAMES = 30
GAME_FRAME = 0x901812A4   # g_GameFrame's frame counter (1 at the match's first frame)


@pytest.fixture(autouse=True)
def _server_logs(backend: OnlineBackend, request: pytest.FixtureRequest) -> Any:
    """mm's and accounts' logs next to the screenshots."""
    yield
    out = ART / request.node.name
    out.mkdir(parents=True, exist_ok=True)
    for name in ("mm", "accounts"):
        (out / f"{name}.log").write_text(backend.log_text(name), encoding="utf-8")


def _party_css(g: Game) -> str:
    """WITH ANYONE > 2V2: the player's own party, a character picked (the match's rules are the
    pair room's: no test rules here). Returns the party's code."""
    g.to_css("twos")
    v = until_view(g, lambda v: v["flags"] & ppom.RF_IN and v["flags"] & ppom.RF_TWOS, "the 2v2 party")
    return v["code"]


def _pair(backend: OnlineBackend, dolphin: Callable[..., DolphinInstance], gpu_backend: str, test: str,
          env: dict[str, str] | None = None) -> tuple[Game, Game]:
    """A and B in one party: B types A's code on its open partner panel."""
    import secrets
    users = [backend.create_user(n, f"PR{'AB'[k]}", email=f"pair{k}-{secrets.token_hex(4)}@example.test")
             for k, n in enumerate(["pra", "prb"])]
    a, b = [_boot(dolphin, f"twos-{x}", backend, u, gpu_backend, test, "gameplay", artifacts=ART, env=env)
            for x, u in zip("ab", users)]
    online_set.both(a.to_main_menu, b.to_main_menu)
    online_set.both(a.to_online_page, b.to_online_page)
    code = _party_css(a)
    _party_css(b)
    hand_to(b, PANEL_X[1], -10.0)
    b.steps("tap A 8", "wait 40")
    _wait(lambda: b.debug_scratch()[4] == 1 and b.debug_scratch()[5] == 8, 5, "the keypad on the open panel")
    _type_code(b, code)
    until_view(b, lambda v: v["flags"] & ppom.RF_TWOS and v["code"] == code, "B in A's party")
    until_view(a, lambda v: all(v["slots"][k]["bits"] & ppom.SLOT_TAKEN for k in (0, 1)), "A sees B")
    return a, b


def _search_both(a: Game, b: Game) -> None:
    for g in (a, b):
        g.press_until("START", lambda g=g: online_set.local(g.c)["lock"]["ready"], f"{g.name} ready")
    _wait(lambda: a.finds() and b.finds(), 30, "the pair searching")


def _pair_room(g: Game) -> dict[str, Any] | None:
    """The pair room's match runs on this game: the session's, SESSION's mode MODE_PAIR."""
    st = online_set.gstatus(g.c)
    if st["phase"] != "running" or g.scene() != "scMelee":
        return None
    se = online_set.session(g.c)
    return se if se["mode"] == ppom.MODE_PAIR else None


def _queues(backend: OnlineBackend, g: Game) -> dict[str, Any]:
    return backend._http("GET", "/v1/rooms", token=g.user.session_token)


def _idle_on_css(g: Game, what: str) -> None:
    """Back on the party's CSS, out of the queue: nothing searching, no session."""
    _wait(lambda: g.scene() == CSS and online_set.local(g.c)["state"] == 0, 60, f"{g.name}: {what}")
    _wait(lambda: g.c.mm_status()["state"] in ("idle", "error"), 20, f"{g.name}: the search over")
    st = g.c.mm_status()
    assert not st["searching"], st
    assert not online_set.local(g.c)["lock"]["ready"], f"{g.name}: still locked in"


def test_pair_room_both_in_then_hold_start(backend: OnlineBackend, dolphin: Callable[..., DolphinInstance],
                                           gpu_backend: str) -> None:
    """The pair searches: both games connect and play the waiting room together, straight from the
    party's CSS (never the waiting room alone first), with no countdown and the waiting room's song
    (the Training Room, two fighters, the same setup on both, a time match with no time limit, the
    Sandbag, the waiting room's banner), counted only in the queue; then A holds START: both are
    back on the party's CSS, out of the queue, without DISCONNECTED."""
    a, b = _pair(backend, dolphin, gpu_backend, "both-in")
    _search_both(a, b)
    ses = [_wait(lambda g=g: _pair_room(g), 120, f"{g.name}: in the pair room") for g in (a, b)]
    setups = [B.read_match_setup(g.c.read_mem) for g in (a, b)]
    assert setups[0] == setups[1], setups
    s = setups[0]
    assert s.stage_kind == TRAINING_ROOM, s
    assert s.game_rule == 0 and s.time_limit_frames <= 0, s   # time, no time limit
    assert sum(1 for p in s.players if p.present) == 2, s
    assert [p["present"] for p in ses[0]["players"]][:2] == [1, 1], ses[0]
    assert ses[0]["rules"]["style"] == "time" and ses[0]["rules"]["timeMinutes"] == 0, ses[0]["rules"]
    # The Sandbag (scratch[5] 0x20000), the banner at the top (scratch[10] 0x40000000).
    _wait(lambda: all(g.debug_scratch()[5] & 0x20000 for g in (a, b)), 30, "the Sandbag in the pair room")
    _wait(lambda: all(g.debug_scratch()[10] & 0x40000000 for g in (a, b)), 30, "the pair room's banner")
    for g in (a, b):
        sc = g.debug_scratch()
        # Straight from the party's CSS (scratch[5] 0x400000), never the waiting room alone (its
        # steps 2, 3 and 7 in scratch[5]'s low bits); no countdown (scratch[10] 0x80000000); the
        # waiting room's song (scratch[5] 0x80000).
        assert sc[5] & 0x400000 and (sc[5] & 0xF) not in (2, 3, 7), (g.name, hex(sc[5]))
        assert sc[10] & 0x80000000, (g.name, hex(sc[10]))
        assert sc[5] & 0x80000, (g.name, hex(sc[5]))
    # The players move once the session runs its inputs (game frame 120): A walks right.
    _wait(lambda: a.c.read_u32(GAME_FRAME) > 200, 30, "the pair room past its start")
    a.shot("00-moving")
    port_a = online_set.local(a.c)["local_port"]
    x0 = next(p.x for p in B.read_players(a.c.read_mem) if p.port == port_a)
    a.steps("stick right 40")
    x1 = next(p.x for p in B.read_players(a.c.read_mem) if p.port == port_a)
    assert x1 is not None and x0 is not None and x1 > x0 + 5, (x0, x1)
    time.sleep(3)
    for g in (a, b):
        g.shot("01-pair-room")
    # Only the queue counts them: both 2v2 tickets wait, nobody plays.
    q = _queues(backend, a)
    assert q["queues"]["twos"] == 2 and q["playing"]["twos"] == 0, q
    # Time mode without a time limit: still on well after the solo room's minute, nothing ended.
    time.sleep(20)
    assert all(_pair_room(g) for g in (a, b)), "the pair room ended by itself"
    for g in (a, b):
        g.shot("02-later")
    # The checksums agree.
    sts = [online_set.gstatus(g.c) for g in (a, b)]
    limit = min(st["current_frame"] for st in sts) - online_set.CONFIRM_MARGIN
    cks = [{r[0]: r[1] for r in g.c.call("gprb_checksums", since=0)["rows"] if r[0] <= limit} for g in (a, b)]
    common = sorted(set(cks[0]) & set(cks[1]))
    bad = [f for f in common if cks[0][f] != cks[1][f]]
    assert common and not bad, (len(common), bad[:5])
    # A holds START: both out.
    a.steps(f"hold START {HOLD_START_FRAMES + 20}")
    for g in (a, b):
        _idle_on_css(g, "back on the CSS after the hold")
        lo = online_set.local(g.c)
        assert not lo["hud_disconnected"], (g.name, lo)
        g.steps("wait 60")
        g.shot("03-back-on-css")
    q = _queues(backend, a)
    assert q["queues"]["twos"] == 0, q
    for g in (a, b):
        log = (g.inst.user_dir / "Logs" / "dolphin.log").read_text(errors="replace")
        assert "the 2v2 search ends with its pair room" in log, g.name
        # No game: no replay recorded (nothing streamed to spectators either).
        assert "replay: recording" not in log, g.name


def test_pair_room_partners_who_cannot_connect_leave_the_queue(
        backend: OnlineBackend, dolphin: Callable[..., DolphinInstance], gpu_backend: str) -> None:
    """The pair's games cannot connect (PPR_TEST_PAIR_NO_P2P: their connect times out): both are
    out of the queue with the connection's error, "Couldn't connect to <partner>."."""
    a, b = _pair(backend, dolphin, gpu_backend, "no-connect", env={"PPR_TEST_PAIR_NO_P2P": "1"})
    _search_both(a, b)

    def failed(g: Game) -> Any:
        st = g.c.mm_status()
        return st if st["state"] == "error" or (st.get("pair_room") or {}).get("error") else None
    for g, other in ((a, "prb"), (b, "pra")):
        st = _wait(lambda g=g: failed(g), 60, f"{g.name}: the pair room's connect failed")
        assert st["pair_room"]["error"] == f"Couldn't connect to {other}.", st
    for g in (a, b):
        _wait(lambda g=g: g.scene() == CSS, 60, f"{g.name}: back on the CSS")
        g.steps("wait 60")
        g.shot("01-could-not-connect")
        assert not g.c.mm_status()["searching"]
    q = _queues(backend, a)
    assert q["queues"]["twos"] == 0, q


def test_pair_room_stays_in_sync_under_play(backend: OnlineBackend, dolphin: Callable[..., DolphinInstance],
                                            gpu_backend: str) -> None:
    """Both partners play the pair room for 80 s (random moves, the Sandbag in the middle): the two
    machines stay in sync. The Sandbag used to be made as soon as its archive had loaded, a frame or
    two apart on the two machines, and 4 of 4 such games desynced (a fighter's effect on one machine
    only); it is made at a fixed frame after the session's start now."""
    a, b = _pair(backend, dolphin, gpu_backend, "in-sync")
    _search_both(a, b)
    for g in (a, b):
        _wait(lambda g=g: _pair_room(g), 120, f"{g.name}: in the pair room")
    _wait(lambda: all(g.debug_scratch()[5] & 0x20000 for g in (a, b)), 30, "the Sandbag in the pair room")

    def play(g: Game, seed: str) -> None:
        port = online_set.local(g.c)["local_port"]
        F.fight([F.Seat(g.c, port, 0, g.name)], 60 * 80, mode="random", seed=seed,
                stop=lambda: online_set.gstatus(g.c)["phase"] != "running")
    th = [threading.Thread(target=play, args=(g, f"sync{k}")) for k, g in enumerate((a, b))]
    for t in th:
        t.start()
    for t in th:
        t.join(200)
    sts = [online_set.gstatus(g.c) for g in (a, b)]
    assert all(st["phase"] == "running" for st in sts), [(st["phase"], st.get("end_reason")) for st in sts]
    limit = min(st["current_frame"] for st in sts) - online_set.CONFIRM_MARGIN
    cks = [{r[0]: r[1] for r in g.c.call("gprb_checksums", since=0)["rows"] if r[0] <= limit} for g in (a, b)]
    common = sorted(set(cks[0]) & set(cks[1]))
    bad = [f for f in common if cks[0][f] != cks[1][f]]
    assert len(common) > 60 * 60 and not bad, (len(common), bad[:5])
    for g in (a, b):
        g.shot("01-after-play")
