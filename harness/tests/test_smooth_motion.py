"""Smooth Motion takes a frame of input delay for its pictures.

Its in-between pictures put each frame's own picture a part of a frame later (8.3 ms at 120 Hz),
so a match with it on plays one frame (16.7 ms) of input delay lower than the player's setting
gives: inputs show sooner than with it off. A match set to 0 frames has no frame to take and draws
the pictures all the same (``Gprb::Session::SmoothMotionDelay``).

Two-client GekkoNet sessions (``harness/tools/gprb_session.py``) in which only joiner B has the
setting on (120 Hz); host A never does:

- on a real video backend in dual core (what the pictures need), B set to 2 frames plays 1 and
  draws the pictures; A, also set to 2, plays 2;
- B set to 0 frames plays 0, and still draws the pictures;
- on the Null backend, which cannot draw them, B's 2 frames stay 2;
- no session desyncs: every confirmed frame's checksum agrees on both machines.

Run: ``pytest -m dolphin tests/test_smooth_motion.py`` (three short matches, about 5 minutes).
"""

from __future__ import annotations

import json
import sys
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))

pytestmark = [pytest.mark.dolphin, pytest.mark.slow]


def _run_match(tmp_path: Path, name: str, video: str, cpu: str, delay_b: int) -> dict:
    import gprb_session

    out = tmp_path / f"{name}.json"
    rc = gprb_session.main([
        "--preset", "typical", "--cpu", cpu, "--video", video,
        "--p1", "fox", "--p2", "falco", "--stage", "battlefield",
        "--frames", "1200", "--timeout", "900",
        "--delay-a", "2", "--delay-b", str(delay_b),
        "--config-b", "Dolphin.Online.SmoothMotion=120",
        "--name-prefix", f"smooth-{name}", "--json", str(out),
    ])
    assert rc == 0
    rep = json.loads(out.read_text())[0]
    assert not rep.get("error"), rep.get("error")
    assert all(st["desyncs_detected"] == 0 for st in rep["status"]), rep["status"]
    assert rep["checksums"]["compared"] > 500 and rep["checksums"]["mismatches"] == 0, rep["checksums"]
    return rep


def _delays(rep: dict) -> list[tuple[int, bool]]:
    """Each player's delay, and whether their game draws the pictures."""
    return [(st["input_delay"], st["smooth_motion"]) for st in rep["status"]]


@pytest.mark.gpu
def test_a_match_with_smooth_motion_plays_a_frame_less(tmp_path: Path, gpu_backend: str) -> None:
    rep = _run_match(tmp_path, "two", gpu_backend, "dc", 2)
    assert _delays(rep) == [(2, False), (1, True)], rep["status"]


@pytest.mark.gpu
def test_zero_frames_keeps_the_pictures(tmp_path: Path, gpu_backend: str) -> None:
    rep = _run_match(tmp_path, "zero", gpu_backend, "dc", 0)
    assert _delays(rep) == [(2, False), (0, True)], rep["status"]


def test_no_pictures_no_frame_taken(tmp_path: Path) -> None:
    rep = _run_match(tmp_path, "null", "Null", "sc", 2)
    assert _delays(rep) == [(2, False), (2, False)], rep["status"]
