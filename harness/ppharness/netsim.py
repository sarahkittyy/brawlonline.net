"""Userspace UDP network impairment proxy ("netsim").

Put it between a netplay client and host on one machine (or on a VPS) to get
realistic latency, jitter, loss, duplication, reordering and bandwidth limits::

    joiner --UDP--> [listen 127.0.0.1:2627] netsim [upstream sock] --UDP--> host 127.0.0.1:2626

Each client address gets its own upstream socket, so the host sees one distinct peer
per client, and replies are routed back to the right client. Both directions are
impaired independently: ``up`` is client -> host, ``down`` is host -> client.

Timing model (per direction, per client)::

    arrival --loss/burst--> [bandwidth queue] --latency+jitter--> [order] --> send

* Every packet consumes the same six uniforms from its link's own RNG, in the same
  order, whatever happens to it. With a fixed seed, the n-th packet of a link gets the
  same fate on every run (except where fates depend on wall-clock timing: bandwidth
  queues and time-based spikes).
* Jitter: ``normal`` (jitter_ms = standard deviation), ``uniform`` (+-jitter_ms) or
  ``pareto`` (one-sided heavy tail with mean jitter_ms). ``jitter_corr_ms`` > 0 makes
  successive jitter samples correlated (Ornstein-Uhlenbeck on a Gaussian copula), which
  keeps the configured marginal distribution while making the delay wander smoothly
  like real queueing delay.
* Ordering (``order``): ``fifo`` (default) models a FIFO path: a packet is never
  sent before the previous one, it waits behind it instead. Sparse traffic (pings)
  sees exactly the configured delay distribution; a dense stream (60-120 packets/s)
  whose jitter is large compared to the packet spacing gets "compressed" behind slow
  packets, which raises its mean delay (see README for numbers). ``none`` is netem's
  behaviour: every packet gets exactly its own sampled delay, so jitter reorders.
  Explicit reordering (``reorder``) holds a packet back by ``reorder_delay_ms`` so
  later packets overtake it, in either mode.
* Loss: independent (``loss``) and/or Gilbert-Elliott bursts (``burst``). Spikes add
  loss/latency for a time window, measured from ``NetSim.epoch`` (``reset_epoch()``).

Scheduling: a heap of (deadline, packet) served by one thread using
``time.perf_counter`` (QPC on Windows). It waits on a Condition until ~2 ms before the
deadline (with the Windows timer raised to 1 ms), then finishes with sub-millisecond
sleeps. Typical scheduling error is well under 1 ms. See README for measurements.

Presets are *round-trip* figures as players see "ping": ``typical`` = 40 ms +- 8 ms
RTT means 20 ms one way per direction with sigma 8/sqrt(2) ms each.

CLI::

    python -m ppharness.netsim --listen 127.0.0.1:2627 --forward 127.0.0.1:2626 --preset typical
"""

from __future__ import annotations

import argparse
import dataclasses
import heapq
import itertools
import json
import math
import random
import selectors
import signal
import socket
import statistics
import sys
import threading
import time
from collections import Counter
from dataclasses import dataclass, field
from typing import Any, Iterable, Mapping

from . import _platform

clock = time.perf_counter  # high-resolution monotonic clock on every OS

DIRECTIONS = ("up", "down")
ORDER_MODES = ("fifo", "none")
_NORMAL = statistics.NormalDist()


# --------------------------------------------------------------------------- profiles


@dataclass(frozen=True)
class GilbertElliott:
    """Two-state burst-loss model. Probabilities are per packet."""

    p_enter_bad: float          # P(good -> bad)
    p_exit_bad: float           # P(bad -> good); mean burst length = 1 / p_exit_bad
    loss_good: float = 0.0
    loss_bad: float = 1.0

    @classmethod
    def from_rate(cls, rate: float, mean_burst: float = 3.0, loss_bad: float = 1.0) -> "GilbertElliott":
        """Average loss ``rate`` in bursts averaging ``mean_burst`` packets (in the bad state)."""
        if not 0 <= rate < loss_bad:
            raise ValueError("rate must be in [0, loss_bad)")
        if mean_burst < 1:
            raise ValueError("mean_burst must be >= 1")
        r = 1.0 / mean_burst
        pb = rate / loss_bad                 # stationary P(bad)
        p = pb * r / (1.0 - pb)
        return cls(p_enter_bad=p, p_exit_bad=r, loss_good=0.0, loss_bad=loss_bad)

    @property
    def mean_loss(self) -> float:
        denom = self.p_enter_bad + self.p_exit_bad
        pb = self.p_enter_bad / denom if denom else 0.0
        return pb * self.loss_bad + (1 - pb) * self.loss_good


