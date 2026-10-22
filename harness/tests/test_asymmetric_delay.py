"""Input delay is a personal tradeoff (Skullgirls/GGPO style): raising your own delay must
smooth your own screen, at the cost of your own responsiveness -- it must not instead smooth
your opponent's screen while yours stays the same.

Two two-client GekkoNet sessions (``harness/tools/gprb_session.py``) on the same simulated
link (140 ms round trip, ~4.2 frames one way, seeded random fight inputs, same characters and
stage) differ only in host A's delay: 1 frame in the first, 8 in the second. Joiner B stays at
2 frames throughout. Under the fixed behaviour each side rolls back about
``one-way latency - own delay`` frames, so:

- A's rollback rate must fall sharply from the 1f match to the 8f match;
- B's rollback rate must stay in the same range in both matches (its own delay never changed);
- neither match desyncs: every confirmed frame's checksum agrees on both machines.

Run: ``pytest -m dolphin tests/test_asymmetric_delay.py`` (two matches, about 5 minutes).
"""

from __future__ import annotations

import json
import sys
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))

from ppharness import netsim  # noqa: E402

pytestmark = [pytest.mark.dolphin, pytest.mark.slow]

# The experiment's link, registered under its own name so the presets players use stay as they are.
netsim.PRESETS["asym-delay"] = netsim.NetProfile.from_rtt(140, 4, name="asym-delay")


def _run_match(tmp_path: Path, delay_a: int) -> dict:
    import gprb_session

    out = tmp_path / f"a{delay_a}.json"
    rc = gprb_session.main([
        "--preset", "asym-delay", "--cpu", "sc",
        "--p1", "fox", "--p2", "falco", "--stage", "battlefield",
        "--frames", "3000", "--timeout", "900",
        "--delay-a", str(delay_a), "--delay-b", "2",
        "--name-prefix", "asym", "--json", str(out),
    ])
    assert rc == 0
    return json.loads(out.read_text())[0]


def _rate(status: dict) -> float:
    """Rollbacks per 1000 session frames (gprb_status counters)."""
    return status["rollbacks"] / max(1, status["current_frame"]) * 1000.0


def test_own_delay_smooths_own_screen(tmp_path: Path) -> None:
    reps = [_run_match(tmp_path, 1), _run_match(tmp_path, 8)]
    for rep, delay_a in zip(reps, (1, 8)):
        a, b = rep["status"]
        assert (a["input_delay"], b["input_delay"]) == (delay_a, 2), rep["status"]
        assert rep["checksums"]["compared"] > 1000 and rep["checksums"]["mismatches"] == 0, rep["checksums"]
        assert a["desyncs_detected"] == 0 and b["desyncs_detected"] == 0, rep["status"]

    a1, b1 = (_rate(s) for s in reps[0]["status"])
    a8, b8 = (_rate(s) for s in reps[1]["status"])
    # A's own screen: at 8f on a ~4.2f one-way link A should need almost no prediction,
    # where at 1f it rolls back on a large share of frames.
    assert a8 < 0.5 * a1, f"A's rollback rate barely moved with its own delay: {a1:.1f} -> {a8:.1f} per 1000 frames"
    # B's screen: B's delay never changed, so B keeps rolling back about the same in both
    # matches (it must not be the one A's 8f made smooth, as the old sign did: B went to 0).
    assert b8 > 0.4 * b1, f"B's rollback rate collapsed when only A's delay rose: {b1:.1f} -> {b8:.1f} per 1000 frames"
