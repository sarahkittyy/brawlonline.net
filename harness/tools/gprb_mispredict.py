"""Misprediction sync test: one instance, the input of the frame's first run is wrong on purpose
(gameplay rollback, docs/gameplay-rollback-status.md, the dual-core divergence).

A rollback session runs a frame first with a predicted input and, when the prediction was wrong,
rolls back and runs it again with the true input. The plain sync test cannot see state that a
mispredicted run leaves behind, because it resimulates with the same input. Here:

``prep``    boot, set up a match (``gprb_synctest`` scenarios), save a state in the countdown.
``record``  load it, play closed-loop (flows.Fighter, anchored pads) and record the slots every
            game frame consumed (``game_pads``) until --frames or game set.
``run``     load it, arm a sync test that takes the driven ports' input from the recording
            (``inject_input``); with ``--mispredict`` the first run of every frame gives those
            ports the recorded input of another frame (``mispredict_offset``), and the
            resimulations run the true one. Each ``--modes`` entry is one run (``ref`` = no
            misprediction, ``mp`` = misprediction); every run's final per-frame trace is compared
            with the first run's.

    python harness/tools/gprb_mispredict.py prep --scenario fd-mario-marth --out run/qa/gprbw/fd-cd.sav --cpu dc
    python harness/tools/gprb_mispredict.py record --state run/qa/gprbw/fd-cd.sav --out run/qa/gprbw/fd-in.json
    python harness/tools/gprb_mispredict.py run --state run/qa/gprbw/fd-cd.sav --inputs run/qa/gprbw/fd-in.json \
        --cpu dc --modes ref,mp,mp,mp --json run/qa/gprbw/mp.json
"""

from __future__ import annotations

import argparse
import contextlib
import json
import os
import shutil
import sys
import threading
import time
from pathlib import Path
from typing import Any, Dict, List, Optional, Sequence

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
sys.path.insert(0, str(Path(__file__).resolve().parent))

import gameplay_rollback as G  # noqa: E402
import gprb_synctest as T  # noqa: E402
from gprb_ab import compare_traces, gentle_macro, norm  # noqa: E402
from ppharness import brawl as B  # noqa: E402
from ppharness import flows as F  # noqa: E402
from ppharness.client import HarnessError  # noqa: E402
from gprb_hang import dump_stacks  # noqa: E402

PREFIX = os.environ.get("GPRB_NAME_PREFIX", "gprbw")


def with_args(extra: Sequence[str]):
    orig = G.make_instance

    def mk(name, **kw):
        inst = orig(name, **kw)
        inst.config.config_args = list(inst.config.config_args or []) + list(extra)
        return inst
    G.make_instance = mk
    return orig


def cmd_prep(args) -> int:
    p1, p2, stage, items = T.SCENARIOS[args.scenario]
    with G.instances([(f"{PREFIX}-prep", dict(cpu_thread=args.cpu == "dc"))]) as (inst,):
        c = inst.client
        T.setup_match(c, p1, p2, stage, items)
        B.wait_scene(c, [B.Scene.IN_MATCH], 600)
        while not 100 <= G.game_frame(c) < 400:
            time.sleep(0.02)
        c.pause()
        if not 100 <= G.game_frame(c) < 200:
            raise RuntimeError(f"missed the countdown: game frame {G.game_frame(c)}")
        Path(args.out).parent.mkdir(parents=True, exist_ok=True)
        c.save_state(str(Path(args.out).resolve()))
        print("saved at game frame", G.game_frame(c))
    return 0


def cmd_record(args) -> int:
    F._macro = gentle_macro
    orig = with_args([f"Dolphin.Core.EmulationSpeed={args.speed}"])
    try:
        with G.instances([(f"{PREFIX}-rec", dict(cpu_thread=False))]) as (inst,):
            c = inst.client
            G.load_fixture(c, Path(args.state))
            setup = B.read_match_setup(c.read_mem)
            c.call("game_pads", record=True, anchor=3)
            c.call("frame_trace_config", enabled=True)
            g0 = G.game_frame(c)
            fighters = [F.Fighter(F.Seat(c, port), "random", args.seed, setup.stage_kind if setup else None)
                        for port in (0, 1)]
            busy = [0, 0]
            c.resume()
            t0 = time.monotonic()
            while True:
                now = c.status().input_polls
                st = B.read_match_state(c.read_mem)
                gf = G.game_frame(c)
                if st.game_set or gf - g0 >= args.frames or not st.in_match or time.monotonic() - t0 > args.timeout:
                    break
                for port, ft in enumerate(fighters):
                    if busy[port] <= now:
                        # Nothing before GO (the session runs the countdown with neutral input).
                        seq = norm(ft.decide(st)) if gf >= args.start_frame else [dict(G.NEUTRAL, hold=2)]
                        w = c.pad_script(port, seq)
                        busy[port] = w.ends_at
                with contextlib.suppress(HarnessError):
                    c.wait_frame(input_polls=max(min(busy), now + 1), timeout_ms=20000)
            c.pause()
            rows = c.call("game_pads", since=0)["rows"]
            end = G.game_frame(c)
    finally:
        G.make_instance = orig
    rows = [r for r in rows if r[0] >= args.start_frame]
    Path(args.out).write_text(json.dumps({"state": args.state, "seed": args.seed, "g0": g0, "end_frame": end,
                                          "pads": rows}))
    print(f"recorded {len(rows)} frames {rows[0][0] if rows else None}..{rows[-1][0] if rows else None}, "
          f"end {end}")
    return 0