@dataclass(frozen=True)
class LinkProfile:
    """Impairments of one direction."""

    latency_ms: float = 0.0
    jitter_ms: float = 0.0
    jitter_dist: str = "normal"        # normal | uniform | pareto
    jitter_corr_ms: float = 0.0        # 0 = independent samples
    loss: float = 0.0
    burst: GilbertElliott | None = None
    duplicate: float = 0.0
    reorder: float = 0.0
    reorder_delay_ms: float = 0.0      # 0 = auto (max(2 * jitter, 10 ms))
    rate_kbps: float | None = None     # bandwidth cap, kilobits per second
    queue_ms: float = 250.0            # max queueing delay before tail drop (with rate_kbps)
    order: str = "fifo"                # fifo | none (see module docstring)

    def __post_init__(self) -> None:
        if self.jitter_dist not in ("normal", "uniform", "pareto"):
            raise ValueError(f"unknown jitter distribution {self.jitter_dist!r}")
        if self.order not in ORDER_MODES:
            raise ValueError(f"order must be one of {ORDER_MODES}, got {self.order!r}")
        for name in ("loss", "duplicate", "reorder"):
            v = getattr(self, name)
            if not 0.0 <= v <= 1.0:
                raise ValueError(f"{name} must be a probability, got {v}")
        if self.latency_ms < 0 or self.jitter_ms < 0:
            raise ValueError("latency and jitter must be >= 0")

    def replace(self, **kw: Any) -> "LinkProfile":
        return dataclasses.replace(self, **kw)

    def describe(self) -> str:
        parts = [f"{self.latency_ms:g}ms"]
        if self.jitter_ms:
            parts.append(f"+-{self.jitter_ms:g}ms {self.jitter_dist}"
                         + (f" corr {self.jitter_corr_ms:g}ms" if self.jitter_corr_ms else ""))
        if self.loss:
            parts.append(f"loss {self.loss:.2%}")
        if self.burst:
            parts.append(f"burst loss {self.burst.mean_loss:.2%} "
                         f"(mean burst {1 / self.burst.p_exit_bad:.1f})")
        if self.duplicate:
            parts.append(f"dup {self.duplicate:.2%}")
        if self.reorder:
            parts.append(f"reorder {self.reorder:.2%}")
        if self.rate_kbps:
            parts.append(f"{self.rate_kbps:g} kbit/s")
        return ", ".join(parts)


@dataclass(frozen=True)
class Spike:
    """A temporary impairment: e.g. 300 ms of 50 % loss at t = 30 s."""

    at: float                       # seconds after the epoch
    duration: float                 # seconds
    loss: float | None = None       # added loss probability during the spike
    extra_latency_ms: float = 0.0
    jitter_ms: float | None = None  # replaces jitter during the spike
    direction: str = "both"         # both | up | down

    def active(self, t: float, direction: str) -> bool:
        return (self.direction in ("both", direction)) and self.at <= t < self.at + self.duration

    @classmethod
    def parse(cls, text: str) -> "Spike":
        """``AT:DURATION[:key=value,...]``, seconds; keys loss, latency, jitter, dir.

        Example: ``30:0.3:loss=0.5`` or ``10:2:latency=200,dir=down``.
        """
        parts = text.split(":", 2)
        if len(parts) < 2:
            raise ValueError(f"spike must be AT:DURATION[:k=v,...], got {text!r}")
        kw: dict[str, Any] = {"at": float(parts[0]), "duration": float(parts[1])}
        if len(parts) == 3 and parts[2]:
            for item in parts[2].split(","):
                k, _, v = item.partition("=")
                k = k.strip()
                if k == "loss":
                    kw["loss"] = float(v)
                elif k in ("latency", "extra_latency_ms"):
                    kw["extra_latency_ms"] = float(v)
                elif k in ("jitter", "jitter_ms"):
                    kw["jitter_ms"] = float(v)
                elif k in ("dir", "direction"):
                    if v not in ("both", "up", "down"):
                        raise ValueError(f"bad spike direction {v!r}")
                    kw["direction"] = v
                else:
                    raise ValueError(f"unknown spike key {k!r}")
        return cls(**kw)


