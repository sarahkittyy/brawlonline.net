"""Plays a corpus of recorded online games (.brep) through a Dolphin build and records how each
one went: the baseline of docs/design/brawl-online-core.md §4, run before and after every step of
moving the online code into core/, and compared with ``--compare``.

Each replay is played by Dolphin's own replay playback (docs/design/replays.md): the recorded
setup, the recorded host's sync block and task order, the recorded inputs, and every frame's
gameplay checksum checked against the file's. A replay passes when it plays to its end with no
checksum difference. The record per replay: the outcome, how many frames were checked, the first
differing frame if any.

    python harness/tools/core_replay_corpus.py run --replays run/core/corpus.txt --sd E:/emissary/live/card/sd.raw \\
        --build E:/emissary/dbuild/Binaries --jobs 4 --out run/core/baseline.jsonl
    python harness/tools/core_replay_corpus.py compare run/core/baseline.jsonl run/core/step1.jsonl

The SD card is the online card the replays were played on (P+ with the plugin); it is opened read
only, so the same card serves every run and every build. The build's Dolphin is chosen with
PPHARNESS_DOLPHIN_DIR (``--build`` sets it).
"""

from __future__ import annotations

import argparse
import json
import os
import sys
import time
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path
from typing import Any, Dict, List

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
sys.path.insert(0, str(Path(__file__).resolve().parent))


def play_one(path: Path, idx: int, args: argparse.Namespace) -> Dict[str, Any]:
    import gameplay_rollback as G
    from ppharness import brawl as B

    rec: Dict[str, Any] = {"replay": str(path), "name": path.name}
    started = time.monotonic()
    d = G.make_instance(
        f"{args.prefix}-{idx}",
        video=args.video,
        keep=False,
        dolphin_ini={"General": {"WiiSDCardPath": args.sd},
                     "Core": {"WiiSDCardAllowWrites": False, "WiiSDCardEnableFolderSync": False}},
    )
    try:
        d.launch()
        d.connect()
        c = d.client
        c.wait_state("running", timeout=120)
        boot_deadline = time.monotonic() + 180
        while B.read_scene(c.read_mem).name != "muMenuMain":
            if time.monotonic() > boot_deadline:
                raise TimeoutError("did not reach the main menu")
            B.tap(c, 0, ["A"], hold=8, release=60)
        c.call("replay_play", path=str(path), play_anyway=False)
        deadline = time.monotonic() + args.timeout
        status: Dict[str, Any] = {}
        while time.monotonic() < deadline:
            if d.process is not None and d.process.poll() is not None:
                raise RuntimeError(f"Dolphin exited {d.process.returncode}")
            status = c.call("replay_status")
            if status.get("error") or status.get("desync") or status.get("desync_frame", -1) >= 0:
                break
            if status.get("finished"):
                break
            time.sleep(0.5)
        else:
            raise TimeoutError("the replay did not finish")
        rec.update(
            outcome="pass" if status.get("finished") and not status.get("desync") and status.get("desync_frame", -1) < 0
            and not status.get("error") else "fail",
            frames_checked=status.get("frames_checked"),
            first_frame=status.get("first_frame"),
            final_frame=status.get("final_frame"),
            desync=status.get("desync"),
            desync_frame=status.get("desync_frame"),
            error=status.get("error"),
            end_method=status.get("end_method"),
        )
    except Exception as e:  # noqa: BLE001 - recorded, the run goes on
        rec.update(outcome="error", error=f"{type(e).__name__}: {e}")
    finally:
        try:
            d.stop()
        finally:
            d.cleanup()
    rec["seconds"] = round(time.monotonic() - started, 1)
    return rec


def cmd_run(args: argparse.Namespace) -> int:
    if args.build:
        os.environ["PPHARNESS_DOLPHIN_DIR"] = args.build
    lines = [ln.strip() for ln in Path(args.replays).read_text(encoding="utf-8").splitlines()]
    replays = [Path(ln) for ln in lines if ln and not ln.startswith("#")]
    out = Path(args.out)
    out.parent.mkdir(parents=True, exist_ok=True)
    done = set()
    if out.exists():
        done = {json.loads(ln)["replay"] for ln in out.read_text(encoding="utf-8").splitlines() if ln.strip()}
    todo = [(i, p) for i, p in enumerate(replays) if str(p) not in done]
    print(f"{len(todo)} of {len(replays)} replays to play", flush=True)

    def task(item):
        i, p = item
        rec = play_one(p, i, args)
        with out.open("a", encoding="utf-8") as f:
            f.write(json.dumps(rec) + "\n")
        print(f"{rec['outcome']:5} {rec.get('frames_checked')} {rec['seconds']}s {p.name}"
              + (f" ({rec.get('error') or rec.get('desync_frame')})" if rec["outcome"] != "pass" else ""), flush=True)
        return rec

    with ThreadPoolExecutor(max_workers=args.jobs) as pool:
        results = list(pool.map(task, todo))
    bad = [r for r in results if r["outcome"] != "pass"]
    print(f"{len(results) - len(bad)} passed, {len(bad)} not", flush=True)
    return 0 if not bad else 1


def load(path: str) -> Dict[str, Dict[str, Any]]:
    return {json.loads(ln)["replay"]: json.loads(ln)
            for ln in Path(path).read_text(encoding="utf-8").splitlines() if ln.strip()}


def cmd_compare(args: argparse.Namespace) -> int:
    """Every replay must have the same outcome and frame counts in both runs (errors are listed:
    they say nothing about the build until played again)."""
    a, b = load(args.before), load(args.after)
    keys = ("outcome", "frames_checked", "final_frame", "desync_frame", "end_method")
    diff: List[str] = []
    for k in sorted(set(a) | set(b)):
        ra, rb = a.get(k), b.get(k)
        if ra is None or rb is None:
            diff.append(f"only in {'after' if ra is None else 'before'}: {k}")
            continue
        if "error" in (ra["outcome"], rb["outcome"]):
            diff.append(f"error (play again): {Path(k).name}: {ra.get('error') or rb.get('error')}")
            continue
        changed = {f: (ra.get(f), rb.get(f)) for f in keys if ra.get(f) != rb.get(f)}
        if changed:
            diff.append(f"CHANGED {Path(k).name}: {changed}")
    passed = sum(1 for r in b.values() if r["outcome"] == "pass")
    print(f"{len(b)} replays after, {passed} pass; {len(diff)} differences")
    for d in diff:
        print(" ", d)
    return 0 if not any(d.startswith("CHANGED") or d.startswith("only") for d in diff) else 1


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    r = sub.add_parser("run")
    r.add_argument("--replays", required=True, help="a text file: one .brep path per line")
    r.add_argument("--sd", required=True, help="the online SD card image (opened read only)")
    r.add_argument("--out", required=True, help="JSON lines, appended (a rerun skips the replays already there)")
    r.add_argument("--build", help="the Binaries folder of the Dolphin build (else PPHARNESS_DOLPHIN_DIR)")
    r.add_argument("--jobs", type=int, default=4)
    r.add_argument("--video", default="Null")
    r.add_argument("--timeout", type=float, default=1800, help="seconds per replay once it plays")
    r.add_argument("--prefix", default="corecorpus")
    c = sub.add_parser("compare")
    c.add_argument("before")
    c.add_argument("after")
    args = ap.parse_args()
    return cmd_run(args) if args.cmd == "run" else cmd_compare(args)


if __name__ == "__main__":
    raise SystemExit(main())