def one_run(args, inp: Dict[str, Any], mode: str, run: int) -> Dict[str, Any]:
    rows = inp["pads"]
    replay = mode.startswith("replay=")
    until = 10 ** 9 if replay else min(args.frames or 10 ** 9, rows[-1][0] - 2)
    rep: Dict[str, Any] = {"mode": mode, "run": run, "cpu": args.cpu, "distance": args.distance}
    extra = [f"Dolphin.Core.EmulationSpeed={args.speed}"]
    if args.interpreter:
        extra.append("Dolphin.Core.CPUCore=0")
    orig = with_args(extra)
    t0 = time.monotonic()
    try:
        with G.instances([(f"{PREFIX}-mp-{'replay' if replay else mode}", dict(cpu_thread=args.cpu == "dc"))]) as (inst,):
            try:
                c = inst.client
                G.load_fixture(c, Path(args.state))
                c.call("frame_trace_config", enabled=True)
                c.call("game_pads", record=False, clear=True)
                for k in range(0, len(rows), 400):
                    c.call("game_pads", inject=rows[k:k + 400])
                opts = dict(distance=args.distance, region_set=args.region_set, hash_regions=False,
                            start_frame=args.sync_start or args.start_frame, inject_input=True)
                if args.no_rollback:
                    opts["no_rollback"] = True
                if mode == "mp":
                    opts.update(mispredict_ports=args.mispredict_ports, mispredict_offset=args.offset,
                                mispredict_every=args.every)
                if replay:
                    # A recorded network session's passes (PPR_GPRB_PASS_LOG), loads and inputs.
                    opts = dict(distance=7, region_set=args.region_set, hash_regions=False,
                                start_frame=args.start_frame, replay_path=str(Path(mode[7:]).resolve()))
                if args.sound_mode != "play":
                    opts[f"{args.sound_mode}_resim_sounds"] = True
                c.call("gprb_synctest", **opts)
                if args.trace_frame:
                    # The first run and the resimulation of that game frame (main thread), for
                    # gprb_resimtrace.diff: the first load whose value differs is the leaked state.
                    Path(args.trace_out).mkdir(parents=True, exist_ok=True)
                    c.call("cpu_trace", path=str(Path(args.trace_out, "resim.bin").resolve()),
                           first_frame=args.trace_frame, frames=1, occurrences=2, max_records=80_000_000)
                c.resume()
                snd_samples: List[Dict[str, Any]] = []
                stop_poll = threading.Event()
                polls = [0]

                def poller() -> None:
                    # Like the closed-loop players of a session: many short reads (each takes the CPU
                    # thread guard), plus a pad_set now and then.
                    while not stop_poll.is_set():
                        with contextlib.suppress(Exception):
                            B.read_match_state(c.read_mem)
                            polls[0] += 1
                th = None
                if args.poll:
                    th = threading.Thread(target=poller, daemon=True)
                    th.start()
                last = 0.0
                interp_resumed = False
                while True:
                    time.sleep(0.5)
                    if os.environ.get("PPR_GPRB_INTERP_FROM") and not interp_resumed:
                        # The session switched to the interpreter and broke: continue.
                        with contextlib.suppress(Exception):
                            if c.status().state.lower() in ("paused", "pause"):
                                c.resume()
                                interp_resumed = True
                    s = c.call("gprb_status")
                    gf = G.game_frame(c)
                    if gf >= until or s["phase"] in ("ended", "error") or time.monotonic() - t0 > args.timeout:
                        break
                    if args.sound_sample and s["phase"] == "running":
                        with contextlib.suppress(Exception):
                            ss = c.call("gprb_sound_state")
                            snd_samples.append({k: ss[k] for k in ("active", "orphans", "duplicate_ids")})
                    if time.monotonic() - last > 30:
                        last = time.monotonic()
                        print(f"  [{mode} {run}] game frame {gf} phase {s['phase']} mispredicted "
                              f"{s.get('mispredicted_passes')}", flush=True)
                stop_poll.set()
                if th:
                    th.join(5)
                # Let the frames after the session's end (game set + margin) be traced too.
                with contextlib.suppress(Exception):
                    end_gf = G.game_frame(c) + 40
                    t1 = time.monotonic()
                    while G.game_frame(c) < end_gf and time.monotonic() - t1 < 20:
                        time.sleep(0.2)
                rep["polls"] = polls[0]
                if snd_samples:
                    n = len(snd_samples)
                    rep["sound_samples"] = {
                        "n": n,
                        **{f"{k}_avg": round(sum(x[k] for x in snd_samples) / n, 2) for k in ("active", "orphans", "duplicate_ids")},
                        **{f"{k}_max": max(x[k] for x in snd_samples) for k in ("active", "orphans", "duplicate_ids")},
                    }
                c.pause()
                s = c.call("gprb_status")
                rep["status"] = {k: s.get(k) for k in ("phase", "current_frame", "rollbacks", "mispredicted_passes",
                                                        "end_reason", "error", "frames_resimulated",
                                                        "partial_granule_skips", "sound_allocs",
                                                        "resim_sound_allocs", "suppressed_sound_allocs",
                                                        "sound_reattached", "sound_reattach_gone", "sound_stopped",
                                                        "sound_stop_gone", "sound_gone_why", "sound_moved",
                                                        "desyncs_detected", "desync_log")}
                rep["trace"] = c.call("frame_trace", since=args.start_frame)["rows"]
                rep["final"] = G.small_state(c)
                if os.environ.get("PPR_GPRB_CENSUS"):
                    rep["census"] = c.call("gprb_census")["ranges"]
                rep["exit"] = inst.process.poll() if inst.process else None
                if args.log_dir:
                    d = Path(args.log_dir)
                    d.mkdir(parents=True, exist_ok=True)
                    with contextlib.suppress(OSError):
                        shutil.copy(inst.user_dir / "Logs/dolphin.log", d / f"{mode.split('=')[0]}-{run}.log")
            except Exception:
                # A hung instance: every thread's stack before it is killed.
                if args.log_dir:
                    Path(args.log_dir).mkdir(parents=True, exist_ok=True)
                    rep["alive"] = inst.process.poll() is None if inst.process else None
                    rep["stacks"] = dump_stacks(inst, str(Path(args.log_dir, f"{mode.split('=')[0]}-{run}-stacks.txt").resolve()))
                    for src, suffix in (("Logs/dolphin.log", "log"), ("harness-stderr.txt", "stderr.txt")):
                        with contextlib.suppress(OSError):
                            shutil.copy(inst.user_dir / src, Path(args.log_dir, f"{mode.split('=')[0]}-{run}-failed.{suffix}"))
                raise
    except Exception as e:  # noqa: BLE001
        rep["error"] = f"{type(e).__name__}: {e}"
    finally:
        G.make_instance = orig
    rep["wall_s"] = round(time.monotonic() - t0, 1)
    return rep


