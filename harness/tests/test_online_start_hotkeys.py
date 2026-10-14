"""Start-of-match hotkeys online: P+ Samus starts in ice mode when the side taunt is held during
her entrance (PM 3.6). Players reported it never worked online.

The game reads such holds while the fighters enter (game frames ~2-122), all before the session's
start_frame (120), when no input of the match can be taken: every port ran neutral there. Now each
machine samples its player's held buttons when it reaches the first barrier (the match appears
after the load), the host fixes every port's buttons once all are in and the joiners take them with
its sync block, and every machine runs the countdown with them (GameplaySession.cpp
CountdownSlots; docs/gameplay-rollback-status.md Phase 27).

A Direct game, Samus against Samus: A (the host) holds D-pad right from its search through the
load, B holds nothing. After GO both forward smash: A's must be ice, B's fire, on both machines
(screenshots), with every confirmed frame agreeing. Then a fresh Dolphin plays A's replay of the
game back (the countdown's buttons are in its SETP) with every frame's checksum equal to the
recorded one, and shows the same smashes.

Screenshots: run/artifacts/game-bridge/start-hotkeys/ (live), run/artifacts/replays/start-hotkeys/
(playback).
"""

from __future__ import annotations

import re
import sys
from pathlib import Path
from typing import Any, Callable

import pytest

from ppharness.backend import OnlineBackend
from ppharness.client import PadInput
from ppharness.instance import DolphinInstance
from ppharness import brawl as B

sys.path.insert(0, str(Path(__file__).resolve().parent))
from test_online_game import Game, _confirmed_checksums, _connected_direct, _wait, backend  # noqa: E402,F401
from test_replay import _recording, _status, _viewer  # noqa: E402
import online_set  # noqa: E402

pytestmark = [pytest.mark.dolphin, pytest.mark.server, pytest.mark.gpu, pytest.mark.slow]

DPAD_RIGHT = 0x0002   # gfPadStatus +0x0 (the bitfield's m_dpadRight; BrawlHeaders' #define says 0x1)
SMASH_FRAME = 150     # session frame of the forward smashes (game frame 270, after GO at 214)
SHOT_OFFSETS = (10, 14, 18, 24)   # frames into the smash for the screenshots


def _pick_samus(g: Game) -> None:
    B.css_pick_character(g.c, 0, B.CSS_ID["samus"])
    B.step(g.c, 10)


def _hold_side_taunt(g: Game) -> None:
    """From A's search on (before B's search starts the game) through the load: D-pad right."""
    g.c.pad_set(0, buttons=["DRIGHT"])


def _log(g: Game) -> str:
    return (g.inst.user_dir / "Logs" / "dolphin.log").read_text(errors="replace")


def _countdown_buttons(g: Game) -> list[int]:
    m = re.findall(r"gprb: countdown buttons (0x[0-9a-f]+) (0x[0-9a-f]+) (0x[0-9a-f]+) (0x[0-9a-f]+)", _log(g))
    assert m, (g.name, "no countdown buttons logged")
    return [int(v, 16) for v in m[-1]]


def _smash_and_shoot(g: Game, frame_of: Callable[[], int], prefix: str) -> None:
    """Forward smash (C-stick right) at SMASH_FRAME, screenshots during it."""
    _wait(lambda: frame_of() >= SMASH_FRAME, 60, f"{g.name}: frame {SMASH_FRAME}")
    g.c.pad_script(0, [PadInput(c=(255, 128), hold=6), PadInput(hold=1)])
    last = 0
    for off in SHOT_OFFSETS:
        g.steps(f"wait {off - last}")
        last = off
        g.shot(f"{prefix}-fsmash-{off:02d}")


def test_samus_starts_in_ice_mode_online(backend: OnlineBackend, dolphin: Callable[..., DolphinInstance],
                                         gpu_backend: str) -> None:
    a, b, _, _ = _connected_direct(backend, dolphin, gpu_backend, "start-hotkeys", ("ivy", "flint"),
                                   stocks=1, on_css=(_pick_samus, _pick_samus),
                                   after_first_search=_hold_side_taunt)
    online_set.wait(lambda: all(online_set.gstatus(g.c)["phase"] == "running" for g in (a, b)), 240,
                    "the match to run on both")
    a.c.pad_set(0)   # released once the countdown's buttons are fixed (before GO: no side taunt)
    host, join = (a, b) if online_set.local(a.c)["local_port"] == 0 else (b, a)
    port_a = online_set.local(a.c)["local_port"]
    port_b = online_set.local(b.c)["local_port"]
    for g in (a, b):
        held = _countdown_buttons(g)
        print(g.name, "countdown buttons", [hex(v) for v in held])
        assert held[port_a] & DPAD_RIGHT, (g.name, held)
        assert held[port_b] == 0, (g.name, held)
    assert _countdown_buttons(a) == _countdown_buttons(b)

    def frame(g: Game) -> Callable[[], int]:
        return lambda: online_set.gstatus(g.c)["current_frame"]
    import threading
    th = [threading.Thread(target=_smash_and_shoot, args=(g, frame(g), "live")) for g in (a, b)]
    for t in th:
        t.start()
    for t in th:
        t.join()
    a.steps("wait 120")
    st = [online_set.gstatus(g.c) for g in (host, join)]
    assert all(s["phase"] == "running" for s in st), st
    ck = _confirmed_checksums(host, join)
    print("confirmed checksums", ck)
    assert ck["mismatches"] == 0 and ck["compared"] > 100, ck
    # Game over: A quits from the pause screen (Direct goes back to the CSS).
    b.steps("wait 30")
    a.steps("tap START 8", "wait 60", "hold L+R+A+START 90")
    online_set.wait(lambda: not any(g.c.call("replay_recording")["recording"] for g in (a, b)), 120,
                    "both recordings to end")
    fa = _recording(a)
    # The frame the holder's smash input arrived (C-stick X, gfPadStatus +0x32), in the file.
    smash = next(f for f in range(fa.first_frame, fa.last_frame + 1) if fa.pad(f, port_a)[0x32] != 0)
    print("smash input at session frame", smash)
    for g in (a, b):
        g.c.mm_cancel()

    v = _viewer(dolphin, backend, gpu_backend, "start-hotkeys")
    v.c.call("replay_play", path=str(fa.path), hold_at_end=True)
    _wait(lambda: _status(v)["started"], 240, "the replay's match to start")
    _wait(lambda: _status(v)["shown"] >= smash + 60, 240, "the smashes played")
    for off in SHOT_OFFSETS:
        v.c.call("replay_control", action="pause")
        v.c.call("replay_control", action="seek", arg=smash + off)
        _wait(lambda off=off: (lambda s: s["paused"] and not s["seeking"] and s["shown"] == smash + off)(
            _status(v)), 120, f"playback at the smash +{off}")
        v.shot(f"replay-fsmash-{off:02d}")
    v.c.call("replay_control", action="play")
    st = _wait(lambda: (lambda s: s if s["at_end"] or s["phase"] == "error" else None)(_status(v)), 600,
               "the replay's end")
    print("playback", {k: st.get(k) for k in ("phase", "desync_frame", "frames_checked", "last_frame")})
    assert st["phase"] != "error", st
    assert st["desync_frame"] == -1, st
    assert st["frames_checked"] >= fa.last_frame - fa.first_frame - 2, (st, fa.first_frame, fa.last_frame)
    v.c.call("replay_control", action="play")
