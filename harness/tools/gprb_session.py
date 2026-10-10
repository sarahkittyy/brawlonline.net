"""Two-instance gameplay-only rollback sessions (phase 4, docs/gameplay-rollback-status.md).

Two instances boot independently and take different menu paths (A straight to the character
select, B with a detour: picks and drops characters, backs out to the main menu, opens Rules,
comes back, picks in another order, lingers on the stage select). While both are on the CSS
they connect with ``gprb_connect`` through a netsim proxy (A hosts, B joins), exchange their
selections with ``gprb_set_selections``, and each drives its own CSS/SSS to the agreed match.
Each instance starts the match on its own; the rollback session begins at the first simulation
frame behind the barrier. During the match each instance plays only its local player
(closed-loop on its own, possibly speculative, state). At the end the confirmed-frame checksums
(``gprb_checksums``) and the per-frame traces (``frame_trace``) of both are compared.

    python harness/tools/gprb_session.py --preset typical --cpu sc
    python harness/tools/gprb_session.py --preset lan,typical,bad_wifi --cpu sc,dc --json run/qa/gprb2/sessions.json

With --ports, 2-4 players, one instance each (docs/nplayer/session.md, "Harness"):

    python harness/tools/gprb_session.py --ports 1,2,3,4 --chars fox,falco,mario,marth --region-set gp-v20
    python harness/tools/gprb_session.py --ports 1,2,3,4 --teams 0,0,1,1 --drop 4:40:stop --plugin game-code/PPOnline/PPOnline.rel
"""

from __future__ import annotations

import argparse
import contextlib
import json
import socket
import sys
import threading
import time
from pathlib import Path
from typing import Any, Dict, List, Optional, Sequence

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
sys.path.insert(0, str(Path(__file__).resolve().parent))

import gameplay_rollback as G  # noqa: E402
from gprb_ab import compare_traces, gentle_macro  # noqa: E402
from ppharness import brawl as B  # noqa: E402
from ppharness import flows as F  # noqa: E402
from ppharness.client import HarnessError  # noqa: E402
from ppharness.netsim import NetSim  # noqa: E402

CONFIRM_MARGIN = 16  # frames behind the slower peer's current frame that can no longer roll back


def sound_opts(args) -> Dict[str, bool]:
    return {"suppress_resim_sounds": args.resim_sounds == "suppress",
            "dedupe_resim_sounds": args.resim_sounds == "dedupe"}


def free_udp_port() -> int:
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as s:
        s.bind(("127.0.0.1", 0))
        return s.getsockname()[1]


def wait(pred, timeout: float, what: str, every: float = 0.2):
    deadline = time.monotonic() + timeout
    while True:
        v = pred()
        if v:
            return v
        if time.monotonic() > deadline:
            raise RuntimeError(f"timed out waiting for {what}")
        time.sleep(every)


def compare_checksums(ra: Sequence[Sequence[Any]], rb: Sequence[Sequence[Any]], limit: int) -> Dict[str, Any]:
    ma = {r[0]: r for r in ra if r[0] <= limit}
    mb = {r[0]: r for r in rb if r[0] <= limit}
    common = sorted(set(ma) & set(mb))
    bad = [f for f in common if ma[f][1] != mb[f][1]]
    return {"compared": len(common), "from": common[0] if common else None, "to": common[-1] if common else None,
            "mismatches": len(bad), "first_mismatch": bad[0] if bad else None,
            "first_rows": [ma[bad[0]], mb[bad[0]]] if bad else None}


def chunk_addr(ranges: Sequence[Sequence[int]], chunk: int, index: int) -> int:
    for addr, size in ranges:
        n = (size + chunk - 1) // chunk
        if index < n:
            return addr + index * chunk
        index -= n
    return 0


def compare_samples(ca, cb, limit: int) -> Dict[str, Any]:
    """Region-set samples of both peers (gprb_samples), confirmed frames only: the first sampled
    frame whose digests differ, and the chunks that start to differ there and at the next samples
    (chunks differing at the first common sample are listed separately: per-peer noise such as
    tick-derived counters and dead stack)."""
    sa, sb = ca.call("gprb_samples"), cb.call("gprb_samples")
    da = dict(zip(sa["frames"], sa["digests"]))
    db = dict(zip(sb["frames"], sb["digests"]))
    common = sorted(f for f in set(da) & set(db) if f <= limit)
    out: Dict[str, Any] = {"compared": len(common)}
    if not common:
        return out
    ranges, chunk = sa["ranges"], int(sa["chunk_size"])

    def full(c, frames):
        r = c.call("gprb_samples", frames=frames)["full"]
        return {int(k): v for k, v in r.items()}

    def hashes(e):
        h = e.get("chunks", "")
        return [h[i:i + 16] for i in range(0, len(h), 16)]
    first = common[0]
    fa, fb = full(ca, [first]), full(cb, [first])
    ha0, hb0 = hashes(fa[first]), hashes(fb[first])
    noise = {i for i in range(min(len(ha0), len(hb0))) if ha0[i] != hb0[i]}
    out["noise_chunks"] = [f"{chunk_addr(ranges, chunk, i):08x}" for i in sorted(noise)][:64]
    # Frames where something beyond the initial noise differs: bisect on digests is not possible
    # (noise differs everywhere), so walk the samples in batches.
    events: List[Dict[str, Any]] = []
    seen = set(noise)
    for k in range(0, len(common), 20):
        batch = common[k:k + 20]
        fa, fb = full(ca, batch), full(cb, batch)
        for f in batch:
            ha, hb = hashes(fa.get(f, {})), hashes(fb.get(f, {}))
            new = [i for i in range(min(len(ha), len(hb))) if ha[i] != hb[i] and i not in seen]
            if new:
                seen.update(new)
                ev = {"frame": f, "chunks": [f"{chunk_addr(ranges, chunk, i):08x}" for i in new][:32],
                      "count": len(new)}
                wa, wb = fa.get(f, {}).get("watch"), fb.get(f, {}).get("watch")
                if wa != wb:
                    ev["watch"] = [wa, wb]
                events.append(ev)
        if len(events) >= 12:
            break
    out["events"] = events
    return out


