"""netsim's link model, without sockets: exact, fast, deterministic."""

from __future__ import annotations

import math
import random
import statistics

import pytest

from ppharness.netsim import (
    PRESETS,
    GilbertElliott,
    LinkModel,
    LinkProfile,
    NetProfile,
    Spike,
    build_arg_parser,
    parse_hostport,
    percentile,
    profile_from_args,
)


def run(prof: LinkProfile, n: int = 50_000, spacing: float = 1 / 120, seed: str = "t",
        size: int = 100, spikes_fn=None):
    m = LinkModel(random.Random(seed))
    out = []
    for i in range(n):
        now = i * spacing
        spikes = spikes_fn(now) if spikes_fn else ()
        out.append((now, m.decide(prof, now, size, spikes)))
    return out


def delays_ms(results):
    return [(d.deliveries[0] - now) * 1000 for now, d in results if d.deliveries]


def test_no_impairment_is_transparent():
    res = run(LinkProfile(), n=1000)
    assert all(d.deliveries == [now] and d.dropped is None for now, d in res)


@pytest.mark.parametrize("loss", [0.005, 0.05, 0.3])
def test_random_loss_rate(loss):
    res = run(LinkProfile(loss=loss), n=200_000)
    rate = sum(1 for _, d in res if d.dropped == "loss") / len(res)
    sigma = math.sqrt(loss * (1 - loss) / len(res))
    assert abs(rate - loss) < 5 * sigma


def test_gilbert_elliott_rate_and_bursts():
    ge = GilbertElliott.from_rate(0.02, mean_burst=3)
    assert ge.mean_loss == pytest.approx(0.02)
    res = run(LinkProfile(burst=ge), n=300_000)
    lost = [d.dropped == "burst" for _, d in res]
    rate = sum(lost) / len(lost)
    assert rate == pytest.approx(0.02, rel=0.1)
    bursts, run_len = [], 0
    for x in lost:
        if x:
            run_len += 1
        elif run_len:
            bursts.append(run_len)
            run_len = 0
    assert statistics.fmean(bursts) == pytest.approx(3.0, rel=0.1)
    # independent loss at the same rate has bursts of ~1.02
    res2 = run(LinkProfile(loss=0.02), n=100_000)
    lost2 = [d.dropped is not None for _, d in res2]
    b2 = [len(s) for s in "".join("x" if x else "." for x in lost2).split(".") if s]
    assert statistics.fmean(b2) < 1.1


def test_gilbert_elliott_validation():
    with pytest.raises(ValueError):
        GilbertElliott.from_rate(1.5)
    with pytest.raises(ValueError):
        GilbertElliott.from_rate(0.1, mean_burst=0.5)


@pytest.mark.parametrize("dist", ["normal", "uniform", "pareto"])
def test_jitter_distributions(dist):
    prof = LinkProfile(latency_ms=50, jitter_ms=10, jitter_dist=dist, order="none")
    d = delays_ms(run(prof, n=100_000))
    mean, sd = statistics.fmean(d), statistics.pstdev(d)
    if dist == "normal":
        assert mean == pytest.approx(50, abs=0.2)
        assert sd == pytest.approx(10, rel=0.02)
        assert percentile(sorted(d), 84.13) == pytest.approx(60, abs=0.3)
    elif dist == "uniform":
        assert min(d) >= 40 - 1e-9 and max(d) <= 60 + 1e-9
        assert sd == pytest.approx(20 / math.sqrt(12), rel=0.02)
    else:
        assert min(d) >= 50 - 1e-9            # one-sided
        assert mean == pytest.approx(60, rel=0.03)  # mean extra = jitter
        assert percentile(sorted(d), 99.9) > 100    # heavy tail


def test_jitter_never_negative():
    d = delays_ms(run(LinkProfile(latency_ms=2, jitter_ms=20, order="none"), n=20_000))
    assert min(d) >= 0


