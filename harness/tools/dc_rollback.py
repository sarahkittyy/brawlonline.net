"""Dual-core rollback measurements and desync hunting (two local instances through netsim).

``run``     One rollback netplay session: CSS -> Fox vs Falco -> Battlefield -> seeded random play,
            again and again (results -> CSS -> next match) until ``--seconds`` of match time have
            been played. Per side it reports:

            * snapshot cost during the matches from ``rollback_timings`` (every SaveFrame's wall
              time, the dual-core GPU sync inside it, and the pipelines the video thread compiled
              synchronously meanwhile): p50 / p90 / p99 / max;
            * game speed (GekkoNet frames per second while in a match);
            * rollback counters, GekkoNet desyncs, and the per-frame pads and checksums on every
              frame both peers confirmed (``rollback_pad_history``), with the first differing frame.

            With ``--chunk-hashes`` (sets ``PPR_ROLLBACK_CHUNK_HASHES``) a monitor thread watches
            the confirmed checksums; on the first mismatch it pulls ``rollback_chunk_hashes`` for
            the frames still kept (the last 256) from both peers and records which MEM1/MEM2 chunks
            differ, from the earliest frame on.

``matrix``  ``run`` repeated over presets x video backends x repeats; one JSON line per run.

Examples::

    python harness/tools/dc_rollback.py run --preset bad_wifi --video D3D11 --seconds 60
    python harness/tools/dc_rollback.py matrix --presets typical,bad_wifi --videos D3D11,Vulkan \\
        --repeats 2 --seconds 60 --out run/qa/dc/perf.jsonl
    python harness/tools/dc_rollback.py matrix --presets bad_wifi,awful --videos D3D11 \\
        --repeats 20 --seconds 120 --chunk-hashes 4096 --out run/qa/dc/stress.jsonl
"""

from __future__ import annotations

import argparse
import contextlib
import json
import logging
import random
import sys
import threading
import time
from pathlib import Path
from typing import Any, Dict, List, Optional

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from ppharness import brawl as B  # noqa: E402
from ppharness import flows as F  # noqa: E402
from ppharness.client import HarnessClient, HarnessError  # noqa: E402
from ppharness.instance import InstanceConfig  # noqa: E402
from ppharness.paths import instances_root  # noqa: E402
from ppharness.session import two_player_netplay  # noqa: E402

log = logging.getLogger("dc_rollback")

CHARS = ["fox", "falco"]  # the CSS recipes are verified for these
STAGES = ["battlefield"]  # the SSS recipe is verified for Battlefield


def pct(values: List[float], q: float) -> Optional[float]:
    if not values:
        return None
    v = sorted(values)
    k = min(len(v) - 1, max(0, int(round(q / 100.0 * (len(v) - 1)))))
    return v[k]


def summarize(values: List[float]) -> Dict[str, Any]:
    if not values:
        return {"n": 0}
    return {"n": len(values), "mean": round(sum(values) / len(values), 1),
            "p50": pct(values, 50), "p90": pct(values, 90), "p99": pct(values, 99), "max": max(values)}


class Timings:
    """Collects rollback_timings samples of one instance, tagged in-match or not."""

    def __init__(self, client: HarnessClient):
        self.c = client
        self.next = 0
        self.save: List[float] = []
        self.sync: List[float] = []
        self.compiles: List[int] = []
        self.phases: List[tuple] = []  # (save, sync, evict, dostate, ram) per sample
        self.supported = True

    def poll(self, keep: bool) -> None:
        if not self.supported:
            return
        try:
            r = self.c.call("rollback_timings", since=self.next)
        except HarnessError as e:
            if "unknown" in str(e).lower():
                self.supported = False
            return
        self.next = int(r["next"])
        if keep:
            self.save += r["save_us"]
            self.sync += r["sync_us"]
            self.compiles += r["compiles"]
            if "ram_us" in r:
                self.phases += list(zip(r["save_us"], r["sync_us"], r["evict_us"], r["dostate_us"],
                                        r["ram_us"]))

    def report(self) -> Dict[str, Any]:
        with_compiles = [s for s, c in zip(self.sync, self.compiles) if c]
        spikes = [(s, c) for s, c in zip(self.sync, self.compiles) if s > 50_000]
        worst = sorted(self.phases, reverse=True)[:8]
        return {"save_us": summarize(self.save), "sync_us": summarize(self.sync),
                "worst_saves_save_sync_evict_dostate_ram": worst,
                "ram_us": summarize([p[4] for p in self.phases]),
                "dostate_us": summarize([p[3] for p in self.phases]),
                "syncs_with_compiles": len(with_compiles),
                "sync_us_with_compiles": summarize(with_compiles),
                "sync_spikes_over_50ms": len(spikes),
                "sync_spikes_with_compiles": sum(1 for s, c in spikes if c)}


