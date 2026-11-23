"""What the game did with each player's controller tape (tools/input_fidelity.py).

Inputs, per player: the tape it played (ppharness/inputtape.py) and its device model; per machine:
the pad slots every game frame consumed (``game_pads``: confirmed frames only) and the session's
input trace (``PPR_GPRB_INPUT_TRACE``: L/U/W/P lines, Core/Rollback/InputTrace.h).

Checks (T = the frames' sampling period, a 59.94 Hz field):

- exact: every frame of the player's own machine shows the tape's segment at the instant the
  trace says its controller read's state was the controller's (``data_us``, through the device
  model); the other machine shows the same bytes on every confirmed frame;
- cadence: consecutive frames' reads stand one field apart (a repeated read is a sample taken
  twice, a skipped one a sample the game never saw), apart from waits for the peer (W) and
  catch-up frames (two frames in one update without a load);
- holds: a segment held d shows on floor(d/T) or ceil(d/T) frames; never on none when d is longer
  than the largest gap between two reads; taps shorter than a frame are seen with probability
  d/T on average;
- latency: from a segment's start to the first present on the player's own screen of a picture
  of a frame that ran with it, and on the other screen of one drawn with it confirmed.
"""

from __future__ import annotations

import bisect
import math
import statistics
from collections import Counter
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any, Dict, Iterable, List, Mapping, Optional, Sequence, Tuple

from .inputtape import BUTTON_MASK, FIELD_US, LIGHT_PRESS, L, R, Pad, Segment, Tape, id_to_cstick, quantize, slot_id

SLOT = 0x40
EDGE_US = 250  # holds: an edge this close to a frame's grid instant may fall either way


@dataclass
class LLine:
    frame: int
    instant: int
    data: int
    kind: str
    how: str
    src: str
    b03: str
    b30: str


@dataclass
class PLine:
    pass_frame: int
    picture: int
    confirmed: int
    after_rb: bool
    copy: int
    done: int


@dataclass
class ULine:
    t: int
    first: int
    count: int
    load_back: int
    confirmed: int


@dataclass
class Trace:
    pid: int = 0
    local: Dict[int, LLine] = field(default_factory=dict)  # the last L line of each frame
    local_all: List[LLine] = field(default_factory=list)
    updates: List[ULine] = field(default_factory=list)
    waits: List[Tuple[int, int, str]] = field(default_factory=list)
    presents: List[PLine] = field(default_factory=list)


def read_trace(path: Path | str) -> Trace:
    tr = Trace()
    for line in Path(path).read_text(errors="replace").splitlines():
        p = line.split()
        if not p:
            continue
        try:
            if p[0] == "H":
                tr.pid = int(p[1])
            elif p[0] == "L" and len(p) >= 9:
                ll = LLine(int(p[1]), int(p[2]), int(p[3]), p[4], p[5], p[6], p[7], p[8])
                tr.local[ll.frame] = ll
                tr.local_all.append(ll)
            elif p[0] == "U" and len(p) >= 6:
                tr.updates.append(ULine(*(int(x) for x in p[1:6])))
            elif p[0] == "W" and len(p) >= 4:
                tr.waits.append((int(p[1]), int(p[2]), p[3]))
            elif p[0] == "P" and len(p) >= 7:
                tr.presents.append(PLine(int(p[1]), int(p[2]), int(p[3]), p[4] == "1", int(p[5]), int(p[6])))
        except ValueError:
            continue  # a line cut short at the end of the file
    return tr


def slots_of(row_hex: str) -> List[bytes]:
    """A game_pads row: the 4 ports' pad slots (0x40 bytes each), hex."""
    raw = bytes.fromhex(row_hex)
    n = len(raw) // 4
    return [raw[i * n:(i + 1) * n][:SLOT] for i in range(4)]


def s8(v: int) -> int:
    return v - 256 if v >= 128 else v


def expected_slot(seg: Segment, light: int = 0) -> bytes:
    """The pad slot the game makes of a segment's state (HostDecodePadSlot with a centred origin;
    ``light``: the L/R bits P+'s light presses add on this player's layout)."""
    p = seg.pad
    cx, cy = id_to_cstick(seg.index)
    b = p.buttons & BUTTON_MASK
    if light & L and p.l >= LIGHT_PRESS:
        b |= L
    if light & R and p.r >= LIGHT_PRESS:
        b |= R
    out = bytearray(SLOT)
    out[2], out[3] = b >> 8, b & 0xFF
    out[6], out[7] = out[2], out[3]
    for i, v in enumerate((p.sx, p.sy, cx, cy)):
        out[0x30 + i] = (v - 128) & 0xFF
    out[0x34], out[0x35] = p.l, p.r
    return bytes(out)


