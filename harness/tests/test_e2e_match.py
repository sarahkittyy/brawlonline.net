"""End-to-end: boot -> CSS -> Fox vs Falco -> stage -> match -> ~20 s of play, on the real build.

* offline, one instance driving both ports (Battlefield, then Final Destination);
* two-instance netplay in fixed-delay mode (each instance drives only its own port, only the host
  picks the stage), with banned-state checks during the match;
* the same over rollback netplay, which is known to freeze at scene transitions: that test
  records where and how it fails (artifacts) and xfails instead of failing.

Artifacts (screenshots, diagnose-*.json with status, netplay_status and log tails) go to
``run/artifacts/<test name>/``; they are deleted when a test passes unless ``--keep``.

Run:  pytest -m dolphin tests/test_e2e_match.py      (needs PPHARNESS_DOLPHIN_DIR or the build)
"""

from __future__ import annotations

import os
import shutil
import sys
import time
from pathlib import Path

import pytest

from ppharness import brawl as B
from ppharness import flows as F
from ppharness import paths
from ppharness.instance import DolphinInstance, InstanceConfig
from ppharness.session import two_player_netplay

pytestmark = pytest.mark.dolphin

P1, P2 = "fox", "falco"
PLAY_FRAMES = 1200          # ~20 s of match
# Screenshots need a real backend (headless D3D11 on Windows). Elsewhere default to Null (no GPU in
# containers or CI; screenshots are then skipped, they are best effort); PPHARNESS_E2E_VIDEO overrides.
VIDEO = os.environ.get("PPHARNESS_E2E_VIDEO") or ("D3D11" if sys.platform == "win32" else "Null")


@pytest.fixture
def art(request: pytest.FixtureRequest):
    root = paths.workspace_root() / "run" / "artifacts" / request.node.name.replace("/", "_")
    shutil.rmtree(root, ignore_errors=True)
    a = F.Artifacts(root)
    yield a
    a.write_notes()
    failed = any(getattr(request.node, f"rep_{w}", None) is not None and getattr(request.node, f"rep_{w}").failed
                 for w in ("setup", "call"))
    if not failed and not getattr(request.node, "keep_artifacts", False) and not request.config.getoption("--keep"):
        shutil.rmtree(root, ignore_errors=True)


def _check_setup(c, picks, stage: str) -> None:
    want = F.expected_char_kinds(picks)
    problems = B.verify_match_setup(c.read_mem, want, B.STAGE_KIND[stage], stocks=4, minutes=8)
    assert not problems, problems
    st = B.read_match_state(c.read_mem)
    assert st.in_match and st.started
    # 8:00 timer, counting down from 28800 frames.
    assert st.remaining_frames is not None and 28800 - 600 < st.remaining_frames <= 28800
    assert st.remaining_frames + (st.frames_elapsed or 0) == 28800
    for p in st.players:
        assert p.stocks == 4 and p.damage == 0.0, p
    assert {p.port: p.character for p in st.players} == want


def _assert_damage_changed(logs, victim_port: int) -> None:
    series = [d for lg in logs for d in lg.damage_series(victim_port)]
    assert series, "no damage samples"
    assert max(series) > 0.0, f"P{victim_port + 1} never took damage: {series}"


@pytest.mark.parametrize("stage", ["battlefield", "final_destination"])
def test_offline_match(dolphin, art, stage):
    inst = dolphin(f"e2e-offline-{stage}", config=InstanceConfig(video_backend=VIDEO, cpu_thread=False),
                   connect_timeout=90)
    inst.start()
    c = inst.client
    s1, s2 = F.Seat(c, 0), F.Seat(c, 1)
    picks = [(s1, P1), (s2, P2)]
    try:
        F.to_css([s1, s2])
        F.pick_characters(picks)
        art.shoot(c, "css-picked")
        F.start_and_pick_stage(s1, stage)
        B.wait_match_start(c)
        _check_setup(c, picks, stage)
        art.shoot(c, "go")
        assert B.check_banned(c.read_mem) == []
        logs = F.play([(s1, "chase"), (s2, "random")], PLAY_FRAMES, seed=1, stage_kind=B.STAGE_KIND[stage])
        art.shoot(c, "after-play")
        assert logs[0].frames >= PLAY_FRAMES - 30
        _assert_damage_changed(logs, victim_port=1)
        assert B.check_banned(c.read_mem) == []
    except BaseException:
        F.diagnose([inst], art)
        raise


