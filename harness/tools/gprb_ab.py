"""A/B match determinism with different menu histories, compared every game frame (gameplay rollback,
phases 1 and 2; docs/gameplay-rollback-status.md).

Fixtures: the feasibility spike's sim-start savestates (``gameplay_rollback.py prep`` + ``simstart``):
``As.sav`` / ``Bs.sav``, two instances that reached the same match through different menu histories.

``record``  One instance (single core) loads As.sav and plays closed-loop (flows.Fighter decisions from
            the live state, free-running) until the match ends or --frames. It records the gfPadStatus
            slots every game frame consumed (``game_pads``, read at the top of the main loop) and the
            pad scripts it submitted. Output: one JSON input file.
``pair``    Two instances load A and B (or any pair) at the sim start, write the sync values from the
            first (RNG seeds, g_GameFrame, gfApplication counter, optionally the object serial
            counter), then run freely (no per-frame pauses, unthrottled) with the recorded input:
              --input inject  the recorded per-frame slots, frame-anchored (written at the main-loop
                              top and after every pad-thread read), identical on both by construction;
              --input si      the recorded pad scripts replayed through the SI as one script per port
                              (the feasibility spike's method).
            At the end both pause; the per-frame traces (``frame_trace``: fighters, RNG, game set) are
            compared frame by frame.

    python harness/tools/gprb_ab.py record --dir run/qa/gprb/ps2 --out run/qa/gprb2/in-ps2-1.json --seed r1
    python harness/tools/gprb_ab.py pair --dir run/qa/gprb/ps2 --inputs run/qa/gprb2/in-ps2-1.json \
        --cpu dc --input inject --runs 3 --json run/qa/gprb2/ab-dc.json
"""

from __future__ import annotations

import argparse
import contextlib
import json
import random
import sys
import time
from pathlib import Path
from typing import Any, Dict, List, Optional, Sequence

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
sys.path.insert(0, str(Path(__file__).resolve().parent))

import gameplay_rollback as G  # noqa: E402
from ppharness import brawl as B  # noqa: E402
from ppharness import flows as F  # noqa: E402
from ppharness.client import HarnessClient, HarnessError  # noqa: E402

NEUTRAL = G.NEUTRAL
# frame_trace row: [frame, persistent, steps, ticks, rng0, rng1, rng2, fighters_crc, range_hash, game_set, resim,
#                   fighters (24 u32)]
CMP_FIELDS = {"rng": (4, 5, 6), "fighters_crc": (7,), "game_set": (9,), "fighters": (11,)}


def gentle_macro(rng: random.Random, toward: int) -> List[Dict[str, Any]]:
    """flows._macro without smash attacks and with more spacing: long matches, fewer early KOs."""
    t = 255 if toward > 0 else 1
    away = 1 if toward > 0 else 255
    r = rng.random()
    if r < 0.20:
        return [{"main": [t, 128], "hold": rng.randint(4, 16)}]
    if r < 0.32:
        return [{"main": [away, 128], "hold": rng.randint(4, 14)}]
    if r < 0.42:
        return [{"buttons": ["X"], "hold": 2}, {"hold": rng.randint(6, 20)}]
    if r < 0.58:
        return [{"buttons": ["A"], "hold": 2}, {"hold": rng.randint(6, 14)}]
    if r < 0.64:
        return [{"main": [t, 128], "buttons": ["A"], "hold": 2}, {"hold": 16}]
    if r < 0.70:
        return [{"buttons": ["X"], "hold": 2}, {"hold": 4}, {"main": [t, 128], "buttons": ["A"], "hold": 2}, {"hold": 18}]
    if r < 0.76:
        return [{"buttons": ["B"], "hold": 2}, {"hold": 24}]
    if r < 0.80:
        return [{"main": [128, 1], "buttons": ["B"], "hold": 2}, {"hold": 30}]
    if r < 0.84:
        return [{"main": [t, 128], "buttons": ["B"], "hold": 2}, {"hold": 30}]
    if r < 0.90:
        return [{"r": 255, "buttons": ["R"], "hold": rng.randint(4, 14)}, {"hold": 2}]
    if r < 0.94:
        return [{"r": 255, "buttons": ["R"], "hold": 2}, {"r": 255, "buttons": ["R", "A"], "hold": 2}, {"hold": 30}]
    return [{"hold": rng.randint(4, 20)}]