@contextlib.contextmanager
def _keep_logs(args: argparse.Namespace, insts, rep: Dict[str, Any]):
    """On the way out (also on errors): exit codes, and each instance's log and stderr."""
    try:
        yield
    finally:
        rep["exit_codes"] = [i.process.poll() if i.process else None for i in insts]
        if args.log_dir and "Timeout" in json.dumps(rep):
            # A hung instance: every thread's stack before it is killed.
            from gprb_hang import dump_stacks
            for i in insts:
                with contextlib.suppress(Exception):
                    out = Path(args.log_dir, f"{i.name}-r{rep.get('run', 0)}-stacks.txt").resolve()
                    dump_stacks(i, str(out))
        if args.log_dir:
            import shutil
            d = Path(args.log_dir)
            d.mkdir(parents=True, exist_ok=True)
            for i in insts:
                for src, suffix in (("Logs/dolphin.log", "log"), ("harness-stderr.txt", "stderr.txt")):
                    with contextlib.suppress(OSError):
                        shutil.copy(i.user_dir / src, d / f"{i.name}-r{rep.get('run', 0)}.{suffix}")


def second_history(c, chars: Sequence[str], stage: str, notes: List[str]) -> None:
    """After a match: confirm the results on both ports, then on the CSS drop the tokens still
    placed from the last match and pick the next match."""
    if B.read_scene(c.read_mem).scene is not B.Scene.CSS:
        B.results_to_css(c, [0, 1])
    B.wait_css_ready(c, [0, 1])
    notes.append(f"CSS at poll {c.status().input_polls}")
    for port in (0, 1):
        a = B.read_css_area(c.read_mem, port)
        if a is not None and a.placed:
            G.css_drop_token(c, port)
    B.css_pick_character(c, 0, B.CSS_ID[chars[0]])
    B.css_pick_character(c, 1, B.CSS_ID[chars[1]])
    notes.append(f"picked at poll {c.status().input_polls}")
    B.css_start(c, 0)
    B.sss_pick_stage(c, B.STAGE_KIND[stage], 0)


