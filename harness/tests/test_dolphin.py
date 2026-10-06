"""End-to-end tests against the real Dolphin build (skipped without harness support).

Run only these:  pytest -m dolphin
Skip them:       pytest -m "not dolphin"   (or --no-dolphin)
"""

from __future__ import annotations

import time

import pytest

from ppharness.client import HarnessBusyError, HarnessClient, HarnessCommandError, PadInput
from ppharness.instance import DolphinInstance
from ppharness.session import FrameKey, compare_state, two_player_netplay

pytestmark = pytest.mark.dolphin

GAME_ID_ADDR = 0x80000000  # disc header: "RSBE01"


@pytest.fixture(scope="module")
def booted(request, dolphin_exe):
    """One offline-launcher boot shared by the single-instance tests."""
    keep = request.config.getoption("--keep")
    inst = DolphinInstance("dolphin-e2e", exe=dolphin_exe, keep=keep, connect_timeout=90)
    inst.start()
    yield inst
    failed = request.session.testsfailed > 0
    if failed:
        inst.mark_failed()
    inst.cleanup(not failed)


def test_status_and_frames(booted):
    c = booted.client
    assert c.ping() == 1
    st = c.wait_state("running", timeout=60)
    assert st.game_id.startswith("RSB")
    assert st.video_backend == "Null"
    assert st.raw.get("audio_muted", True) is True          # sound is off
    r = c.wait_frame(st.frame + 60, timeout_ms=20000)
    assert r.frame >= st.frame + 60
    assert r.input_polls > st.input_polls
    assert c.read_u32(GAME_ID_ADDR) == int.from_bytes(b"RSBE", "big")
    assert c.read_mem(GAME_ID_ADDR, 6) == b"RSBE01"


def test_log_mark_reaches_dolphin_log(booted):
    booted.mark("e2e marker 123")
    assert "[HARNESS] e2e marker 123" in booted.log.read()


def test_pause_and_frame_advance(booted):
    c = booted.client
    c.pause()
    try:
        f0 = c.status().frame
        time.sleep(0.2)
        assert c.status().frame == f0
        assert c.frame_advance(5) == f0 + 5
    finally:
        c.resume()


def test_hash_is_stable_while_paused(booted):
    c = booted.client
    c.pause()
    try:
        ranges = [[0x80000000, 0x1800000], [0x90000000, 0x4000000]]
        assert c.hash_mem(ranges) == c.hash_mem(ranges)
    finally:
        c.resume()


def test_pads(booted):
    c = booted.client
    c.pad_set(0, buttons=["A"])
    c.pad_clear(0)
    with pytest.raises(HarnessCommandError, match="no SI device"):
        c.pad_set(2, buttons=["A"])                          # port 2 configured as None
    win = c.pad_script(1, [PadInput(hold=3), PadInput(buttons=["B"], hold=2)])
    assert win.ends_at - win.starts_at == 5
    c.wait_frame(input_polls=win.ends_at, timeout_ms=10000)
    assert c.pad_script_status(1).active is False


def test_screenshot_needs_a_real_backend(booted, tmp_path):
    with pytest.raises(HarnessCommandError, match="Null"):
        booted.client.screenshot(tmp_path / "x.png")


def test_second_client_is_refused(booted):
    with pytest.raises(HarnessBusyError):
        HarnessClient.connect(booted.port, timeout=3)
    assert booted.client.ping() == 1


def test_quit_is_clean(dolphin):
    inst = dolphin("quit-clean")
    inst.start()
    inst.client.wait_state("running", timeout=60)
    assert inst.stop() == 0
    assert not inst.killed


def test_netplay_rollback_through_netsim(request, dolphin_exe):
    keep = request.config.getoption("--keep")
    with two_player_netplay("good", rollback=True, exe=dolphin_exe, keep=keep,
                            name="e2e-netplay") as s:
        hc, jc = s.clients
        deadline = time.monotonic() + 60
        while hc.netplay_status().rollback.get("current_frame", 0) < 120:
            assert time.monotonic() < deadline, "rollback session did not advance"
            time.sleep(0.2)
        for c in (hc, jc):
            np = c.netplay_status()
            assert np.connected and np.game_running and len(np.players) == 2
            assert np.rollback.get("enabled") is True
            assert np.rollback.get("desyncs_detected", 0) == 0
        header = compare_state(s.instances, [[0x80000000, 0x100]], key=FrameKey.netplay())
        assert header.match, header.describe()
        mem2 = compare_state(s.instances, [[0x90000000, 0x4000000]], key=FrameKey.netplay())
        print(mem2.describe())
        stats = s.netsim.stats()
        assert stats["up"]["packets_out"] > 0 and stats["down"]["packets_out"] > 0
        assert stats["up"]["delay_ms"]["p50"] == pytest.approx(7.5, abs=3)
