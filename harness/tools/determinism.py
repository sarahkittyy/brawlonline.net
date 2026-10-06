"""Offline determinism measurements: same inputs from boot, where does game state diverge?

Two steps:

``record``   Boot one instance and play boot -> CSS (Fox vs Falco) -> Battlefield -> ~20 s of
             chase/random play with the closed-loop recipes, while recording every pad change as
             an exact per-poll timeline (pad_set is turned into a poll-scheduled pad_script).
             Writes a timeline JSON.

``compare``  Boot N instances with the same settings, schedule the whole timeline on each at
             boot (one absolute-start pad_script per port, so every run gets identical input on
             identical game frames), and at checkpoints pause all of them at the same
             ``input_polls`` and hash labelled regions: brawl.gameplay_ranges() in a match,
             brawl.menu_ranges() on menus, plus RNG seeds, frame counters, per-player state and
             (for reference) whole MEM1/MEM2. The first checkpoint where a region differs is then
             narrowed down to 64-byte chunks with hash_mem bisection, and the bytes are dumped.

Settings are named profiles (``--profile``): RTC (host clock or fixed via ``-C``), CPU thread
(single/dual core) and GPUDeterminismMode (P+'s launcher game INI sets fake-completion; the
profile rewrites that INI in the instance).

    python harness/tools/determinism.py record --out run/qa/timeline.json
    python harness/tools/determinism.py compare --timeline run/qa/timeline.json --profile sc-rtc
    python harness/tools/determinism.py matrix --timeline run/qa/timeline.json --out run/qa/det.json
"""

from __future__ import annotations

import argparse
import concurrent.futures as cf
import contextlib
import json
import logging
import struct
import sys
import time
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any, Dict, List, Mapping, Optional, Sequence, Tuple

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from ppharness import brawl as B  # noqa: E402
from ppharness import flows as F  # noqa: E402
from ppharness.client import HarnessClient, HarnessCommandError, PadInput, ScriptWindow  # noqa: E402
from ppharness.inifile import IniFile  # noqa: E402
from ppharness.instance import DolphinInstance, InstanceConfig  # noqa: E402
from ppharness.session import FrameKey, OvershotError, pause_at  # noqa: E402

log = logging.getLogger("determinism")

FIXED_RTC = 1735689600  # 2025-01-01 00:00:00 UTC (Orca uses the same value)
NEUTRAL = {"buttons": [], "main": [128, 128], "c": [128, 128], "l": 0, "r": 0}
LONG_HOLD = 1_000_000
STAY_PAUSED = 40   # replay: items closer than this many VI frames are reached by frame_advance


# --------------------------------------------------------------------------- profiles


@dataclass
class Profile:
    name: str
    cpu_thread: bool
    fixed_rtc: bool
    gpu_determinism: Optional[str]  # None: keep the launcher INI ("fake-completion"); else override
    note: str = ""

    def config(self) -> InstanceConfig:
        args = []
        if self.fixed_rtc:
            args += ["Dolphin.Core.EnableCustomRTC=True", f"Dolphin.Core.CustomRTCValue={FIXED_RTC:#x}"]
        return InstanceConfig(cpu_thread=self.cpu_thread, video_backend="Null", config_args=args)


PROFILES: Dict[str, Profile] = {p.name: p for p in (
    Profile("sc-hostrtc", False, False, None, "single core, host clock RTC (Dolphin default)"),
    Profile("sc-rtc", False, True, None, "single core, fixed RTC"),
    Profile("dc-hostrtc", True, False, None, "dual core, host clock RTC"),
    Profile("dc-rtc", True, True, None, "dual core, fixed RTC, GPUDeterminismMode fake-completion (P+ INI)"),
    Profile("dc-rtc-gpuauto", True, True, "auto", "dual core, fixed RTC, GPUDeterminismMode auto (Dolphin default)"),
    Profile("dc-rtc-gpunone", True, True, "none", "dual core, fixed RTC, GPUDeterminismMode none"),
    Profile("sc-rtc-gpuauto", False, True, "auto", "single core, fixed RTC, GPUDeterminismMode auto"),
)}

LAUNCHER_INI = "ID-Project+ Offline Launcher.ini"