def slot_content_matches(slot: bytes, seg: Segment) -> bool:
    """Buttons, sticks and triggers as the segment has them (L/R bits from a light press either way)."""
    p = seg.pad
    b = (slot[2] << 8) | slot[3]
    want = p.buttons & BUTTON_MASK
    loose = 0
    if p.l >= LIGHT_PRESS and not p.buttons & L:
        loose |= L
    if p.r >= LIGHT_PRESS and not p.buttons & R:
        loose |= R
    if (b & ~loose) != (want & ~loose):
        return False
    return (s8(slot[0x30]) == p.sx - 128 and s8(slot[0x31]) == p.sy - 128 and slot[0x34] == p.l and slot[0x35] == p.r)


@dataclass
class Model:
    name: str = "ideal"  # ideal | mapped | adapter
    period_us: float = 0.0
    phase_us: float = 0.0
    latency_us: float = 0.0

    def q(self, t: float) -> float:
        return quantize(t, self.name, self.period_us, self.phase_us, self.latency_us)

    def visible(self, start: float, end: float) -> Tuple[float, float]:
        """The host times during which the device presents a state held from ``start`` to ``end``:
        an adapter shows it from its first report at or after the start (plus its latency) until
        its first report at or after the end."""
        if self.name != "adapter" or self.period_us <= 0:
            return start, end

        def up(t: float) -> float:
            k = math.ceil((t - self.phase_us) / self.period_us)
            return self.phase_us + k * self.period_us + self.latency_us
        return up(start), up(end)


def pct(v: Sequence[float], q: float) -> Optional[float]:
    if not v:
        return None
    s = sorted(v)
    i = min(len(s) - 1, max(0, int(round(q * (len(s) - 1)))))
    return s[i]


def summary_stats(v: Sequence[float]) -> Dict[str, Any]:
    if not v:
        return {"n": 0}
    return {"n": len(v), "min": round(min(v), 2), "p50": round(pct(v, 0.5), 2), "p95": round(pct(v, 0.95), 2),
            "p99": round(pct(v, 0.99), 2), "max": round(max(v), 2), "mean": round(statistics.fmean(v), 2),
            "sd": round(statistics.pstdev(v), 2) if len(v) > 1 else 0.0}


def _in_any(t0: int, t1: int, spans: Iterable[Tuple[int, int]]) -> bool:
    return any(a < t1 and t0 < b for a, b in spans)