def test_fifo_keeps_order_none_reorders():
    prof = LinkProfile(latency_ms=40, jitter_ms=15)
    fifo = run(prof, n=5000)
    t = [d.deliveries[0] for _, d in fifo]
    assert all(b >= a for a, b in zip(t, t[1:]))
    assert any(d.held_back for _, d in fifo)
    none = run(prof.replace(order="none"), n=5000)
    t2 = [d.deliveries[0] for _, d in none]
    assert sum(1 for a, b in zip(t2, t2[1:]) if b < a) > 100


def test_fifo_sparse_traffic_sees_exact_distribution():
    """With spacing >> jitter, FIFO never holds a packet back."""
    res = run(LinkProfile(latency_ms=40, jitter_ms=5), n=20_000, spacing=1.0)
    assert not any(d.held_back for _, d in res)
    d = delays_ms(res)
    assert statistics.fmean(d) == pytest.approx(40, abs=0.15)
    assert statistics.pstdev(d) == pytest.approx(5, rel=0.03)


def test_correlated_jitter_keeps_marginal():
    prof = LinkProfile(latency_ms=50, jitter_ms=10, jitter_corr_ms=100, order="none")
    d = delays_ms(run(prof, n=200_000))
    assert statistics.fmean(d) == pytest.approx(50, abs=0.5)
    assert statistics.pstdev(d) == pytest.approx(10, rel=0.05)
    # successive samples are strongly correlated (rho = exp(-8.3/100) ~ 0.92)
    diffs = [b - a for a, b in zip(d, d[1:])]
    assert statistics.pstdev(diffs) < 0.5 * 10 * math.sqrt(2)


def test_duplicate_and_reorder_rates():
    res = run(LinkProfile(latency_ms=10, duplicate=0.05, reorder=0.02, order="none"), n=100_000)
    dup = sum(d.duplicated for _, d in res) / len(res)
    reo = sum(d.reordered for _, d in res) / len(res)
    assert dup == pytest.approx(0.05, rel=0.08)
    assert reo == pytest.approx(0.02, rel=0.1)
    for now, d in res:
        if d.duplicated:
            assert len(d.deliveries) == 2
        if d.reordered:
            assert (d.deliveries[0] - now) * 1000 == pytest.approx(20.0)  # 10 + auto hold 10


def test_reorder_overtakes_under_fifo():
    res = run(LinkProfile(latency_ms=10, reorder=0.05, reorder_delay_ms=30), n=5000)
    t = [d.deliveries[0] for _, d in res]
    assert sum(1 for a, b in zip(t, t[1:]) if b < a) > 50


def test_bandwidth_cap_serializes_and_drops():
    # 1000 byte packets every 4 ms = 2 Mbit/s offered into a 1 Mbit/s link
    prof = LinkProfile(rate_kbps=1000, queue_ms=100)
    res = run(prof, n=2000, spacing=0.004, size=1000)
    sent = [(now, d) for now, d in res if d.deliveries]
    dropped = [d for _, d in res if d.dropped == "queue"]
    assert dropped, "queue should overflow"
    # delivered throughput ~= 1 Mbit/s = 125 packets/s of 1000 bytes
    t = [d.deliveries[0] for _, d in sent]
    rate = (len(t) - 1) / (t[-1] - t[0])
    assert rate == pytest.approx(125, rel=0.02)
    # queueing delay never exceeds queue_ms + one packet time
    assert max((d.deliveries[0] - now) for now, d in sent) <= 0.100 + 0.008 + 1e-9


def test_spike_window():
    sp = Spike(at=10.0, duration=0.3, loss=0.5, extra_latency_ms=100)
    prof = LinkProfile(latency_ms=5, order="none")

    def spikes(now):
        return [sp] if sp.active(now, "up") else []

    res = run(prof, n=20 * 120, spikes_fn=spikes)
    inside = [d for now, d in res if 10.0 <= now < 10.3]
    outside = [d for now, d in res if not 10.0 <= now < 10.3]
    assert all(d.dropped is None for d in outside)
    lost = sum(d.dropped == "spike" for d in inside)
    assert 0.2 * len(inside) < lost < 0.8 * len(inside)
    for now, d in res:
        if 10.0 <= now < 10.3 and d.deliveries:
            assert (d.deliveries[0] - now) * 1000 == pytest.approx(105)


