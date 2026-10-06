"""netsim over real UDP sockets: is the configured impairment actually achieved?

Tolerances are deliberately loose enough for a busy desktop (a Dolphin build running in
the background) yet tight enough to catch timer-granularity bugs (e.g. Windows' 15.6 ms
tick would blow every latency check here).
"""

from __future__ import annotations

import json
import math
import socket
import subprocess
import sys
import threading
import time
from pathlib import Path

import pytest

from ppharness.netsim import PRESETS, LinkProfile, NetProfile, NetSim, Spike, clock
from ppharness.netsim_bench import run_bench

HARNESS_DIR = Path(__file__).resolve().parents[1]


def timing_retry(attempts: int = 3):
    """Re-run a timing test a few times: a desktop that is busy (a Dolphin build, another
    test run) can delay any thread by milliseconds. A real timing bug fails every time."""
    def deco(fn):
        import functools

        @functools.wraps(fn)
        def wrapper(*a, **kw):
            for i in range(attempts):
                try:
                    return fn(*a, **kw)
                except AssertionError:
                    if i == attempts - 1:
                        raise
                    time.sleep(0.5)
        return wrapper
    return deco


def _binomial_ok(observed: float, p: float, n: int, k: float = 4.0) -> bool:
    return abs(observed - p) <= k * math.sqrt(p * (1 - p) / n) + 1e-9


@timing_retry()
def test_passthrough_overhead_is_small():
    r = run_bench("none", seconds=2, rate_hz=200)
    s = r.summary()
    assert s["up"]["loss"] == 0 and s["down"]["loss"] == 0
    assert s["up"]["one_way_ms"]["p50"] < 1.0
    assert s["up"]["one_way_ms"]["p99"] < 3.0
    assert s["rtt_ms"]["p50"] < 2.0


@timing_retry()
def test_latency_and_jitter_achieved():
    prof = NetProfile.symmetric(LinkProfile(latency_ms=30, jitter_ms=5, order="none"))
    r = run_bench(prof, seconds=4, rate_hz=200)
    for d in ("up", "down"):
        ow = r.summary()[d]["one_way_ms"]
        assert ow["n"] > 700
        assert ow["p50"] == pytest.approx(30, abs=1.0), ow
        assert ow["stdev"] == pytest.approx(5, rel=0.2), ow
        # normal distribution: p10/p90 at -+1.2816 sigma
        assert ow["p10"] == pytest.approx(30 - 6.41, abs=1.5), ow
        assert ow["p90"] == pytest.approx(30 + 6.41, abs=1.5), ow
    sched = r.netsim_stats["up"]["sched_error_ms"]
    assert sched["p50"] < 1.0, sched
    assert sched["p99"] < 5.0, sched


@timing_retry()
def test_fixed_latency_is_tight():
    prof = NetProfile.symmetric(LinkProfile(latency_ms=12))
    r = run_bench(prof, seconds=2, rate_hz=200)
    ow = r.summary()["up"]["one_way_ms"]
    assert 12.0 <= ow["p50"] < 13.0, ow
    assert ow["p90"] < 13.5, ow


@timing_retry()
def test_typical_preset_rtt():
    r = run_bench("typical", seconds=6, rate_hz=60)
    s = r.summary()
    rtt = s["rtt_ms"]
    assert rtt["p50"] == pytest.approx(40, abs=3), rtt
    assert 5.5 < rtt["stdev"] < 10.5, rtt
    assert s["up"]["out_of_order"] == 0 and s["down"]["out_of_order"] == 0  # FIFO


def test_loss_rate_achieved():
    prof = NetProfile(up=LinkProfile(loss=0.10), down=LinkProfile(), name="lossy-up")
    r = run_bench(prof, seconds=3, rate_hz=400)
    assert _binomial_ok(r.up_loss, 0.10, r.sent), r.up_loss
    assert r.down_loss == 0
    st = r.netsim_stats["up"]
    assert st["dropped_by_reason"].get("loss", 0) == r.sent - len(set(r.up_seqs))


