"""Find what a resimulated frame reads differently from its first run (gameplay rollback phase 3).

1. ``prep``: boot (JIT), set up a match on the CSS/SSS, save a state in the countdown.
2. ``find``: load it, arm the sync test, play a fixed pad script; report the first frame whose
   checksum differs from its first run (``desync_log``).
3. ``trace``: same under the interpreter, recording both the first run and the resimulation of
   that frame (``cpu_trace`` occurrences=2, main thread), then diff the two instruction streams:
   the first load whose value differs (with identical control flow up to there) is the culprit.

    python harness/tools/gprb_resimtrace.py prep --out run/qa/gprb4/bf-cd.sav
    python harness/tools/gprb_resimtrace.py find --state run/qa/gprb4/bf-cd.sav --region-set gp-v4
    python harness/tools/gprb_resimtrace.py trace --state run/qa/gprb4/bf-cd.sav --region-set gp-v4 --frame 265
"""

from __future__ import annotations

import argparse
import json
import sys
import time
from pathlib import Path
from typing import Any, Dict, List

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
sys.path.insert(0, str(Path(__file__).resolve().parent))

import numpy as np  # noqa: E402

import gameplay_rollback as G  # noqa: E402
import gprb_synctest as T  # noqa: E402
from gprb_debug import sym  # noqa: E402

DT = np.dtype([("pc", "<u4"), ("op", "<u4"), ("ea", "<u4"), ("flags", "<u4"), ("val", "<u8")])

def make_script(seed: str = "rt1", frames: int = 1500) -> Dict[int, List[Dict[str, Any]]]:
    """Seeded open-loop macros (gprb_ab.gentle_macro) for ports 0 and 1, ~`frames` game frames."""
    import random
    from gprb_ab import gentle_macro, norm
    out: Dict[int, List[Dict[str, Any]]] = {}
    for port in (0, 1):
        rng = random.Random(f"{seed}:{port}")
        seq: List[Dict[str, Any]] = [{"hold": 10}]
        t = 10
        toward = 1 if port == 0 else -1
        while t < frames:
            if rng.random() < 0.1:
                toward = -toward
            for x in norm(gentle_macro(rng, toward)):
                seq.append(x)
                t += int(x["hold"])
        out[port] = seq
    return out


SCRIPT = make_script()


def with_core(core: int):
    orig = G.make_instance

    def mk(name, **kw):
        inst = orig(name, **kw)
        inst.config.config_args = list(inst.config.config_args or []) + [f"Dolphin.Core.CPUCore={core}"]
        return inst
    G.make_instance = mk


def cmd_prep(args) -> int:
    p1, p2, stage, items = T.SCENARIOS[args.scenario]
    with G.instances([("gprb-rt-prep", dict())]) as (inst,):
        c = inst.client
        T.setup_match(c, p1, p2, stage, items)
        while True:
            fc = G.B.read_frame_counters(c.read_mem)
            if fc.get("game_frame") and fc.get("persistent") and fc["persistent"] > fc["game_frame"] >= 100:
                break
            time.sleep(0.05)
        c.pause()
        c.save_state(str(Path(args.out).resolve()))
        print("saved at game frame", G.game_frame(c))
    return 0


def start_run(c, args, trace_frame: int = 0, trace_path: str = "") -> None:
    G.load_fixture(c, Path(args.state))
    c.call("gprb_synctest", distance=args.distance, region_set=args.region_set, hash_regions=False,
           start_frame=args.start_frame)
    if trace_frame:
        c.call("cpu_trace", path=trace_path, first_frame=trace_frame, frames=1, occurrences=2,
               max_records=60_000_000)
    p0 = c.status().input_polls
    g0 = G.game_frame(c)
    for port, seq in SCRIPT.items():
        c.pad_script(port, seq, start=p0 + max(1, args.input_frame - g0))
    c.resume()


def cmd_find(args) -> int:
    with G.instances([("gprb-rt-find", dict())]) as (inst,):
        c = inst.client
        start_run(c, args)
        t0 = time.monotonic()
        while time.monotonic() - t0 < 240:
            s = c.call("gprb_status")
            if s["desync_log"] or (s["phase"] == "running" and s["current_frame"] > 1500):
                break
            time.sleep(0.5)
        s = c.call("gprb_status")
        print(json.dumps({k: s[k] for k in ("phase", "current_frame", "desyncs_detected", "desync_log")}, indent=1))
    return 0


def cmd_trace(args) -> int:
    with_core(0)
    out = Path(args.out_dir)
    out.mkdir(parents=True, exist_ok=True)
    path = str((out / "resim.bin").resolve())
    with G.instances([("gprb-rt-trace", dict())]) as (inst,):
        c = inst.client
        start_run(c, args, args.frame, path)
        t0 = time.monotonic()
        while not c.call("cpu_trace")["done"]:
            if time.monotonic() - t0 > args.timeout:
                print("trace timeout", c.call("cpu_trace"), c.call("gprb_status")["current_frame"])
                return 1
            time.sleep(1)
        print("traced", c.call("cpu_trace"), "desyncs", c.call("gprb_status")["desync_log"][:3])
    diff(path + ".0", path + ".1")
    return 0


def diff(pa: str, pb: str) -> None:
    A = np.fromfile(pa, dtype=DT)
    B = np.fromfile(pb, dtype=DT)
    n = min(len(A), len(B))
    pcd = np.nonzero(A["pc"][:n] != B["pc"][:n])[0]
    first_pc = int(pcd[0]) if len(pcd) else n
    loads = ((A["flags"][:first_pc] & 1) != 0)
    vd = np.nonzero(loads & (A["val"][:first_pc] != B["val"][:first_pc]))[0]
    print(f"records {len(A)} / {len(B)}; first control-flow difference at #{first_pc}; "
          f"{len(vd)} loads with different values before it")
    for i in vd[:25]:
        a, b = A[i], B[i]
        print(f"  #{i:8d} pc {a['pc']:08x} {sym(int(a['pc'])):40s} ea {a['ea']:08x} first {a['val']:x} resim {b['val']:x}")
    if first_pc < n:
        print("  control flow:", f"{A['pc'][first_pc]:08x} / {B['pc'][first_pc]:08x}", sym(int(A["pc"][first_pc - 1])))


def main() -> int:
    ap = argparse.ArgumentParser()
    sub = ap.add_subparsers(dest="cmd", required=True)
    p = sub.add_parser("prep")
    p.add_argument("--out", required=True)
    p.add_argument("--scenario", default="bf-fox-falco")
    for nm in ("find", "trace"):
        p = sub.add_parser(nm)
        p.add_argument("--state", required=True)
        p.add_argument("--region-set", default="gp-v4")
        p.add_argument("--distance", type=int, default=2)
        p.add_argument("--start-frame", type=int, default=240)
        p.add_argument("--input-frame", type=int, default=250)
        if nm == "trace":
            p.add_argument("--frame", type=int, required=True)
            p.add_argument("--out-dir", default="run/qa/gprb4/trace")
            p.add_argument("--timeout", type=float, default=1800)
    a = ap.parse_args()
    return {"prep": cmd_prep, "find": cmd_find, "trace": cmd_trace}[a.cmd](a)


if __name__ == "__main__":
    sys.exit(main())