@dataclass(frozen=True)
class NetProfile:
    up: LinkProfile = field(default_factory=LinkProfile)
    down: LinkProfile = field(default_factory=LinkProfile)
    spikes: tuple[Spike, ...] = ()
    name: str = "custom"

    def link(self, direction: str) -> LinkProfile:
        return self.up if direction == "up" else self.down

    @classmethod
    def symmetric(cls, link: LinkProfile, spikes: Iterable[Spike] = (), name: str = "custom") -> "NetProfile":
        return cls(up=link, down=link, spikes=tuple(spikes), name=name)

    @classmethod
    def from_rtt(cls, rtt_ms: float, jitter_ms: float = 0.0, *, loss: float = 0.0,
                 burst_loss: float = 0.0, mean_burst: float = 3.0, name: str = "custom",
                 **link_kw: Any) -> "NetProfile":
        """Symmetric profile from round-trip figures.

        Each direction gets ``rtt/2`` latency and ``jitter/sqrt(2)`` jitter (so the RTT's
        standard deviation is ``jitter_ms`` for normal jitter). Loss figures are per
        direction.
        """
        link = LinkProfile(
            latency_ms=rtt_ms / 2.0,
            jitter_ms=jitter_ms / math.sqrt(2.0),
            loss=loss,
            burst=GilbertElliott.from_rate(burst_loss, mean_burst) if burst_loss else None,
            **link_kw,
        )
        return cls.symmetric(link, name=name)

    def with_spikes(self, *spikes: Spike) -> "NetProfile":
        return dataclasses.replace(self, spikes=self.spikes + tuple(spikes))

    def describe(self) -> str:
        s = f"{self.name}: up [{self.up.describe()}] down [{self.down.describe()}]"
        for sp in self.spikes:
            s += f"\n  spike t={sp.at:g}s for {sp.duration:g}s dir={sp.direction}"
            if sp.loss:
                s += f" loss+{sp.loss:.0%}"
            if sp.extra_latency_ms:
                s += f" latency+{sp.extra_latency_ms:g}ms"
            if sp.jitter_ms is not None:
                s += f" jitter={sp.jitter_ms:g}ms"
        return s


#: Named presets. Figures are round trip ("ping"); loss is per direction.
PRESETS: dict[str, NetProfile] = {
    "none": NetProfile(name="none"),
    "lan": NetProfile.from_rtt(1, 0, name="lan"),
    "good": NetProfile.from_rtt(15, 2, name="good"),
    "typical": NetProfile.from_rtt(40, 8, loss=0.005, name="typical"),
    "bad_wifi": NetProfile.from_rtt(60, 25, burst_loss=0.02, mean_burst=3, name="bad_wifi"),
    "cross_country": NetProfile.from_rtt(80, 5, name="cross_country"),
    "awful": NetProfile.from_rtt(150, 50, loss=0.05, name="awful"),
}


def get_profile(profile: str | NetProfile | LinkProfile) -> NetProfile:
    if isinstance(profile, NetProfile):
        return profile
    if isinstance(profile, LinkProfile):
        return NetProfile.symmetric(profile)
    try:
        return PRESETS[profile]
    except KeyError:
        raise ValueError(f"unknown preset {profile!r}; choose from {', '.join(PRESETS)}") from None


# --------------------------------------------------------------------------- link model


@dataclass
class Decision:
    deliveries: list[float]     # absolute clock() times; empty if dropped
    dropped: str | None = None  # "loss" | "burst" | "spike" | "queue"
    reordered: bool = False
    duplicated: bool = False
    target_delay: float = 0.0   # seconds of latency+jitter drawn for this packet
    held_back: bool = False     # FIFO made it wait for the previous packet


