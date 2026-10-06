"""compare_state and two_player_netplay against mocks / fake Dolphins."""

from __future__ import annotations

import time

import pytest

from ppharness.client import HarnessClient
from ppharness.instance import InstanceError
from ppharness.mock_server import GAME_FRAME_ADDR, GAME_RANGES, MockHarnessServer
from ppharness.session import (
    DesyncError,
    FrameKey,
    OvershotError,
    assert_same_state,
    compare_state,
    pause_at,
    two_player_netplay,
)


@pytest.fixture
def two_mocks():
    """Two fake 'Dolphins' booted ~0.4 s apart, so their counters never line up in time."""
    a = MockHarnessServer(speed=2.0)
    a.start()
    time.sleep(0.4)
    b = MockHarnessServer(speed=2.0)
    b.start()
    ca, cb = HarnessClient.connect(a.port), HarnessClient.connect(b.port)
    yield (a, b), (ca, cb)
    ca.close()
    cb.close()
    a.stop()
    b.stop()


def test_pause_at_is_exact(two_mocks):
    (a, _), (ca, _) = two_mocks
    target = ca.status().input_polls + 20
    assert pause_at(ca, target, FrameKey.input_polls()) == target
    assert ca.status().state == "paused"
    assert ca.status().input_polls == target
    with pytest.raises(OvershotError):
        pause_at(ca, target - 5, FrameKey.input_polls())
    ca.resume()


def test_pause_at_memory_key(two_mocks):
    _, (ca, _) = two_mocks
    key = FrameKey.memory(GAME_FRAME_ADDR)
    target = key.read(ca) + 15
    assert pause_at(ca, target, key) == target
    assert ca.read_u32(GAME_FRAME_ADDR) == target
    ca.resume()


@pytest.mark.parametrize("key", [FrameKey.input_polls(), FrameKey.frame(),
                                 FrameKey.memory(GAME_FRAME_ADDR)], ids=str)
def test_compare_pause_matches_across_offset_instances(two_mocks, key):
    _, clients = two_mocks
    cmp = compare_state(clients, GAME_RANGES, key=key, names=["a", "b"])
    assert cmp.match, cmp.describe()
    assert cmp.reached["a"] == cmp.reached["b"] == cmp.at
    # both resumed afterwards
    assert all(c.status().state == "running" for c in clients)


def test_compare_pause_detects_desync(two_mocks):
    (a, b), clients = two_mocks
    # Instance b gets one different input in its past: its state diverges for good.
    clients[1].pad_script(0, [{"buttons": ["A"]}])
    clients[1].wait_frame(input_polls=clients[1].status().input_polls + 2)
    cmp = compare_state(clients, GAME_RANGES, key=FrameKey.memory(GAME_FRAME_ADDR))
    assert not cmp.match
    assert "MISMATCH" in cmp.describe()
    with pytest.raises(DesyncError):
        assert_same_state(clients, GAME_RANGES, key=FrameKey.memory(GAME_FRAME_ADDR))


def test_compare_same_inputs_same_state(two_mocks):
    """Identical pad scripts at identical poll indices -> identical state, despite timing."""
    _, clients = two_mocks
    start = max(c.status().input_polls for c in clients) + 30
    script = [{"buttons": ["A"], "hold": 3}, {"main": [0, 128], "hold": 5}, {"buttons": ["B"]}]
    for c in clients:
        c.pad_script(0, script, start=start)
    cmp = compare_state(clients, GAME_RANGES, at=start + 20, key=FrameKey.input_polls())
    assert cmp.match, cmp.describe()
    assert cmp.at == start + 20


def test_compare_at_past_frame_fails(two_mocks):
    _, clients = two_mocks
    with pytest.raises(OvershotError):
        compare_state(clients, GAME_RANGES, at=1, key=FrameKey.input_polls())
    assert all(c.status().state == "running" for c in clients)


def test_compare_sample(two_mocks):
    _, clients = two_mocks
    cmp = compare_state(clients, GAME_RANGES, key=FrameKey.memory(GAME_FRAME_ADDR),
                        method="sample", timeout=10)
    assert cmp.match, cmp.describe()
    assert cmp.method == "sample"
    assert all(c.status().state == "running" for c in clients)   # never paused


def test_compare_sample_detects_desync(two_mocks):
    (_, b), clients = two_mocks
    b.inject_desync()
    cmp = compare_state(clients, GAME_RANGES, key=FrameKey.memory(GAME_FRAME_ADDR),
                        method="sample", timeout=10)
    assert not cmp.match


def test_compare_needs_two():
    with pytest.raises(ValueError):
        compare_state([], GAME_RANGES)


# --------------------------------------------------------------------------- netplay


def test_two_player_netplay_through_netsim(template, instances_root, fake_exe):
    with two_player_netplay("good", rollback=True, seed=1, name="np", exe=fake_exe,
                            template=template, instances_root=instances_root,
                            connect_timeout=20) as s:
        hc, jc = s.clients
        hs, js = hc.netplay_status(), jc.netplay_status()
        assert hs.role == "host" and js.role == "client"
        assert hs.connected and js.connected
        assert [p.pid for p in hs.players] == [1, 2]
        assert hs.game_running and js.game_running
        assert hc.status().running and jc.status().running
        # the fake netplay pings through netsim: RTT ~ 15 ms (good preset)
        deadline = time.monotonic() + 5
        ping = None
        while time.monotonic() < deadline:
            ping = next((p.ping_ms for p in hc.netplay_status().players if p.pid == 2), None)
            if ping:
                break
            time.sleep(0.1)
        assert ping is not None and 10 <= ping <= 45, ping
        st = s.netsim.stats()
        assert st["sessions"] == 1
        assert st["up"]["packets_out"] > 0 and st["down"]["packets_out"] > 0
        assert st["up"]["delay_ms"]["p50"] == pytest.approx(7.5, abs=2.5)
        cmp = s.compare_state(GAME_RANGES, key=FrameKey.memory(GAME_FRAME_ADDR))
        assert cmp.match, cmp.describe()
        dirs = [i.user_dir for i in s.instances]
        assert all(d.exists() for d in dirs)
        assert "host" in s.report() or s.report()["netsim"]
    assert not any(d.exists() for d in dirs)
    assert all(i.exit_code == 0 for i in s.instances)


def test_two_player_netplay_failure_cleans_processes(template, instances_root, fake_exe):
    with pytest.raises(InstanceError):
        two_player_netplay("lan", name="npfail", exe=fake_exe, template=template,
                           instances_root=instances_root, env={"PPH_FAKE": "crash"},
                           connect_timeout=10)
    kept = sorted(p.name for p in instances_root.iterdir())
    assert kept == ["npfail-host-0", "npfail-joiner-0"]   # kept for debugging
