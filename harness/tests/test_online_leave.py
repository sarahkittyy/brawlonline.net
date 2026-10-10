"""Closing the game window in the middle of a match (emulation stops; the process exits later).

Dolphin's Qt window close stops emulation first (MainWindow::ForceStop) and the process exits
seconds later. The leave used to go out only at process exit (Online::Client::Shutdown() from
~MainWindow), so the other players stalled on the missing input meanwhile, up to the 7.2 s
silence timeout (testers, 2026-10-10: "freezes for 5 seconds, then the game continues"). The
gameplay backend now leaves the session as soon as emulation stops (GameplayOnlineBackend.cpp).

The harness's `emu_stop` stops emulation as the window close does and keeps the process, so
anything still waiting for process exit shows up here as a stall. (`quit` under DolphinNoGUI
shuts the online client down before it stops emulation, which is why the older leave tests never
saw this; `kill` is a crash, which still takes the silence timeout.)

Run with a Dolphin built from this tree (PPHARNESS_DOLPHIN_DIR) and the game plugin.
"""

from __future__ import annotations

import contextlib
import threading
import time
from typing import Any, Callable

import pytest

from ppharness.backend import OnlineBackend
from ppharness.instance import DolphinInstance

from test_online import _wait
from test_online_game import Game, _boot, _connected_direct, backend  # noqa: F401  (the backend fixture)
from test_rooms_game import (ART, CSS, PANEL_X, _all, create_room, hand_to, pick, to_join_room,
                             type_room_code, until_view)

import online_set  # noqa: E402  (harness/tools, on the path through test_rooms_game)
import ppom  # noqa: E402
from ppharness import brawl as B  # noqa: E402

pytestmark = [pytest.mark.dolphin, pytest.mark.server, pytest.mark.gpu]

# A leave reaches the others within a round trip; GekkoNet's disconnect claims then settle in a
# few more. Well under the 7.2 s silence timeout that the old behaviour waited for.
LEAVE_LIMIT_S = 1.5
# The longest the remaining players' session frame may stand still around the leave.
FREEZE_LIMIT_S = 1.0


def close_window(g: Game) -> None:
    """The game window closed: emulation stops (asynchronously, as MainWindow::ForceStop), the
    process stays."""
    g.c.call("log_mark", text="test: closing the game window (emu_stop)")
    g.c.call("emu_stop")


def stopped(g: Game) -> None:
    _wait(lambda: g.c.status().state == "uninitialized", 30, f"{g.name}: emulation stopped", interval=0.1)


def peer(st: dict[str, Any], slot: int) -> dict[str, Any]:
    return next(p for p in st.get("peers", []) if p["slot"] == slot)


class FrameSampler:
    """Polls each game's session frame and keeps (time, frame) so the longest stretch without a
    new frame (what the player sees as a freeze) can be measured."""

    def __init__(self, games: list[Game], interval: float = 0.02) -> None:
        self.games = games
        self.interval = interval
        self.samples: dict[str, list[tuple[float, int]]] = {g.name: [] for g in games}
        self._stop = threading.Event()
        self._threads = [threading.Thread(target=self._run, args=(g,), daemon=True) for g in games]

    def _run(self, g: Game) -> None:
        while not self._stop.is_set():
            with contextlib.suppress(Exception):
                st = online_set.gstatus(g.c)
                if st.get("phase") == "running":
                    self.samples[g.name].append((time.monotonic(), int(st["current_frame"])))
            time.sleep(self.interval)

    def __enter__(self) -> "FrameSampler":
        for t in self._threads:
            t.start()
        return self

    def __exit__(self, *exc: Any) -> None:
        self._stop.set()
        for t in self._threads:
            t.join(5)

    def longest_freeze(self, name: str, since: float) -> tuple[float, int]:
        """The longest time after `since` the frame stood still, and the frame it stood on."""
        rows = [r for r in self.samples[name] if r[0] >= since]
        worst, at = 0.0, -1
        start_t, start_f = rows[0] if rows else (since, -1)
        for t, f in rows[1:]:
            if f != start_f:
                start_t, start_f = t, f
            elif t - start_t > worst:
                worst, at = t - start_t, f
        return worst, at


@pytest.mark.slow
def test_closing_the_window_in_a_direct_match(backend: OnlineBackend,
                                              dolphin: Callable[..., DolphinInstance],
                                              gpu_backend: str) -> None:
    """1v1: the opponent closes their game window in the middle of a game. The remaining game
    sees `peer left` within a round trip, not after the 7.2 s silence timeout, then the usual
    DISCONNECTED flow takes it back to its online CSS, idle."""
    a, b, _, _ = _connected_direct(backend, dolphin, gpu_backend, "gameplay-close-window", ("nora", "otto"),
                                   stocks=4)
    online_set.wait(lambda: all(online_set.gstatus(g.c)["phase"] == "running" for g in (a, b)), 240,
                    "the match to run on both")
    a.steps("wait 300")
    t0 = time.monotonic()
    close_window(b)
    _wait(lambda: online_set.gstatus(a.c)["disconnected"], 15, "the drop", interval=0.02)
    dropped = time.monotonic() - t0
    stopped(b)
    st = online_set.gstatus(a.c)
    print(f"1v1: drop {dropped:.2f} s after the window closed ({st['error']})")
    assert st["error"] == "peer left", st["error"]
    assert dropped < LEAVE_LIMIT_S, dropped
    log = (b.inst.user_dir / "Logs" / "dolphin.log").read_text(errors="replace")
    assert "emulation is stopping; leaving the session" in log
    _wait(lambda: online_set.local(a.c)["hud_disconnected"] == 1, 5, "DISCONNECTED in the HUD", interval=0.05)
    a.shot("01-dropped-in-match")
    _wait(lambda: online_set.scene(a.c) == online_set.CSS, 60, "back on the CSS")
    _wait(lambda: a.c.mm_status()["state"] == "idle", 30, "idle")
    a.steps("wait 60")
    a.shot("02-back-on-css-idle")


