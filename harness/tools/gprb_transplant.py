"""Late-divergence root cause by memory transplant (docs/gameplay-rollback-status.md, phase 2).

Two savestates A and B taken at the same game frame of a lockstep A/B run (identical inputs,
synced RNG) diverge some frames later. A savestate holds memory *and* hardware state (CoreTiming
events, VI/AI/DSP phase, CPU registers). This tool decides which of the two carries the cause:

1. Load A and B, dump both memories, list the byte ranges that differ.
2. Baselines: run A and B (pre-scheduled recorded inputs) to --to and record the observables.
3. Trials: load A, write B's bytes for a subset of the differing ranges, run, and classify the
   outcome as "A", "B" or "other". Delta-debug the subset down to the ranges that flip A to B.

    python harness/tools/gprb_transplant.py --dir run/qa/gprb/ps2 --inputs run/qa/gprb/ps2/play-AB-sc-long.json \
        --from 8420 --to 8462
"""

from __future__ import annotations

import argparse
import json
import sys
import time
from pathlib import Path
from typing import Any, Dict, List, Sequence, Tuple

sys.path.insert(0, str(Path(__file__).resolve().parent))

import numpy as np  # noqa: E402

import gameplay_rollback as G  # noqa: E402

Range = Tuple[int, int]


def dump(c) -> Dict[int, bytes]:
    return {G.MEM1[0]: G.read_big(c, *G.MEM1), G.MEM2[0]: G.read_big(c, *G.MEM2)}


def diff_ranges(da: Dict[int, bytes], db: Dict[int, bytes], gran: int = 32, gap: int = 64) -> List[Range]:
    out: List[Range] = []
    for base in (G.MEM1[0], G.MEM2[0]):
        for o, n in G.diff_runs(da[base], db[base], gran=gran, merge_gap=gap):
            out.append((base + o, n))
    return out


def scripts_from(rec: Sequence[Sequence[Any]], f0: int, to: int) -> Dict[int, List[Dict[str, Any]]]:
    """The recorded closed-loop submissions as one script per port starting at frame f0+1."""
    tl = G.timeline_from_inputs(rec, to + 5)   # sim-relative: entry t is used at game frame t
    out = {}
    for port, seq in tl.items():
        t = 1
        rest = []
        for x in seq:
            h = int(x["hold"])
            if t + h > f0 + 1:
                skip = max(0, f0 + 1 - t)
                rest.append(dict(x, hold=h - skip))
            t += h
        out[port] = rest
    return out


class Runner:
    def __init__(self, c, d: Path, rec, f0: int, to: int):
        self.c, self.d, self.f0, self.to = c, d, f0, to
        self.scripts = scripts_from(rec, f0, to)

    def run(self, state: str, writes: Sequence[Tuple[int, bytes]] = ()) -> Dict[str, Any]:
        c = self.c
        G.load_fixture(c, self.d / state)
        for a, data in writes:
            for o in range(0, len(data), G.CHUNK):
                c.write_mem(a + o, data[o:o + G.CHUNK])
        p0 = c.status().input_polls
        g0 = G.game_frame(c)
        for port, seq in self.scripts.items():
            c.pad_script(int(port), seq, start=p0 + 1)
        rows = []
        for f in range(g0 + 1, g0 + (self.to - self.f0) + 1):
            G.step_to_game_frame(c, f, max_fields=10)
            s = G.small_state(c)
            rows.append([f, s["players"], s["rng"]])
        return {"rows": rows}


