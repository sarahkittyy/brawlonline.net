"""Input fidelity: what a real two-instance rollback session does with each player's controller.

Two instances play a match as ``gprb_session.py`` does (independent boots and menu paths, a direct
``gprb_connect`` through a netsim proxy, no backend), but during the match each player's controller
plays a tape (``pad_tape``, ppharness/inputtape.py): human-like presses at any instant of the host
clock, not on the game's reads, with a device model (``ideal``: the state at any instant, past ones
too; ``mapped``: a pad mapped in Dolphin, read when called, no past; ``adapter``: the Wii U adapter,
the report of its own grid). Both instances write their input trace (``PPR_GPRB_INPUT_TRACE``).
Afterwards ppharness/inputoracle.py checks, for each player, that every frame ran with the
segment its read stood for, on both machines; that reads came one field apart; that no press
longer than the largest read gap was lost and holds lasted what they were held; and how long each
press took to reach either screen.

    python harness/tools/input_fidelity.py --preset lan --model ideal --out run/qa/inputfid/lan
    python harness/tools/input_fidelity.py --preset r130j3 --model mapped --seconds 120 --out ...
    python harness/tools/input_fidelity.py --preset typical --model adapter --adapter-hz 125 --out ...

Options it does not know go to gprb_session.py (``--cpu dc``, ``--delay 2``, ``--env-b ...``,
``--video Vulkan --platform x11`` for real presents). Presets ``r<rtt>[j<jitter>][l<loss %>]``
are a round trip in ms through netsim. Exit status 1 when any check fails.
"""

from __future__ import annotations

import argparse
import contextlib
import json
import os
import random
import re
import sys
import threading
import time
from pathlib import Path
from typing import Any, Dict, List, Optional, Sequence

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
sys.path.insert(0, str(Path(__file__).resolve().parent))

import gprb_session as GS  # noqa: E402
from ppharness import brawl as B  # noqa: E402
from ppharness import flows as F  # noqa: E402
from ppharness import inputoracle as O  # noqa: E402
from ppharness import inputtape as T  # noqa: E402
from ppharness import netsim as N  # noqa: E402

ADAPTER_SIDEVICE = 12  # SerialInterface::SIDEVICE_WIIU_ADAPTER


def add_presets(names: str) -> None:
    for name in names.split(","):
        m = re.fullmatch(r"r(\d+)(?:j(\d+))?(?:l([\d.]+))?", name)
        if m and name not in N.PRESETS:
            N.PRESETS[name] = N.NetProfile.from_rtt(int(m.group(1)), int(m.group(2) or 0),
                                                    loss=float(m.group(3) or 0) / 100, name=name)


def host_us(c) -> int:
    """Dolphin's steady clock (us), checked against ours: time.perf_counter_ns() is the same clock
    (CLOCK_MONOTONIC on Linux, QueryPerformanceCounter on Windows, where time.monotonic() is
    GetTickCount64)."""
    t0 = time.perf_counter_ns() // 1000
    now = int(c.call("pad_tape", port=0)["now_us"])
    t1 = time.perf_counter_ns() // 1000
    off = now - (t0 + t1) // 2
    if abs(off) > (t1 - t0) // 2 + 2000:
        raise RuntimeError(f"Dolphin's clock is {off} us off ours: pad_tape rows would land at the wrong time")
    return now


class Ctx:
    """What the run's tape fights share with the analysis."""

    def __init__(self, opts: argparse.Namespace):
        self.opts = opts
        self.run_dir: Path = Path(opts.out)
        self.seed = "0"
        self.tapes: Dict[str, T.Tape] = {}
        self.models: Dict[str, O.Model] = {}
        self.pads: Dict[str, Dict[int, str]] = {}
        self.status: Dict[str, Dict[str, Any]] = {}
        self.start_frame = 0
        self.lock = threading.Lock()
        self.fetched = threading.Barrier(2)
        self.reports: List[Dict[str, Any]] = []
        self.polls: Dict[str, List[Any]] = {}


CTX: Optional[Ctx] = None


def model_for(name: str, opts: argparse.Namespace) -> O.Model:
    if opts.model == "adapter":
        rng = random.Random(f"{CTX.seed}-{name}-phase")
        period = 1e6 / opts.adapter_hz
        # The grid's phase on the host clock: anywhere within a period.
        return O.Model("adapter", period_us=period, phase_us=rng.uniform(0, period),
                       latency_us=opts.adapter_latency_us)
    return O.Model(opts.model)


def fighter_x(c, port: int) -> Optional[float]:
    with contextlib.suppress(Exception):
        st = B.read_match_state(c.read_mem)
        for p in st.players:
            if p.port == port and p.x is not None:
                return p.x
    return None