class LinkModel:
    """Fate of packets on one direction of one client's link. Pure apart from its RNG."""

    UNIFORMS_PER_PACKET = 6

    def __init__(self, rng: random.Random):
        self.rng = rng
        self.ge_bad = False
        self.z = 0.0                 # OU state for correlated jitter
        self.last_arrival: float | None = None
        self.link_free_at = 0.0
        self.last_delivery = 0.0

    def _jitter(self, prof: LinkProfile, jitter_ms: float, u: float, now: float) -> float:
        if jitter_ms <= 0:
            return 0.0
        u = min(max(u, 1e-12), 1 - 1e-12)
        eps = _NORMAL.inv_cdf(u)
        if prof.jitter_corr_ms > 0 and self.last_arrival is not None:
            dt_ms = max(0.0, (now - self.last_arrival) * 1000.0)
            rho = math.exp(-dt_ms / prof.jitter_corr_ms)
            self.z = rho * self.z + math.sqrt(1.0 - rho * rho) * eps
        else:
            self.z = eps
        z = self.z
        if prof.jitter_dist == "normal":
            return jitter_ms * z
        q = _NORMAL.cdf(z)
        if prof.jitter_dist == "uniform":
            return jitter_ms * (2.0 * q - 1.0)
        # pareto: Lomax(alpha=3) has mean 0.5 -> scale by 2 so the mean extra is jitter_ms
        alpha = 3.0
        q = min(q, 1 - 1e-12)
        return 2.0 * jitter_ms * ((1.0 - q) ** (-1.0 / alpha) - 1.0)

    def decide(self, prof: LinkProfile, now: float, size: int,
               spikes: Iterable[Spike] = ()) -> Decision:
        """Decide the fate of a packet that arrived at ``now`` (a ``clock()`` time)."""
        r = self.rng.random
        u_loss, u_ge, u_ge_loss, u_jit, u_reorder, u_dup = r(), r(), r(), r(), r(), r()

        latency_ms = prof.latency_ms
        jitter_ms = prof.jitter_ms
        spike_loss = 0.0
        for sp in spikes:
            if sp.loss:
                spike_loss = 1.0 - (1.0 - spike_loss) * (1.0 - sp.loss)
            latency_ms += sp.extra_latency_ms
            if sp.jitter_ms is not None:
                jitter_ms = sp.jitter_ms

        jitter = self._jitter(prof, jitter_ms, u_jit, now)
        self.last_arrival = now

        # Burst loss: advance the Markov chain once per packet.
        ge = prof.burst
        burst_drop = False
        if ge is not None:
            if self.ge_bad:
                if u_ge < ge.p_exit_bad:
                    self.ge_bad = False
            elif u_ge < ge.p_enter_bad:
                self.ge_bad = True
            p = ge.loss_bad if self.ge_bad else ge.loss_good
            burst_drop = u_ge_loss < p

        if u_loss < prof.loss:
            return Decision([], "loss")
        if burst_drop:
            return Decision([], "burst")
        total_loss = 1.0 - (1.0 - prof.loss) * (1.0 - spike_loss)
        if u_loss < total_loss:
            return Decision([], "spike")

        t0 = now
        if prof.rate_kbps:
            tx = size * 8.0 / (prof.rate_kbps * 1000.0)
            start = max(now, self.link_free_at)
            if start - now > prof.queue_ms / 1000.0:
                return Decision([], "queue")
            self.link_free_at = start + tx
            t0 = self.link_free_at

        delay = max(0.0, latency_ms + jitter) / 1000.0
        deliver = t0 + delay
        duplicated = u_dup < prof.duplicate
        reordered = u_reorder < prof.reorder
        held = False
        if reordered:
            hold = prof.reorder_delay_ms or max(2.0 * jitter_ms, 10.0)
            deliver += hold / 1000.0
        elif prof.order == "fifo":
            if deliver < self.last_delivery:
                deliver = self.last_delivery
                held = True
            self.last_delivery = deliver
        deliveries = [deliver, deliver] if duplicated else [deliver]
        return Decision(deliveries, None, reordered, duplicated, delay, held)


# --------------------------------------------------------------------------- stats


def percentile(sorted_vals: list[float], pct: float) -> float:
    if not sorted_vals:
        return float("nan")
    k = (len(sorted_vals) - 1) * pct / 100.0
    lo, hi = math.floor(k), math.ceil(k)
    if lo == hi:
        return sorted_vals[lo]
    return sorted_vals[lo] + (sorted_vals[hi] - sorted_vals[lo]) * (k - lo)


def summarize(values: Iterable[float]) -> dict[str, float]:
    v = sorted(values)
    if not v:
        return {"n": 0}
    return {
        "n": len(v),
        "min": v[0],
        "p1": percentile(v, 1),
        "p10": percentile(v, 10),
        "p50": percentile(v, 50),
        "p90": percentile(v, 90),
        "p99": percentile(v, 99),
        "max": v[-1],
        "mean": statistics.fmean(v),
        "stdev": statistics.pstdev(v) if len(v) > 1 else 0.0,
    }


class _Reservoir:
    def __init__(self, limit: int, rng: random.Random):
        self.limit = limit
        self.rng = rng
        self.items: list[float] = []
        self.seen = 0

    def add(self, x: float) -> None:
        self.seen += 1
        if len(self.items) < self.limit:
            self.items.append(x)
        else:
            j = self.rng.randrange(self.seen)
            if j < self.limit:
                self.items[j] = x


