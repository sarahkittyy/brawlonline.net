"""Replay stats data (docs/design/replays.md §0, "Stats data"): what a .brep records for the stats page.

A two-machine rollback session (``harness/tools/gprb_session.py``) on Final Destination, P1's Fox
in the "lcancel" fight mode (``flows.Fighter``: short-hop nairs, an L press 4 frames before every
other landing) and P2's Marth standing still, for 1,800 frames. Both machines' replays are decoded
here:

- P+'s L-cancel counters (FTSP block ``lCancel``, docs/brawl-memory-map.md "Per player"): Fox's
  aerial landings grow by one on exactly the frames Fox enters ``Landing_Attack_Air`` (0x18) and
  on no other; the L-cancelled ones grow only with them, for some landings but not all. Marth's
  stay 0.
- Both machines' files hold the same fighter memory for the frames both recorded (the only check
  of the recorded fighter memory under real rollbacks; test_replay.py compares the checksums).

What the stats make of the recorded states is the launcher's (``launcher/src/replays/stats``).

Run: ``pytest -m dolphin tests/test_replay_stats.py`` (about 3 minutes).
"""

from __future__ import annotations

import json
import struct
import sys
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "harness" / "tools"))

from ppharness import replay as R  # noqa: E402

pytestmark = [pytest.mark.dolphin, pytest.mark.slow]

LANDING_ATTACK_AIR = 0x18


def read_frames(rep: R.Replay, index: int) -> int:
    """The index-th fighter's frames up to the last one it was read on (the recorder writes zeros
    after it: here on the frame the session was stopped on)."""
    spec = {b["name"]: b for b in rep.fighter_spec["blocks"]}
    blk, data, entry = rep.fighter_block, rep.fighters[index], spec["entry"]["offset"] + 0x04
    frames = len(data) // blk
    while frames and struct.unpack_from(">I", data, (frames - 1) * blk + entry)[0] == 0:
        frames -= 1
    return frames


def fighter_columns(rep: R.Replay, index: int) -> tuple[list[int], list[tuple[float, float]]]:
    """Per frame, for the index-th recorded fighter: its status kind and P+'s L-cancel counters
    (aerial landings, L-cancelled)."""
    spec = {b["name"]: b for b in rep.fighter_spec["blocks"]}
    assert "lCancel" in spec and spec["lCancel"]["size"] == 8, spec.keys()
    blk, data = rep.fighter_block, rep.fighters[index]
    st, lc = spec["status"]["offset"] + 0x34, spec["lCancel"]["offset"]
    frames = read_frames(rep, index)
    status = [struct.unpack_from(">i", data, f * blk + st)[0] for f in range(frames)]
    counts = [struct.unpack_from(">2f", data, f * blk + lc) for f in range(frames)]
    return status, counts


def entered(status: list[int], kind: int) -> list[int]:
    return [f for f in range(1, len(status)) if status[f] == kind and status[f - 1] != kind]


def grew(values: list[float]) -> list[int]:
    return [f for f in range(1, len(values)) if values[f] > values[f - 1]]


def test_replay_records_p_plus_l_cancel_counters(tmp_path: Path) -> None:
    import gprb_session

    out, reps = tmp_path / "session.json", tmp_path / "replays"
    rc = gprb_session.main([
        "--p1", "fox", "--p2", "marth", "--stage", "final_destination", "--modes", "lcancel,idle",
        "--preset", "typical", "--cpu", "dc", "--frames", "1800", "--name-prefix", "replaystats",
        "--save-replays", str(reps), "--json", str(out),
    ])
    assert rc == 0
    rep = json.loads(out.read_text())[0]
    assert rep.get("error") is None, rep.get("error")
    assert rep["checksums"]["mismatches"] == 0, rep["checksums"]
    files = sorted(reps.glob("*.brep"))
    assert len(files) == 2, files
    a, b = (R.read(p) for p in files)

    for r in (a, b):
        assert r.fighter_ports == 0b11, r.fighter_ports
        status, counts = fighter_columns(r, 0)
        landings = entered(status, LANDING_ATTACK_AIR)
        total, cancelled = [c[0] for c in counts], [c[1] for c in counts]
        assert len(landings) >= 10, len(landings)
        assert grew(total) == landings, (grew(total)[:10], landings[:10])
        assert all(total[f] - total[f - 1] == 1 for f in landings)
        assert set(grew(cancelled)) <= set(landings)
        assert 0 < len(grew(cancelled)) < len(landings), (len(grew(cancelled)), len(landings))
        assert total[-1] == total[landings[0] - 1] + len(landings)
        marth = fighter_columns(r, 1)[1]
        assert all(c == (0.0, 0.0) for c in marth), set(marth)
        print(f"{r.path.name}: {len(landings)} aerial landings, {len(grew(cancelled))} L-cancelled, "
              f"{len(status)} frames")

    # The same game on both machines: the same fighter memory where both have the frame.
    assert a.first_frame == b.first_frame
    n = min(read_frames(r, i) for r in (a, b) for i in range(2))
    assert n > 1500, n
    for i in range(2):
        assert a.fighters[i][:n * a.fighter_block] == b.fighters[i][:n * b.fighter_block], i