def tape_fight(seats, frames, *, mode=None, seed=None, stage_kind=None, stop=None, **kw) -> None:
    """Replaces flows.fight for gprb_session's match: the seat's controller plays a tape."""
    ctx = CTX
    opts = ctx.opts
    seat = seats[0] if isinstance(seats, (list, tuple)) else seats
    c, pad, name = seat.client, seat.pp, seat.name
    stop = stop or (lambda: False)
    model = model_for(name, opts)
    player = T.Player(f"{ctx.seed}-{name}", taps_under_a_frame=opts.short_taps)
    tape = T.Tape()
    # Past the start's time sync (HardTimeSync, the first frames) and the GO.
    t_settle = time.monotonic() + opts.settle
    while time.monotonic() < t_settle and not stop():
        time.sleep(0.1)
    now = host_us(c)
    t0 = now + 400_000
    end = t0 + int(opts.seconds * 1e6)
    t = player.chunk(tape, t0, t0 + 2_000_000)
    if opts.poll_status > 0:
        def poll() -> None:
            nxt = time.monotonic() + opts.poll_status
            while not stop() and (time.perf_counter_ns() // 1000) < end:
                time.sleep(max(0.0, nxt - time.monotonic()))
                nxt += opts.poll_status
                t_us = time.perf_counter_ns() // 1000
                with contextlib.suppress(Exception):
                    c.call("gprb_status")
                with ctx.lock:
                    ctx.polls.setdefault(name, []).append((t_us, time.perf_counter_ns() // 1000))
        threading.Thread(target=poll, daemon=True).start()
    args: Dict[str, Any] = {"model": model.name}
    if model.name == "adapter":
        args.update(hz=opts.adapter_hz, phase_us=model.phase_us, latency_us=model.latency_us,
                    history=opts.adapter_history)
    c.call("pad_tape", port=pad, rows=tape.rows(), **args)
    sent = len(tape.segments)
    while t < end and not stop():
        # Keep 1 s of tape ahead; lean the stick back towards the middle of the stage.
        while (time.perf_counter_ns() // 1000) < t - 1_000_000 and not stop():
            time.sleep(0.05)
        x = fighter_x(c, seat.port) if not opts.no_lean else None
        bias = 0 if x is None else (-1 if x > opts.lean_at else (1 if x < -opts.lean_at else 0))
        t = player.chunk(tape, t, min(end, t + 1_000_000), bias)
        c.call("pad_tape", port=pad, rows=tape.rows(sent), append=True)
        sent = len(tape.segments)
    # Let the last presses through, then take the frames' pads before the session ends.
    while (time.perf_counter_ns() // 1000) < t + 300_000:
        time.sleep(0.05)
    with ctx.lock:
        ctx.tapes[name] = tape
        ctx.models[name] = model
    st = c.call("gprb_status")
    rows = c.call("game_pads", since=ctx.start_frame)["rows"]
    with ctx.lock:
        ctx.status[name] = st
        ctx.pads[name] = {int(r[0]): r[1] for r in rows}
    with contextlib.suppress(Exception):
        c.call("game_pads", record=False)
    # Both machines have their frames' pads before either leaves the session.
    with contextlib.suppress(threading.BrokenBarrierError):
        ctx.fetched.wait(timeout=15)
    with contextlib.suppress(Exception):
        c.call("gprb_stop")


def save_run(ctx: Ctx) -> None:
    """The run's tapes, models, frames' pads and statuses, for analyze_dir."""
    d = ctx.run_dir
    (d / "tapes.json").write_text(json.dumps({n: {"model": vars(ctx.models[n]), "rows": t.rows(),
                                                  "end_us": t.segments[-1].end_us if t.segments else 0}
                                              for n, t in ctx.tapes.items()}))
    if ctx.polls:
        (d / "polls.json").write_text(json.dumps(ctx.polls))
    (d / "pads.json").write_text(json.dumps({"start_frame": ctx.start_frame, "status": ctx.status,
                                             "pads": {n: {str(f): h for f, h in p.items()} for n, p in ctx.pads.items()}}))


def load_tape(rows: List[List[int]], end_us: int) -> T.Tape:
    tape = T.Tape()
    for r in rows:
        tape.add(r[0], T.Pad(buttons=r[1], sx=r[2], sy=r[3], l=r[6], r=r[7]))
    tape.close(end_us)
    return tape


def analyze_dir(d: Path, meta: Optional[Dict[str, Any]] = None) -> Dict[str, Any]:
    """Check a run directory's tapes, pads and traces (inputoracle)."""
    meta = meta or {}
    out: Dict[str, Any] = dict(meta)
    try:
        tj = json.loads((d / "tapes.json").read_text())
        pj = json.loads((d / "pads.json").read_text())
    except (OSError, ValueError) as e:
        out["error"] = f"no run data in {d}: {e}"
        return out
    traces = {n: O.read_trace(d / f"trace-{n}.txt") for n in ("A", "B") if (d / f"trace-{n}.txt").exists()}
    tapes = {n: load_tape(v["rows"], v["end_us"]) for n, v in tj.items()}
    models = {n: O.Model(**v["model"]) for n, v in tj.items()}
    pads = {n: {int(f): h for f, h in p.items()} for n, p in pj["pads"].items()}
    sts = pj["status"]
    if set(traces) != {"A", "B"} or set(pads) != {"A", "B"} or set(tapes) != {"A", "B"}:
        out["error"] = f"missing data: traces {sorted(traces)}, pads {sorted(pads)}, tapes {sorted(tapes)}"
        return out
    out.setdefault("status", [{k: s.get(k) for k in ("input_delay", "rollbacks", "max_rollback_frames",
                                                     "frames_resimulated", "desyncs_detected", "current_frame",
                                                     "end_reason")} for s in sts.values()])
    limit = min(int(s["current_frame"]) for s in sts.values()) - GS.CONFIRM_MARGIN + int(pj["start_frame"])
    out["players"] = {}
    for name, (other, port) in {"A": ("B", 0), "B": ("A", 1)}.items():
        r = O.analyze_player(name, tapes[name], models[name], port, pads[name], pads[other], traces[name],
                             traces[other], limit)
        r["verdict"] = O.verdict(r)
        out["players"][name] = r
    f0 = min((min(t.local) for t in traces.values() if t.local), default=0)
    out["pacing"] = {name: O.pacing(tr, f0, limit) for name, tr in traces.items()}
    return out


def print_report(r: Dict[str, Any]) -> None:
    print(f"== {r.get('preset')}/{r.get('cpu')} run {r.get('run')} model {r.get('model')}: "
          f"delay {[s.get('input_delay') for s in r.get('status', [])]}, rollbacks "
          f"{[s.get('rollbacks') for s in r.get('status', [])]}, desyncs "
          f"{[s.get('desyncs_detected') for s in r.get('status', [])]}", flush=True)
    if r.get("error"):
        print(f"   error: {r['error']}", flush=True)
        return
    for name, p in r["players"].items():
        if p.get("error"):
            print(f"   {name}: error {p['error']}", flush=True)
            continue
        cad, h, lat = p["cadence"], p["holds"], p["latency_ms"]
        print(f"   {name}: {p['frames']} frames; exact {p['exact']['checked']} checked, {p['exact']['wrong']} wrong, "
              f"{p['exact']['content_differs']} content; machines differ {p['agreement']['differ']}/"
              f"{p['agreement']['compared']}; cadence 1-field {cad['one_field_pct']}% {cad['classes']} "
              f"jitter p99 {cad['jitter_ms'].get('p99')} ms, off grid {cad.get('off_grid_pct')}% (p99 {cad.get('grid_ms', {}).get('p99')} ms); holds {h['segments']} segs, off "
              f"{h['off_by_more_than_rounding']}, lost {h['lost_n']}, mean {h['mean_frames_minus_expected']:+.3f} "
              f"(z {h['z']}), taps<frame {h['short_taps']['seen']}/{h['short_taps']['n']} (exp "
              f"{h['short_taps']['expected_seen']})", flush=True)
        print(f"      latency own p50/p99/max {lat['own_screen'].get('p50')}/{lat['own_screen'].get('p99')}/"
              f"{lat['own_screen'].get('max')} ms, other {lat['other_screen'].get('p50')}/"
              f"{lat['other_screen'].get('p99')}/{lat['other_screen'].get('max')} ms; reads {p['reads']}", flush=True)
        for v in p["verdict"]:
            print(f"      FAIL: {v}", flush=True)
    for name, pc in r.get("pacing", {}).items():
        print(f"   pacing {name}: {pc['presents']} presents, interval {pc['interval_ms'].get('p50')}/"
              f"{pc['interval_ms'].get('p99')}/{pc['interval_ms'].get('max')} ms, long {pc['long']}, short "
              f"{pc['short']}, waits {pc['waits']} ({pc['wait_ms']} ms)", flush=True)


def main(argv: Optional[Sequence[str]] = None) -> int:
    global CTX
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--out", required=True, help="directory for the traces, tapes and reports")
    ap.add_argument("--model", choices=("ideal", "mapped", "adapter"), default="ideal")
    ap.add_argument("--adapter-hz", type=float, default=125.0, help="--model adapter: its report rate")
    ap.add_argument("--adapter-latency-us", type=float, default=0.0,
                    help="--model adapter: from the controller's read to the report reaching Dolphin")
    ap.add_argument("--adapter-history", type=int, default=128,
                    help="--model adapter: reports kept for past instants (GCAdapter keeps 128)")
    ap.add_argument("--seconds", type=float, default=90, help="tape length")
    ap.add_argument("--settle", type=float, default=4.0, help="seconds after the match runs before the tape")
    ap.add_argument("--short-taps", type=float, default=0.15, help="share of presses shorter than a frame")
    ap.add_argument("--lean-at", type=float, default=30.0, help="lean the stick back beyond this |x|")
    ap.add_argument("--seed", default="if", help="tape seed (with the run index)")
    ap.add_argument("--reanalyze", action="store_true", help="only check the run directories under --out again")
    ap.add_argument("--no-lean", action="store_true", help="never read the fighters' positions during the tape")
    ap.add_argument("--poll-status", type=float, default=0.0,
                    help="diagnostics: call gprb_status on the seat's instance every this many seconds")
    opts, rest = ap.parse_known_args(argv)
    if opts.reanalyze:
        reports = []
        for d in sorted(p.parent for p in Path(opts.out).glob("**/tapes.json")):
            old = json.loads((d / "report.json").read_text()) if (d / "report.json").exists() else {}
            r = analyze_dir(d, {k: old.get(k) for k in ("preset", "cpu", "run", "model", "session_error", "checksums")})
            (d / "report.json").write_text(json.dumps(r, indent=1))
            print_report(r)
            reports.append(r)
        return 1 if any(r.get("error") or any(p.get("verdict") for p in r.get("players", {}).values())
                        for r in reports) else 0
    out = Path(opts.out).resolve()
    out.mkdir(parents=True, exist_ok=True)
    opts.out = str(out)
    CTX = Ctx(opts)

    gs_args = list(rest)

    def has(flag: str) -> bool:
        return any(a == flag or a.startswith(flag + "=") for a in gs_args)
    if not has("--delay"):
        gs_args += ["--delay", "auto"]
    if not has("--minutes"):
        gs_args += ["--minutes", str(max(3, int(opts.seconds / 60) + 3))]
    if not has("--damage-ratio"):
        gs_args += ["--damage-ratio", "0.5"]
    if not has("--stage"):
        gs_args += ["--stage", "final_destination"]
    if opts.model == "adapter":
        gs_args += ["--config", f"Dolphin.Core.SIDevice0={ADAPTER_SIDEVICE}"]
    for i, a in enumerate(gs_args):
        if a == "--preset" and i + 1 < len(gs_args):
            add_presets(gs_args[i + 1])

    orig_run = GS.run_session

    def run_session(preset, cpu, args, run):
        CTX.run_dir = out / f"{preset}-{cpu}-r{run}"
        CTX.run_dir.mkdir(parents=True, exist_ok=True)
        CTX.seed = f"{opts.seed}-{run}"
        CTX.tapes, CTX.models, CTX.pads, CTX.status = {}, {}, {}, {}
        CTX.fetched = threading.Barrier(2)
        for p in CTX.run_dir.glob("trace-*.txt"):
            p.unlink()
        os.environ["PPR_GPRB_INPUT_TRACE"] = str(CTX.run_dir / "trace-A.txt")
        env_b = list(args.env_b)
        args.env_b = env_b + [f"PPR_GPRB_INPUT_TRACE={CTX.run_dir / 'trace-B.txt'}"]
        CTX.start_frame = int(args.start_frame)
        try:
            rep = orig_run(preset, cpu, args, run)
        finally:
            args.env_b = env_b
        if set(CTX.tapes) == {"A", "B"}:
            save_run(CTX)
        r = analyze_dir(CTX.run_dir, {"preset": preset, "cpu": cpu, "run": run, "model": opts.model,
                                      "session_error": rep.get("error"), "checksums": rep.get("checksums")})
        (CTX.run_dir / "report.json").write_text(json.dumps(r, indent=1))
        print_report(r)
        CTX.reports.append(r)
        return rep

    GS.run_session = run_session
    F.fight = tape_fight
    GS.main(gs_args)
    (out / "reports.json").write_text(json.dumps(CTX.reports, indent=1))
    failed = [r for r in CTX.reports
              if r.get("error") or any(p.get("verdict") for p in r.get("players", {}).values())]
    print(f"input fidelity: {len(CTX.reports)} runs, {len(failed)} with failures", flush=True)
    return 1 if failed or not CTX.reports else 0


if __name__ == "__main__":
    sys.exit(main())