class DirectionStats:
    def __init__(self, sample_limit: int, rng: random.Random):
        self.packets_in = 0
        self.bytes_in = 0
        self.packets_out = 0
        self.bytes_out = 0
        self.dropped: Counter[str] = Counter()
        self.duplicated = 0
        self.reordered = 0
        self.delayed = 0
        self.held_back = 0
        self.send_errors = 0
        self.delays_ms = _Reservoir(sample_limit, rng)
        self.sched_err_ms = _Reservoir(sample_limit, rng)

    def as_dict(self) -> dict[str, Any]:
        dropped_total = sum(self.dropped.values())
        return {
            "packets_in": self.packets_in,
            "bytes_in": self.bytes_in,
            "packets_out": self.packets_out,
            "bytes_out": self.bytes_out,
            "dropped": dropped_total,
            "dropped_by_reason": dict(self.dropped),
            "loss_rate": dropped_total / self.packets_in if self.packets_in else 0.0,
            "duplicated": self.duplicated,
            "reordered": self.reordered,
            "delayed": self.delayed,
            "held_back_fifo": self.held_back,
            "send_errors": self.send_errors,
            "delay_ms": summarize(self.delays_ms.items),
            "sched_error_ms": summarize(self.sched_err_ms.items),
        }


# --------------------------------------------------------------------------- proxy


def parse_hostport(text: str, default_host: str = "127.0.0.1") -> tuple[str, int]:
    text = text.strip()
    if text.startswith("["):
        host, _, rest = text[1:].partition("]")
        return host, int(rest.lstrip(":"))
    if text.count(":") == 1:
        host, port = text.split(":")
        return host or default_host, int(port)
    if text.isdigit():
        return default_host, int(text)
    raise ValueError(f"expected HOST:PORT, got {text!r}")


def _family(host: str) -> int:
    return socket.AF_INET6 if ":" in host else socket.AF_INET


class _Session:
    def __init__(self, index: int, client: tuple, upstream: socket.socket, seed: Any, now: float):
        self.index = index
        self.client = client
        self.upstream = upstream
        self.models = {d: LinkModel(random.Random(f"{seed}:{index}:{d}")) for d in DIRECTIONS}
        self.last_activity = now
        self.closed = False