def _netplay_match(s, art, stage: str, phases: list[str]) -> list:
    hc, jc = s.clients
    host, joiner = F.netplay_seat(hc, "host"), F.netplay_seat(jc, "joiner")
    assert (host.port, joiner.port) == (0, 1)
    picks = [(host, P1), (joiner, P2)]
    phases.append("boot->css")
    F.to_css([host, joiner])
    phases.append("css pick")
    F.pick_characters(picks)          # each instance drives only its own port, concurrently
    art.shoot(hc, "host-picked")
    art.shoot(jc, "joiner-picked")
    phases.append("css->sss (host Start)")
    B.css_start(hc, host.pp)
    phases.append("sss pick (host)")
    B.sss_pick_stage(hc, B.STAGE_KIND[stage], host.pp)
    phases.append("sss->match")
    F.wait_match_started([hc, jc])
    phases.append("verify setup")
    for c in (hc, jc):
        _check_setup(c, picks, stage)
    art.shoot(hc, "host-go")
    art.shoot(jc, "joiner-go")
    banned = {"host": B.check_banned(hc.read_mem), "joiner": B.check_banned(jc.read_mem)}
    assert banned == {"host": [], "joiner": []}, banned
    phases.append("play")
    checks: list[dict] = []
    stop_at = time.monotonic() + 120

    def watch() -> bool:
        # Banned-state checks during the match, on both instances, every few seconds.
        if not checks or time.monotonic() - checks[-1]["t"] > 3:
            checks.append({"t": time.monotonic(), "host": B.check_banned(hc.read_mem),
                           "joiner": B.check_banned(jc.read_mem)})
        return time.monotonic() > stop_at

    logs = F.play([(host, "chase"), (joiner, "random")], PLAY_FRAMES, seed=2, stage_kind=B.STAGE_KIND[stage],
                  stop=watch)
    phases.append("after play")
    art.shoot(hc, "host-after")
    art.shoot(jc, "joiner-after")
    bad = [c for c in checks if c["host"] or c["joiner"]]
    assert not bad, bad
    assert len(checks) >= 3
    _assert_damage_changed(logs, victim_port=1)
    # Both sides see the same match: damage and stocks agree once inputs are settled.
    for c in (hc, jc):
        B.neutral(c, 0)
    time.sleep(1.0)
    seen = []
    for c in (hc, jc):
        seen.append({p.port: (p.stocks, p.character) for p in B.read_players(c.read_mem)})
    assert seen[0] == seen[1], seen
    return logs


def test_netplay_fixed_delay_match(request, dolphin_exe, art):
    phases: list[str] = []
    keep = request.config.getoption("--keep")
    with two_player_netplay("lan", rollback=False, exe=dolphin_exe, keep=keep, name="e2e-np-fd",
                            config=InstanceConfig(video_backend=VIDEO, cpu_thread=False)) as s:
        try:
            _netplay_match(s, art, "battlefield", phases)
        except BaseException:
            art.note(f"failed during phase: {phases[-1] if phases else '?'}")
            F.diagnose(s.instances, art)
            raise


def test_netplay_rollback_match(request, dolphin_exe, art):
    """Rollback mode is known to freeze at scene transitions. Records the failing phase, both
    netplay_status results, log tails and screenshots in the artifacts, then xfails."""
    phases: list[str] = []
    keep = request.config.getoption("--keep")
    failure = None
    with two_player_netplay("lan", rollback=True, exe=dolphin_exe, keep=keep, name="e2e-np-rb",
                            config=InstanceConfig(video_backend=VIDEO, cpu_thread=False)) as s:
        try:
            _netplay_match(s, art, "battlefield", phases)
        except Exception as e:
            phase = phases[-1] if phases else "?"
            diag = F.diagnose(s.instances, art)
            summary = {}
            for name, d in diag.items():
                st = d.get("status") if isinstance(d.get("status"), dict) else {}
                np_ = d.get("netplay_status") if isinstance(d.get("netplay_status"), dict) else {}
                summary[name] = {"scene": d.get("scene"), "vi_frame": st.get("frame"),
                                 "input_polls": st.get("input_polls"), "rollback": np_.get("rollback")}
            art.note(f"rollback e2e failed during phase {phase!r}: {e!r}")
            art.note(f"summary: {summary}")
            request.node.keep_artifacts = True
            failure = f"rollback netplay failed during {phase!r}: {type(e).__name__}: {e} -- {summary}"
    # Outside the session, so both Dolphins are shut down and their dirs cleaned first.
    if failure is not None:
        pytest.xfail(failure)
