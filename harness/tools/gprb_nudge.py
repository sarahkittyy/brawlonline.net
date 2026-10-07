"""Does the match simulation depend on the emulated-time phase? (gameplay-rollback phase 2)

Loads one savestate, lets N cycles of emulated time pass without executing code (`timing_nudge`),
then replays recorded inputs while the instance runs freely, and compares the per-game-frame trace
(`frame_trace`: fighters, RNG) with the un-nudged run. Memory is identical at the start of every
run, so any difference comes from where CoreTiming events (VI, audio DMA/DSP, IO, alarms) land
relative to the code.

    python harness/tools/gprb_nudge.py --state run/qa/gprb/ps2/lockstep-8420-0.sav \
        --inputs run/qa/gprb/ps2/play-AB-sc-long.json --from 8420 --frames 600 --nudges 0,1000,100000,1000000
"""

from __future__ import annotations

import argparse
import json
import sys
import time
from pathlib import Path
from typing import Any, Dict, List, Optional

sys.path.insert(0, str(Path(__file__).resolve().parent))

import gameplay_rollback as G  # noqa: E402
from gprb_transplant import scripts_from  # noqa: E402


def trace(c, since: int) -> List[List[Any]]:
    return c.call("frame_trace", since=since)["rows"]


def run_free(c, state: Path, scripts, nudge: int, until: int, cfg: Optional[Dict[str, Any]] = None,
             timeout: float = 900.0) -> List[List[Any]]:
    G.load_fixture(c, state)
    if cfg is not None:
        c.call("frame_trace_config", **cfg)
    g0 = G.game_frame(c)
    if nudge:
        c.call("timing_nudge", cycles=nudge)
    p0 = c.status().input_polls
    for port, seq in scripts.items():
        c.pad_script(int(port), seq, start=p0 + 1)
    c.resume()
    deadline = time.monotonic() + timeout
    while G.game_frame(c) < until:
        if time.monotonic() > deadline:
            raise RuntimeError("timeout")
        time.sleep(0.2)
    c.pause()
    rows = trace(c, g0 + 1)
    return [r for r in rows if r[0] <= until]


def first_diff(a: List[List[Any]], b: List[List[Any]], fields=(4, 5, 6, 7, 11)) -> Optional[Dict[str, Any]]:
    """Rows: [frame, persistent, steps, ticks, rng0, rng1, rng2, fighters_crc, range_hash, game_set, resim, fighters]."""
    bm = {r[0]: r for r in b}
    for r in a:
        s = bm.get(r[0])
        if s is None:
            continue
        for i in fields:
            if r[i] != s[i]:
                return {"frame": r[0], "field": i, "a": r[i], "b": s[i]}
    return None


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--state", required=True)
    ap.add_argument("--inputs", required=True)
    ap.add_argument("--from", dest="start", type=int, default=8420)
    ap.add_argument("--frames", type=int, default=600)
    ap.add_argument("--nudges", default="0,0,1000,100000,1000000")
    ap.add_argument("--cpu", default="sc", choices=("sc", "dc"))
    ap.add_argument("--json", default=None)
    args = ap.parse_args()
    rec = json.loads(Path(args.inputs).read_text())[0]["inputs"]
    until = args.start + args.frames
    scripts = scripts_from(rec, args.start, until)
    out: Dict[str, Any] = {"runs": []}
    with G.instances([("gprb-nudge", dict(video="Null", cpu_thread=args.cpu == "dc"))]) as (inst,):
        c = inst.client
        base = None
        for n in [int(x) for x in args.nudges.split(",")]:
            t0 = time.monotonic()
            rows = run_free(c, Path(args.state), scripts, n, until)
            if base is None:
                base = rows
                d = None
            else:
                d = first_diff(base, rows)
            ticks = rows[0][3] if rows else None
            out["runs"].append({"nudge": n, "rows": len(rows), "first_diff_vs_first_run": d, "ticks0": ticks})
            print(f"nudge {n:>9}: {len(rows)} frames, first diff vs run 0: {d}  ({time.monotonic() - t0:.0f} s)", flush=True)
            if args.json:
                Path(args.json).write_text(json.dumps(out, indent=1))
    return 0


if __name__ == "__main__":
    sys.exit(main())