def calm_macro(rng: random.Random, toward: int) -> List[Dict[str, Any]]:
    """Mostly movement, shields and jumps with occasional jabs: a match that runs to the time limit."""
    t = 255 if toward > 0 else 1
    away = 1 if toward > 0 else 255
    r = rng.random()
    if r < 0.25:
        return [{"main": [t, 128], "hold": rng.randint(4, 12)}]
    if r < 0.45:
        return [{"main": [away, 128], "hold": rng.randint(4, 12)}]
    if r < 0.55:
        return [{"buttons": ["X"], "hold": 2}, {"hold": rng.randint(10, 24)}]
    if r < 0.65:
        return [{"r": 255, "buttons": ["R"], "hold": rng.randint(6, 20)}, {"hold": 2}]
    if r < 0.72:
        return [{"buttons": ["A"], "hold": 2}, {"hold": rng.randint(10, 20)}]
    if r < 0.78:
        return [{"main": [128, 1], "hold": rng.randint(6, 14)}]
    return [{"hold": rng.randint(8, 30)}]


def norm(seq: Sequence[Dict[str, Any]]) -> List[Dict[str, Any]]:
    out = []
    for x in seq:
        d = dict(NEUTRAL)
        d.update({k: v for k, v in x.items() if k != "hold"})
        d["buttons"] = sorted(d.get("buttons", []))
        out.append(dict(d, hold=int(x.get("hold", 1))))
    return out


def sim_frame(c: HarnessClient) -> int:
    return G.game_frame(c)


def cmd_record(args: argparse.Namespace) -> int:
    d = Path(args.dir)
    F._macro = {"gentle": gentle_macro, "calm": calm_macro, "random": G._macro}[args.policy]
    orig = G.make_instance

    def mk(name, **kw):
        inst = orig(name, **kw)
        inst.config.config_args = list(inst.config.config_args or []) + [f"Dolphin.Core.EmulationSpeed={args.speed}"]
        return inst
    G.make_instance = mk
    with G.instances([("gprb-rec", dict(cpu_thread=False))]) as (inst,):
        c = inst.client
        G.load_fixture(c, d / f"{args.state}.sav")
        setup = B.read_match_setup(c.read_mem)
        # Anchor ports 0/1: every frame consumes one pad-thread sample for the whole frame, so the
        # recording reproduces this game exactly when injected.
        c.call("game_pads", record=True, anchor=3)
        c.call("frame_trace_config", enabled=True)
        g0 = sim_frame(c)
        p0 = c.status().input_polls
        fighters = [F.Fighter(F.Seat(c, port), "random", args.seed, setup.stage_kind if setup else None)
                    for port in (0, 1)]
        busy = [0, 0]
        decisions: List[List[Any]] = []
        c.resume()
        t0 = time.monotonic()
        while True:
            now = c.status().input_polls
            st = B.read_match_state(c.read_mem)
            gf = sim_frame(c)
            if st.game_set or gf - g0 >= args.frames or not st.in_match:
                break
            if time.monotonic() - t0 > args.timeout:
                print("timeout", flush=True)
                break
            for port, ft in enumerate(fighters):
                if busy[port] <= now:
                    seq = norm(ft.decide(st))
                    w = c.pad_script(port, seq)
                    busy[port] = w.ends_at
                    decisions.append([w.starts_at - p0, port, seq])
            with contextlib.suppress(HarnessError):
                c.wait_frame(input_polls=max(min(busy), now + 1), timeout_ms=20000)
        # Let the game-set screen settle a little so the end is recorded too.
        end_frame = sim_frame(c)
        with contextlib.suppress(HarnessError):
            c.wait_frame(input_polls=c.status().input_polls + 30, timeout_ms=20000)
        c.pause()
        rows = c.call("game_pads", since=0)["rows"]
        trace = c.call("frame_trace", since=0)["rows"]
        final = G.small_state(c)
    out = {"state": args.state, "seed": args.seed, "policy": args.policy, "g0": g0, "end_frame": end_frame,
           "trace": trace,
           "wall_s": round(time.monotonic() - t0, 1), "decisions": decisions, "pads": rows,
           "final": final, "trace_last": trace[-1] if trace else None}
    Path(args.out).write_text(json.dumps(out))
    print(f"recorded frames {rows[0][0] if rows else None}..{rows[-1][0] if rows else None} ({len(rows)}), "
          f"end at game frame {end_frame}, final {final['players']}, wall {out['wall_s']} s")
    return 0


