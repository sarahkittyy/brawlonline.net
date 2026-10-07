"""Cross-platform determinism: replay one recorded input timeline on one machine, write a per-frame
state trace, then compare traces made on different OSes (or builds).

``run``   Boot one instance (default profile sc-rtc: single core, fixed RTC, Null video), replay
          a ``determinism.py record`` timeline exactly like ``determinism.py compare`` does in
          "events" mode (every pad submission while paused at its recorded VI field), and pause at
          checkpoints: every ``--every`` VI fields before GO and every ``--match-every`` (default 1,
          i.e. every frame) from GO to the end. At each checkpoint it hashes each labelled region
          (``brawl.gameplay_ranges`` in the match, ``brawl.menu_ranges`` on menus; both already
          leave out ``brawl.STATE_EXCLUDED_RANGES``), whole MEM1/MEM2 for reference, and records
          the per-player state, RNG seeds, frame counters and the OS start time (0x800030D8).

``diff``  Compare two traces frame by frame: first divergence overall, first gameplay
          divergence (labelled regions or player/RNG state; whole MEM1/MEM2 are reported
          separately), and how many checkpoints matched.

    python harness/tools/xplat_trace.py run --timeline run/qa/timeline.json --out run/qa/trace-linux.json
    python harness/tools/xplat_trace.py diff run/qa/trace-windows.json run/qa/trace-linux.json

The offline custom RTC still follows the host clock (docs/determinism-findings.md, cause 1), so
two runs can boot one RTC second apart; ``diff`` checks the OS start time first and says so.
``run --osstart HEX`` retries (up to ``--attempts``) until the OS start time equals a reference.
"""

from __future__ import annotations

import argparse
import contextlib
import json
import logging
import platform
import sys
import time
from pathlib import Path
from typing import Any, Dict, List, Mapping, Optional, Sequence

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
sys.path.insert(0, str(Path(__file__).resolve().parent))

from ppharness import brawl as B  # noqa: E402
from ppharness.client import HarnessCommandError  # noqa: E402
from ppharness.session import FrameKey, pause_at  # noqa: E402

import determinism as D  # noqa: E402

log = logging.getLogger("xplat_trace")

OS_START_TIME = 0x800030D8  # __OSStartTime (u64 timebase ticks), set from the RTC at boot


def _os_start(c) -> str:
    return c.read_mem(OS_START_TIME, 8).hex()


def run(timeline: Mapping[str, Any], profile: D.Profile, every: int = 30, match_every: int = 1,
        keep: bool = False, os_start_ref: Optional[str] = None) -> Dict[str, Any]:
    inst = D.make_instance(f"xplat-{profile.name}", profile, keep)
    trace: Dict[str, Any] = {
        "profile": profile.name, "host": {"system": platform.system(), "machine": platform.machine(),
                                          "python": platform.python_version()},
        "exe": inst.command_prefix[0], "every": every, "match_every": match_every,
        "checkpoints": {}, "events_submitted": 0, "input_errors": []}
    ok = False
    try:
        inst.launch()
        c = inst.connect()
        c.wait_state("running", timeout=120)
        for port in (0, 1):
            c.pad_set(port)
        trace["version"] = c.status().raw.get("version")
        events = sorted(timeline["events"], key=lambda e: e["frame"])
        go, end = int(timeline["go_frame"]), int(timeline["end_frame"])
        cps = sorted(set(range(every, go, every)) | set(range(go, end, match_every)))
        schedule: Dict[int, List[Any]] = {}
        for f in cps:
            schedule.setdefault(f, []).append(("cp", None))
        for e in events:
            schedule.setdefault(int(e["frame"]), []).append(("ev", e))
        order = sorted(schedule)
        boot_deadline = time.monotonic() + 90
        while c.status().frame < 120:
            if time.monotonic() > boot_deadline:
                raise RuntimeError(f"boot stalled at VI frame {c.status().frame}")
            time.sleep(0.2)
        floor = c.status().frame + 2
        early = [f for f in order if f < floor]
        if any(k == "ev" for f in early for k, _ in schedule[f]):
            raise RuntimeError("the instance passed an input frame before the replay could stop there")
        order = [f for f in order if f >= floor]
        trace["os_start"] = _os_start(c)
        if os_start_ref and trace["os_start"] != os_start_ref:
            trace["error"] = f"os start time {trace['os_start']} != reference {os_start_ref}"
            return trace
        t0 = time.monotonic()
        for idx, target in enumerate(order):
            pause_at(c, target, FrameKey.frame(), margin=D.STAY_PAUSED + 1, timeout=60,
                     max_advance=D.STAY_PAUSED + 200)
            items = schedule[target]
            if any(k == "cp" for k, _ in items):
                scene, rs = D.labelled_ranges(c)
                rs = rs + [(0x80000000, 0x1800000, "MEM1 (all)"), (0x90000000, 0x4000000, "MEM2 (all)")]
                trace["checkpoints"][str(target)] = {
                    "scene": scene, "poll": c.status().input_polls,
                    "regions": {lbl: c.hash_mem([[a, n]]) for a, n, lbl in rs},
                    "small": D.small_state(c),
                    "checksum": B.brawlback_checksum(c.read_mem, B.CORRECTED_CHECKSUM_FIELDS),
                }
            for k, e in items:
                if k != "ev":
                    continue
                try:
                    c.pad_script(int(e["port"]), e["frames"], start=int(e["start"]))
                except HarnessCommandError as err:
                    trace["input_errors"].append(f"frame {e['frame']}: {err}")
                    c.pad_script(int(e["port"]), e["frames"], start="next")
                trace["events_submitted"] += 1
            if idx + 1 >= len(order) or order[idx + 1] - target > D.STAY_PAUSED:
                c.resume()
        trace["wall_s"] = round(time.monotonic() - t0, 1)
        trace["final"] = {p.port: {"damage": p.damage, "stocks": p.stocks} for p in B.read_players(c.read_mem)}
        trace["recorded_final"] = timeline.get("final")
        ok = True
        return trace
    except Exception as e:  # noqa: BLE001 - keep what was traced so far
        trace["error"] = f"{type(e).__name__}: {e}"
        with contextlib.suppress(Exception):
            trace["log_tail"] = inst.log.tail(10)
        return trace
    finally:
        with contextlib.suppress(Exception):
            inst.cleanup(ok)