class NetSim:
    """The proxy. Start with ``start()`` or use as a context manager."""

    def __init__(
        self,
        forward: tuple[str, int] | str,
        listen: tuple[str, int] | str = ("127.0.0.1", 0),
        profile: str | NetProfile | LinkProfile = "none",
        *,
        seed: Any = 0,
        idle_timeout: float = 300.0,
        sample_limit: int = 200_000,
        recv_buffer: int = 4 << 20,
    ):
        self.forward = parse_hostport(forward) if isinstance(forward, str) else tuple(forward)
        self.listen_requested = parse_hostport(listen) if isinstance(listen, str) else tuple(listen)
        self.profile = get_profile(profile)
        self.seed = seed
        self.idle_timeout = idle_timeout
        self.recv_buffer = recv_buffer
        self.epoch = clock()
        self.started_at = self.epoch

        rng = random.Random(f"{seed}:stats")
        self._stats = {d: DirectionStats(sample_limit, rng) for d in DIRECTIONS}
        self._stats_lock = threading.Lock()
        self._sessions: dict[tuple, _Session] = {}
        self._session_counter = itertools.count()
        # entries: (deliver_at, seq, direction, session, data, recv_at)
        self._heap: list[tuple[float, int, str, _Session, bytes, float]] = []
        self._seq = itertools.count()
        self._cond = threading.Condition()
        self._stop = threading.Event()
        self._threads: list[threading.Thread] = []
        self._sel: selectors.BaseSelector | None = None
        self._listen_sock: socket.socket | None = None
        self.listen_addr: tuple[str, int] = ("", 0)
        self.hi_res_timer = False
        self._slack = 0.002

    # ---------------------------------------------------------------- lifecycle

    @property
    def listen_port(self) -> int:
        return self.listen_addr[1]

    def start(self) -> "NetSim":
        host, port = self.listen_requested
        s = socket.socket(_family(host), socket.SOCK_DGRAM)
        self._tune(s)
        s.bind((host, port))
        s.setblocking(False)
        self._listen_sock = s
        self.listen_addr = s.getsockname()[:2]
        self._sel = selectors.DefaultSelector()
        self._sel.register(s, selectors.EVENT_READ, None)
        self.started_at = self.epoch = clock()
        ready = threading.Event()
        self._threads = [
            threading.Thread(target=self._recv_loop, daemon=True, name="netsim-recv"),
            threading.Thread(target=self._sched_loop, args=(ready,), daemon=True, name="netsim-sched"),
        ]
        for t in self._threads:
            t.start()
        ready.wait(2.0)
        return self

    def stop(self) -> None:
        self._stop.set()
        with self._cond:
            self._cond.notify_all()
        for t in self._threads:
            t.join(timeout=2.0)
        for sess in list(self._sessions.values()):
            self._close_session(sess)
        if self._listen_sock is not None:
            self._listen_sock.close()
        if self._sel is not None:
            self._sel.close()

    def __enter__(self) -> "NetSim":
        if self._listen_sock is None:
            self.start()
        return self

    def __exit__(self, *exc: Any) -> None:
        self.stop()

    def _tune(self, s: socket.socket) -> None:
        for opt in (socket.SO_RCVBUF, socket.SO_SNDBUF):
            try:
                s.setsockopt(socket.SOL_SOCKET, opt, self.recv_buffer)
            except OSError:
                pass

    # ---------------------------------------------------------------- control

    def set_profile(self, profile: str | NetProfile | LinkProfile) -> None:
        """Change conditions on the fly (link state such as burst state is kept)."""
        self.profile = get_profile(profile)

    def set_link(self, direction: str, link: LinkProfile) -> None:
        if direction not in ("up", "down", "both"):
            raise ValueError("direction must be up, down or both")
        p = self.profile
        up = link if direction in ("up", "both") else p.up
        down = link if direction in ("down", "both") else p.down
        self.profile = dataclasses.replace(p, up=up, down=down)

    def add_spike(self, spike: Spike) -> None:
        self.profile = self.profile.with_spikes(spike)

    def reset_epoch(self) -> None:
        """Spikes are timed from the epoch; call this e.g. when the game starts."""
        self.epoch = clock()

    def elapsed(self) -> float:
        return clock() - self.epoch

    # ---------------------------------------------------------------- receive

    def _new_session(self, client: tuple, now: float) -> _Session:
        fhost = self.forward[0]
        up = socket.socket(_family(fhost), socket.SOCK_DGRAM)
        self._tune(up)
        bind_host = fhost if fhost in ("127.0.0.1", "::1", "localhost") else (
            "::" if _family(fhost) == socket.AF_INET6 else "0.0.0.0")
        up.bind((bind_host, 0))
        up.setblocking(False)
        sess = _Session(next(self._session_counter), client, up, self.seed, now)
        self._sessions[client] = sess
        assert self._sel is not None
        self._sel.register(up, selectors.EVENT_READ, sess)
        return sess

    def _close_session(self, sess: _Session) -> None:
        if sess.closed:
            return
        sess.closed = True
        try:
            if self._sel is not None:
                self._sel.unregister(sess.upstream)
        except (KeyError, ValueError, OSError):
            pass
        sess.upstream.close()
        self._sessions.pop(sess.client, None)

    def _recv_loop(self) -> None:
        assert self._sel is not None and self._listen_sock is not None
        _platform.boost_current_thread()  # arrival timestamps matter too
        last_gc = clock()
        while not self._stop.is_set():
            try:
                events = self._sel.select(timeout=0.1)
            except OSError:
                if self._stop.is_set():
                    return
                continue
            for key, _ in events:
                sock: socket.socket = key.fileobj  # type: ignore[assignment]
                sess: _Session | None = key.data
                while True:
                    try:
                        data, addr = sock.recvfrom(65536)
                    except (BlockingIOError, InterruptedError):
                        break
                    except ConnectionResetError:
                        # Windows reports ICMP port-unreachable on the next recv; ignore.
                        continue
                    except OSError:
                        break
                    now = clock()
                    if sess is None:
                        s = self._sessions.get(addr) or self._new_session(addr, now)
                        self._ingest("up", s, data, now)
                    else:
                        self._ingest("down", sess, data, now)
            now = clock()
            if now - last_gc > 1.0:
                last_gc = now
                for s in list(self._sessions.values()):
                    if now - s.last_activity > self.idle_timeout:
                        self._close_session(s)

    def _ingest(self, direction: str, sess: _Session, data: bytes, now: float) -> None:
        sess.last_activity = now
        prof = self.profile
        link = prof.link(direction)
        t = now - self.epoch
        active = [sp for sp in prof.spikes if sp.active(t, direction)]
        st = self._stats[direction]
        with self._cond:
            dec = sess.models[direction].decide(link, now, len(data), active)
            if not dec.dropped:
                for when in dec.deliveries:
                    heapq.heappush(self._heap, (when, next(self._seq), direction, sess, data, now))
                self._cond.notify()
        with self._stats_lock:
            st.packets_in += 1
            st.bytes_in += len(data)
            if dec.dropped:
                st.dropped[dec.dropped] += 1
                return
            if dec.reordered:
                st.reordered += 1
            if dec.duplicated:
                st.duplicated += 1
            if dec.deliveries[0] > now + 0.0001:
                st.delayed += 1
            if dec.held_back:
                st.held_back += 1

    # ---------------------------------------------------------------- schedule

    def _sched_loop(self, ready: threading.Event) -> None:
        _platform.boost_current_thread()
        with _platform.high_resolution_timer() as hires:
            self.hi_res_timer = hires
            # Without a 1 ms OS tick, Condition.wait can overshoot by ~15.6 ms on Windows.
            self._slack = _platform.coarse_wait_slack() if hires else 0.017
            ready.set()
            while not self._stop.is_set():
                due: list[tuple] = []
                with self._cond:
                    if not self._heap:
                        self._cond.wait(0.25)
                        continue
                    deadline = self._heap[0][0]
                    remaining = deadline - clock()
                    if remaining > self._slack:
                        self._cond.wait(remaining - self._slack)
                        continue
                    if remaining <= 0:
                        now = clock()
                        while self._heap and self._heap[0][0] <= now:
                            due.append(heapq.heappop(self._heap))
                if due:
                    for item in due:
                        self._deliver(item)
                elif remaining > 0.0003:
                    # Fine wait: high-resolution sleep in small steps so an earlier packet
                    # arriving meanwhile is not delayed by more than ~1 ms.
                    time.sleep(min(remaining - 0.0002, 0.001))
                else:
                    # Spin for the last ~0.3 ms, but release the GIL each turn so other
                    # threads (receiver, the caller) are not held up by the switch interval.
                    time.sleep(0)

    def _deliver(self, item: tuple) -> None:
        when, _, direction, sess, data, recv_at = item
        try:
            if direction == "up":
                sess.upstream.sendto(data, self.forward)
            else:
                assert self._listen_sock is not None
                self._listen_sock.sendto(data, sess.client)
            ok = True
        except OSError:
            ok = False
        sent = clock()
        st = self._stats[direction]
        with self._stats_lock:
            if ok:
                st.packets_out += 1
                st.bytes_out += len(data)
                st.delays_ms.add((sent - recv_at) * 1000.0)
                st.sched_err_ms.add((sent - when) * 1000.0)
            else:
                st.send_errors += 1

    # ---------------------------------------------------------------- stats

    def stats(self) -> dict[str, Any]:
        with self._stats_lock:
            d = {k: v.as_dict() for k, v in self._stats.items()}
        return {
            "profile": self.profile.name,
            "elapsed_s": clock() - self.started_at,
            "sessions": len(self._sessions),
            "hi_res_timer": self.hi_res_timer,
            "queued": len(self._heap),
            **d,
        }

    def format_stats(self) -> str:
        s = self.stats()
        lines = [f"netsim [{s['profile']}] t={s['elapsed_s']:.1f}s sessions={s['sessions']} "
                 f"queued={s['queued']}"]
        for d in DIRECTIONS:
            x = s[d]
            dm = x["delay_ms"]
            delay = (f"delay p50={dm['p50']:.2f} p90={dm['p90']:.2f} p99={dm['p99']:.2f} "
                     f"max={dm['max']:.2f} ms" if dm.get("n") else "delay n/a")
            lines.append(
                f"  {d:>4}: in={x['packets_in']} ({x['bytes_in']} B) out={x['packets_out']} "
                f"dropped={x['dropped']} ({x['loss_rate']:.2%}) dup={x['duplicated']} "
                f"reord={x['reordered']} {delay}")
        return "\n".join(lines)