class Monitor(threading.Thread):
    """Every 0.5 s: GekkoNet frame/time samples, timing samples, and (optionally) the first
    confirmed-frame checksum mismatch with chunk hashes pulled right away."""

    def __init__(self, s, chunk_hashes: bool):
        super().__init__(daemon=True)
        self.s = s
        self.clients = s.clients
        self.names = [i.name for i in s.instances]
        self.chunk_hashes = chunk_hashes
        self.stop_ev = threading.Event()
        self.in_match = threading.Event()
        self.frames: Dict[str, List[tuple]] = {n: [] for n in self.names}
        self.timings = [Timings(c) for c in self.clients]
        self.first_mismatch: Optional[Dict[str, Any]] = None
        self.checked_upto = 0
        self.compared = self.n_in_match = self.pad_mismatches = self.state_mismatches = 0
        self.first_state: Optional[Dict[str, Any]] = None
        self.first_pad: Optional[Dict[str, Any]] = None
        self.lock = threading.Lock()
        self.cmp_lock = threading.Lock()

    def run(self) -> None:
        t0 = time.monotonic()
        while not self.stop_ev.is_set():
            keep = self.in_match.is_set()
            for n, c, tm in zip(self.names, self.clients, self.timings):
                try:
                    rb = c.netplay_status().rollback or {}
                    if rb.get("session_started"):
                        self.frames[n].append((time.monotonic() - t0, int(rb["current_frame"]), keep))
                except HarnessError:
                    pass
                with self.lock:
                    tm.poll(keep)
            with contextlib.suppress(HarnessError):
                self.check_mismatch()
            self.stop_ev.wait(0.5)

    def check_mismatch(self) -> None:
        with self.cmp_lock:
            self._check_mismatch()

    def _check_mismatch(self) -> None:
        """Compare every confirmed frame once (the servers keep only the last 2048)."""
        since = self.checked_upto + 1
        h, j = (c.rollback_pad_history(since) for c in self.clients)
        upto = min(max(h, default=0), max(j, default=0)) - 12
        if upto < since:
            return
        frames = [f for f in range(since, upto + 1) if f in h and f in j]
        self.compared += len(frames)
        self.n_in_match += sum(1 for f in frames if h[f][5] and j[f][5])
        pads = [f for f in frames if h[f][:5] != j[f][:5]]
        bad = [f for f in frames if h[f][5] and j[f][5] and h[f][5:9] != j[f][5:9]]
        self.pad_mismatches += len(pads)
        self.state_mismatches += len(bad)
        if pads and self.first_pad is None:
            self.first_pad = {"frame": pads[0], "host_row": h[pads[0]], "joiner_row": j[pads[0]],
                              "raw_diffs": raw_pad_diffs(self.clients, pads[0] - 3, pads[0] + 12)}
        if bad and self.first_state is None:
            self.first_state = {"frame": bad[0], "host_row": h[bad[0]], "joiner_row": j[bad[0]]}
        self.checked_upto = upto
        if not bad or not self.chunk_hashes or self.first_mismatch is not None:
            return
        first = bad[0]
        log.warning("checksum mismatch at frame %d; pulling chunk hashes", first)
        rec: Dict[str, Any] = {"frame": first, "host_row": h[first], "joiner_row": j[first], "frames": {}}
        for f in range(first, first - 260, -1):
            try:
                a = self.clients[0].call("rollback_chunk_hashes", frame=f)
                b = self.clients[1].call("rollback_chunk_hashes", frame=f)
            except HarnessError:
                break
            diff = [i for i, (x, y) in enumerate(zip(a["hashes"], b["hashes"])) if x != y]
            rec["chunk_size"] = a["chunk_size"]
            rec["frames"][f] = diff
        self.first_mismatch = rec

    def game_fps(self) -> Dict[str, Optional[float]]:
        """GekkoNet frames per second over the in-match samples."""
        out = {}
        for n, pts in self.frames.items():
            frames, secs = 0, 0.0
            for (t0, f0, k0), (t1, f1, k1) in zip(pts, pts[1:]):
                if k0 and k1 and f1 >= f0:
                    frames += f1 - f0
                    secs += t1 - t0
            out[n] = round(frames / secs, 2) if secs > 0 else None
        return out

    def longest_stall(self, n: str) -> float:
        worst, last_v, last_t = 0.0, None, None
        for t, f, _ in self.frames[n]:
            if f != last_v:
                last_v, last_t = f, t
            else:
                worst = max(worst, t - last_t)
        return worst