def play_match(ca, cb, sim, args, preset: str, cpu: str, run: int, mi: int, sel, rep: Dict[str, Any]) -> None:
    """Exchange selections, take both instances (independent menu paths) to the match, play it
    closed-loop on each side's own port until game set, then compare what both confirmed."""
    p1, p2, stage = sel
    try:
        ca.call("gprb_set_selections", selections={"character": p1, "stage": stage, "match": mi})
        cb.call("gprb_set_selections", selections={"character": p2, "match": mi})
        sa = wait(lambda: (lambda s: s if s["peer"]["selections"].get("match") == mi else None)(ca.call("gprb_status")),
                  10, "the joiner's selections")
        sb = wait(lambda: (lambda s: s if s["peer"]["selections"].get("match") == mi else None)(cb.call("gprb_status")),
                  10, "the host's selections")
        chars = [sb["peer"]["selections"]["character"], sa["peer"]["selections"]["character"]]
        agreed_stage = sb["peer"]["selections"]["stage"]
        rep["agreed"] = {"chars": chars, "stage": agreed_stage}
        notes_a: List[str] = []
        notes_b: List[str] = []
        if args.minutes:
            # Same rules on both (part of the setup hash): a shorter time limit ends matches sooner.
            for c in (ca, cb):
                B.wait_scene(c, [B.Scene.CSS, B.Scene.RESULTS], 60 * 120)
                if B.read_scene(c.read_mem).scene is B.Scene.CSS:
                    B.write_rules(c, stocks=4, minutes=args.minutes, items_off=True)
        if mi == 0:
            # Independent menu paths to the same match.
            G.par([lambda: G.history_a(ca, chars, agreed_stage, notes_a),
                   lambda: G.history_b(cb, chars, agreed_stage, notes_b)])
        else:
            if args.minutes:
                # Results first, then the rules on the CSS.
                G.par([lambda: B.results_to_css(ca, [0, 1]), lambda: B.results_to_css(cb, [0, 1])])
                for c in (ca, cb):
                    B.write_rules(c, stocks=4, minutes=args.minutes, items_off=True)
            G.par([lambda: second_history(ca, chars, agreed_stage, notes_a),
                   lambda: (B.step(cb, 97), second_history(cb, chars, agreed_stage, notes_b))])
        rep["notes"] = {"A": notes_a, "B": notes_b}
        for c in (ca, cb):
            for port in (0, 1):
                c.pad_set(port)
        if args.save_countdown:
            # Diagnostics: each peer's state in the countdown (after the barrier, before GekkoNet
            # starts), so its recorded passes (PPR_GPRB_PASS_LOG) can be replayed from it.
            def save_cd(c, who: str) -> None:
                wait(lambda: c.call("gprb_status")["phase"] == "countdown" and
                     B.read_frame_counters(c.read_mem).get("game_frame", 0) >= 60, 180, f"{who} countdown")
                c.pause()
                d = Path(args.save_countdown)
                d.mkdir(parents=True, exist_ok=True)
                c.save_state(str((d / f"r{run}-m{mi}-{who}.sav").resolve()))
                c.resume()
            G.par([lambda: save_cd(ca, "host"), lambda: save_cd(cb, "join")])
        wait(lambda: all(c.call("gprb_status")["phase"] in ("running", "ended", "error") for c in (ca, cb)),
             180, "both sessions to start")
        st = [c.call("gprb_status") for c in (ca, cb)]
        if any(s["phase"] != "running" for s in st):
            raise RuntimeError(f"session did not start: {[s['phase'] + ' ' + s['error'] for s in st]}")
        rep["match_index"] = [s.get("match_index") for s in st]
        for c in (ca, cb):
            # The slots every game frame consumed (its newest pass): the confirmed inputs.
            c.call("game_pads", record=True)
        sim.reset_epoch()
        setup = B.read_match_setup(ca.read_mem)
        F._macro = gentle_macro
        seats = [F.Seat(ca, 0, 0, "A"), F.Seat(cb, 1, 0, "B")]
        stop_at = time.monotonic() + args.timeout
        stop_flag = threading.Event()

        def stop() -> bool:
            return stop_flag.is_set() or time.monotonic() > stop_at

        def play(seat: F.Seat, seed: str) -> None:
            with contextlib.suppress(HarnessError):
                F.fight([seat], args.frames, mode="random", seed=seed,
                        stage_kind=setup.stage_kind if setup else None, stop=stop)
            stop_flag.set()

        th = [threading.Thread(target=play, args=(s, f"{run}-{mi}-{s.name}")) for s in seats]
        for t in th:
            t.start()
        last = 0.0
        while any(t.is_alive() for t in th):
            time.sleep(0.5)
            if time.monotonic() - last > 30:
                last = time.monotonic()
                st = [c.call("gprb_status") for c in (ca, cb)]
                print(f"  [{preset}/{cpu} match {mi}] frames {[s['current_frame'] for s in st]} rollbacks "
                      f"{[s['rollbacks'] for s in st]} desyncs {[s['desyncs_detected'] for s in st]} "
                      f"phase {[s['phase'] for s in st]}", flush=True)
                if any(s["phase"] != "running" for s in st):
                    stop_flag.set()
        for t in th:
            t.join()
        # Let both reach the same point after game set (or the frame limit), then compare.
        with contextlib.suppress(RuntimeError):
            wait(lambda: all(c.call("gprb_status")["phase"] != "running" for c in (ca, cb)), 60,
                 "both sessions to end")
        st = [c.call("gprb_status") for c in (ca, cb)]
        limit = min(s["current_frame"] for s in st) - CONFIRM_MARGIN
        cks = [c.call("gprb_checksums", since=0)["rows"] for c in (ca, cb)]
        rep["checksums"] = compare_checksums(cks[0], cks[1], limit)
        tr = [c.call("frame_trace", since=0)["rows"] for c in (ca, cb)]
        # Only frames both have confirmed. GekkoNet frame f is the pass that produces game frame
        # start_frame + f; before start_frame (the countdown) there is no rollback.
        gf_limit = limit + args.start_frame
        ta = [r for r in tr[0] if r[0] <= gf_limit]
        tb = [r for r in tr[1] if r[0] <= gf_limit]
        rep["trace"] = compare_traces(ta, tb)
        if args.save_traces:
            d = Path(args.save_traces)
            d.mkdir(parents=True, exist_ok=True)
            (d / f"{preset}-{cpu}-r{run}-m{mi}.json").write_text(json.dumps({"a": ta, "b": tb}))
        with contextlib.suppress(Exception):
            pads = [{r[0]: r[1] for r in c.call("game_pads", since=args.start_frame)["rows"]} for c in (ca, cb)]
            pf = sorted(f for f in set(pads[0]) & set(pads[1]) if f <= gf_limit)
            # Ports 0 and 1 (0x40 bytes each, hex).
            bad = [f for f in pf if pads[0][f][:256] != pads[1][f][:256]]
            rep["pads"] = {"compared": len(pf), "mismatches": len(bad), "first": bad[:5],
                           "first_rows": [pads[0][bad[0]][:256], pads[1][bad[0]][:256]] if bad else None}
            print("  pads:", json.dumps(rep["pads"])[:600], flush=True)
        if args.sample_every:
            try:
                rep["samples"] = compare_samples(ca, cb, limit)
            except Exception as e:  # noqa: BLE001
                rep["samples"] = {"error": f"{type(e).__name__}: {e}"}
            print("  samples:", json.dumps(rep["samples"])[:3000], flush=True)
        rep["status"] = st
        rep["final"] = [G.small_state(c) for c in (ca, cb)]
    except Exception as e:  # noqa: BLE001
        rep["error"] = f"{type(e).__name__}: {e}"
    st = rep.get("status") or [{}, {}]
    ck = rep.get("checksums") or {}
    trc = rep.get("trace") or {}
    print(f"  {preset}/{cpu} run {run} match {mi} {rep.get('agreed')}: frames {[s.get('current_frame') for s in st]}, "
          f"rollbacks {[s.get('rollbacks') for s in st]}, end {[s.get('end_reason') for s in st]}; confirmed "
          f"checksums {ck.get('compared')} compared, {ck.get('mismatches')} mismatches; trace {trc.get('compared')} "
          f"frames diverged at {trc.get('diverged_at')}; error {rep.get('error')}", flush=True)