# --------------------------------------------------------------------------- CLI


_LINK_FLAGS: list[tuple[str, str, type, str]] = [
    ("latency", "latency_ms", float, "one-way base latency, ms"),
    ("jitter", "jitter_ms", float, "jitter, ms (normal: sigma; uniform: +-; pareto: mean)"),
    ("dist", "jitter_dist", str, "jitter distribution: normal|uniform|pareto"),
    ("jitter-corr", "jitter_corr_ms", float, "jitter correlation time, ms (0 = independent)"),
    ("loss", "loss", float, "random loss probability (0-1)"),
    ("dup", "duplicate", float, "duplication probability (0-1)"),
    ("reorder", "reorder", float, "reorder probability (0-1)"),
    ("reorder-delay", "reorder_delay_ms", float, "hold-back for reordered packets, ms"),
    ("rate-kbps", "rate_kbps", float, "bandwidth cap, kbit/s"),
    ("queue-ms", "queue_ms", float, "max queueing delay before tail drop, ms"),
]


def build_arg_parser() -> argparse.ArgumentParser:
    ap = argparse.ArgumentParser(
        prog="python -m ppharness.netsim",
        description="UDP impairment proxy. 'up' = client->forward target, 'down' = replies.",
    )
    ap.add_argument("--listen", default="127.0.0.1:0", help="HOST:PORT to listen on")
    ap.add_argument("--forward", help="HOST:PORT of the real server")
    ap.add_argument("--preset", default="none", help=f"one of: {', '.join(PRESETS)}")
    ap.add_argument("--seed", default="0")
    for prefix in ("", "up-", "down-"):
        g = ap.add_argument_group(f"{prefix or 'both '}link overrides")
        for flag, attr, typ, helptext in _LINK_FLAGS:
            g.add_argument(f"--{prefix}{flag}", dest=f"{prefix.replace('-', '_')}{attr}",
                           type=typ, default=None, help=helptext)
        g.add_argument(f"--{prefix}burst", dest=f"{prefix.replace('-', '_')}burst", default=None,
                       metavar="RATE[:MEANLEN]", help="Gilbert-Elliott burst loss")
        g.add_argument(f"--{prefix}order", dest=f"{prefix.replace('-', '_')}order",
                       choices=ORDER_MODES, default=None,
                       help="fifo (default: never overtake the previous packet) or none "
                            "(netem style: jitter may reorder)")
    ap.add_argument("--spike", action="append", default=[], metavar="AT:DUR[:k=v,...]",
                    help="e.g. 30:0.3:loss=0.5 or 10:2:latency=200,dir=down (repeatable)")
    ap.add_argument("--stats-interval", type=float, default=5.0, help="seconds, 0 = off")
    ap.add_argument("--duration", type=float, default=0.0, help="exit after N seconds (0 = run)")
    ap.add_argument("--json", action="store_true", help="print final stats as JSON")
    ap.add_argument("--list-presets", action="store_true")
    return ap