def si_scripts(decisions: Sequence[Sequence[Any]], total: int) -> Dict[int, List[Dict[str, Any]]]:
    """The recorder's decisions [(start relative to the first poll, port, seq)] as one script per port,
    each decision running until the next one starts (pad_script replaces)."""
    out = {}
    for port in (0, 1):
        subs = sorted((int(s), seq) for s, p, seq in decisions if int(p) == port)
        seq_out: List[Dict[str, Any]] = []
        t = 1
        for i, (start, seq) in enumerate(subs):
            nxt = subs[i + 1][0] if i + 1 < len(subs) else total
            if start > t:
                seq_out.append(dict(NEUTRAL, hold=start - t))
                t = start
            for x in seq:
                if t >= nxt:
                    break
                h = min(int(x["hold"]), nxt - t)
                if h > 0:
                    seq_out.append(dict(x, hold=h))
                    t += h
        if t < total:
            seq_out.append(dict(NEUTRAL, hold=total - t))
        out[port] = seq_out
    return out


def compare_traces(ta: Sequence[Sequence[Any]], tb: Sequence[Sequence[Any]]) -> Dict[str, Any]:
    mb = {r[0]: r for r in tb}
    common = [r for r in ta if r[0] in mb]
    first: Dict[str, Any] = {}
    for r in common:
        s = mb[r[0]]
        for name, idx in CMP_FIELDS.items():
            if name in first:
                continue
            if any(r[i] != s[i] for i in idx):
                first[name] = {"frame": r[0], "a": [r[i] for i in idx], "b": [s[i] for i in idx]}
    frames = [r[0] for r in common]
    game_set = min((r[0] for r in common if r[9]), default=None)
    div = min((v["frame"] for v in first.values()), default=None)
    return {"compared": len(common), "from": min(frames) if frames else None, "to": max(frames) if frames else None,
            "first_diff": first, "diverged_at": div, "game_set_frame": game_set,
            "identical_through_game_set": game_set is not None and (div is None or div > game_set)}


def run_one(c: HarnessClient, d: Path, state: str, ref_writes, inp: Dict[str, Any], mode: str,
            until: int, timeout: float) -> Dict[str, Any]:
    G.load_fixture(c, d / f"{state}.sav")
    for a, data, _ in ref_writes:
        c.write_mem(a, data)
    c.call("frame_trace_config", enabled=True)
    c.call("game_pads", record=False, clear=True)
    p0 = c.status().input_polls
    g0 = sim_frame(c)
    if mode == "inject":
        rows = inp["pads"]
        for k in range(0, len(rows), 400):
            c.call("game_pads", inject=rows[k:k + 400])
        c.call("game_pads", ports=3)
    else:
        for port, seq in si_scripts(inp["decisions"], until - g0 + 60).items():
            c.pad_script(int(port), seq, start=p0 + 1)
    c.resume()
    t0 = time.monotonic()
    while True:
        time.sleep(0.5)
        gf = sim_frame(c)
        st = B.read_match_state(c.read_mem)
        if gf >= until or st.game_set or not st.in_match:
            break
        if time.monotonic() - t0 > timeout:
            raise RuntimeError(f"timeout at game frame {gf}")
    c.pause()
    trace = c.call("frame_trace", since=g0 + 1)["rows"]
    return {"trace": trace, "g0": g0, "end": sim_frame(c), "injected": c.call("game_pads")["injected"],
            "wall_s": round(time.monotonic() - t0, 1), "final": G.small_state(c)}


