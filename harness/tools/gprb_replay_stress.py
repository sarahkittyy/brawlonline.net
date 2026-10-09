"""Replay one pass log many times, optionally on a loaded machine, and count the outcomes
(gameplay rollback, docs/gameplay-rollback-status.md, open issue 9).

A replay (``gprb_synctest`` ``replay_path``) runs a recorded sync test or network peer again with
its exact passes and loads, from the run's countdown savestate. Its emulated state is determined by
the log alone, so every replay must equal the ground truth (the same log flattened: every frame
once, no rollback). Outcomes per replay:

- ``ok``     the per-frame trace equals the ground truth to the end of the log
- ``drift``  it differs from some frame on (first differing frame and fields)
- ``hang``   no session frame for ``--hang-s`` seconds; the guest CPU state (``cpu_state``) and the
             main thread's back chain are kept
- ``crash``  the Dolphin process died
- ``error``  a harness error (launch, connection)

Host load: ``--burn N`` runs N CPU-burning processes at normal priority for the whole run (the
test instances run below normal), as the other agents' instances did when the crashes clustered.
``--env K=V`` passes environment variables to every instance (``PPR_GPRB_EVICT_DELAY_US``, ...).

    python harness/tools/gprb_replay_stress.py --state run/qa/i9/gen1/work/m-yoshi/m-yoshi.sav \\
        --log run/qa/i9/gen1/work/m-yoshi/m-yoshi.synctest.m0 --n 25 --jobs 4 --burn 12 \\
        --build run/bin/gprb-e642b3ce98 --out run/qa/i9/before.jsonl
"""

from __future__ import annotations

import argparse
import contextlib
import json
import os
import subprocess
import sys
import threading
import time
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path
from typing import Any, Dict, List, Optional

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
sys.path.insert(0, str(Path(__file__).resolve().parent))

import gameplay_rollback as G  # noqa: E402
import gprb_passlog as PL  # noqa: E402
from gprb_ab import compare_traces  # noqa: E402

ROOT = Path(__file__).resolve().parents[2]
MAIN_THREAD = 0x804DD558
BURN_SRC = "while True:\n    pass\n"


def flatten(log: Path, out: Path) -> None:
    header, ups = PL.read(str(log))
    last = PL.final_inputs(ups)
    for u in ups:
        u[0] = 0
        u[2] = [[p[0], p[1], last[p[0]]] for p in u[2][-1:]]
    PL.write(str(out), header, ups)


def one(args: argparse.Namespace, idx: int, log: Path, label: str) -> Dict[str, Any]:
    import gprb_sweep as SW
    rep: Dict[str, Any] = {"run": idx, "label": label}
    t0 = time.monotonic()
    env = dict(kv.split("=", 1) for kv in args.env)
    players = args.players
    if not players:
        # A gprb_mispredict fixture says how many players it has; sweep work dirs are 2-player.
        meta = Path(str(args.state) + ".json")
        players = int(json.loads(meta.read_text()).get("players", 2)) if meta.exists() else 2
    try:
        with SW.instance(f"{args.prefix}-{label}{idx}", args.cpu, args.video, unthrottled=True, env=env,
                         controllers=range(players)) as inst:
            try:
                c = inst.client
                G.load_fixture(c, Path(args.state))
                c.call("frame_trace_config", enabled=True)
                c.call("gprb_synctest", distance=7, region_set=args.region_set, hash_regions=False,
                       start_frame=args.start_frame, replay_path=str(log.resolve()), ports=(1 << players) - 1)
                c.resume()
                last, since = -1, time.monotonic()
                s: Dict[str, Any] = {}
                while True:
                    time.sleep(1)
                    if not inst.is_running():
                        rep["result"] = "crash"
                        rep["exit_code"] = inst.process.poll() if inst.process else None
                        break
                    try:
                        s = c.call("gprb_status")
                    except Exception as e:  # noqa: BLE001
                        if time.monotonic() - since > args.hang_s:
                            rep["result"], rep["detail"] = "hang", f"harness not answering: {e}"
                            break
                        continue
                    if s["phase"] in ("ended", "error"):
                        break
                    # The countdown runs before the first rollback and is slow on a starved machine:
                    # the stall limit counts only in the running phase.
                    if s["current_frame"] != last or s["phase"] != "running":
                        last, since = s["current_frame"], time.monotonic()
                    elif time.monotonic() - since > args.hang_s:
                        rep["result"] = "hang"
                        rep["frame"] = last
                        with contextlib.suppress(Exception):
                            rep["cpu"] = c.call("cpu_state")
                        with contextlib.suppress(Exception):
                            from gprb_debug import thread_backtrace
                            rep["main_thread"] = thread_backtrace(c, MAIN_THREAD)
                        break
                    if time.monotonic() - t0 > args.timeout:
                        rep["result"], rep["frame"] = "timeout", last
                        break
                with contextlib.suppress(Exception):
                    s = c.call("gprb_status")
                rep["status"] = {k: s.get(k) for k in (
                    "phase", "current_frame", "rollbacks", "end_reason", "error", "load_count",
                    "load_us_max", "save_us_max", "load_evict_waits", "load_evict_wait_us_max",
                    "end_game_frame")}
                if rep.get("result") not in ("crash",):
                    with contextlib.suppress(Exception):
                        rep["_trace"] = c.call("frame_trace", since=args.start_frame)["rows"]
            finally:
                with contextlib.suppress(Exception):
                    inst.cleanup(True)
    except Exception as e:  # noqa: BLE001
        rep["result"], rep["detail"] = "error", f"{type(e).__name__}: {e}"
    rep["wall_s"] = round(time.monotonic() - t0, 1)
    return rep