def test_spike_parse_and_direction():
    sp = Spike.parse("30:0.3:loss=0.5,latency=200,dir=down")
    assert (sp.at, sp.duration, sp.loss, sp.extra_latency_ms, sp.direction) == (30, 0.3, 0.5, 200, "down")
    assert sp.active(30.1, "down") and not sp.active(30.1, "up") and not sp.active(30.3, "down")
    with pytest.raises(ValueError):
        Spike.parse("30")
    with pytest.raises(ValueError):
        Spike.parse("1:2:color=red")


def test_seeded_reproducibility():
    prof = LinkProfile(latency_ms=20, jitter_ms=5, loss=0.1, burst=GilbertElliott.from_rate(0.05),
                       duplicate=0.02, reorder=0.01)
    a = [(d.dropped, tuple(d.deliveries)) for _, d in run(prof, n=5000, seed="s1")]
    b = [(d.dropped, tuple(d.deliveries)) for _, d in run(prof, n=5000, seed="s1")]
    c = [(d.dropped, tuple(d.deliveries)) for _, d in run(prof, n=5000, seed="s2")]
    assert a == b
    assert a != c


def test_fates_independent_of_timing():
    """Packet n gets the same loss fate whatever the arrival times (fixed RNG use)."""
    prof = LinkProfile(latency_ms=20, jitter_ms=5, loss=0.1)
    a = [d.dropped for _, d in run(prof, n=3000, spacing=0.001)]
    b = [d.dropped for _, d in run(prof, n=3000, spacing=0.05)]
    assert a == b


def test_presets():
    assert set(PRESETS) >= {"lan", "good", "typical", "bad_wifi", "cross_country", "awful"}
    t = PRESETS["typical"]
    assert t.up.latency_ms == 20 and t.down.latency_ms == 20
    assert t.up.jitter_ms == pytest.approx(8 / math.sqrt(2))
    assert t.up.loss == 0.005
    assert PRESETS["bad_wifi"].up.burst.mean_loss == pytest.approx(0.02)
    assert PRESETS["awful"].up.loss == 0.05
    assert "typical" in t.describe()


def test_profile_validation():
    with pytest.raises(ValueError):
        LinkProfile(loss=2)
    with pytest.raises(ValueError):
        LinkProfile(jitter_dist="cauchy")
    with pytest.raises(ValueError):
        LinkProfile(order="random")
    with pytest.raises(ValueError):
        LinkProfile(latency_ms=-1)


def test_cli_profile_overrides():
    ap = build_arg_parser()
    args = ap.parse_args(["--forward", "127.0.0.1:2626", "--preset", "typical", "--loss", "0.1",
                          "--down-latency", "100", "--up-burst", "0.03:4", "--order", "none",
                          "--spike", "5:1:loss=1"])
    p = profile_from_args(args)
    assert p.up.loss == 0.1 and p.down.loss == 0.1
    assert p.up.latency_ms == 20 and p.down.latency_ms == 100
    assert p.up.burst is not None and p.up.burst.mean_loss == pytest.approx(0.03)
    assert p.down.burst is None
    assert p.up.order == "none"
    assert p.spikes[-1] == Spike(5, 1, loss=1.0)
    assert p.name == "typical+custom"
    assert profile_from_args(ap.parse_args(["--preset", "lan"])) == PRESETS["lan"]


def test_parse_hostport():
    assert parse_hostport("127.0.0.1:2626") == ("127.0.0.1", 2626)
    assert parse_hostport("2626") == ("127.0.0.1", 2626)
    assert parse_hostport("[::1]:5") == ("::1", 5)
    assert parse_hostport(":7") == ("127.0.0.1", 7)
    with pytest.raises(ValueError):
        parse_hostport("nohost")


def test_netprofile_from_rtt_roundtrip_sigma():
    p = NetProfile.from_rtt(100, 20)
    # two independent normal directions with sigma 20/sqrt(2) give an RTT sigma of 20
    assert math.hypot(p.up.jitter_ms, p.down.jitter_ms) == pytest.approx(20)