def run_session(preset: str, cpu: str, args: argparse.Namespace, run: int) -> Dict[str, Any]:
    p1, p2, stage = args.p1, args.p2, args.stage
    rep: Dict[str, Any] = {"preset": preset, "cpu": cpu, "run": run, "p1": p1, "p2": p2, "stage": stage,
                           "delay": args.delay, "region_set": args.region_set}
    t0 = time.monotonic()
    sim: Optional[NetSim] = None
    specs = [(f"{args.name_prefix}-host-{preset}-{cpu}", dict(cpu_thread=cpu == "dc", gpu_determinism=args.gpu,
                                                              video=args.video)),
             (f"{args.name_prefix}-join-{preset}-{cpu}", dict(cpu_thread=cpu == "dc", gpu_determinism=args.gpu,
                                                              video=args.video))]
    rtc_b = args.rtc_b
    orig = G.make_instance

    def mk(name, **kw):
        if "join" in name and rtc_b:
            kw["rtc"] = rtc_b
        if "host" in name and args.rtc_a:
            kw["rtc"] = args.rtc_a
        return orig(name, **kw)
    G.make_instance = mk
    if args.pass_log:
        # Every update's passes (PPR_GPRB_PASS_LOG, read by Dolphin at match start), one directory
        # per run: <dir>/r<run>/pl.<host|join>.m<match>.
        d = Path(args.pass_log, f"r{run}").resolve()
        d.mkdir(parents=True, exist_ok=True)
        import os
        os.environ["PPR_GPRB_PASS_LOG"] = str(d / "pl")
    try:
        with G.instances(specs) as (ia, ib), _keep_logs(args, [ia, ib], rep):
            ca, cb = ia.client, ib.client
            # Both on the CSS first (independent boots), then connect.
            G.par([lambda: B.wait_scene(ca, [B.Scene.CSS], 60 * 120),
                   lambda: B.wait_scene(cb, [B.Scene.CSS], 60 * 120)])
            if args.sample_every:
                watch = [[int(a, 0), int(n, 0)] for a, n in (w.split(":") for w in args.watch.split(",") if w)]
                for c in (ca, cb):
                    c.call("gprb_samples", every=args.sample_every, watch=watch)
            host_port = free_udp_port()
            ca.call("gprb_connect", role="host", port=host_port, region_set=args.region_set, delay=args.delay,
                    name="A", hash_regions=False, start_frame=args.start_frame, **sound_opts(args))
            sim = NetSim(("127.0.0.1", host_port), ("127.0.0.1", 0), preset, seed=f"gprb-{run}").start()
            cb.call("gprb_connect", role="join", host="127.0.0.1", remote_port=sim.listen_port,
                    region_set=args.region_set, delay=args.delay, name="B", hash_regions=False,
                    start_frame=args.start_frame, **sound_opts(args))
            wait(lambda: ca.call("gprb_status")["phase"] == "connected" and cb.call("gprb_status")["phase"] == "connected",
                 30, "the peers to connect")
            matches = [(p1, p2, stage)] + [tuple(m.split(":")) for m in args.next_matches.split(",") if m][: args.matches - 1]
            rep["matches"] = []
            for mi, (m1, m2, mstage) in enumerate(matches):
                mrep: Dict[str, Any] = {"match": mi}
                rep["matches"].append(mrep)
                play_match(ca, cb, sim, args, preset, cpu, run, mi, (m1, m2, mstage), mrep)
                if mrep.get("error"):
                    break
            # The first match's report stays at the top level (older tooling reads it there).
            for k in ("agreed", "notes", "checksums", "trace", "status", "final"):
                if k in rep["matches"][0]:
                    rep[k] = rep["matches"][0][k]
            if rep["matches"][-1].get("error"):
                rep["error"] = f"match {rep['matches'][-1]['match']}: {rep['matches'][-1]['error']}"
            rep["netsim"] = sim.format_stats() if hasattr(sim, "format_stats") else None
            rep["died"] = [i.process.poll() if i.process else None for i in (ia, ib)]
            for c in (ca, cb):
                with contextlib.suppress(Exception):
                    c.call("gprb_stop")
    except Exception as e:  # noqa: BLE001
        rep["error"] = f"{type(e).__name__}: {e}"
    finally:
        G.make_instance = orig
        if sim is not None:
            with contextlib.suppress(Exception):
                sim.stop()
    rep["wall_s"] = round(time.monotonic() - t0, 1)
    st = rep.get("status") or [{}, {}]
    ck = rep.get("checksums") or {}
    trc = rep.get("trace") or {}

    def avg(s: Dict[str, Any], k: str) -> float:
        return s.get(f"{k}_us_total", 0) / max(1, s.get(f"{k}_count", 0))
    print(f"{preset}/{cpu} run {run}: frames {[s.get('current_frame') for s in st]}, rollbacks "
          f"{[s.get('rollbacks') for s in st]} (max {[s.get('max_rollback_frames') for s in st]}, resim "
          f"{[s.get('frames_resimulated') for s in st]}), desyncs {[s.get('desyncs_detected') for s in st]}, "
          f"stalls {[s.get('stall_polls') for s in st]}, ahead {[round(s.get('frames_ahead', 0), 2) for s in st]}, "
          f"save avg {[round(avg(s, 'save')) for s in st]} us, load avg {[round(avg(s, 'load')) for s in st]} us; "
          f"confirmed checksums {ck.get('compared')} compared, {ck.get('mismatches')} mismatches; "
          f"trace {trc.get('compared')} frames diverged at {trc.get('diverged_at')} (game set {trc.get('game_set_frame')}); "
          f"error {rep.get('error')} wall {rep['wall_s']} s", flush=True)
    return rep