def burners(n: int) -> List[subprocess.Popen]:
    return [subprocess.Popen([sys.executable, "-c", BURN_SRC], stdout=subprocess.DEVNULL,
                             stderr=subprocess.DEVNULL) for _ in range(n)]


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--state", required=True)
    ap.add_argument("--log", required=True, help="the pass log (PPR_GPRB_PASS_LOG / the sweep's .synctest.m0)")
    ap.add_argument("--n", type=int, default=10)
    ap.add_argument("--jobs", type=int, default=3)
    ap.add_argument("--burn", type=int, default=0, help="CPU-burning processes at normal priority")
    ap.add_argument("--build", default=str(ROOT / "run/bin/npd-d2a443a6"))
    ap.add_argument("--cpu", default="sc", choices=("sc", "dc"))
    ap.add_argument("--cpu-core", type=int, default=None, help="Dolphin.Core.CPUCore (0 interpreter, 1 JIT64, 5 cached interpreter)")
    ap.add_argument("--video", default="Null")
    ap.add_argument("--region-set", default="gp-v21")
    ap.add_argument("--start-frame", type=int, default=240)
    ap.add_argument("--players", type=int, default=0,
                    help="players of the fixture (default: <state>.json of gprb_mispredict prep, else 2)")
    ap.add_argument("--env", action="append", default=[], help="K=V for every instance")
    ap.add_argument("--hang-s", type=float, default=30)
    ap.add_argument("--timeout", type=float, default=1800)
    ap.add_argument("--gt", default=None, help="ground-truth trace JSON (written by an earlier run); default: replay the flattened log first")
    ap.add_argument("--prefix", default=os.environ.get("GPRB_NAME_PREFIX", "gprbstress"))
    ap.add_argument("--out", required=True, help="JSON lines, one per replay (appended)")
    args = ap.parse_args()
    os.environ["PPHARNESS_DOLPHIN_DIR"] = str(Path(args.build).resolve())
    if args.cpu_core is not None:
        orig = G.make_instance

        def mk(name, **kw):
            inst = orig(name, **kw)
            inst.config.config_args = list(inst.config.config_args or []) + [f"Dolphin.Core.CPUCore={args.cpu_core}"]
            return inst
        G.make_instance = mk
    out = Path(args.out)
    out.parent.mkdir(parents=True, exist_ok=True)
    log = Path(args.log)

    gt_path = Path(args.gt) if args.gt else out.with_suffix(".gt.json")
    if gt_path.exists():
        gt = json.loads(gt_path.read_text())
    else:
        flat = out.with_suffix(".flat")
        flatten(log, flat)
        r = one(args, 0, flat, "gt")
        gt = r.pop("_trace", None)
        if not gt or r.get("result"):
            print("ground truth failed:", json.dumps(r)[:600])
            return 1
        gt_path.write_text(json.dumps(gt))
        print(f"ground truth: {len(gt)} frames, end {r.get('status')}", flush=True)

    procs = burners(args.burn)
    lock = threading.Lock()
    counts: Dict[str, int] = {}
    try:
        def task(i: int) -> None:
            r = one(args, i, log, "r")
            tr = r.pop("_trace", None) or []
            if tr:
                # Up to the game frame where the log ended (the game runs on without input after it).
                end = (r.get("status") or {}).get("end_game_frame") or 10 ** 9
                hi = min(max(x[0] for x in tr), max(x[0] for x in gt), end) - 8
                cmp = compare_traces([x for x in gt if x[0] <= hi], [x for x in tr if x[0] <= hi])
                r["compared"], r["to"] = cmp["compared"], cmp["to"]
                if cmp["diverged_at"] is not None:
                    r["diverged_at"], r["fields"] = cmp["diverged_at"], sorted(cmp["first_diff"])
                    r.setdefault("result", "drift")
            r.setdefault("result", "ok" if tr else "error")
            r["burn"], r["env"], r["build"] = args.burn, args.env, Path(args.build).name
            with lock:
                counts[r["result"]] = counts.get(r["result"], 0) + 1
                with out.open("a") as f:
                    f.write(json.dumps(r) + "\n")
                st = r.get("status") or {}
                print(f"[{time.strftime('%H:%M:%S')}] {i}: {r['result']} frame {r.get('frame', st.get('current_frame'))} "
                      f"diverged {r.get('diverged_at')} evict_waits {st.get('load_evict_waits')} "
                      f"bt {(r.get('main_thread') or [])[:3]} wall {r['wall_s']} s  totals {counts}", flush=True)

        with ThreadPoolExecutor(args.jobs) as pool:
            list(pool.map(task, range(1, args.n + 1)))
    finally:
        for p in procs:
            with contextlib.suppress(Exception):
                p.kill()
    print("totals", counts)
    return 0


if __name__ == "__main__":
    sys.exit(main())