def cmd_pair(args: argparse.Namespace) -> int:
    d = Path(args.dir)
    inp = json.loads(Path(args.inputs).read_text())
    until = min(args.frames or 10 ** 9, int(inp["end_frame"]) + 20)
    pair = args.pair.split(",")
    extra = ["Dolphin.Core.EmulationSpeed=0"] if args.unthrottled else []
    results = []
    for run in range(args.runs):
        specs = [(f"gprb-ab{i}", dict(cpu_thread=args.cpu == "dc")) for i in range(2)]
        orig = G.make_instance

        def mk(name, **kw):
            inst = orig(name, **kw)
            inst.config.config_args = list(inst.config.config_args or []) + extra
            return inst
        G.make_instance = mk
        rep: Dict[str, Any] = {"run": run, "cpu": args.cpu, "input": args.input, "pair": pair, "sync": args.sync}
        try:
            with G.instances(specs) as insts:
                cs = [i.client for i in insts]
                G.load_fixture(cs[0], d / f"{pair[0]}.sav")
                ws = G.sync_writes(cs[0], [x for x in args.sync.split("+") if x and x != "none"])
                res = G.par([lambda c=c, st=st: run_one(c, d, st, ws, inp, args.input, until, args.timeout)
                             for c, st in zip(cs, pair)])
                cmp = compare_traces(res[0]["trace"], res[1]["trace"])
                rep.update(cmp)
                if inp.get("trace"):
                    vr = compare_traces(inp["trace"], res[0]["trace"])
                    rep["vs_recorder"] = {k: vr[k] for k in ("compared", "diverged_at", "first_diff")}
                rep["ends"] = [r["end"] for r in res]
                rep["injected"] = [r["injected"] for r in res]
                rep["wall_s"] = [r["wall_s"] for r in res]
                rep["final"] = [r["final"]["players"] for r in res]
                rep["final_rng"] = [r["final"]["rng"] for r in res]
                rep["died"] = [i.process.poll() if i.process else None for i in insts]
        except Exception as e:  # noqa: BLE001
            rep["error"] = f"{type(e).__name__}: {e}"
        finally:
            G.make_instance = orig
        results.append(rep)
        print(f"run {run} cpu={args.cpu} input={args.input}: compared {rep.get('compared')} frames "
              f"({rep.get('from')}..{rep.get('to')}), diverged at {rep.get('diverged_at')}, "
              f"first {json.dumps(rep.get('first_diff'))[:300]}, error {rep.get('error')}, wall {rep.get('wall_s')}, "
              f"A vs recorder: {json.dumps(rep.get('vs_recorder'))[:200]}",
              flush=True)
        if args.json:
            p = Path(args.json)
            prev = json.loads(p.read_text()) if p.exists() else []
            prev.append(rep)
            p.write_text(json.dumps(prev, indent=1))
    return 0


def main(argv: Optional[Sequence[str]] = None) -> int:
    ap = argparse.ArgumentParser()
    sub = ap.add_subparsers(dest="cmd", required=True)
    p = sub.add_parser("record")
    p.add_argument("--dir", required=True)
    p.add_argument("--state", default="As")
    p.add_argument("--out", required=True)
    p.add_argument("--seed", default="r1")
    p.add_argument("--policy", default="gentle", choices=("gentle", "calm", "random"))
    p.add_argument("--frames", type=int, default=30000)
    p.add_argument("--timeout", type=float, default=3600)
    p.add_argument("--speed", type=float, default=1.0, help="emulation speed while recording (0 = unlimited)")
    p = sub.add_parser("pair")
    p.add_argument("--dir", required=True)
    p.add_argument("--inputs", required=True)
    p.add_argument("--pair", default="As,Bs")
    p.add_argument("--sync", default="rng+frames")
    p.add_argument("--cpu", default="sc", choices=("sc", "dc"))
    p.add_argument("--input", default="inject", choices=("inject", "si"))
    p.add_argument("--runs", type=int, default=1)
    p.add_argument("--frames", type=int, default=0, help="stop at this game frame (default: the recording's end)")
    p.add_argument("--unthrottled", action="store_true")
    p.add_argument("--timeout", type=float, default=3600)
    p.add_argument("--json", default=None)
    args = ap.parse_args(argv)
    logging_setup()
    return {"record": cmd_record, "pair": cmd_pair}[args.cmd](args)


def logging_setup() -> None:
    import logging
    logging.basicConfig(level=logging.WARNING, format="%(asctime)s %(name)s %(message)s")


if __name__ == "__main__":
    sys.exit(main())
