"""Sudden death online (bug reports #215 and #216, 2026-11-01: "sudden death desyncs different
spawns"): both players lose their last stock on the same frame, Brawl goes on to sudden death in
the same match, and the rollback session plays on through it.

Before, the session ended at the tie's game set (and reported a winner by damage), and each
machine then played the sudden death alone, with only its own player's inputs and its own spawn
points. Now the session goes on: it waits at the loop top while the sudden death's music stream
is prepared (it took one frame more on the machine whose song was on the SD card, which shuffled
the spawn points a frame later there), the match ends at the sudden death's own game set, both
players' replays have the sudden death, and a fresh Dolphin plays one back to its end.

The double KO is the plugin's test hook (CFG_TEST_GONE with TEST_GONE_KO_ONLY: both ports lose
their last stock at game frame 400, only while they have 2 or more stocks, so not again in the
sudden death). Screenshots and replays: run/artifacts/sudden-death/.
"""

from __future__ import annotations

import re
import shutil
import sys
from pathlib import Path
from typing import Any, Callable

import pytest

from ppharness.backend import OnlineBackend
from ppharness.instance import DolphinInstance

sys.path.insert(0, str(Path(__file__).resolve().parent))
from test_online_game import ROOT, _connected_direct, _log, backend  # noqa: E402,F401
from test_replay import _play, _recording, _viewer  # noqa: E402
import online_set  # noqa: E402
import ppom  # noqa: E402
from ppharness import brawl as B  # noqa: E402

ARTIFACTS = ROOT / "run" / "artifacts" / "sudden-death"

pytestmark = [pytest.mark.dolphin, pytest.mark.server, pytest.mark.gpu]

KO_FRAME = 400   # game frame of the double KO (after GO)


def _ko_both(g: Any) -> None:
    ppom.set_test_gone(g.c, ppom.find_block(g.c), KO_FRAME, 0b11, ko_only=True)


@pytest.mark.slow
def test_a_double_ko_plays_its_sudden_death_in_the_session(
        backend: OnlineBackend, dolphin: Callable[..., DolphinInstance], gpu_backend: str) -> None:
    a, b, ua, ub = _connected_direct(backend, dolphin, gpu_backend, "sudden-death", ("suda", "sudb"),
                                     on_css=(_ko_both, _ko_both))
    players = [a, b]
    online_set.wait(lambda: all(online_set.gstatus(g.c)["phase"] == "running" for g in players), 240,
                    "the game running on both")
    online_set.wait(lambda: all("goes on to sudden death" in _log(g) for g in players), 90,
                    "the tie on both")
    online_set.wait(lambda: all("gprb: sudden death from frame" in _log(g) for g in players), 60,
                    "the sudden death on both")
    for g in players:
        g.shot("sudden-death")
        assert online_set.gstatus(g.c)["phase"] == "running", online_set.gstatus(g.c)
    # The tie's game set ended nothing, and both started the sudden death on the same frame.
    starts = [re.findall(r"gprb: sudden death from frame (\d+)", _log(g)) for g in players]
    assert starts[0] == starts[1] and len(starts[0]) == 1, starts

    # The sudden death: the host's player stands still, the other walks off the stage.
    host, join = online_set.roles(players)
    rep = online_set.play_game(players, 1, timeout=240, winner=host)
    print({k: rep.get(k) for k in ("frames", "end", "checksums")}, flush=True)
    assert rep["end"] == ["game set", "game set"], rep
    assert rep["checksums"]["mismatches"] == 0, rep["checksums"]
    assert min(rep["frames"]) > int(starts[0][0]) + 120, rep["frames"]
    # Both report the same winner, the sudden death's (1 stock left, the other none), not a draw
    # nor a winner by damage.
    won = []
    for g in players:
        assert not re.search(r"desync at frame|the game states differ", _log(g)), g.name
        won.append(re.findall(
            r"gprb lobby: game 1 winner (\d+) stage pickers 0x\w+ \(stocks/damage: (.*)\)", _log(g)))
    assert won[0] == won[1] and len(won[0]) == 1, won
    port = int(won[0][0][0])
    assert port in (0, 1), won
    assert f"P{port + 1} 1/" in won[0][0][1] and f"P{2 - port} 0/" in won[0][0][1], won

    # Both players' replays have the whole match, the sudden death too, and agree.
    online_set.wait(lambda: not any(g.c.call("replay_recording")["recording"] for g in players), 60,
                    "both recordings to end")
    fa, fb = _recording(a), _recording(b)
    assert fa.uid == fb.uid
    assert fa.endg and fb.endg and fa.endg["endMethod"] == fb.endg["endMethod"] == "game", (fa.endg, fb.endg)
    final = min(fa.endg["finalFrame"], fb.endg["finalFrame"])
    assert final > int(starts[0][0]) + 120, (final, starts)
    n = final - fa.first_frame + 1
    assert fa.slots[:n] == fb.slots[:n]
    assert fa.checksums[:n] == fb.checksums[:n]
    out = ARTIFACTS
    out.mkdir(parents=True, exist_ok=True)
    for f in (fa, fb):
        shutil.copy2(f.path, out / f.path.name)
    for g in players:
        g.c.mm_cancel()

    # A fresh Dolphin plays it back to its end, through the sudden death, every frame's checksum
    # equal to the recorded one (the playback has the live games' rules and test hook).
    v = _viewer(dolphin, backend, gpu_backend, "sudden-death")
    B.write_rules(v.c, stocks=2, minutes=2, items_off=True)
    _ko_both(v)
    st = _play(v, fa.path)
    assert st["phase"] != "error", st
    assert st["desync_frame"] == -1, st
    assert st["frames_checked"] >= fa.endg["finalFrame"] - fa.first_frame, st
    assert re.search(r"gprb: sudden death from frame", _log(v)), "the playback's sudden death"
