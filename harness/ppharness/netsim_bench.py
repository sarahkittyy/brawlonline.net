"""Measure what a NetSim actually does: send timestamped UDP packets through it to a
local echo server and record one-way delays (both directions), RTT, loss and order.

    python -m ppharness.netsim_bench --preset typical --seconds 10 --rate 120

Sender, echo server and receiver share one process, so they share ``perf_counter``
and one-way delays are exact (no clock sync needed).
"""

from __future__ import annotations

import argparse
import json
import socket
import struct
import threading
import time
from dataclasses import dataclass, field
from typing import Any

from . import _platform
from .netsim import NetProfile, NetSim, clock, get_profile, summarize

_FMT = ">Iddd"
_HDR = struct.calcsize(_FMT)


@dataclass
class BenchResult:
    sent: int
    up_received: int
    down_received: int
    up_ms: list[float] = field(repr=False)
    down_ms: list[float] = field(repr=False)
    rtt_ms: list[float] = field(repr=False)
    up_seqs: list[int] = field(repr=False)
    down_seqs: list[int] = field(repr=False)
    netsim_stats: dict[str, Any] = field(repr=False)

    @property
    def up_loss(self) -> float:
        return 1.0 - len(set(self.up_seqs)) / self.sent if self.sent else 0.0

    @property
    def down_loss(self) -> float:
        n = len(set(self.up_seqs))
        return 1.0 - len(set(self.down_seqs)) / n if n else 0.0

    @staticmethod
    def _inversions(seqs: list[int]) -> int:
        hi, inv = -1, 0
        for s in seqs:
            if s < hi:
                inv += 1
            hi = max(hi, s)
        return inv

    def summary(self) -> dict[str, Any]:
        return {
            "sent": self.sent,
            "up": {"loss": self.up_loss, "dups": len(self.up_seqs) - len(set(self.up_seqs)),
                   "out_of_order": self._inversions(self.up_seqs), "one_way_ms": summarize(self.up_ms)},
            "down": {"loss": self.down_loss,
                     "dups": len(self.down_seqs) - len(set(self.down_seqs)),
                     "out_of_order": self._inversions(self.down_seqs),
                     "one_way_ms": summarize(self.down_ms)},
            "rtt_ms": summarize(self.rtt_ms),
        }


def run_bench(profile: str | NetProfile, *, seconds: float = 5.0, rate_hz: float = 120.0,
              size: int = 64, seed: Any = 0, settle: float | None = None) -> BenchResult:
    prof = get_profile(profile)
    size = max(size, _HDR)
    echo = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    echo.bind(("127.0.0.1", 0))
    echo.settimeout(0.1)
    up_ms: list[float] = []
    up_seqs: list[int] = []
    stop = threading.Event()

    def echo_loop() -> None:
        _platform.boost_current_thread()
        while not stop.is_set():
            try:
                data, addr = echo.recvfrom(65536)
            except (socket.timeout, ConnectionResetError):
                continue
            except OSError:
                return
            t = clock()
            seq, t_send, _, _ = struct.unpack_from(_FMT, data)
            up_ms.append((t - t_send) * 1000.0)
            up_seqs.append(seq)
            out = bytearray(data)
            struct.pack_into(_FMT, out, 0, seq, t_send, t, clock())
            try:
                echo.sendto(bytes(out), addr)
            except OSError:
                pass

    sim = NetSim(echo.getsockname(), ("127.0.0.1", 0), prof, seed=seed)
    sim.start()
    cli = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    cli.bind(("127.0.0.1", 0))
    cli.settimeout(0.1)
    down_ms: list[float] = []
    rtt_ms: list[float] = []
    down_seqs: list[int] = []

    def recv_loop() -> None:
        _platform.boost_current_thread()
        while not stop.is_set():
            try:
                data, _ = cli.recvfrom(65536)
            except (socket.timeout, ConnectionResetError):
                continue
            except OSError:
                return
            t = clock()
            seq, t_send, _, t_echo_send = struct.unpack_from(_FMT, data)
            down_ms.append((t - t_echo_send) * 1000.0)
            rtt_ms.append((t - t_send) * 1000.0)
            down_seqs.append(seq)

    threads = [threading.Thread(target=echo_loop, daemon=True),
               threading.Thread(target=recv_loop, daemon=True)]
    for t in threads:
        t.start()
    n = int(seconds * rate_hz)
    period = 1.0 / rate_hz
    dest = ("127.0.0.1", sim.listen_port)
    pad = bytes(size - _HDR)
    t0 = clock()
    for seq in range(n):
        target = t0 + seq * period
        while True:
            rem = target - clock()
            if rem <= 0:
                break
            time.sleep(min(rem, 0.001) if rem > 0.0003 else 0)
        cli.sendto(struct.pack(_FMT, seq, clock(), 0.0, 0.0) + pad, dest)
    # Let in-flight packets drain: worst-case delay of the profile plus margin.
    if settle is None:
        worst = max(prof.up.latency_ms + 6 * prof.up.jitter_ms, prof.down.latency_ms + 6 * prof.down.jitter_ms)
        settle = 0.3 + 2 * worst / 1000.0
    time.sleep(settle)
    stop.set()
    for t in threads:
        t.join(1.0)
    stats = sim.stats()
    sim.stop()
    cli.close()
    echo.close()
    return BenchResult(n, len(up_seqs), len(down_seqs), up_ms, down_ms, rtt_ms, up_seqs,
                       down_seqs, stats)


def format_result(name: str, r: BenchResult) -> str:
    s = r.summary()

    def fmt(d: dict[str, Any]) -> str:
        if not d.get("n"):
            return "n/a"
        return (f"p1={d['p1']:.2f} p10={d['p10']:.2f} p50={d['p50']:.2f} p90={d['p90']:.2f} "
                f"p99={d['p99']:.2f} mean={d['mean']:.2f} sd={d['stdev']:.2f}")

    sched = r.netsim_stats["up"]["sched_error_ms"]
    return "\n".join([
        f"[{name}] sent={s['sent']}",
        f"  RTT ms      {fmt(s['rtt_ms'])}",
        f"  up   ms     {fmt(s['up']['one_way_ms'])} loss={s['up']['loss']:.2%} "
        f"ooo={s['up']['out_of_order']}",
        f"  down ms     {fmt(s['down']['one_way_ms'])} loss={s['down']['loss']:.2%} "
        f"ooo={s['down']['out_of_order']}",
        f"  sched error ms p50={sched.get('p50', float('nan')):.3f} "
        f"p99={sched.get('p99', float('nan')):.3f} max={sched.get('max', float('nan')):.3f}",
    ])


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(prog="python -m ppharness.netsim_bench")
    ap.add_argument("--preset", action="append", help="preset(s) to measure (default: all)")
    ap.add_argument("--seconds", type=float, default=5.0)
    ap.add_argument("--rate", type=float, default=120.0, help="packets per second")
    ap.add_argument("--size", type=int, default=64)
    ap.add_argument("--seed", default="0")
    ap.add_argument("--json", action="store_true")
    args = ap.parse_args(argv)
    from .netsim import PRESETS
    names = args.preset or [p for p in PRESETS if p != "none"]
    out = {}
    for name in names:
        r = run_bench(name, seconds=args.seconds, rate_hz=args.rate, size=args.size, seed=args.seed)
        out[name] = r.summary()
        if not args.json:
            print(format_result(name, r), flush=True)
    if args.json:
        print(json.dumps(out, indent=2))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