def analyze_player(name: str, tape: Tape, model: Model, port: int, own_pads: Mapping[int, str],
                   other_pads: Mapping[int, str], own: Trace, other: Trace, limit: int,
                   margin_us: int = 300_000) -> Dict[str, Any]:
    """One player's tape against what both machines did with it (see the module docstring)."""
    out: Dict[str, Any] = {"player": name, "port": port, "model": model.name}
    if not tape.segments:
        out["error"] = "empty tape"
        return out
    t_first, t_last = tape.segments[0].start_us + margin_us, tape.segments[-1].end_us - margin_us
    frames = sorted(f for f, ll in own.local.items()
                    if f in own_pads and f <= limit and t_first <= ll.data <= t_last and ll.data > 0)
    out["frames"] = len(frames)
    if len(frames) < 10:
        out["error"] = f"only {len(frames)} frames with the tape"
        return out
    f0, f1 = frames[0], frames[-1]
    span = [f for f in range(f0, f1 + 1) if f in own_pads]
    waits = [(a, b) for a, b, _ in own.waits]
    catch_up = set()
    for u in own.updates:
        if u.count >= 2 and u.load_back == 0:
            catch_up.update(range(u.first, u.first + u.count))

    # Which segment every frame shows, and which it should by its read's instant.
    act: Dict[int, int] = {}
    exp: Dict[int, int] = {}
    wrong: List[Dict[str, Any]] = []
    content_bad: List[int] = []
    no_id = 0
    ties = 0
    prev_act: Optional[int] = None
    for f in span:
        slot = slots_of(own_pads[f])[port]
        ident = slot_id(slot)
        ll = own.local.get(f)
        e = tape.index_at(model.q(ll.data)) if ll and ll.data > 0 else None
        if e is not None:
            exp[f] = e
        near = e if e is not None else (prev_act + 1 if prev_act is not None else None)
        if ident is None or near is None:
            no_id += ident is None
            continue
        a = tape.resolve(ident, near)
        if a is None:
            no_id += 1
            continue
        act[f] = a
        prev_act = a
        if not slot_content_matches(slot, tape.segments[a]):
            content_bad.append(f)
        if e is not None and a != e:
            seg = tape.segments[e]
            q = model.q(ll.data)
            # A read taken now records its instant a few us before the controller is read: one on
            # a segment's edge may show the next (or previous) segment.
            if abs(a - e) == 1 and min(q - seg.start_us, seg.end_us - q) < 50:
                ties += 1
                continue
            wrong.append({"frame": f, "expected": e, "shown": a, "kind": ll.kind, "how": ll.how, "src": ll.src,
                          "from_start_ms": round((q - seg.start_us) / 1000, 2),
                          "to_end_ms": round((seg.end_us - q) / 1000, 2)})
    out["exact"] = {"checked": len(exp), "wrong": len(wrong), "edge_ties": ties, "no_id": no_id, "first_wrong": wrong[:8],
                    "content_differs": len(content_bad), "first_content_differs": content_bad[:5],
                    "wrong_by": dict(Counter(f"{w['how']}/{w['kind']}/{w['src']}" for w in wrong))}

    # Both machines: the same bytes on every confirmed frame.
    both = [f for f in span if f in other_pads]
    diff = [f for f in both if slots_of(own_pads[f])[port] != slots_of(other_pads[f])[port]]
    out["agreement"] = {"compared": len(both), "differ": len(diff), "first": diff[:5]}

    # Order: a frame showing an earlier segment than the frame before it.
    seq = [act[f] for f in span if f in act]
    back = sum(1 for x, y in zip(seq, seq[1:]) if y < x)
    out["order_back"] = back

    # Cadence of the reads (data instants) frame to frame.
    steps: List[float] = []
    classes: Counter = Counter()
    odd: List[Dict[str, Any]] = []
    for fa, fb in zip(span, span[1:]):
        la, lb = own.local.get(fa), own.local.get(fb)
        if not la or not lb or fb != fa + 1 or la.data <= 0 or lb.data <= 0:
            continue
        d = lb.data - la.data
        excused = _in_any(la.data, lb.data, waits) or fb in catch_up or "@catchup" in lb.how
        if "@catchup" in lb.how:
            classes["catch_up"] += 1
        c = round(d / FIELD_US)
        if excused:
            if "@catchup" not in lb.how:
                classes["excused"] += 1
            continue
        steps.append(d / 1000)
        key = "1" if c == 1 else ("0" if c <= 0 else f"{c}")
        classes[key] += 1
        if c != 1 and len(odd) < 12:
            odd.append({"frame": fb, "step_ms": round(d / 1000, 2), "kind": lb.kind, "how": lb.how,
                        "prev_kind": la.kind, "prev_how": la.how})
    n = sum(v for k, v in classes.items() if k not in ("excused", "catch_up"))
    period = statistics.median(steps) * 1000 if steps else FIELD_US
    max_gap = max(steps) * 1000 if steps else FIELD_US
    # Each read against the frames' grid around it: the median offset of data - f * period over the
    # 31 frames centred on it (a time sync's 0.5-1% drift stays within it). A read off its grid
    # by d lands in the neighbouring field's sample with probability d / T (test_diag_bug70's
    # counter sees exactly that); holds then last a frame more or less.
    pts = [(f, own.local[f].data) for f in span if f in own.local and own.local[f].data > 0]
    # Each frame's instant on the frames' local grid (median offset and step of the 31 frames around
    # it), for the holds' expected counts: a time sync's speed (up to 1% faster, 0.5% slower) moves
    # the step, the grid follows it.
    grid_at: Dict[int, float] = {}
    for i, (f, t) in enumerate(pts):
        win = pts[max(0, i - 15):i + 16]
        st = [(tb - ta) / (fb - fa) for (fa, ta), (fb, tb) in zip(win, win[1:]) if fb > fa and 0 < tb - ta < 2 * FIELD_US]
        step = statistics.median(st) if st else FIELD_US
        grid_at[f] = statistics.median(tw - (fw - f) * step for fw, tw in win)
    grid_sorted = sorted(grid_at.values())
    resid: List[float] = []
    off_grid = 0
    off_examples: List[Dict[str, Any]] = []
    for i, (f, t) in enumerate(pts):
        win = pts[max(0, i - 15):i + 16]
        if len(win) < 11 or _in_any(win[0][1], win[-1][1], waits) or any(
                "@catchup" in own.local[fw].how for fw, _ in win):
            continue
        base = statistics.median(tw - fw * period for fw, tw in win)
        r = (t - f * period - base) / 1000
        resid.append(r)
        if abs(r) > 1.5:
            off_grid += 1
            if len(off_examples) < 12:
                ll = own.local[f]
                off_examples.append({"frame": f, "ms": round(r, 2), "kind": ll.kind, "how": ll.how})
    out["cadence"] = {"steps": n, "classes": dict(classes), "one_field_pct": round(100 * classes["1"] / n, 3) if n else None,
                      "step_ms": summary_stats(steps), "jitter_ms": summary_stats([abs(s - FIELD_US / 1000) for s in steps]),
                      "grid_ms": summary_stats([abs(r) for r in resid]),
                      "off_grid_pct": round(100 * off_grid / len(resid), 3) if resid else None,
                      "off_grid_examples": off_examples,
                      "off_grid_by": dict(Counter(f"{e['how']}/{e['kind']}" for e in off_examples)),
                      "odd": odd}

    # Holds: frames each segment showed on. A time sync catch-up frame takes the instant of the
    # frame before it again (by design): a segment it falls in shows a frame more.
    shown = Counter(act.values())
    catchups = Counter(act[f] for f in span if f in act and f in own.local and "@catchup" in own.local[f].how)
    lo_idx, hi_idx = min(seq), max(seq)
    hold_err: Counter = Counter()
    lost: List[Dict[str, Any]] = []
    unseen: List[Dict[str, Any]] = []
    short_seen = short_n = 0
    short_expect = 0.0
    resid: List[float] = []
    bad_holds: List[Dict[str, Any]] = []
    for k in range(lo_idx + 1, hi_idx):  # whole segments inside the window
        seg = tape.segments[k]
        vis0, vis1 = model.visible(seg.start_us, seg.end_us)
        d = vis1 - vis0  # as long as the device presented it
        if _in_any(seg.start_us, seg.end_us, waits):
            continue
        nk = shown.get(k, 0)
        x = d / period
        resid.append(nk - x)
        if d < period:
            short_n += 1
            short_seen += nk > 0
            short_expect += x
        if nk == 0 and d > max_gap + 1000:
            lost.append({"segment": k, "ms": round(d / 1000, 2)})
        if nk == 0 and d > period + 2000:
            # Held longer than a frame and never seen: on a console every such press is.
            unseen.append({"segment": k, "ms": round(d / 1000, 2)})
        # The frames whose grid instants fall in it: exact for reads on their grid; an edge within
        # 0.25 ms of a grid instant may go either way (jitter well inside the grid check's 1.5 ms).
        lo = max(0, bisect.bisect_left(grid_sorted, vis1 - EDGE_US) - bisect.bisect_left(grid_sorted, vis0 + EDGE_US))
        hi = (bisect.bisect_left(grid_sorted, vis1 + EDGE_US) - bisect.bisect_left(grid_sorted, vis0 - EDGE_US)
              + catchups.get(k, 0))
        if nk < lo or nk > hi:
            hold_err["short" if nk < lo else "long"] += 1
            if len(bad_holds) < 10:
                bad_holds.append({"segment": k, "ms": round(d / 1000, 2), "frames": nk, "expected": [lo, hi]})
    m = statistics.fmean(resid) if resid else 0.0
    se = (statistics.pstdev(resid) / math.sqrt(len(resid))) if len(resid) > 1 else 0.0
    out["holds"] = {"segments": len(resid), "off_by_more_than_rounding": dict(hold_err), "examples": bad_holds,
                    "lost_longer_than_gap": lost[:10], "lost_n": len(lost),
                    "unseen_longer_than_a_frame": unseen[:10], "unseen_n": len(unseen),
                    "mean_frames_minus_expected": round(m, 4), "z": round(m / se, 2) if se else None,
                    "short_taps": {"n": short_n, "seen": short_seen,
                                   "expected_seen": round(short_expect, 1)},
                    "period_ms": round(period / 1000, 3), "max_gap_ms": round(max_gap / 1000, 2)}

    # Latency: segment start -> first present of a picture with it, own screen and the other one.
    own_p = sorted(own.presents, key=lambda p: p.done)
    oth_p = sorted(other.presents, key=lambda p: p.done)
    first_frame: Dict[int, int] = {}
    for f in span:
        a = act.get(f)
        if a is not None and a not in first_frame:
            first_frame[a] = f
    lat_own: List[float] = []
    lat_oth: List[float] = []
    age: List[float] = []
    for k, fk in first_frame.items():
        if not (lo_idx < k < hi_idx):
            continue
        seg = tape.segments[k]
        ll = own.local.get(fk)
        if ll and ll.data > 0:
            age.append((model.q(ll.data) - seg.start_us) / 1000)
        t_own = next((p.done for p in own_p if p.done >= seg.start_us and p.picture >= fk), None)
        if t_own is not None:
            lat_own.append((t_own - seg.start_us) / 1000)
        t_oth = next((p.done for p in oth_p if p.done >= seg.start_us and p.picture >= fk and p.confirmed >= fk), None)
        if t_oth is not None:
            lat_oth.append((t_oth - seg.start_us) / 1000)
    out["latency_ms"] = {"sample_age": summary_stats(age), "own_screen": summary_stats(lat_own),
                         "other_screen": summary_stats(lat_oth)}
    out["reads"] = {"kind": dict(Counter(ll.kind for f, ll in own.local.items() if f0 <= f <= f1)),
                    "how": dict(Counter(ll.how for f, ll in own.local.items() if f0 <= f <= f1)),
                    "catch_up_frames": sum(1 for f, ll in own.local.items() if f0 <= f <= f1 and "@catchup" in ll.how),
                    "src": dict(Counter(ll.src for f, ll in own.local.items() if f0 <= f <= f1))}
    return out