@pytest.mark.slow
def test_closing_the_window_in_a_three_player_room(backend: OnlineBackend,
                                                   dolphin: Callable[..., DolphinInstance],
                                                   gpu_backend: str) -> None:
    """Testers, 2026-10-10: three players in a room, one closes their game window in the middle
    of the game; the other two froze for about 5 s, then their game went on. Now the two see
    `peer left` within a round trip, the closed player's fighter is removed on both from the
    same frame, their frame never stands still for long, and they play the game out with
    matching confirmed checksums."""
    test = "close-window-3p"
    import secrets
    users = [backend.create_user(n, f"RW{'ABC'[k]}P", email=f"roomcw{k}-{secrets.token_hex(4)}@example.test")
             for k, n in enumerate(["rcwa", "rcwb", "rcwc"])]
    a, b, c = gs = [_boot(dolphin, f"roomcw-{x}", backend, u, gpu_backend, test, "gameplay", artifacts=ART)
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
        B.write_rules(g.c, stocks=2, minutes=2, items_off=True)
        ppom.allow_test_rules(g.c)
    for k, g in ((1, b), (2, c)):
        type_room_code(g, code)
        vk = until_view(g, lambda v: v["flags"] & ppom.RF_IN, f"player {k + 1} in the room")
        assert vk["local_port"] == k, vk
    until_view(a, lambda v: all(v["slots"][p]["bits"] & ppom.SLOT_TAKEN for p in range(3)), "all players")
    for g in gs:
        g.steps("wait 60")
        g.steps("tap START 8", "wait 10")
    online_set.wait(lambda: all(online_set.gstatus(g.c)["phase"] in ("running", "error", "ended") for g in gs),
                    240, "the room's game running on all three")
    st = [online_set.gstatus(g.c) for g in gs]
    assert all(s["phase"] == "running" for s in st), [(s["phase"], s.get("error")) for s in st]
    time.sleep(6)
    for g in gs:
        g.shot("01-three-playing")

    rest = [a, b]
    with FrameSampler(rest) as frames:
        time.sleep(1)
        t0 = time.monotonic()
        close_window(c)
        _wait(lambda: all(peer(online_set.gstatus(g.c), 2)["left"] for g in rest), 15,
              "A and B see P3 leave", interval=0.02)
        left = time.monotonic() - t0
        stopped(c)
        _wait(lambda: all(peer(online_set.gstatus(g.c), 2)["gone_frame"] >= 0 for g in rest), 10,
              "P3's fighter gone on A and B", interval=0.05)
        time.sleep(4)
    st = [online_set.gstatus(g.c) for g in rest]
    reasons = [peer(s, 2)["left_reason"] for s in st]
    gone = [peer(s, 2)["gone_frame"] for s in st]
    freezes = {g.name: frames.longest_freeze(g.name, t0) for g in rest}
    print(f"3 players: P3 left after {left:.2f} s ({reasons}), gone from frame {gone}, "
          f"longest freeze {[f'{n} {s:.2f} s at {f}' for n, (s, f) in freezes.items()]}")
    for g in rest:
        g.shot("02-after-the-close")
    assert reasons == ["peer left", "peer left"], reasons
    assert left < LEAVE_LIMIT_S, left
    assert gone[0] == gone[1], gone
    assert all(s["phase"] == "running" and s["players"] == 2 for s in st), \
        [(s["phase"], s.get("players"), s.get("error")) for s in st]
    assert all(sec < FREEZE_LIMIT_S for sec, _ in freezes.values()), freezes

    # B walks off; A wins. The two agree on every confirmed frame, through the leave.
    from ppharness import flows as F
    with contextlib.suppress(Exception):
        F.fight([F.Seat(b.c, 1, 0, "B")], 60 * 60, mode="selfdestruct", seed="cw",
                stop=lambda: online_set.gstatus(b.c)["phase"] != "running")
    online_set.wait(lambda: all(online_set.gstatus(g.c)["phase"] != "running" for g in rest), 240,
                    "the game to end")
    st = [online_set.gstatus(g.c) for g in rest]
    limit = min(s["current_frame"] for s in st) - online_set.CONFIRM_MARGIN
    cks = [{r[0]: r[1] for r in g.c.call("gprb_checksums", since=0)["rows"] if r[0] <= limit} for g in rest]
    common = sorted(set(cks[0]) & set(cks[1]))
    bad = [f for f in common if cks[0][f] != cks[1][f]]
    print(f"end {[s['end_reason'] for s in st]}, checksums compared {len(common)}, mismatches {len(bad)}")
    assert common and not bad, (len(common), bad[:5])
    _wait(lambda: all(g.scene() == CSS for g in rest), 180, "A and B back on the room's CSS")
    for g in rest:
        g.steps("wait 60")
        g.shot("03-back-on-the-css")
