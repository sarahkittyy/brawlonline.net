"""What a gameplay rollback session sends: every datagram between the two peers, by kind.

Runs gprb_session.py (two instances through a netsim proxy, A hosts, B joins) with a netsim that
also classifies each datagram it relays: the session's prefix byte ('G' GekkoNet, 'C' control,
'F' a piece of a split control message, 'P'/'Q' ping/pong) and, for GekkoNet, its message type
(inputs, input acks, session health = the per-frame checksum, network health, ...). For control
messages the JSON's "t". For GekkoNet inputs it also counts the frames per packet (the redundant
unacknowledged inputs every packet carries again) and their compressed size.

Rates are over the seconds of the match: those with GekkoNet inputs in both directions, without
the first and the last of them (the start barrier and the teardown).

    python harness/tools/gprb_traffic.py --preset typical --minutes 1 --json run/qa/traffic/before.json
    python harness/tools/gprb_traffic.py --compare run/qa/traffic/before.json run/qa/traffic/after.json

``--dolphin-b`` runs the joiner on another build (old and new builds together); ``--define``
adds a netsim preset (e.g. loss in bursts of time rather than of packets, see define_preset).
Every other option goes to gprb_session.py.
"""

from __future__ import annotations

import argparse
import collections
import dataclasses
import json
import struct
import sys
import threading
import time
from pathlib import Path
from typing import Any, Dict, List, Optional, Sequence

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
sys.path.insert(0, str(Path(__file__).resolve().parent))

import gprb_session  # noqa: E402
from ppharness import netsim  # noqa: E402
from ppharness.netsim import NetProfile, NetSim, Spike  # noqa: E402

UDP_IP_HEADER = 28  # IPv4 (20) + UDP (8): what every datagram costs on the wire on top of its payload

# GekkoNet's PacketType (GekkoLib/include/private/net.h)
GEKKO_TYPES = {1: "inputs", 2: "spectator_inputs", 3: "input_ack", 4: "sync_request", 5: "sync_response",
               6: "session_health", 7: "network_health", 8: "disconnect", 9: "disconnect_claim",
               10: "input_ack_v2"}


def gekko_message(msg: bytes) -> tuple[str, Optional[Dict[str, Any]]]:
    """One GekkoNet message (a 'G' packet without its 'G'): its type and, for inputs, what it carries."""
    if not msg:
        return "empty", None
    kind = GEKKO_TYPES.get(msg[0], f"type{msg[0]}")
    if msg[0] in (1, 2) and len(msg) >= 17:
        # type u8, magic u16, variant index u8, then InputMsg: start_frame i32, input_count u16,
        # total_size u16, compressed bool, inputs (u32 length + bytes).
        start, count, total, comp, n = struct.unpack_from("<iHH?I", msg, 4)
        return kind, {"frames": count, "inputs_bytes": n, "compressed": comp, "start": start}
    return kind, None


def split_bundle(data: bytes) -> Optional[List[bytes]]:
    """The GekkoNet messages of a 'B' datagram (PeerData::SplitGekkoBundle), None if malformed."""
    out, pos = [], 1
    while pos < len(data):
        if pos + 2 > len(data):
            return None
        n = data[pos] | (data[pos + 1] << 8)
        pos += 2
        if n == 0 or pos + n > len(data):
            return None
        out.append(data[pos:pos + n])
        pos += n
    return out or None


def classify(data: bytes) -> tuple[str, Optional[Dict[str, Any]]]:
    """The kind of one session datagram and, for GekkoNet inputs, what it carries."""
    if not data:
        return "empty", None
    p = chr(data[0])
    if p == "G" and len(data) >= 2:
        kind, extra = gekko_message(data[1:])
        return "G:" + kind, extra
    if p == "B":
        msgs = split_bundle(data)
        if msgs is None:
            return "B:malformed", None
        kinds, extra = [], None
        for m in msgs:
            k, e = gekko_message(m)
            kinds.append(k)
            extra = extra or e
        return "B:" + "+".join(sorted(kinds)), extra
    if p == "C":
        try:
            t = json.loads(data[1:].decode("utf-8", "replace")).get("t", "?")
        except ValueError:
            t = "?"
        return f"C:{t}", None
    if p == "F":
        return "F:piece", None
    if p in "PQ":
        return f"{p}:ping" if p == "P" else "Q:pong", None
    return f"?{data[0]:02x}", None