def pacing(tr: Trace, f0: int, f1: int) -> Dict[str, Any]:
    ps = [p for p in tr.presents if f0 <= p.pass_frame <= f1]
    iv = [(b.done - a.done) / 1000 for a, b in zip(ps, ps[1:])]
    t = FIELD_US / 1000
    return {"presents": len(ps), "interval_ms": summary_stats(iv),
            "long": sum(1 for x in iv if x > 1.5 * t), "short": sum(1 for x in iv if x < 0.5 * t),
            "after_rollback": sum(1 for p in ps if p.after_rb),
            "waits": len([w for w in tr.waits]), "wait_ms": round(sum((b - a) for a, b, _ in tr.waits) / 1000, 1)}


def verdict(rep: Mapping[str, Any]) -> List[str]:
    """What is wrong in one player's report (empty: nothing)."""
    bad = []
    if rep.get("error"):
        return [rep["error"]]
    ex = rep["exact"]
    if ex["wrong"]:
        bad.append(f"{ex['wrong']} frames ran with another segment than their read's ({ex['wrong_by']})")
    if ex["no_id"]:
        bad.append(f"{ex['no_id']} frames without a segment id")
    if ex.get("content_differs"):
        bad.append(f"{ex['content_differs']} frames whose buttons/sticks/triggers differ from their segment")
    if rep["agreement"]["differ"]:
        bad.append(f"{rep['agreement']['differ']} frames differ between the machines")
    if rep["order_back"]:
        bad.append(f"{rep['order_back']} frames went back to an earlier segment")
    cad = rep["cadence"]
    if cad["one_field_pct"] is not None and cad["one_field_pct"] < 99.0:
        bad.append(f"reads one field apart on only {cad['one_field_pct']}% of frames ({cad['classes']})")
    if cad.get("off_grid_pct") is not None and cad["off_grid_pct"] > 1.0:
        bad.append(f"{cad['off_grid_pct']}% of reads more than 1.5 ms off the frames' grid "
                   f"(p99 {cad['grid_ms'].get('p99')} ms)")
    h = rep["holds"]
    if h["lost_n"]:
        bad.append(f"{h['lost_n']} segments longer than the largest read gap never seen")
    elif h.get("unseen_n"):
        bad.append(f"{h['unseen_n']} segments held longer than a frame never seen")
    if h["off_by_more_than_rounding"]:
        bad.append(f"holds off by more than rounding: {h['off_by_more_than_rounding']}")
    if h["z"] is not None and abs(h["z"]) > 4:
        bad.append(f"holds systematically {'long' if h['mean_frames_minus_expected'] > 0 else 'short'} "
                   f"({h['mean_frames_minus_expected']:+.3f} frames, z {h['z']})")
    return bad