def test_burst_loss_is_bursty():
    from ppharness.netsim import GilbertElliott
    prof = NetProfile(up=LinkProfile(burst=GilbertElliott.from_rate(0.05, mean_burst=4)),
                      down=LinkProfile())
    r = run_bench(prof, seconds=4, rate_hz=500)
    got = set(r.up_seqs)
    lost = [i not in got for i in range(r.sent)]
    rate = sum(lost) / len(lost)
    assert 0.025 < rate < 0.09, rate
    bursts = [len(x) for x in "".join("x" if v else "." for v in lost).split(".") if x]
    assert sum(bursts) / len(bursts) > 2.5


def test_duplication_and_reorder_counts():
    prof = NetProfile(up=LinkProfile(latency_ms=5, duplicate=0.1, reorder=0.1, reorder_delay_ms=15),
                      down=LinkProfile())
    r = run_bench(prof, seconds=2, rate_hz=300)
    s = r.summary()
    assert s["up"]["dups"] == pytest.approx(0.1 * r.sent, rel=0.35)
    assert s["up"]["out_of_order"] > 0.04 * r.sent


def test_same_seed_same_drops():
    prof = NetProfile.symmetric(LinkProfile(loss=0.2))
    a = run_bench(prof, seconds=1, rate_hz=200, seed="x")
    b = run_bench(prof, seconds=1, rate_hz=200, seed="x")
    c = run_bench(prof, seconds=1, rate_hz=200, seed="y")
    assert sorted(set(a.up_seqs)) == sorted(set(b.up_seqs))
    assert sorted(set(a.up_seqs)) != sorted(set(c.up_seqs))


@timing_retry()
def test_bandwidth_cap_throughput():
    # 1200-byte packets at 250/s = 2.4 Mbit/s offered, capped at 800 kbit/s
    prof = NetProfile(up=LinkProfile(rate_kbps=800, queue_ms=100), down=LinkProfile())
    r = run_bench(prof, seconds=3, rate_hz=250, size=1200)
    delivered_bits = len(set(r.up_seqs)) * 1200 * 8
    kbps = delivered_bits / 3.0 / 1000
    assert kbps == pytest.approx(800, rel=0.12), kbps
    assert r.netsim_stats["up"]["dropped_by_reason"].get("queue", 0) > 0


class _Echo:
    def __init__(self):
        self.sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.sock.bind(("127.0.0.1", 0))
        self.sock.settimeout(0.1)
        self.peers: set[tuple] = set()
        self.stop = threading.Event()
        self.t = threading.Thread(target=self.run, daemon=True)
        self.t.start()

    @property
    def addr(self):
        return self.sock.getsockname()

    def run(self):
        while not self.stop.is_set():
            try:
                data, addr = self.sock.recvfrom(65536)
            except (socket.timeout, ConnectionResetError):
                continue
            except OSError:
                return
            self.peers.add(addr)
            self.sock.sendto(b"echo:" + data, addr)

    def close(self):
        self.stop.set()
        self.t.join(1)
        self.sock.close()


def test_clients_are_tracked_separately():
    echo = _Echo()
    with NetSim(echo.addr, ("127.0.0.1", 0), NetProfile.symmetric(LinkProfile(latency_ms=3))) as sim:
        a = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        b = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        for s in (a, b):
            s.bind(("127.0.0.1", 0))
            s.settimeout(2)
        for i in range(5):
            a.sendto(f"a{i}".encode(), ("127.0.0.1", sim.listen_port))
            b.sendto(f"b{i}".encode(), ("127.0.0.1", sim.listen_port))
        got_a = sorted(a.recvfrom(100)[0] for _ in range(5))
        got_b = sorted(b.recvfrom(100)[0] for _ in range(5))
        assert got_a == [f"echo:a{i}".encode() for i in range(5)]
        assert got_b == [f"echo:b{i}".encode() for i in range(5)]
        assert len(echo.peers) == 2          # one upstream socket per client
        assert sim.stats()["sessions"] == 2
        a.close()
        b.close()
    echo.close()