class TrafficNetSim(NetSim):
    """A netsim that also records every datagram's kind and size, per direction and second."""

    instances: List["TrafficNetSim"] = []

    def __init__(self, *a: Any, **kw: Any):
        super().__init__(*a, **kw)
        self._tlock = threading.Lock()
        # per direction: second -> kind -> [packets, bytes]
        self.seconds: Dict[str, Dict[int, Dict[str, List[int]]]] = {
            d: collections.defaultdict(lambda: collections.defaultdict(lambda: [0, 0])) for d in ("up", "down")}
        # per direction: second -> frames-per-inputs-packet histogram, inputs bytes
        self.inputs: Dict[str, Dict[int, List[tuple]]] = {
            d: collections.defaultdict(list) for d in ("up", "down")}
        self.sizes: Dict[str, Dict[str, collections.Counter]] = {
            d: collections.defaultdict(collections.Counter) for d in ("up", "down")}
        TrafficNetSim.instances.append(self)

    def _ingest(self, direction: str, sess: Any, data: bytes, now: float) -> None:
        kind, extra = classify(data)
        sec = int(now - self.started_at)
        with self._tlock:
            c = self.seconds[direction][sec][kind]
            c[0] += 1
            c[1] += len(data)
            self.sizes[direction][kind][len(data)] += 1
            if extra:
                self.inputs[direction][sec].append((extra["frames"], extra["inputs_bytes"], extra["compressed"]))
        super()._ingest(direction, sess, data, now)

    def report(self) -> Dict[str, Any]:
        with self._tlock:
            return summarize(self.seconds, self.inputs, self.sizes)


def summarize(seconds: Dict[str, Dict[int, Dict[str, List[int]]]], inputs: Dict[str, Dict[int, List[tuple]]],
              sizes: Dict[str, Dict[str, collections.Counter]]) -> Dict[str, Any]:
    def has_inputs(d: str, s: int) -> bool:
        # inputs alone ('G') or bundled with other messages ('B')
        return sum(p for k, (p, _) in seconds[d].get(s, {}).items() if "inputs" in k) >= 20

    # The match: seconds with inputs both ways, without the first and the last (barrier, teardown).
    both = sorted(s for s in seconds["up"] if has_inputs("up", s) and has_inputs("down", s))
    match = both[1:-1] if len(both) > 2 else both
    out: Dict[str, Any] = {"match_seconds": len(match)}
    for d in ("up", "down"):
        n = max(1, len(match))
        kinds: Dict[str, List[int]] = collections.defaultdict(lambda: [0, 0])
        for s in match:
            for k, (p, b) in seconds[d].get(s, {}).items():
                kinds[k][0] += p
                kinds[k][1] += b
        tot_p = sum(p for p, _ in kinds.values())
        tot_b = sum(b for _, b in kinds.values())
        rows = {}
        for k, (p, b) in sorted(kinds.items(), key=lambda kv: -(kv[1][1] + UDP_IP_HEADER * kv[1][0])):
            common = sizes[d][k].most_common(3)
            rows[k] = {"pps": round(p / n, 1), "avg_bytes": round(b / max(1, p), 1),
                       "kbps_wire": round((b + UDP_IP_HEADER * p) * 8 / n / 1000, 1),
                       "share": round((b + UDP_IP_HEADER * p) / max(1, tot_b + UDP_IP_HEADER * tot_p), 3),
                       "common_sizes": [list(c) for c in common]}
        frames = [f for s in match for (f, _, _) in inputs[d].get(s, [])]
        ib = [b for s in match for (_, b, _) in inputs[d].get(s, [])]
        hist = collections.Counter(frames)
        out[d] = {
            "pps": round(tot_p / n, 1),
            "avg_payload": round(tot_b / max(1, tot_p), 1),
            "kbps_payload": round(tot_b * 8 / n / 1000, 1),
            "kbps_wire": round((tot_b + UDP_IP_HEADER * tot_p) * 8 / n / 1000, 1),
            "packets_per_frame": round(tot_p / n / 60, 2),
            "kinds": rows,
            "inputs_frames_per_packet": {"mean": round(sum(frames) / max(1, len(frames)), 2),
                                         "max": max(frames, default=0),
                                         "hist": {str(k): v for k, v in sorted(hist.items())}},
            "inputs_bytes_mean": round(sum(ib) / max(1, len(ib)), 1),
        }
    return out


def format_report(name: str, r: Dict[str, Any]) -> str:
    lines = [f"{name}: {r['match_seconds']} s of match"]
    for d in ("up", "down"):
        x = r[d]
        lines.append(f"  {d:>4}: {x['pps']:6.1f} packets/s ({x['packets_per_frame']} per frame), avg payload "
                     f"{x['avg_payload']} B, {x['kbps_payload']} kbit/s payload, {x['kbps_wire']} kbit/s with UDP/IP")
        for k, row in x["kinds"].items():
            lines.append(f"        {k:<22} {row['pps']:6.1f}/s  {row['avg_bytes']:7.1f} B  {row['kbps_wire']:6.1f} kbit/s "
                         f"({row['share'] * 100:4.1f}%)  sizes {row['common_sizes']}")
        f = x["inputs_frames_per_packet"]
        lines.append(f"        inputs: {f['mean']} frames per packet (max {f['max']}), {x['inputs_bytes_mean']} B of "
                     f"inputs; frames/packet histogram {f['hist']}")
    return "\n".join(lines)