def raw_pad_diffs(clients, first: int, last: int) -> Dict[str, Any]:
    """Byte-level differences of the four raw gfPadStatus slots (0x40 each) between the peers."""
    rows = []
    for c in clients:
        try:
            r = c.call("rollback_pad_history", since=max(0, first), raw=True)
        except HarnessError as e:
            return {"error": str(e)}
        rows.append({int(x[0]): x[-1] for x in r.get("frames", []) if isinstance(x[-1], str)})
    out: Dict[str, Any] = {}
    for f in range(first, last + 1):
        a, b = rows[0].get(f), rows[1].get(f)
        if not a or not b:
            continue
        ba, bb = bytes.fromhex(a), bytes.fromhex(b)
        diff = [(i // 0x40, i % 0x40, ba[i], bb[i]) for i in range(min(len(ba), len(bb))) if ba[i] != bb[i]]
        out[str(f)] = {"diff_port_off_host_joiner": diff[:40],
                       "host": [a[k * 128:(k + 1) * 128] for k in range(2)],
                       "joiner": [b[k * 128:(k + 1) * 128] for k in range(2)]}
    return out


def compare_histories(host: HarnessClient, joiner: HarnessClient) -> Dict[str, Any]:
    h, j = host.rollback_pad_history(0), joiner.rollback_pad_history(0)
    upto = min(max(h, default=0), max(j, default=0)) - 10
    common = sorted(f for f in h if f in j and f <= upto)
    pads = [f for f in common if h[f][:5] != j[f][:5]]
    state = [f for f in common if len(h[f]) > 8 and h[f][5] and j[f][5] and h[f][5:9] != j[f][5:9]]
    in_match = [f for f in common if len(h[f]) > 8 and h[f][5] and j[f][5]]
    out: Dict[str, Any] = {"compared": len(common), "in_match": len(in_match),
                           "pad_mismatches": len(pads), "state_mismatches": len(state),
                           "first_pad_mismatch": pads[0] if pads else None,
                           "first_state_mismatch": state[0] if state else None}
    if state:
        f = state[0]
        parts = ["combined", "legacy", "frame_counter", "fighters"]
        out["first_state_rows"] = {"host": h[f], "joiner": j[f]}
        out["first_state_parts"] = [p for k, p in enumerate(parts) if h[f][5 + k] != j[f][5 + k]]
    return out


def chunk_addr(index: int, chunk: int) -> int:
    """rollback_chunk_hashes covers MEM1 (24 MiB) then MEM2 (64 MiB)."""
    off = index * chunk
    return 0x80000000 + off if off < 0x01800000 else 0x90000000 + (off - 0x01800000)


def chunk_diff(host: HarnessClient, joiner: HarnessClient, samples: int) -> Dict[str, Any]:
    """MEM1/MEM2 chunks that differ between the peers on confirmed frames (the newest ones both
    still keep), and on how many of the sampled frames each one differed."""
    h = host.rollback_pad_history(0)
    j = joiner.rollback_pad_history(0)
    upto = min(max(h, default=0), max(j, default=0)) - 12
    frames = [f for f in range(upto, upto - 250, -max(1, 240 // max(1, samples))) if f in h and f in j][:samples]
    counts: Dict[int, int] = {}
    size = None
    used = 0
    for f in frames:
        try:
            a = host.call("rollback_chunk_hashes", frame=f)
            b = joiner.call("rollback_chunk_hashes", frame=f)
        except HarnessError:
            continue
        used += 1
        size = a["chunk_size"]
        for i, (x, y) in enumerate(zip(a["hashes"], b["hashes"])):
            if x != y:
                counts[i] = counts.get(i, 0) + 1
    return {"chunk_size": size, "frames": used,
            "differing": [[hex(chunk_addr(i, size)), n] for i, n in sorted(counts.items())]}


def suspend_processes(pids: List[int], suspend: bool) -> None:
    """Freeze (or thaw) whole processes, like a host-wide stall (Windows: NtSuspendProcess)."""
    if sys.platform == "win32":
        import ctypes
        ntdll = ctypes.WinDLL("ntdll")
        k32 = ctypes.WinDLL("kernel32")
        handles = [k32.OpenProcess(0x0800, False, pid) for pid in pids]  # PROCESS_SUSPEND_RESUME
        try:
            for h in handles:
                if h:
                    (ntdll.NtSuspendProcess if suspend else ntdll.NtResumeProcess)(h)
        finally:
            for h in handles:
                if h:
                    k32.CloseHandle(h)
    else:
        import os
        import signal
        for pid in pids:
            os.kill(pid, signal.SIGSTOP if suspend else signal.SIGCONT)


class Freezer(threading.Thread):
    """After `at` seconds in the first match, suspends the chosen Dolphins for `length` seconds."""

    def __init__(self, pids: List[int], at: float, length: float, started: threading.Event):
        super().__init__(daemon=True)
        self.pids, self.at, self.length, self.started = pids, at, length, started
        self.done_at: Optional[float] = None

    def run(self) -> None:
        self.started.wait()
        time.sleep(self.at)
        log.warning("freezing %s for %.1f s", self.pids, self.length)
        suspend_processes(self.pids, True)
        try:
            time.sleep(self.length)
        finally:
            suspend_processes(self.pids, False)
        self.done_at = time.monotonic()


def run_session(args, preset: str, video: str, seed: int) -> Dict[str, Any]:
    env: Dict[str, str] = {}
    if args.chunk_hashes:
        env["PPR_ROLLBACK_CHUNK_HASHES"] = str(args.chunk_hashes)
    for kv in args.env or []:
        k, _, v = kv.partition("=")
        env[k] = v
    cfg = InstanceConfig(cpu_thread=args.cpu_thread == "on", video_backend=video,
                         config_args=list(args.config or []))
    rng = random.Random(seed)
    result: Dict[str, Any] = {"preset": preset, "video": video, "cpu_thread": args.cpu_thread,
                              "seed": seed, "env": env, "config": args.config or [],
                              "exe": str(args.exe) if args.exe else None}
    t_start = time.monotonic()
    matches = 0
    with two_player_netplay(preset, rollback=True, exe=args.exe, name=f"dcrb-{preset}-{video}{args.tag}",
                            config=cfg, seed=seed, env=env or None, keep=args.keep) as s:
        host, joiner = s.clients
        mon = Monitor(s, bool(args.chunk_hashes))
        mon.start()
        if args.freeze:
            who = {"both": s.instances, "host": s.instances[:1], "joiner": s.instances[1:]}[args.freeze_who]
            freezer = Freezer([i.process.pid for i in who], args.freeze_at, args.freeze, mon.in_match)
            freezer.start()
            result["freeze"] = {"at": args.freeze_at, "seconds": args.freeze, "who": args.freeze_who}
        try:
            hs, js = F.netplay_seat(host, "host"), F.netplay_seat(joiner, "joiner")
            played = 0.0
            while played < args.seconds and (mon.first_mismatch is None or not args.stop_on_mismatch):
                for attempt in range(3):
                    try:
                        F.to_css([hs, js])
                        F.pick_characters([(hs, rng.choice(CHARS)), (js, rng.choice(CHARS))])
                        F.start_and_pick_stage(hs, rng.choice(STAGES))
                        F.wait_match_started([host, joiner])
                        break
                    except B.RecipeError as e:
                        log.warning("menu navigation failed (%s), retrying", e)
                        if attempt == 2:
                            raise
                        result.setdefault("menu_retries", 0)
                        result["menu_retries"] += 1
                        if B.read_scene(host.read_mem).scene is B.Scene.SSS:
                            with contextlib.suppress(HarnessError, B.RecipeError):
                                B.tap(host, hs.pp, ["B"], hold=3, release=10)
                matches += 1
                mon.in_match.set()
                t0 = time.monotonic()
                frames = int(60 * min(args.seconds - played, args.match_seconds))
                F.play([(hs, "random"), (js, "random")], frames, seed=rng.randrange(1 << 30))
                mon.in_match.clear()
                played += time.monotonic() - t0
                st = B.read_match_state(host.read_mem)
                if st.in_match and not st.game_set:
                    break  # time is up and the match is still on
                with contextlib.suppress(HarnessError):
                    B.wait_scene(host, [B.Scene.RESULTS], 60 * 20)
            for c, seat in ((host, hs), (joiner, js)):
                with contextlib.suppress(HarnessError):
                    B.neutral(c, seat.pp)
            time.sleep(2)
            result["compare"] = compare_histories(host, joiner)
            if args.chunk_hashes and args.chunk_diff:
                result["chunk_diff"] = chunk_diff(host, joiner, args.chunk_diff)
            with contextlib.suppress(HarnessError):
                mon.check_mismatch()
            result["compare_all"] = {"compared": mon.compared, "in_match": mon.n_in_match,
                                     "pad_mismatches": mon.pad_mismatches,
                                     "state_mismatches": mon.state_mismatches,
                                     "first_state": mon.first_state, "first_pad": mon.first_pad}
            stats = {i.name: i.client.netplay_status().rollback for i in s.instances}
            gpu_writes = {i.name: (i.client.status().raw.get("presentation") or {}).get("gpu_ram_writes")
                          for i in s.instances}
        finally:
            mon.stop_ev.set()
            mon.join(timeout=30)
        result["matches"] = matches
        result["match_seconds"] = round(played, 1)
        result["wall_seconds"] = round(time.monotonic() - t_start, 1)
        result["game_fps"] = mon.game_fps()
        result["longest_stall_s"] = {n: round(mon.longest_stall(n), 2) for n in mon.names}
        result["sides"] = {}
        for n, tm in zip(mon.names, mon.timings):
            rb = stats[n]
            keys = ["rollbacks", "max_rollback_frames", "frames_resimulated", "desyncs_detected",
                    "last_desync_frame", "save_count", "save_us_total", "save_us_max",
                    "save_sync_count", "save_sync_us_total", "save_sync_us_max", "load_count",
                    "load_us_total", "load_us_max", "gpu_deterministic", "gpu_sync_compiles",
                    "gpu_sync_compile_us_total", "gpu_sync_compile_us_max", "gpu_sync_utility_compiles",
                    "gpu_sync_utility_compile_us_total", "shader_compilation_mode",
                    "stall_polls", "peer_disconnects", "skipped_loads", "dropped_advances", "stall_fallbacks"]
            side = {k: rb.get(k) for k in keys}
            side["timings"] = tm.report()
            side["by_depth"] = rb.get("by_depth")
            side["gpu_ram_writes"] = gpu_writes.get(n)
            result["sides"][n] = side
        if mon.first_mismatch:
            result["first_mismatch_chunks"] = mon.first_mismatch
        desync = any((rb.get("desyncs_detected") or 0) for rb in stats.values())
        if desync or result["compare"]["state_mismatches"] or mon.state_mismatches:
            s.mark_failed()  # keep the instance dirs (logs) for this run
    return result


def prune_instances(prefix: str, keep_logs: bool) -> None:
    """Instance dirs are kept when a run fails; each holds a 2 GB sd.raw. Delete them, or with
    keep_logs everything but Logs/ and the harness output."""
    import shutil
    root = instances_root()
    for d in root.glob(prefix + "-*"):
        if not d.is_dir():
            continue
        if not keep_logs:
            shutil.rmtree(d, ignore_errors=True)
            continue
        for child in d.iterdir():
            if child.name in ("Logs",) or child.name.startswith("harness-"):
                continue
            if child.is_dir() and not child.is_symlink():
                shutil.rmtree(child, ignore_errors=True)
            else:
                with contextlib.suppress(OSError):
                    child.unlink()
        with contextlib.suppress(OSError):
            d.rename(root / f"kept-{time.strftime('%m%d-%H%M%S')}-{d.name}")


def headline(r: Dict[str, Any]) -> str:
    parts = [f"{r['preset']}/{r['video']}/{r['cpu_thread']} seed={r['seed']}",
             f"matches={r.get('matches')} t={r.get('match_seconds')}s fps={r.get('game_fps')}"]
    for n, sd in (r.get("sides") or {}).items():
        t = sd["timings"]
        parts.append(f"{n}: save p50/p99/max={t['save_us'].get('p50')}/{t['save_us'].get('p99')}/"
                     f"{t['save_us'].get('max')}us sync p50/p99/max={t['sync_us'].get('p50')}/"
                     f"{t['sync_us'].get('p99')}/{t['sync_us'].get('max')}us rb={sd['rollbacks']} "
                     f"desyncs={sd['desyncs_detected']} compiles={sd['gpu_sync_compiles']}")
    c = r.get("compare", {})
    parts.append(f"compare: {c.get('compared')} frames, pad={c.get('pad_mismatches')} "
                 f"state={c.get('state_mismatches')} first={c.get('first_state_mismatch')}")
    ca = r.get("compare_all")
    if ca:
        parts.append(f"all confirmed: {ca['compared']} ({ca['in_match']} in match) pad={ca['pad_mismatches']} "
                     f"state={ca['state_mismatches']}")
    return " | ".join(parts)


def main(argv: Optional[List[str]] = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)

    def common(p):
        p.add_argument("--exe", default=None, help="DolphinNoGUI (default: the harness default)")
        p.add_argument("--cpu-thread", choices=["on", "off"], default="on")
        p.add_argument("--seconds", type=float, default=60, help="match time to play per session")
        p.add_argument("--match-seconds", type=float, default=90, help="longest single match")
        p.add_argument("--chunk-hashes", type=int, default=0, help="PPR_ROLLBACK_CHUNK_HASHES chunk size")
        p.add_argument("--stop-on-mismatch", action="store_true")
        p.add_argument("--freeze", type=float, default=0, help="suspend Dolphin(s) this many seconds")
        p.add_argument("--freeze-at", type=float, default=20, help="seconds into the first match")
        p.add_argument("--freeze-who", choices=["both", "host", "joiner"], default="both")
        p.add_argument("--chunk-diff", type=int, default=0,
                       help="with --chunk-hashes: diff this many confirmed frames' chunk hashes at the end")
        p.add_argument("--env", action="append", help="extra KEY=VALUE for both Dolphins")
        p.add_argument("--config", action="append", help="extra -C System.Section.Key=Value")
        p.add_argument("--keep", action="store_true")
        p.add_argument("--tag", default="", help="suffix for instance names (concurrent runs)")
        p.add_argument("--out", default=None, help="append JSON lines here")
        p.add_argument("-v", "--verbose", action="store_true")

    p = sub.add_parser("run")
    common(p)
    p.add_argument("--preset", default="bad_wifi")
    p.add_argument("--video", default="D3D11")
    p.add_argument("--seed", type=int, default=1)
    p = sub.add_parser("matrix")
    common(p)
    p.add_argument("--presets", default="typical,bad_wifi")
    p.add_argument("--videos", default="D3D11")
    p.add_argument("--repeats", type=int, default=1)
    p.add_argument("--seed", type=int, default=1, help="first seed")
    args = ap.parse_args(argv)
    logging.basicConfig(level=logging.INFO if args.verbose else logging.WARNING,
                        format="%(asctime)s %(name)s %(message)s")

    if args.cmd == "run":
        plan = [(args.preset, args.video, args.seed)]
    else:
        plan = []
        seed = args.seed
        for rep in range(args.repeats):
            for preset in args.presets.split(","):
                for video in args.videos.split(","):
                    plan.append((preset, video, seed))
                    seed += 1
    failures = 0
    for preset, video, seed in plan:
        for attempt in range(2):
            try:
                r = run_session(args, preset, video, seed)
                break
            except Exception as e:  # noqa: BLE001 - keep the matrix going
                log.exception("run failed")
                r = {"preset": preset, "video": video, "seed": seed, "error": repr(e),
                     "env": dict(kv.partition("=")[::2] for kv in (args.env or []))}
                prune_instances(f"dcrb-{preset}-{video}{args.tag}", keep_logs=True)
        if "error" in r:
            failures += 1
        if not args.keep:
            bad = bool(r.get("compare", {}).get("state_mismatches")) or bool(
                r.get("compare_all", {}).get("state_mismatches")) or any(
                (sd.get("desyncs_detected") or 0) for sd in (r.get("sides") or {}).values())
            prune_instances(f"dcrb-{preset}-{video}{args.tag}", keep_logs=bad)
        print(headline(r) if "error" not in r else f"{preset}/{video} seed={seed}: ERROR {r['error']}",
              flush=True)
        if args.out:
            Path(args.out).parent.mkdir(parents=True, exist_ok=True)
            with open(args.out, "a", encoding="utf-8") as f:
                f.write(json.dumps(r) + "\n")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