# --------------------------------------------------------------------------- 2-4 players
#
# One instance per player, each booted alone and driving every port of the match on its own
# character select (independent menu histories: the first goes straight to the match, the others
# take G.history_b's detour with different idle times). Every pair of players exchanges packets
# through a netsim proxy: for ports a < b, b sends to a's proxy (NetSim forwarding to a), and a
# learns b's address from b's first control message (gprb_connect's `peers` with no address).
# The lowest port hosts. Each instance plays only its own port; at the end every pair of
# instances still in the match is compared (confirmed checksums, per-frame traces, the pads every
# frame consumed).


def _alive(insts) -> None:
    for i in insts:
        if i.process is not None and i.process.poll() is not None:
            raise RuntimeError(f"{i.name} exited ({i.process.poll()})")


def play_match_n(insts, clients, ports: List[int], sims, args, preset: str, cpu: str, run: int,
                 chars: List[str], stage: str, rep: Dict[str, Any]) -> None:
    n = len(ports)
    # Players who leave: [(port, seconds after the start, stop|kill)], in order.
    drops = []
    for item in filter(None, args.drop.split(",")):
        dport, dat, dhow = (item.split(":") + ["stop"])[:3]
        drops.append((int(dport) - 1, float(dat), dhow))
    rep["drop"] = [{"port": p + 1, "after_s": a, "how": h} for p, a, h in drops]
    dropped_ports: set = set()
    try:
        for c in clients:
            c.call("gprb_set_selections", selections={"character": chars, "stage": stage, "match": 0})
        notes: List[List[str]] = [[] for _ in clients]
        if args.minutes:
            for c in clients:
                B.wait_scene(c, [B.Scene.CSS], 60 * 120)
                B.write_rules(c, stocks=4, minutes=args.minutes, items_off=True)

        def path(k: int) -> None:
            if k == 0:
                G.history_a(clients[k], chars, stage, notes[k], ports=ports)
            else:
                G.history_b(clients[k], chars, stage, notes[k], ports=ports, idle=137 + 41 * (k - 1),
                            linger=77 + 23 * (k - 1))
        G.par([lambda k=k: path(k) for k in range(n)])
        rep["notes"] = notes
        for c in clients:
            for port in ports:
                c.pad_set(port)
        if args.save_countdown:
            # Diagnostics: each peer's state in the countdown, for replaying its pass log.
            def save_cd(k: int) -> None:
                c = clients[k]
                wait(lambda: c.call("gprb_status")["phase"] == "countdown" and
                     B.read_frame_counters(c.read_mem).get("game_frame", 0) >= 60, 180, f"P{ports[k] + 1} countdown")
                c.pause()
                d = Path(args.save_countdown)
                d.mkdir(parents=True, exist_ok=True)
                c.save_state(str((d / f"r{run}-m0-p{ports[k] + 1}.sav").resolve()))
                c.resume()
            G.par([lambda k=k: save_cd(k) for k in range(n)])
        deadline = time.monotonic() + 180
        while not all(c.call("gprb_status")["phase"] in ("running", "ended", "error") for c in clients):
            _alive(insts)
            if time.monotonic() > deadline:
                raise RuntimeError("timed out waiting for the sessions to start: " +
                                   str([c.call("gprb_status")["phase"] for c in clients]))
            time.sleep(0.2)
        st = [c.call("gprb_status") for c in clients]
        if any(s["phase"] != "running" for s in st):
            raise RuntimeError(f"session did not start: {[s['phase'] + ' ' + s['error'] for s in st]}")
        rep["start"] = [{k: s.get(k) for k in ("local_slot", "host_slot", "match_ports", "num_players",
                                                "gone_flag_addr")} for s in st]
        for c in clients:
            c.call("game_pads", record=True)
        for sim in sims:
            sim.reset_epoch()
        setup = B.read_match_setup(clients[0].read_mem)
        F._macro = gentle_macro
        stop_at = time.monotonic() + args.timeout
        stop_flag = threading.Event()
        gone_seen: Dict[int, Dict[str, Any]] = {}

        def stop() -> bool:
            return stop_flag.is_set() or time.monotonic() > stop_at

        def play(k: int) -> None:
            seat = F.Seat(clients[k], ports[k], ports[k], f"P{ports[k] + 1}")
            with contextlib.suppress(HarnessError, OSError, ConnectionError):
                F.fight([seat], args.frames, mode="random", seed=f"{run}-{ports[k]}",
                        stage_kind=setup.stage_kind if setup else None,
                        stop=lambda: stop() or ports[k] in dropped_ports)
            if ports[k] not in dropped_ports:
                stop_flag.set()

        th = [threading.Thread(target=play, args=(k,)) for k in range(n)]
        for t in th:
            t.start()
        t0 = time.monotonic()
        last = 0.0
        while any(t.is_alive() for t in th):
            time.sleep(0.5)
            live = [k for k in range(n) if ports[k] not in dropped_ports]
            due = [d for d in drops if d[0] not in dropped_ports and time.monotonic() - t0 >= d[1]]
            if due:
                dport, _, dhow = due[0]
                k = ports.index(dport)
                frames = []
                for j in live:
                    with contextlib.suppress(Exception):
                        frames.append(clients[j].call("gprb_status")["current_frame"])
                rep["drop"][drops.index(due[0])]["at_frames"] = frames
                # Marked first: the player's own thread must not take its end for the match's.
                dropped_ports.add(dport)
                if dhow == "kill":
                    insts[k].kill()
                else:
                    clients[k].call("gprb_stop")
                print(f"  dropped P{dport + 1} ({dhow}) at frames {frames}", flush=True)
                continue
            # The gone flags as the remaining games see them (memory, inside the region set).
            if dropped_ports:
                for j in live:
                    with contextlib.suppress(Exception):
                        s = clients[j].call("gprb_status")
                        addr = int(s.get("gone_flag_addr") or 0)
                        flags = list(clients[j].read_mem(addr, 4)) if addr else s.get("gone_flags")
                        g = gone_seen.setdefault(ports[j], {"flags_max": [0, 0, 0, 0]})
                        g["flags_max"] = [max(a, b) for a, b in zip(g["flags_max"], flags)]
                        g["gone_frames"] = {p["slot"]: p["gone_frame"] for p in s["peers"]}
            if time.monotonic() - last > 30:
                last = time.monotonic()
                st = []
                for j in live:
                    with contextlib.suppress(Exception):
                        st.append(clients[j].call("gprb_status"))
                print(f"  [{preset}/{cpu} n{n}] frames {[s['current_frame'] for s in st]} rollbacks "
                      f"{[s['rollbacks'] for s in st]} phase {[s['phase'] for s in st]}", flush=True)
                if any(s["phase"] != "running" for s in st):
                    stop_flag.set()
            for j in live:
                if insts[j].process is not None and insts[j].process.poll() is not None:
                    stop_flag.set()
        for t in th:
            t.join(timeout=60)
        live = [k for k in range(n) if ports[k] not in dropped_ports]
        rep["gone_seen"] = gone_seen
        lc = [clients[k] for k in live]
        with contextlib.suppress(RuntimeError):
            wait(lambda: all(c.call("gprb_status")["phase"] != "running" for c in lc), 60, "the sessions to end")
        st = [c.call("gprb_status") for c in lc]
        rep["status"] = st
        if len(lc) < 2:
            # Fewer than two players left: the session ended (the 1v1 DISCONNECTED flow).
            rep["checksums"], rep["trace"] = [], []
            raise RuntimeError(f"one player left: {[(s['phase'], s['end_reason'], s['disconnected']) for s in st]}")
        limit = min(s["current_frame"] for s in st) - CONFIRM_MARGIN
        cks = [c.call("gprb_checksums", since=0)["rows"] for c in lc]
        rep["checksums"] = [compare_checksums(cks[0], ck, limit) for ck in cks[1:]]
        tr = [c.call("frame_trace", since=0)["rows"] for c in lc]
        gf_limit = limit + args.start_frame
        tr = [[r for r in t if r[0] <= gf_limit] for t in tr]
        rep["trace"] = [compare_traces(tr[0], t) for t in tr[1:]]
        with contextlib.suppress(Exception):
            pads = [{r[0]: r[1] for r in c.call("game_pads", since=args.start_frame)["rows"]} for c in lc]
            pf = sorted(f for f in set.intersection(*(set(p) for p in pads)) if f <= gf_limit)
            bad = [f for f in pf if any(p[f] != pads[0][f] for p in pads[1:])]
            rep["pads"] = {"compared": len(pf), "mismatches": len(bad), "first": bad[:5]}
            if bad:
                # The first frame's slots that differ: per peer and port, the gfPadStatus bytes (hex).
                f0 = bad[0]
                rep["pads"]["first_ports"] = {
                    f"P{ports[live[i]] + 1}": {q + 1: p[f0][q * 128:(q + 1) * 128] for q in range(4)
                                               if p[f0][q * 128:(q + 1) * 128] != pads[0][f0][q * 128:(q + 1) * 128]}
                    for i, p in enumerate(pads)}
        rep["final"] = [G.small_state(c) for c in lc]
    except Exception as e:  # noqa: BLE001
        rep["error"] = f"{type(e).__name__}: {e}"
    st = rep.get("status") or []
    print(f"  n{n} {preset}/{cpu} run {run}: frames {[s.get('current_frame') for s in st]}, rollbacks "
          f"{[s.get('rollbacks') for s in st]} (max {[s.get('max_rollback_frames') for s in st]}), end "
          f"{[s.get('end_reason') for s in st]}; checksums {[(c.get('compared'), c.get('mismatches')) for c in rep.get('checksums', [])]}; "
          f"traces diverged at {[t.get('diverged_at') for t in rep.get('trace', [])]}; error {rep.get('error')}",
          flush=True)