@dataclasses.dataclass(frozen=True)
class PeriodicOutage(Spike):
    """Every ``every`` seconds, a spike of ``duration`` (one object: netsim checks every spike per packet)."""

    every: float = 1.0

    def active(self, t: float, direction: str) -> bool:
        return self.direction in ("both", direction) and t >= self.at and (t - self.at) % self.every < self.duration


def define_preset(text: str) -> str:
    """``NAME=rtt=60,jitter=25,loss=0.02,burst_loss=0,mean_burst=3,outage_ms=0,outage_every_s=0``:
    a netsim preset for ``--preset NAME``. ``outage_ms``/``outage_every_s``: every that many
    seconds the link is down both ways for that long (bursts in time, where netsim's
    ``burst_loss`` counts packets: the fewer packets a sender sends, the longer its bursts)."""
    name, _, spec = text.partition("=")
    kv = {k: float(v) for k, _, v in (item.partition("=") for item in spec.split(",") if item)}
    prof = NetProfile.from_rtt(kv.get("rtt", 0), kv.get("jitter", 0), loss=kv.get("loss", 0),
                               burst_loss=kv.get("burst_loss", 0), mean_burst=kv.get("mean_burst", 3),
                               name=name)
    every, ms = kv.get("outage_every_s", 0), kv.get("outage_ms", 0)
    if every > 0 and ms > 0:
        prof = prof.with_spikes(PeriodicOutage(at=0.0, duration=ms / 1000, loss=1.0, every=every))
    netsim.PRESETS[name] = prof
    return name


def compare(paths: Sequence[str]) -> int:
    reps = [(p, json.loads(Path(p).read_text())) for p in paths]
    for p, rep in reps:
        print(format_report(p, rep["traffic"]))
        s = rep.get("session") or {}
        print(f"  session: rollbacks {s.get('rollbacks')}, max {s.get('max_rollback')}, checksums "
              f"{s.get('checksums')}, error {s.get('error')}")
    return 0


def main(argv: Optional[Sequence[str]] = None) -> int:
    argv = list(sys.argv[1:] if argv is None else argv)
    if argv and argv[0] == "--compare":
        return compare(argv[1:])
    ap = argparse.ArgumentParser(add_help=False)
    ap.add_argument("--json", default=None)
    ap.add_argument("--dolphin-b", default=None,
                    help="the joiner's DolphinNoGUI (another build than the host's, e.g. the previous release)")
    ap.add_argument("--define", action="append", default=[], help="a netsim preset (define_preset)")
    own, rest = ap.parse_known_args(argv)
    for d in own.define:
        define_preset(d)
    gprb_session.NetSim = TrafficNetSim  # type: ignore[misc]
    if own.dolphin_b:
        orig = gprb_session.G.make_instance

        def make_instance(name: str, **kw: Any):
            inst = orig(name, **kw)
            if "join" in name:
                inst.command_prefix = [str(Path(own.dolphin_b))]
            return inst
        gprb_session.G.make_instance = make_instance
    session_json = (own.json + ".session.json") if own.json else None
    if session_json:
        Path(session_json).parent.mkdir(parents=True, exist_ok=True)
        Path(session_json).unlink(missing_ok=True)
    t0 = time.monotonic()
    rc = gprb_session.main(rest + (["--json", session_json] if session_json else []))
    out: Dict[str, Any] = {"argv": rest, "dolphin_b": own.dolphin_b, "rc": rc, "wall_s": round(time.monotonic() - t0, 1)}
    if TrafficNetSim.instances:
        out["traffic"] = TrafficNetSim.instances[-1].report()
        print(format_report("traffic", out["traffic"]))
    if session_json and Path(session_json).exists():
        reps = json.loads(Path(session_json).read_text())
        rep = (reps[-1] if isinstance(reps, list) and reps else reps) or {}
        st = rep.get("status") or [{}, {}]
        out["session"] = {"rollbacks": [s.get("rollbacks") for s in st],
                          "max_rollback": [s.get("max_rollback_frames") for s in st],
                          "frames_resimulated": [s.get("frames_resimulated") for s in st],
                          "frames": [s.get("current_frame") for s in st],
                          "desyncs": [s.get("desyncs_detected") for s in st],
                          "stalls": [s.get("stall_polls") for s in st],
                          "checksums": rep.get("checksums"), "trace": rep.get("trace"), "error": rep.get("error")}
        print("session:", json.dumps(out["session"]))
    if own.json:
        Path(own.json).parent.mkdir(parents=True, exist_ok=True)
        Path(own.json).write_text(json.dumps(out, indent=1))
    return rc


if __name__ == "__main__":
    raise SystemExit(main())