def make_instance(name: str, profile: Profile, keep: bool = False) -> DolphinInstance:
    inst = DolphinInstance(name, config=profile.config(), keep=keep, connect_timeout=90)
    inst.create()
    if profile.gpu_determinism is not None:
        for fname in (LAUNCHER_INI, "RSBE01.ini"):
            p = inst.user_dir / "GameSettings" / fname
            ini = IniFile.load(p)
            ini.update({"Core": {"GPUDeterminismMode": profile.gpu_determinism}})
            ini.save(p)
    # RSBE01.ini in the template forces CPUThread = True; keep it in line with the profile.
    p = inst.user_dir / "GameSettings" / "RSBE01.ini"
    if p.exists():
        ini = IniFile.load(p)
        ini.update({"Core": {"CPUThread": profile.cpu_thread}})
        ini.save(p)
    return inst


# --------------------------------------------------------------------------- recording


def _norm(pad: Mapping[str, Any]) -> Dict[str, Any]:
    d = dict(NEUTRAL)
    for k in ("buttons", "main", "c", "l", "r"):
        if k in pad:
            v = pad[k]
            d[k] = sorted(v) if k == "buttons" else (list(v) if isinstance(v, (list, tuple)) else v)
    return d


class RecordingClient:
    """Wraps a HarnessClient so every input change can be replayed exactly.

    pad_script replaces a port's script at once and leaves the port neutral until the new one
    starts, and which SI latch sees what depends on host timing. So every submission is made
    while the emulation is paused: pause, read the VI frame and poll, submit (start "next"),
    resume. A replay pauses at the same VI frame and submits the same script with the same
    absolute start, which puts every instance in the same state at the moment of submission.
    pad_set is turned into a long pad_script."""

    def __init__(self, client: HarnessClient):
        self._c = client
        self.events: List[Dict[str, Any]] = []

    def __getattr__(self, name: str) -> Any:
        return getattr(self._c, name)

    def pad_set(self, port: int, pad: Any = None, *, buttons: Sequence[str] = (), main: Sequence[int] = (128, 128),
                c: Sequence[int] = (128, 128), l: int = 0, r: int = 0) -> None:  # noqa: E741
        if pad is not None:
            d = pad.to_json() if isinstance(pad, PadInput) else dict(pad)
        else:
            d = {"buttons": list(buttons), "main": list(main), "c": list(c), "l": l, "r": r}
        self.pad_script(port, [dict(d, hold=LONG_HOLD)])

    def pad_script(self, port: int, frames: Sequence[Any], start: Any = "next") -> ScriptWindow:
        fr = []
        for f in frames:
            d = f.to_json(include_hold=True) if isinstance(f, PadInput) else dict(f)
            fr.append(dict(_norm(d), hold=int(d.get("hold", 1))))
        self._c.pause()
        try:
            # pause lands anywhere inside a field; step to the next VI field boundary, which is
            # where a replay's frame_advance stops too.
            self._c.frame_advance(1)
            st = self._c.status()
            win = self._c.pad_script(port, fr, start)
            self.events.append({"frame": st.frame, "poll": st.input_polls, "port": port,
                                "start": win.starts_at, "frames": fr})
        finally:
            self._c.resume()
        return win


def build_timeline(events: Sequence[Mapping[str, Any]]) -> Dict[str, Dict[str, Any]]:
    """The recorded submissions as one script per port: each submission replaces the port's
    script from its start poll on; when a script runs out the port is neutral (the persistent
    neutral pad_set) until the next one. Leading neutral input is dropped."""
    out: Dict[str, Dict[str, Any]] = {}
    for port in sorted({int(e["port"]) for e in events}):
        evs = sorted((e for e in events if int(e["port"]) == port), key=lambda e: (e["start"], e["frame"]))
        seq: List[Dict[str, Any]] = []

        def push(state: Mapping[str, Any], n: int) -> None:
            if n <= 0:
                return
            st = {k: state[k] for k in NEUTRAL}
            if seq and {k: seq[-1][k] for k in NEUTRAL} == st:
                seq[-1]["hold"] += n
            else:
                seq.append(dict(st, hold=n))

        for i, e in enumerate(evs):
            t = int(e["start"])
            total = sum(int(f["hold"]) for f in e["frames"])
            nxt = int(evs[i + 1]["start"]) if i + 1 < len(evs) else t + min(total, 120)
            for f in e["frames"]:
                if t >= nxt:
                    break
                n = min(int(f["hold"]), nxt - t)
                push(f, n)
                t += n
            push(NEUTRAL, nxt - t)
        start0 = int(evs[0]["start"])
        while seq and {k: seq[0][k] for k in NEUTRAL} == NEUTRAL:
            start0 += seq.pop(0)["hold"]
        if seq:
            out[str(port)] = {"start": start0, "frames": seq}
    return out