def run_session_n(preset: str, cpu: str, args: argparse.Namespace, run: int) -> Dict[str, Any]:
    ports = [int(p) - 1 for p in args.ports.split(",")]
    chars = args.chars.split(",")
    if len(chars) != len(ports) or not 2 <= len(ports) <= 4 or len(set(ports)) != len(ports):
        raise SystemExit("--ports and --chars: 2-4 distinct ports, one character each")
    rep: Dict[str, Any] = {"preset": preset, "cpu": cpu, "run": run, "ports": [p + 1 for p in ports],
                           "chars": chars, "stage": args.stage, "teams": args.teams, "delay": args.delay,
                           "region_set": args.region_set}
    t0 = time.monotonic()
    sims: List[NetSim] = []
    import os
    if args.teams:
        # Tests only: every instance makes the match a team battle with these teams (per port).
        os.environ["PPR_GPRB_TEST_TEAMS"] = args.teams
    else:
        os.environ.pop("PPR_GPRB_TEST_TEAMS", None)
    if args.pass_log:
        d = Path(args.pass_log, f"r{run}").resolve()
        d.mkdir(parents=True, exist_ok=True)
        os.environ["PPR_GPRB_PASS_LOG"] = str(d / "pl")
    specs = [(f"{args.name_prefix}-p{p + 1}-{preset}-{cpu}",
              dict(cpu_thread=cpu == "dc", gpu_determinism=args.gpu, video=args.video,
                   controllers=tuple(ports), rtc=None if k == 0 else 0x69C9A3D0 + 3600 * k))
             for k, p in enumerate(ports)]
    orig = G.make_instance

    def mk(name, rtc=None, **kw):
        inst = orig(name, **kw) if rtc is None else orig(name, rtc=rtc, **kw)
        if args.plugin:
            # Our game plugin on the SD card: its PPOM block holds SESSION, where the session writes
            # the gone flags and the plugin removes a gone player's fighter (docs/nplayer/setup.md).
            sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "tools" / "sdcard"))
            import patch_sd
            plugin = Path(args.plugin)
            patch_sd.patch_image(inst.user_dir / "Wii" / "sd.raw",
                                 [(plugin.read_bytes(), f"{patch_sd.PLUGIN_DIR}/{plugin.name}")])
        return inst
    G.make_instance = mk
    try:
        with G.instances(specs) as insts, _keep_logs(args, insts, rep):
            clients = [i.client for i in insts]
            if args.plugin:
                # With the plugin P+ boots to the online menus: back to the main menu, then Versus.
                def to_css(c, port: int) -> None:
                    # GameBridge only locates the block (SessionAddress): it does not service the
                    # plugin's mailbox, whose CLEANUP_CONNECTION (leaving the online menus) would
                    # stop the session, and does not write SESSION.
                    c.call("game_bridge_config", enabled=False)
                    # The ONLINE page; B: the main menu's top page on PLAY ONLINE; up: Brawl.
                    B.wait_scene(c, [B.Scene.MAIN_MENU, B.Scene.CSS], 60 * 120)
                    if B.read_scene(c.read_mem).scene is B.Scene.MAIN_MENU:
                        B.step(c, 60)
                        B.tap(c, port, ["B"], hold=3, release=60)
                        B.tap(c, port, ["DUP"], hold=4, release=40)
                        B.boot_to_css(c, port)
                    sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "tools" / "gamecode"))
                    import ppom
                    # The plugin removes a gone player's fighter in a local Versus match too.
                    ppom.set_cfg_bits(c, ppom.CFG_TEST_GONE, True)
                G.par([lambda c=c: to_css(c, ports[0]) for c in clients])
            G.par([lambda c=c: B.wait_scene(c, [B.Scene.CSS], 60 * 120) for c in clients])
            udp = [free_udp_port() for _ in ports]
            # One proxy per port that has a higher port in the match: everyone above sends through it.
            proxy: Dict[int, NetSim] = {}
            for k, p in enumerate(ports[:-1]):
                proxy[p] = NetSim(("127.0.0.1", udp[k]), ("127.0.0.1", 0), preset, seed=f"gprb-{run}-{p}").start()
                sims.append(proxy[p])
            host = min(ports)
            for k, p in enumerate(ports):
                peers = [{"slot": q, "host": "127.0.0.1", "port": proxy[q].listen_port} if q < p else
                         {"slot": q, "host": "", "port": 0} for q in ports if q != p]
                clients[k].call("gprb_connect", role="host" if p == host else "join", port=udp[k], slot=p,
                                peers=peers, host_slot=host, local_pad=p, region_set=args.region_set, delay=args.delay,
                                name=f"P{p + 1}", hash_regions=False, start_frame=args.start_frame,
                                **sound_opts(args))
            deadline = time.monotonic() + 60
            while not all(c.call("gprb_status")["phase"] == "connected" for c in clients):
                _alive(insts)
                if time.monotonic() > deadline:
                    raise RuntimeError("the peers never connected: " +
                                       str([c.call("gprb_status")["phase"] for c in clients]))
                time.sleep(0.2)
            play_match_n(insts, clients, ports, sims, args, preset, cpu, run, chars, args.stage, rep)
            rep["netsim"] = [s.format_stats() for s in sims if hasattr(s, "format_stats")]
            for c in clients:
                with contextlib.suppress(Exception):
                    c.call("gprb_stop")
    except Exception as e:  # noqa: BLE001
        rep["error"] = f"{type(e).__name__}: {e}"
    finally:
        G.make_instance = orig
        for s in sims:
            with contextlib.suppress(Exception):
                s.stop()
    rep["wall_s"] = round(time.monotonic() - t0, 1)
    return rep