def profile_from_args(args: argparse.Namespace) -> NetProfile:
    base = get_profile(args.preset)
    links = {"up": base.up, "down": base.down}
    for direction in DIRECTIONS:
        for prefix in ("", f"{direction}_"):
            kw: dict[str, Any] = {}
            for _, attr, _, _ in _LINK_FLAGS:
                v = getattr(args, f"{prefix}{attr}")
                if v is not None:
                    kw[attr] = v
            b = getattr(args, f"{prefix}burst")
            if b:
                rate, _, mean = b.partition(":")
                kw["burst"] = GilbertElliott.from_rate(float(rate), float(mean or 3.0))
            if getattr(args, f"{prefix}order"):
                kw["order"] = getattr(args, f"{prefix}order")
            if kw:
                links[direction] = links[direction].replace(**kw)
    spikes = base.spikes + tuple(Spike.parse(s) for s in args.spike)
    changed = links["up"] != base.up or links["down"] != base.down or spikes != base.spikes
    name = base.name + ("+custom" if changed else "")
    return NetProfile(up=links["up"], down=links["down"], spikes=spikes, name=name)


def main(argv: list[str] | None = None) -> int:
    ap = build_arg_parser()
    args = ap.parse_args(argv)
    if args.list_presets:
        for p in PRESETS.values():
            print(p.describe())
        return 0
    if not args.forward:
        ap.error("--forward is required")
    try:
        profile = profile_from_args(args)
    except ValueError as e:
        ap.error(str(e))
    sim = NetSim(args.forward, args.listen, profile, seed=args.seed)
    sim.start()
    print(f"netsim listening on {sim.listen_addr[0]}:{sim.listen_port} -> "
          f"{sim.forward[0]}:{sim.forward[1]} (seed {args.seed}, hi-res timer {sim.hi_res_timer})",
          flush=True)
    print(profile.describe(), flush=True)
    stop = threading.Event()

    def on_signal(*_: Any) -> None:
        stop.set()

    signal.signal(signal.SIGINT, on_signal)
    if hasattr(signal, "SIGTERM"):
        signal.signal(signal.SIGTERM, on_signal)
    t_end = clock() + args.duration if args.duration > 0 else math.inf
    next_stats = clock() + args.stats_interval if args.stats_interval > 0 else math.inf
    try:
        while not stop.is_set() and clock() < t_end:
            stop.wait(min(0.2, max(0.0, t_end - clock())))
            if clock() >= next_stats:
                print(sim.format_stats(), flush=True)
                next_stats += args.stats_interval
    finally:
        sim.stop()
    if args.json:
        print(json.dumps(sim.stats(), indent=2))
    else:
        print(sim.format_stats())
    return 0


if __name__ == "__main__":
    sys.exit(main())