def classify(r: Dict[str, Any], ra: Dict[str, Any], rb: Dict[str, Any]) -> str:
    a = r["rows"] == ra["rows"]
    b = r["rows"] == rb["rows"]
    return "A" if a and not b else "B" if b and not a else "AB" if a and b else "other"


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--dir", required=True)
    ap.add_argument("--inputs", required=True)
    ap.add_argument("--from", dest="start", type=int, default=8420)
    ap.add_argument("--to", type=int, default=8462)
    ap.add_argument("--json", default=None)
    ap.add_argument("--exclude", default="", help="comma list of addr:len ranges never transplanted")
    ap.add_argument("--exclude-os", action="store_true",
                    help="never transplant OS/audio/video/stack state: lowmem, DOL .bss OS+AX+pad areas, .sdata/.sbss "
                         "(except the RNG), the boot stack, SystemFW/Thread/Sound/RenderFifo/CopyFB/Network/Replay/Tmp "
                         "heaps and MEM2 below the first heap")
    ap.add_argument("--only", default="", help="comma list of addr:len: transplant only differing bytes inside these")
    args = ap.parse_args()
    d = Path(args.dir)
    rec = json.loads(Path(args.inputs).read_text())[0]["inputs"]
    out: Dict[str, Any] = {"trials": []}
    with G.instances([("gprb-tp0", dict(video="Null")), ("gprb-tp1", dict(video="Null"))]) as insts:
        ca, cb = insts[0].client, insts[1].client
        G.par([lambda: G.load_fixture(ca, d / f"lockstep-{args.start}-0.sav"),
               lambda: G.load_fixture(cb, d / f"lockstep-{args.start}-1.sav")])
        da, db = G.par([lambda: dump(ca), lambda: dump(cb)])
        ranges = diff_ranges(da, db)
        excl = [tuple(int(x, 0) for x in s.split(":")) for s in args.exclude.split(",") if s]
        if args.exclude_os:
            heaps = G.heap_dicts(ca)
            excl += [(0x80000000, 0x4000), (0x80499000, 0x5000), (0x804B0000, 0xEC420), (0x8059C420, 0x3C98),
                     (0x8059FF5C, 0x15C), (0x805A00C0, 0x360), (0x805A0428, 0x4D38), (0x805A5160, 0x10000)]
            excl += [(h["start"], h["end"] - h["start"]) for h in heaps
                     if h["name"] in ("System FW", "SystemFW", "Thread", "Sound", "RenderFifo", "CopyFB", "Network",
                                      "Replay", "Tmp")]
            mem2_first = min((h["start"] for h in heaps if h["start"] >= 0x90000000), default=0x90000000)
            excl.append((0x90000000, mem2_first - 0x90000000))
            print("excluded:", [(hex(a), hex(n)) for a, n in excl], flush=True)
        only = [tuple(int(x, 0) for x in s.split(":")) for s in args.only.split(",") if s]
        if only:
            ranges = [(max(a, o[0]), min(a + n, o[0] + o[1]) - max(a, o[0])) for a, n in ranges for o in only
                      if a < o[0] + o[1] and o[0] < a + n]
        ranges = [r for r in ranges if not any(r[0] < e[0] + e[1] and e[0] < r[0] + r[1] for e in excl)]
        print(f"{len(ranges)} differing ranges, {sum(n for _, n in ranges)} bytes", flush=True)
        ra_, rb_ = Runner(ca, d, rec, args.start, args.to), Runner(cb, d, rec, args.start, args.to)
        base_a, base_b = G.par([lambda: ra_.run(f"lockstep-{args.start}-0.sav"),
                                lambda: rb_.run(f"lockstep-{args.start}-1.sav")])
        first = next((ra[0] for ra, rb in zip(base_a["rows"], base_b["rows"]) if ra != rb), None)
        print(f"baseline: A and B first differ at game frame {first}", flush=True)
        if first is None:
            return 1

        def bytes_of(rs: Sequence[Range]) -> List[Tuple[int, bytes]]:
            w = []
            for a, n in rs:
                base = G.MEM1[0] if a < G.MEM2[0] else G.MEM2[0]
                w.append((a, db[base][a - base:a - base + n]))
            return w

        def trial(rs: Sequence[Range], tag: str) -> str:
            t0 = time.monotonic()
            try:
                r = ra_.run(f"lockstep-{args.start}-0.sav", bytes_of(rs))
                res = classify(r, base_a, base_b)
            except Exception as e:  # noqa: BLE001
                res = f"error: {type(e).__name__}: {e}"
                # The instance may be wedged; reload happens on the next trial anyway.
            out["trials"].append({"tag": tag, "n": len(rs), "bytes": sum(n for _, n in rs), "result": res,
                                  "ranges": [[hex(a), n] for a, n in rs[:64]]})
            print(f"  {tag}: {len(rs)} ranges, {sum(n for _, n in rs)} B -> {res} ({time.monotonic() - t0:.0f} s)", flush=True)
            if args.json:
                Path(args.json).write_text(json.dumps(out, indent=1))
            return res

        res = trial(ranges, "all")
        if res != "B":
            print("transplanting all differing memory does not give B's outcome: the cause is not (only) in memory")
            return 0
        # Delta debugging (ddmin-lite): keep halving the set that still flips to B.
        cur = list(ranges)
        while len(cur) > 1:
            half = len(cur) // 2
            lo, hi = cur[:half], cur[half:]
            if trial(lo, f"lo{len(lo)}") == "B":
                cur = lo
            elif trial(hi, f"hi{len(hi)}") == "B":
                cur = hi
            else:
                # Needs parts of both halves: shrink each side while keeping the other whole.
                print("  interaction: needs both halves; refining each side", flush=True)
                keep = []
                for side, other in ((lo, hi), (hi, lo)):
                    part = list(side)
                    while len(part) > 1:
                        h = len(part) // 2
                        if trial(part[:h] + other + keep, f"side{len(part[:h])}") == "B":
                            part = part[:h]
                        elif trial(part[h:] + other + keep, f"side{len(part[h:])}") == "B":
                            part = part[h:]
                        else:
                            break
                    keep += part
                cur = keep
                break
        out["culprit"] = [[hex(a), n, da[G.MEM1[0] if a < G.MEM2[0] else G.MEM2[0]][(a & 0x0FFFFFFF):(a & 0x0FFFFFFF) + min(n, 64)].hex(),
                           db[G.MEM1[0] if a < G.MEM2[0] else G.MEM2[0]][(a & 0x0FFFFFFF):(a & 0x0FFFFFFF) + min(n, 64)].hex()]
                          for a, n in cur]
        print("culprit ranges:", json.dumps(out["culprit"], indent=1))
        if args.json:
            Path(args.json).write_text(json.dumps(out, indent=1))
    return 0


if __name__ == "__main__":
    sys.exit(main())