def main(argv: Optional[Sequence[str]] = None) -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--preset", default="typical")
    ap.add_argument("--cpu", default="sc")
    ap.add_argument("--gpu", default=None,
                    help="GPUDeterminismMode for both instances (default: the template's P+ INI, fake-completion); "
                         "'unset' removes it, as on an install without P+'s launcher INI")
    ap.add_argument("--video", default="Null", help="video backend (Null, D3D11, Vulkan)")
    ap.add_argument("--runs", type=int, default=1)
    ap.add_argument("--p1", default="fox")
    ap.add_argument("--p2", default="falco")
    ap.add_argument("--stage", default="battlefield")
    ap.add_argument("--delay", type=int, default=2)
    ap.add_argument("--region-set", default="gp-v19")
    ap.add_argument("--start-frame", type=int, default=240)
    ap.add_argument("--frames", type=int, default=30000)
    ap.add_argument("--rtc-a", type=lambda x: int(x, 0), default=0, help="A's custom RTC (0: the fixed default)")
    ap.add_argument("--rtc-b", type=lambda x: int(x, 0), default=0x69C9A3D0, help="B's custom RTC (A: the default)")
    ap.add_argument("--timeout", type=float, default=1200)
    ap.add_argument("--resim-sounds", choices=("play", "suppress", "dedupe"), default="dedupe",
                    help="sounds started again by resimulated frames")
    ap.add_argument("--minutes", type=int, default=0, help="write this time limit (4 stocks) on both CSSs")
    ap.add_argument("--matches", type=int, default=1, help="consecutive matches on one connection")
    ap.add_argument("--next-matches", default="mario:marth:final_destination,falco:fox:smashville",
                    help="P1:P2:stage of the matches after the first")
    ap.add_argument("--json", default=None)
    ap.add_argument("--log-dir", default=None, help="copy each instance's dolphin.log and stderr here")
    ap.add_argument("--name-prefix", default="gprb", help="instance name prefix (ppharness clean --prefix)")
    ap.add_argument("--save-traces", default=None, help="write both peers' per-frame traces to this directory")
    ap.add_argument("--pass-log", default=None,
                    help="diagnostics: record every update's passes (for replays) under this directory")
    ap.add_argument("--save-countdown", default=None,
                    help="diagnostics: save each peer's state in the countdown to this directory")
    ap.add_argument("--sample-every", type=int, default=0,
                    help="diagnostics: region-set chunk hashes every N session frames on both peers, compared")
    ap.add_argument("--watch", default="", help="diagnostics: addr:size,... recorded with every sample")
    ap.add_argument("--ports", default="",
                    help="2-4 players, one instance each: their in-game ports (1-4, gaps allowed), e.g. 1,2,3,4 "
                         "or 1,3 (default: the two-instance flow above)")
    ap.add_argument("--chars", default="fox,falco,mario,marth", help="with --ports: one character per port")
    ap.add_argument("--teams", default="",
                    help="with --ports: a team battle, the team (0 red, 1 blue, 2 green) of P1..P4, e.g. 0,1,0,1")
    ap.add_argument("--plugin", default="",
                    help="with --ports: our game plugin (PPOnline.rel) on every SD card; the session writes the "
                         "gone flags into its SESSION block and the plugin removes the gone player's fighter")
    ap.add_argument("--drop", default="",
                    help="with --ports: PORT:SECONDS[:stop|kill], that player leaves the match after SECONDS "
                         "(stop = it leaves the session, kill = its Dolphin is killed)")
    args = ap.parse_args(argv)
    for preset in args.preset.split(","):
        for cpu in args.cpu.split(","):
            for run in range(args.runs):
                if args.ports:
                    args.chars = ",".join(args.chars.split(",")[:len(args.ports.split(","))])
                    rep = run_session_n(preset, cpu, args, run)
                else:
                    rep = run_session(preset, cpu, args, run)
                if args.json:
                    p = Path(args.json)
                    prev = json.loads(p.read_text()) if p.exists() else []
                    prev.append(rep)
                    p.write_text(json.dumps(prev, indent=1))
    return 0


if __name__ == "__main__":
    sys.exit(main())