def cmd_run(args) -> int:
    inp = json.loads(Path(args.inputs).read_text())
    modes = args.modes.split(",")
    results: List[Dict[str, Any]] = []
    ref_trace = None
    if args.ref_json:
        ref_trace = json.loads(Path(args.ref_json).read_text())["trace"]
    for run, mode in enumerate(modes):
        rep = one_run(args, inp, mode, run)
        tr = rep.get("trace") or []
        # The newest frames may still be a first (mispredicted) run: drop the last few.
        tr = [r for r in tr if r[0] <= max((x[0] for x in tr), default=0) - 8]
        if ref_trace is None and tr:
            ref_trace = tr
            if args.save_ref:
                Path(args.save_ref).write_text(json.dumps({"trace": tr}))
            cmp = {"reference": True}
        else:
            cmp = compare_traces(ref_trace or [], tr)
        rep["vs_ref"] = cmp
        st = rep.get("status") or {}
        print(f"run {run} {mode} cpu={args.cpu} d={args.distance}: frames {st.get('current_frame')} rollbacks "
              f"{st.get('rollbacks')} mispredicted {st.get('mispredicted_passes')} end {st.get('end_reason')!r}; "
              f"vs ref: compared {cmp.get('compared')} diverged at {cmp.get('diverged_at')} "
              f"{json.dumps(cmp.get('first_diff'))[:400] if cmp.get('first_diff') else ''}; error {rep.get('error')} "
              f"wall {rep['wall_s']} s", flush=True)
        if st.get("desyncs_detected"):
            print(f"    sync-test desyncs {st.get('desyncs_detected')}: {(st.get('desync_log') or [])[:3]}", flush=True)
        if args.save_traces:
            Path(args.save_traces).mkdir(parents=True, exist_ok=True)
            Path(args.save_traces, f"{run}-{mode.split('=')[0]}.json").write_text(json.dumps(rep.get("trace")))
        if rep.get("sound_samples") or (rep.get("status") or {}).get("sound_allocs"):
            st2 = rep.get("status") or {}
            print(f"    sound: {rep.get('sound_samples')} allocs {st2.get('sound_allocs')} resim "
                  f"{st2.get('resim_sound_allocs')} suppressed {st2.get('suppressed_sound_allocs')} reattached "
                  f"{st2.get('sound_reattached')} (gone {st2.get('sound_reattach_gone')}) stopped "
                  f"{st2.get('sound_stopped')} (gone {st2.get('sound_stop_gone')})", flush=True)
        rep.pop("trace", None)
        results.append(rep)
        if args.json:
            p = Path(args.json)
            prev = json.loads(p.read_text()) if p.exists() else []
            prev.append(rep)
            p.write_text(json.dumps(prev, indent=1))
    return 0