def diff(a: Mapping[str, Any], b: Mapping[str, Any]) -> Dict[str, Any]:
    ca, cb = a["checkpoints"], b["checkpoints"]
    frames = sorted({int(f) for f in ca} & {int(f) for f in cb})
    out: Dict[str, Any] = {
        "a": {k: a.get(k) for k in ("host", "exe", "version", "os_start", "final", "error")},
        "b": {k: b.get(k) for k in ("host", "exe", "version", "os_start", "final", "error")},
        "os_start_equal": a.get("os_start") == b.get("os_start"),
        "checkpoints_common": len(frames), "only_a": len(ca) - len(frames), "only_b": len(cb) - len(frames),
        "gameplay_equal": 0, "mem_equal": 0, "in_match": 0, "first_divergence": None,
        "first_gameplay_divergence": None, "differing_frames": 0,
    }
    for f in frames:
        x, y = ca[str(f)], cb[str(f)]
        labels = sorted(set(x["regions"]) | set(y["regions"]))
        regions = [lbl for lbl in labels if x["regions"].get(lbl) != y["regions"].get(lbl)]
        small = [k for k in sorted(set(x["small"]) | set(y["small"])) if x["small"].get(k) != y["small"].get(k)]
        cks = x.get("checksum") != y.get("checksum")
        gameplay = [r for r in regions if not r.startswith("MEM")] + small + (["checksum"] if cks else [])
        if x["scene"] == "scMelee":
            out["in_match"] += 1
        if not gameplay:
            out["gameplay_equal"] += 1
        if not regions:
            out["mem_equal"] += 1
        if regions or small or cks:
            out["differing_frames"] += 1
            rec = {"frame": f, "scene": x["scene"], "regions": regions, "small": {k: [x["small"].get(k), y["small"].get(k)] for k in small}}
            if out["first_divergence"] is None:
                out["first_divergence"] = rec
            if gameplay and out["first_gameplay_divergence"] is None:
                out["first_gameplay_divergence"] = rec
    return out


def main(argv: Optional[Sequence[str]] = None) -> int:
    ap = argparse.ArgumentParser(description="cross-platform per-frame state traces")
    sub = ap.add_subparsers(dest="cmd", required=True)
    p = sub.add_parser("run")
    p.add_argument("--timeline", type=Path, required=True)
    p.add_argument("--out", type=Path, required=True)
    p.add_argument("--profile", default="sc-rtc", choices=sorted(D.PROFILES))
    p.add_argument("--every", type=int, default=30)
    p.add_argument("--match-every", type=int, default=1)
    p.add_argument("--osstart", default=None, help="retry until __OSStartTime equals this hex value")
    p.add_argument("--attempts", type=int, default=4)
    p.add_argument("--keep", action="store_true")
    p = sub.add_parser("diff")
    p.add_argument("a", type=Path)
    p.add_argument("b", type=Path)
    p.add_argument("--out", type=Path, default=None)
    args = ap.parse_args(argv)
    logging.basicConfig(level=logging.INFO, format="%(asctime)s %(name)s %(message)s")
    if args.cmd == "run":
        timeline = json.loads(args.timeline.read_text())
        for attempt in range(1, args.attempts + 1):
            tr = run(timeline, D.PROFILES[args.profile], args.every, args.match_every, args.keep, args.osstart)
            tr["attempt"] = attempt
            if not (args.osstart and str(tr.get("error", "")).startswith("os start time")):
                break
            log.info("attempt %d: %s; retrying", attempt, tr["error"])
        args.out.parent.mkdir(parents=True, exist_ok=True)
        args.out.write_text(json.dumps(tr, indent=0, default=str))
        print(json.dumps({k: tr.get(k) for k in ("host", "version", "os_start", "final", "recorded_final",
                                                   "wall_s", "error", "events_submitted", "input_errors")},
                         indent=1, default=str))
        print(f"{len(tr['checkpoints'])} checkpoints -> {args.out}")
        return 0 if "error" not in tr else 1
    rep = diff(json.loads(args.a.read_text()), json.loads(args.b.read_text()))
    text = json.dumps(rep, indent=1, default=str)
    if args.out:
        args.out.write_text(text)
    print(text)
    return 0 if rep["first_gameplay_divergence"] is None else 2


if __name__ == "__main__":
    sys.exit(main())