@timing_retry()
def test_runtime_profile_change_and_spike():
    echo = _Echo()
    sim = NetSim(echo.addr, ("127.0.0.1", 0), "none").start()
    cli = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    cli.bind(("127.0.0.1", 0))
    cli.settimeout(1.0)

    def rtt() -> float:
        t = clock()
        cli.sendto(b"x", ("127.0.0.1", sim.listen_port))
        cli.recvfrom(100)
        return (clock() - t) * 1000

    try:
        assert rtt() < 5
        sim.set_link("up", LinkProfile(latency_ms=40))
        assert 39 < rtt() < 48
        sim.set_profile(NetProfile.symmetric(LinkProfile(latency_ms=10)))
        assert 19 < rtt() < 28
        sim.set_profile("none")
        sim.reset_epoch()
        sim.add_spike(Spike(at=0.2, duration=0.3, loss=1.0, direction="up"))
        assert rtt() < 5                      # before the spike
        time.sleep(0.25 - sim.elapsed())
        with pytest.raises(socket.timeout):   # inside the spike: everything lost
            cli.settimeout(0.2)
            rtt()
        time.sleep(max(0.0, 0.55 - sim.elapsed()))
        cli.settimeout(1.0)
        assert rtt() < 5                      # after the spike
        assert sim.stats()["up"]["dropped_by_reason"].get("spike", 0) >= 1
    finally:
        cli.close()
        sim.stop()
        echo.close()


def test_cli_standalone():
    echo = _Echo()
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as s:
        s.bind(("127.0.0.1", 0))
        listen_port = s.getsockname()[1]
    proc = subprocess.Popen(
        [sys.executable, "-m", "ppharness.netsim", "--listen", f"127.0.0.1:{listen_port}",
         "--forward", f"127.0.0.1:{echo.addr[1]}", "--preset", "lan", "--duration", "2",
         "--stats-interval", "0", "--json"],
        cwd=HARNESS_DIR, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
    try:
        cli = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        cli.settimeout(0.3)
        got = 0
        deadline = time.monotonic() + 1.5
        while time.monotonic() < deadline and got < 5:
            cli.sendto(b"ping", ("127.0.0.1", listen_port))
            try:
                if cli.recvfrom(100)[0] == b"echo:ping":
                    got += 1
            except (socket.timeout, ConnectionResetError):
                pass
        cli.close()
        out, err = proc.communicate(timeout=10)
    finally:
        if proc.poll() is None:
            proc.kill()
        echo.close()
    assert proc.returncode == 0, err
    assert got >= 3
    stats = json.loads(out[out.index("{"):])
    assert stats["profile"] == "lan"
    assert stats["up"]["packets_out"] >= 3


@pytest.mark.slow
@timing_retry()
@pytest.mark.parametrize("name", [n for n in PRESETS if n != "none"])
def test_presets_achieve_nominal_distribution(name):
    """Every preset's per-packet delay distribution matches its nominal RTT figures.

    Uses order="none" so each packet keeps exactly its sampled delay; FIFO holding
    (the default) is covered separately and its effect on dense streams is in the README.
    """
    import dataclasses
    p = PRESETS[name]
    prof = dataclasses.replace(p, up=p.up.replace(order="none"), down=p.down.replace(order="none"))
    r = run_bench(prof, seconds=6, rate_hz=60, seed="presets")
    rtt = r.summary()["rtt_ms"]
    nominal = p.up.latency_ms + p.down.latency_ms
    sigma = math.hypot(p.up.jitter_ms, p.down.jitter_ms)
    assert rtt["p50"] == pytest.approx(nominal, abs=0.2 * sigma + 1.5), rtt
    if sigma:
        assert rtt["stdev"] == pytest.approx(sigma, rel=0.25), rtt
    else:
        assert rtt["stdev"] < 0.5, rtt