def main(argv: Optional[Sequence[str]] = None) -> int:
    ap = argparse.ArgumentParser()
    sub = ap.add_subparsers(dest="cmd", required=True)
    p = sub.add_parser("prep")
    p.add_argument("--scenario", default="fd-mario-marth")
    p.add_argument("--out", required=True)
    p.add_argument("--cpu", default="dc", choices=("sc", "dc"))
    p = sub.add_parser("record")
    p.add_argument("--state", required=True)
    p.add_argument("--out", required=True)
    p.add_argument("--seed", default="mp1")
    p.add_argument("--frames", type=int, default=8000)
    p.add_argument("--start-frame", type=int, default=240)
    p.add_argument("--speed", type=float, default=1.0)
    p.add_argument("--timeout", type=float, default=1800)
    p = sub.add_parser("run")
    p.add_argument("--state", required=True)
    p.add_argument("--inputs", required=True)
    p.add_argument("--cpu", default="dc", choices=("sc", "dc"))
    p.add_argument("--modes", default="ref,mp")
    p.add_argument("--distance", type=int, default=2)
    p.add_argument("--region-set", default="gp-v12")
    p.add_argument("--start-frame", type=int, default=240)
    p.add_argument("--mispredict-ports", type=int, default=3)
    p.add_argument("--offset", type=int, default=-7)
    p.add_argument("--no-rollback", action="store_true",
                   help="ground truth: every frame runs once, no loads (sync test option no_rollback)")
    p.add_argument("--sync-start", type=int, default=0,
                   help="first rolled-back frame (default --start-frame); past the end = no rollback at all")
    p.add_argument("--every", type=int, default=1)
    p.add_argument("--frames", type=int, default=0)
    p.add_argument("--interpreter", action="store_true")
    p.add_argument("--trace-frame", type=int, default=0,
                   help="cpu_trace the first run and the resimulation of this game frame (use --interpreter)")
    p.add_argument("--trace-out", default="run/qa/gprbw/trace")
    p.add_argument("--speed", type=float, default=0, help="emulation speed (0 = unthrottled)")
    p.add_argument("--poll", action="store_true", help="read guest memory continuously, as closed-loop players do")
    p.add_argument("--sound-mode", default="play", choices=("play", "suppress", "dedupe"),
                   help="what a resimulated pass does with sound starts")
    p.add_argument("--sound-sample", action="store_true",
                   help="sample gprb_sound_state (active sounds, orphans, repeated ids) every 0.5 s")
    p.add_argument("--save-traces", default=None, help="write every run's trace to this directory")
    p.add_argument("--timeout", type=float, default=1800)
    p.add_argument("--ref-json", default=None, help="compare with this saved reference trace")
    p.add_argument("--save-ref", default=None, help="save the first run's trace here")
    p.add_argument("--json", default=None)
    p.add_argument("--log-dir", default=None)
    args = ap.parse_args(argv)
    return {"prep": cmd_prep, "record": cmd_record, "run": cmd_run}[args.cmd](args)


if __name__ == "__main__":
    sys.exit(main())