def record(out: Path, profile: Profile, frames: int = 1200, stage: str = "battlefield", seed: int = 1) -> Dict:
    inst = make_instance("det-record", profile)
    try:
        inst.launch()
        inst.connect()
        # A persistent neutral pad_set on both ports first: while a newly submitted script waits
        # for its start poll, the server hands a port without pad_set back to the (absent) real
        # controller, which the game sees as unplugged. With pad_set the gap is neutral.
        for port in (0, 1):
            inst.client.pad_set(port)
        rc = RecordingClient(inst.client)
        s1, s2 = F.Seat(rc, 0), F.Seat(rc, 1)  # type: ignore[arg-type]
        # No input during boot (P+ boots straight to the CSS), so the replay can be scheduled
        # after connecting.
        inst.client.wait_state("running", timeout=120)
        B.wait_scene(rc, [B.Scene.CSS], 60 * 90)  # type: ignore[arg-type]
        B.wait_css_ready(rc, [0, 1])  # type: ignore[arg-type]
        F.pick_characters([(s1, "fox"), (s2, "falco")])
        F.start_and_pick_stage(s1, stage)
        B.wait_match_start(rc)  # type: ignore[arg-type]
        go = B.polls(rc)  # type: ignore[arg-type]
        go_frame = inst.client.status().frame
        F.play([(s1, "chase"), (s2, "random")], frames, seed=seed, stage_kind=B.STAGE_KIND[stage])
        tl: Dict[str, Any] = {"events": rc.events, "go_frame": go_frame,
                                  "end_frame": inst.client.status().frame}
        tl.update({"recorded_with": profile.name, "go_poll": go, "stage": stage, "frames": frames,
                   "recorded": time.strftime("%Y-%m-%d %H:%M:%S"),
                   "final": {p.port: {"damage": p.damage, "stocks": p.stocks}
                             for p in B.read_players(inst.client.read_mem)}})
        out.parent.mkdir(parents=True, exist_ok=True)
        out.write_text(json.dumps(tl))
        log.info("recorded %d events, frames %d..%d, GO at frame %d (poll %d)", len(rc.events),
                 rc.events[0]["frame"], tl["end_frame"], go_frame, go)
        inst.cleanup(True)
        return tl
    except BaseException:
        inst.mark_failed()
        inst.cleanup(False)
        raise


# --------------------------------------------------------------------------- comparing


def labelled_ranges(c: HarnessClient) -> Tuple[str, List[Tuple[int, int, str]]]:
    info = B.read_scene(c.read_mem)
    if info.scene is B.Scene.IN_MATCH:
        rs = B.gameplay_ranges(c.read_mem)
    else:
        rs = B.menu_ranges(c.read_mem)
    return info.name, rs


def small_state(c: HarnessClient) -> Dict[str, Any]:
    m = B.Mem(c.read_mem)
    st: Dict[str, Any] = {
        "scene": B.read_scene(c.read_mem).name,
        "rng": B._try(lambda: (m.u32(B.MTRAND_DEFAULT + 4), m.u32(B.MTRAND_OTHER + 4))),
        "frames": B.read_frame_counters(c.read_mem),
        "players": [(p.port, p.character, p.ft_kind, p.damage, p.stocks, p.x, p.y, p.action, p.anim_frame)
                    for p in B.read_players(c.read_mem)],
    }
    return st


def diff_chunks(ca: HarnessClient, cb: HarnessClient, addr: int, n: int, min_chunk: int = 64,
                limit: int = 6) -> List[Tuple[int, int]]:
    """Bisect [addr, addr+n) with hash_mem on two paused instances: the differing chunks."""
    out: List[Tuple[int, int]] = []
    stack = [(addr, n)]
    while stack and len(out) < limit:
        a, ln = stack.pop()
        if ca.hash_mem([[a, ln]]) == cb.hash_mem([[a, ln]]):
            continue
        if ln <= min_chunk:
            out.append((a, ln))
            continue
        half = (ln // 2 + 3) & ~3
        stack.append((a + half, ln - half))
        stack.append((a, half))
    return sorted(out)


def heap_of(addr: int, heaps: Sequence[B.Heap]) -> str:
    for h in heaps:
        if h.start <= addr < h.end:
            return f"{h.name}+{addr - h.start:#x}"
    return "static/bss" if addr < 0x80800000 else "?"


@dataclass
class Checkpoint:
    frame: int
    scene: str
    differing: List[str] = field(default_factory=list)
    small: Dict[str, Any] = field(default_factory=dict)
    chunks: List[Dict[str, Any]] = field(default_factory=list)


def compare(timeline: Mapping[str, Any], profile: Profile, runs: int = 2, every: int = 30,
            match_every: int = 10, localize: int = 3, keep: bool = False, mode: str = "events") -> Dict[str, Any]:
    """mode "events": re-submit every recorded pad script while paused at its VI frame (exact,
    but pauses and frame-advances hundreds of times). mode "script": schedule one script per port
    at boot (``build_timeline``) and pause only at checkpoints; use it with sparse checkpoints for
    dual core, where frequent frame_advance crashed DolphinNoGUI."""
    insts = [make_instance(f"det-{profile.name}-{i}", profile, keep) for i in range(runs)]
    report: Dict[str, Any] = {"profile": profile.name, "note": profile.note, "runs": runs, "mode": mode,
                              "every": every, "match_every": match_every, "checkpoints": [],
                              "first_divergence": None, "first_gameplay_divergence": None}
    ok = False
    try:
        with cf.ThreadPoolExecutor(max_workers=runs) as pool:
            list(pool.map(lambda i: (i.launch(), i.connect()), insts))
            clients = [i.client for i in insts]
            for c in clients:
                c.wait_state("running", timeout=120)
                for port in (0, 1):
                    c.pad_set(port)          # same as record(): gaps between scripts are neutral
            events = sorted(timeline["events"], key=lambda e: e["frame"])
            go, end = int(timeline["go_frame"]), int(timeline["end_frame"])
            first = events[0]["frame"] if events else go
            cps = sorted(set(range(every, go, every)) | set(range(go, end, match_every)))
            # One schedule keyed by VI frame: checkpoints hash first, then inputs are submitted.
            schedule: Dict[int, List[Any]] = {}
            for f in cps:
                schedule.setdefault(f, []).append(("cp", None))
            if mode == "script":
                for c in clients:
                    st = c.status()
                    for port, spec in build_timeline(events).items():
                        if st.input_polls >= spec["start"]:
                            raise RuntimeError(f"already at poll {st.input_polls}, script starts at {spec['start']}")
                        c.pad_script(int(port), spec["frames"], start=int(spec["start"]))
            else:
                for e in events:
                    schedule.setdefault(int(e["frame"]), []).append(("ev", e))
            diverged_local = 0
            localized: set = set()
            t0 = time.monotonic()
            input_errors: List[str] = []
            report["input_errors"] = input_errors

            def submit(e: Mapping[str, Any]) -> None:
                for i, c in enumerate(clients):
                    try:
                        c.pad_script(int(e["port"]), e["frames"], start=int(e["start"]))
                    except HarnessCommandError as err:
                        # This run's timing already differs from the recording's.
                        input_errors.append(f"run {i} frame {e['frame']}: {err}")
                        c.pad_script(int(e["port"]), e["frames"], start="next")
            skipped: List[int] = []
            report["skipped_checkpoints"] = skipped
            report["first_input_frame"] = first
            order = sorted(schedule)

            def maybe_resume(i: int) -> None:
                # Close items are reached by frame_advance without resuming: a pause sent after
                # wait_frame lands late under load and would overshoot the next item.
                if i + 1 >= len(order) or order[i + 1] - order[i] > STAY_PAUSED:
                    for c in clients:
                        with contextlib.suppress(Exception):
                            c.resume()

            # Both must be booting normally before the first pause (a dual-core boot was seen to
            # hang in the launcher); checkpoints start after the frame both have reached.
            boot_deadline = time.monotonic() + 90
            while min(c.status().frame for c in clients) < 120:
                if time.monotonic() > boot_deadline:
                    raise RuntimeError("boot stalled: VI frames " + str([c.status().frame for c in clients]))
                time.sleep(0.2)
            floor = max(c.status().frame for c in clients) + 2
            for f in [f for f in order if f < floor]:
                if any(k == "ev" for k, _ in schedule[f]):
                    raise RuntimeError(f"instances passed input frame {f} before the replay could stop there")
                skipped.append(f)
            order = [f for f in order if f >= floor]
            for idx, target in enumerate(order):
                futs = [pool.submit(pause_at, c, target, FrameKey.frame(), margin=STAY_PAUSED + 1, timeout=60,
                                    max_advance=STAY_PAUSED + 200) for c in clients]
                errs = []
                for f in futs:
                    try:
                        f.result()
                    except Exception as e:  # noqa: BLE001
                        errs.append(e)
                if errs:
                    if all(isinstance(e, OvershotError) for e in errs) and not any(k == "ev" for k, _ in schedule[target]):
                        skipped.append(target)
                        for c in clients:
                            with contextlib.suppress(Exception):
                                if c.status().state == "paused":
                                    c.resume()
                        continue
                    raise RuntimeError(f"could not stop both runs at frame {target}: {errs[0]!r}")
                items = schedule[target]
                if not any(k == "cp" for k, _ in items):
                    for _, e in items:
                        submit(e)
                    maybe_resume(idx)
                    continue
                try:
                    scene, rs = labelled_ranges(clients[0])
                    rs += [(0x80000000, 0x1800000, "MEM1 (all)"), (0x90000000, 0x4000000, "MEM2 (all)")]
                    hashes = [{lbl: c.hash_mem([[a, n]]) for a, n, lbl in rs} for c in clients]
                    smalls = [small_state(c) for c in clients]
                    report["last_checkpoint"] = {"frame": target, "players": [sm["players"] for sm in smalls],
                                                 "rng": [sm["rng"] for sm in smalls]}
                    cp = Checkpoint(target, scene)
                    cp.small["poll"] = [c.status().input_polls for c in clients]
                    if len(set(cp.small["poll"])) == 1:
                        del cp.small["poll"]
                    cp.differing = [lbl for _, _, lbl in rs if len({h[lbl] for h in hashes}) > 1]
                    for k in smalls[0]:
                        vals = [s[k] for s in smalls]
                        if any(v != vals[0] for v in vals[1:]):
                            cp.small[k] = vals
                    # Localize the first few divergences, and every region the first time it differs.
                    new_regions = [d for d in cp.differing if not d.startswith("MEM") and d not in localized]
                    if (cp.differing or cp.small) and (diverged_local < localize or new_regions):
                        diverged_local += 1
                        localized.update(new_regions)
                        heaps = B.read_heap_table(clients[0].read_mem)
                        for a, n, lbl in rs:
                            if lbl not in cp.differing or lbl.startswith(("MEM1", "MEM2")):
                                continue
                            for ca, cn in diff_chunks(clients[0], clients[1], a, n, limit=10):
                                cp.chunks.append({"region": lbl, "addr": f"{ca:#010x}", "where": heap_of(ca, heaps),
                                                  "a": clients[0].read_mem(ca, cn).hex(),
                                                  "b": clients[1].read_mem(ca, cn).hex()})
                        if not cp.chunks and any(x.startswith("MEM") for x in cp.differing):
                            for base, ln in ((0x80000000, 0x1800000), (0x90000000, 0x4000000)):
                                for ca, cn in diff_chunks(clients[0], clients[1], base, ln, limit=4):
                                    cp.chunks.append({"region": "raw", "addr": f"{ca:#010x}",
                                                      "where": heap_of(ca, heaps),
                                                      "a": clients[0].read_mem(ca, cn).hex(),
                                                      "b": clients[1].read_mem(ca, cn).hex()})
                finally:
                    for _, e in items:
                        if e is not None:
                            submit(e)
                    maybe_resume(idx)
                gameplay = [d for d in cp.differing if not d.startswith("MEM")] or bool(cp.small)
                report["checkpoints"].append(cp.__dict__)
                if (cp.differing or cp.small) and report["first_divergence"] is None:
                    report["first_divergence"] = {"frame": target, "scene": scene, "regions": cp.differing,
                                                  "small": list(cp.small)}
                if gameplay and report["first_gameplay_divergence"] is None:
                    report["first_gameplay_divergence"] = {"frame": target, "scene": scene,
                                                           "regions": [d for d in cp.differing if not d.startswith("MEM")],
                                                           "small": cp.small}
                log.info("%s frame %d %s: %s %s", profile.name, target, scene, cp.differing[:6], list(cp.small))
            report["wall_s"] = round(time.monotonic() - t0, 1)
            report["recorded_final"] = timeline.get("final")
            report["status"] = [c.status().raw for c in clients]
        ok = True
        return report
    except Exception as e:  # noqa: BLE001 - keep what was measured before the failure
        report["error"] = f"{type(e).__name__}: {e}"
        report["died"] = [{"name": i.name, "exit_code": i.process.poll() if i.process else None,
                           "log_tail": i.log.tail(8)} for i in insts]
        return report
    finally:
        for i in insts:
            with contextlib.suppress(Exception):
                i.cleanup(ok)


def summarize(rep: Mapping[str, Any]) -> str:
    fd, fg = rep.get("first_divergence"), rep.get("first_gameplay_divergence")
    cps = rep.get("checkpoints", [])
    n_diff = sum(1 for c in cps if c["differing"] or c["small"])
    lines = [f"{rep['profile']}: {rep.get('note', '')}" + (f"  ERROR: {rep['error']}" if rep.get("error") else ""),
             f"  input submission errors: {len(rep.get('input_errors', []))}",
             f"  checkpoints {len(cps)}, with any difference {n_diff}",
             f"  first difference: {fd}",
             f"  first gameplay difference: {fg if fg is None else {k: fg[k] for k in ('frame', 'scene', 'regions')}}"]
    if fg and fg.get("small"):
        lines.append(f"    small-state diffs: {json.dumps(fg['small'], default=str)[:600]}")
    for c in cps:
        if c["chunks"]:
            for ch in c["chunks"][:8]:
                lines.append(f"    @{c['frame']} {ch['region']} {ch['addr']} ({ch['where']}): {ch['a'][:48]} | {ch['b'][:48]}")
            break
    last = rep.get("last_checkpoint")
    if last:
        pl = last["players"]
        lines.append(f"  last checkpoint (frame {last['frame']}): players equal across runs: "
                     f"{all(x == pl[0] for x in pl[1:])}; run 0 (port, char, ftKind, damage, stocks, ...): {pl[0]}")
        lines.append(f"  recording's final damage/stocks: {rep.get('recorded_final')}")
    return "\n".join(lines)


def main(argv: Optional[Sequence[str]] = None) -> int:
    ap = argparse.ArgumentParser(description="P+ offline determinism measurements")
    sub = ap.add_subparsers(dest="cmd", required=True)
    p = sub.add_parser("record")
    p.add_argument("--out", type=Path, required=True)
    p.add_argument("--profile", default="sc-rtc", choices=sorted(PROFILES))
    p.add_argument("--frames", type=int, default=1200)
    p.add_argument("--stage", default="battlefield")
    for name in ("compare", "matrix"):
        p = sub.add_parser(name)
        p.add_argument("--timeline", type=Path, required=True)
        p.add_argument("--runs", type=int, default=2)
        p.add_argument("--every", type=int, default=30)
        p.add_argument("--match-every", type=int, default=10)
        p.add_argument("--out", type=Path, default=None)
        p.add_argument("--keep", action="store_true")
        p.add_argument("--mode", default="events", choices=("events", "script"))
        if name == "compare":
            p.add_argument("--profile", default="sc-rtc", choices=sorted(PROFILES))
        else:
            p.add_argument("--profiles", default="sc-hostrtc,sc-rtc,dc-rtc,dc-rtc-gpuauto,dc-hostrtc")
    args = ap.parse_args(argv)
    logging.basicConfig(level=logging.INFO, format="%(asctime)s %(name)s %(message)s")
    if args.cmd == "record":
        tl = record(args.out, PROFILES[args.profile], args.frames, args.stage)
        print(json.dumps({k: v for k, v in tl.items() if k != "ports"}, indent=2, default=str))
        return 0
    timeline = json.loads(args.timeline.read_text())
    names = [args.profile] if args.cmd == "compare" else [x for x in args.profiles.split(",") if x]
    reps = []
    for n in names:
        try:
            rep = compare(timeline, PROFILES[n], args.runs, args.every, args.match_every, keep=args.keep,
                          mode=args.mode)
        except Exception as e:  # noqa: BLE001 - keep going through the matrix
            rep = {"profile": n, "note": PROFILES[n].note, "error": f"{type(e).__name__}: {e}"}
        reps.append(rep)
        print(summarize(rep) if "checkpoints" in rep else f"{n}: ERROR {rep['error']}", flush=True)
        if args.out:
            args.out.parent.mkdir(parents=True, exist_ok=True)
            args.out.write_text(json.dumps(reps, indent=1, default=str))
    return 0


if __name__ == "__main__":
    sys.exit(main())
