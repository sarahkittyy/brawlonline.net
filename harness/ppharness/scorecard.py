"""Rollback scorecard: N two-instance rollback netplay sessions per netsim preset, driven by
seeded pseudo-random but plausible inputs, with per-side health numbers.

    python -m ppharness scorecard --presets lan,typical,bad_wifi --sessions 1 --duration 60 \\
        --json run/scorecard.json

Per side (host / joiner) it reports:

* Brawlback/GekkoNet counters at the end: rollbacks, max rollback depth, frames resimulated,
  desyncs detected (GekkoNet) and Dolphin's own desync reports;
* frames ahead over time (sampled about 4x per second): series, mean, min, max;
* effective speed: VI fields/s, game frames/s (input_polls) and GekkoNet frames/s;
* freezes: a side is *frozen* when its GekkoNet frame (or, before the session starts, its
  input_polls) stops moving for more than ``--freeze-s`` (2 s) and never moves again before the
  run ends; the freeze's start time, frames, scene and how far the peer got are kept. Shorter
  or recovered stalls longer than ``--freeze-s`` are listed separately as ``stalls``;
* the scene reached, and where the setup failed (phase "match" / "auto").

At the end of each session it compares state between the peers with ``compare_state`` keyed on
the GekkoNet frame, after a quiet window of neutral input on both sides. Caveat (protocol doc,
"Proposed changes" 4): the peer that is ahead may hold speculative state for that frame, so a
mismatch is not proof of a desync. Until the server offers confirmed-frame hashes this is the best
available, and the report says so. It is skipped if either side froze.

Everything is bounded by timeouts: no command can block the run for longer than the client
timeout, a frozen side is recorded and the run goes on, and both Dolphins are always shut down
(quit, then kill) when a session ends. Output: JSON plus a short text summary.
"""

from __future__ import annotations

import argparse
import contextlib
import json
import logging
import math
import random
import statistics
import sys
import threading
import time
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any, Dict, List, Optional, Sequence

from . import brawl as B
from . import flows as F
from .client import HarnessClient, HarnessError
from .instance import InstanceConfig
from .session import FrameKey, NetplaySession, compare_state, two_player_netplay

log = logging.getLogger("ppharness.scorecard")

COUNTERS = ("rollbacks", "max_rollback_frames", "frames_resimulated", "desyncs_detected",
            "netplay_desync_reports", "last_desync_frame")


@dataclass
class SideStats:
    name: str
    samples: List[Dict[str, Any]] = field(default_factory=list)  # t, vi, polls, gekko, ahead, scene
    counters: Dict[str, Any] = field(default_factory=dict)
    froze: bool = False
    freeze: Optional[Dict[str, Any]] = None
    longest_stall_s: float = 0.0
    stalls: List[Dict[str, Any]] = field(default_factory=list)   # > freeze_s, then recovered
    errors: List[str] = field(default_factory=list)
    actions: int = 0
    scene_end: str = ""

    def summary(self) -> Dict[str, Any]:
        out: Dict[str, Any] = {"name": self.name, **{k: self.counters.get(k) for k in COUNTERS}}
        ahead = [s["ahead"] for s in self.samples if isinstance(s.get("ahead"), (int, float))]
        if ahead:
            out["frames_ahead"] = {"mean": round(statistics.fmean(ahead), 2), "min": round(min(ahead), 2),
                                   "max": round(max(ahead), 2), "n": len(ahead)}
        good = [s for s in self.samples if s.get("vi") is not None]
        if len(good) >= 2:
            a, b = good[0], good[-1]
            dt = max(1e-6, b["t"] - a["t"])
            out["fps"] = {"vi": round((b["vi"] - a["vi"]) / dt, 1),
                          "game": round((b["polls"] - a["polls"]) / dt, 1)}
            g = [s for s in good if s.get("gekko") is not None]
            if len(g) >= 2:
                out["fps"]["gekko"] = round((g[-1]["gekko"] - g[0]["gekko"]) / max(1e-6, g[-1]["t"] - g[0]["t"]), 1)
        out.update({"froze": self.froze, "freeze": self.freeze, "stalls": self.stalls,
                    "longest_stall_s": round(self.longest_stall_s, 1),
                    "scene_end": self.scene_end, "actions": self.actions, "errors": self.errors[:10]})
        return out

    def ahead_series(self, step_s: float = 1.0) -> List[List[float]]:
        """frames_ahead downsampled to one point per ``step_s`` (t relative to the first sample)."""
        if not self.samples:
            return []
        t0 = self.samples[0]["t"]
        out: List[List[float]] = []
        nxt = 0.0
        for s in self.samples:
            if s["t"] - t0 >= nxt and isinstance(s.get("ahead"), (int, float)):
                out.append([round(s["t"] - t0, 1), round(float(s["ahead"]), 2)])
                nxt += step_s
        return out


class Side:
    """One instance: samples its health and drives its own port without ever blocking long."""

    def __init__(self, name: str, client: HarnessClient, seat: F.Seat, rng: random.Random,
                 freeze_s: float, t0: float):
        self.name, self.c, self.seat, self.rng, self.t0 = name, client, seat, rng, t0
        self.stats = SideStats(name)
        self.freeze_s = freeze_s
        self.busy_until = -1
        self._last_progress = (None, time.monotonic())
        self._open_stall: Optional[Dict[str, Any]] = None
        self.lock = threading.Lock()

    def sample(self, phase: str) -> Dict[str, Any]:
        s: Dict[str, Any] = {"t": time.monotonic(), "phase": phase}
        try:
            st = self.c.status()
            s.update(vi=st.frame, polls=st.input_polls)
            rb = self.c.netplay_status().rollback or {}
            s.update(gekko=rb.get("current_frame"), ahead=rb.get("frames_ahead"))
            self.stats.counters = {k: rb.get(k) for k in COUNTERS}
        except HarnessError as e:
            s["error"] = str(e)
            if len(self.stats.errors) < 50:
                self.stats.errors.append(f"{phase}: sample: {e}")
        self.stats.samples.append(s)
        # Progress: the GekkoNet frame once the session runs, else the game's pad reads.
        key = s.get("gekko") if s.get("gekko") else s.get("polls")
        last_key, since = self._last_progress
        now = s["t"]
        if key is not None and key != last_key:
            if self._open_stall is not None:     # it moved again: a stall, not a freeze
                self._open_stall["duration_s"] = round(now - since, 1)
                self._open_stall["recovered"] = True
                self.stats.stalls.append(self._open_stall)
                self._open_stall = None
            self._last_progress = (key, now)
        else:
            stall = now - since
            self.stats.longest_stall_s = max(self.stats.longest_stall_s, stall)
            if stall > self.freeze_s and self._open_stall is None and key is not None:
                self._open_stall = {"after_s": round(since - self.t0, 1), "phase": phase,
                                    "gekko": s.get("gekko"), "polls": s.get("polls"), "vi": s.get("vi"),
                                    "scene": self.scene()}
        return s

    def finish(self) -> None:
        """A stall still open at the end of the run is a freeze."""
        if self._open_stall is not None:
            self._open_stall["duration_s"] = round(time.monotonic() - self._last_progress[1], 1)
            self._open_stall["recovered"] = False
            self.stats.froze = True
            self.stats.freeze = self._open_stall

    def frozen_now(self) -> bool:
        return time.monotonic() - self._last_progress[1] > self.freeze_s

    def scene(self) -> str:
        with contextlib.suppress(Exception):
            return B.read_scene(self.c.read_mem).name
        return "?"

    def drive(self) -> None:
        """Submit the next input macro if the previous one has finished. Non-blocking."""
        try:
            polls = self.c.status().input_polls
            if polls < self.busy_until:
                return
            info = B.read_scene(self.c.read_mem)
            if info.scene is B.Scene.IN_MATCH:
                st = B.read_match_state(self.c.read_mem)
                me = next((p for p in st.players if p.port == self.seat.port), None)
                other = next((p for p in st.players if p.port != self.seat.port and p.x is not None), None)
                toward = 1 if (me is None or other is None or me.x is None or other.x > me.x) else -1
                if me is not None and me.x is not None and me.y is not None and (abs(me.x) > 63 or me.y < -8):
                    to_c = 255 if me.x < 0 else 1
                    seq = [{"main": [to_c, 128], "buttons": ["X"], "hold": 2}, {"main": [to_c, 255], "buttons": ["B"],
                                                                             "hold": 2}, {"main": [to_c, 160], "hold": 20}]
                else:
                    seq = F._macro(self.rng, toward)
            elif info.scene is B.Scene.CSS:
                a = B.read_css_area(self.c.read_mem, self.seat.port)
                if a is not None and a.kind != B.CSS_AREA_HUMAN:
                    seq = [{"buttons": ["A"], "hold": 3}, {"hold": 10}]
                elif a is not None and a.hand_target in (B.CSS_HAND_GRID, B.CSS_HAND_GRID_HOLDING) \
                        and self.rng.random() < 0.12:
                    seq = [{"buttons": ["A"], "hold": 2}, {"hold": 6}]
                else:
                    ang = self.rng.uniform(0, 2 * math.pi)
                    if a is not None and math.isfinite(a.hand_x) and (a.hand_y > 13 or a.hand_y < -8 or abs(a.hand_x) > 25):
                        ang = math.atan2(4 - a.hand_y, 0 - a.hand_x)   # stay off BACK and the edges
                    seq = [{"main": [int(128 + 100 * math.cos(ang)), int(128 + 100 * math.sin(ang))],
                            "hold": self.rng.randint(4, 20)}]
            else:
                seq = [{"hold": 10}]
            self.busy_until = self.c.pad_script(self.seat.pp, seq).ends_at
            self.stats.actions += 1
        except HarnessError as e:
            if len(self.stats.errors) < 50:
                self.stats.errors.append(f"drive: {e}")


def _ranges_for_compare(c: HarnessClient) -> List[List[int]]:
    info = B.read_scene(c.read_mem)
    rs = B.gameplay_ranges(c.read_mem) if info.scene is B.Scene.IN_MATCH else B.menu_ranges(c.read_mem)
    return [[a, n] for a, n, _ in rs]


def run_session(preset: str, seed: int, duration: float, phase: str, freeze_s: float = 2.0,
                stage: str = "battlefield", exe: Any = None, keep: bool = False,
                video: str = "Null", cpu_thread: Optional[bool] = False, **session_kw: Any) -> Dict[str, Any]:
    out: Dict[str, Any] = {"preset": preset, "seed": seed, "duration_s": duration, "phase_requested": phase,
                           "started": time.strftime("%Y-%m-%d %H:%M:%S")}
    t_start = time.monotonic()
    sess: Optional[NetplaySession] = None
    try:
        sess = two_player_netplay(preset, rollback=True, seed=seed, exe=exe, keep=keep,
                                  name=f"score-{preset}-{seed}",
                                  config=InstanceConfig(video_backend=video, cpu_thread=cpu_thread),
                                  **session_kw)
    except Exception as e:  # noqa: BLE001
        out["error"] = f"session setup failed: {type(e).__name__}: {e}"
        return out
    ok = False
    try:
        hc, jc = sess.clients
        t0 = time.monotonic()
        sides = [Side("host", hc, F.netplay_seat(hc, "host"), random.Random(f"{seed}:host"), freeze_s, t0),
                 Side("joiner", jc, F.netplay_seat(jc, "joiner"), random.Random(f"{seed}:joiner"), freeze_s, t0)]
        deadline = t0 + duration
        stop = threading.Event()
        state = {"phase": "boot"}

        def sampler(side: Side) -> None:
            while not stop.is_set():
                side.sample(state["phase"])
                stop.wait(0.25)

        threads = [threading.Thread(target=sampler, args=(s,), daemon=True, name=f"sample-{s.name}") for s in sides]
        for t in threads:
            t.start()

        # Optional setup into a match (each side picks its own character, the host the stage).
        if phase in ("match", "auto"):
            state["phase"] = "setup"
            setup: Dict[str, Any] = {"reached": None}
            out["setup"] = setup

            def do_setup() -> None:
                try:
                    host, joiner = sides[0].seat, sides[1].seat
                    setup["step"] = "boot->css"
                    F.to_css([host, joiner], timeout_frames=60 * 60)
                    setup["step"] = "css pick"
                    F.pick_characters([(host, "fox"), (joiner, "falco")])
                    setup["step"] = "css->sss"
                    B.css_start(hc, host.pp)
                    setup["step"] = "sss pick"
                    B.sss_pick_stage(hc, B.STAGE_KIND[stage], host.pp)
                    setup["step"] = "sss->match"
                    F.wait_match_started([hc, jc])
                    setup["reached"] = "match"
                except Exception as e:  # noqa: BLE001
                    setup["error"] = f"{type(e).__name__}: {e}"

            th = threading.Thread(target=do_setup, daemon=True, name="setup")
            th.start()
            while th.is_alive() and time.monotonic() < deadline:
                th.join(0.5)
                if any(s.frozen_now() for s in sides) and time.monotonic() - t0 > 20:
                    setup.setdefault("note", "a side froze during setup")
                    break
            if th.is_alive():
                setup.setdefault("error", f"setup still running at {setup.get('step')} (abandoned)")
            out["setup_s"] = round(time.monotonic() - t0, 1)
            if phase == "match" and setup.get("reached") != "match":
                deadline = time.monotonic()  # nothing more to drive; still sample briefly
        state["phase"] = "play"
        while time.monotonic() < deadline:
            for s in sides:
                if not s.frozen_now():
                    s.drive()
            time.sleep(0.05)
        # Quiet window, then a peer state comparison if both sides are alive.
        state["phase"] = "quiet"
        for s in sides:
            with contextlib.suppress(HarnessError):
                s.c.pad_set(s.seat.pp)
        time.sleep(2.0)
        stop.set()
        for t in threads:
            t.join(15)
        for s in sides:
            s.finish()
            s.stats.scene_end = s.scene()
        cmp: Dict[str, Any]
        if any(s.stats.froze for s in sides):
            cmp = {"skipped": "a side froze"}
        else:
            try:
                rs = _ranges_for_compare(hc)
                res = compare_state(sess.instances, rs, key=FrameKey.netplay(), timeout=30, attempts=3)
                bb = compare_state(sess.instances, B.brawlback_checksum_ranges(hc.read_mem, B.CORRECTED_CHECKSUM_FIELDS)
                                   if B.read_scene(hc.read_mem).scene is B.Scene.IN_MATCH else [[0x901812A0, 0x18]],
                                   key=FrameKey.netplay(), timeout=30, attempts=3)
                cmp = {"match": res.match, "at_gekko_frame": res.at, "regions": len(rs), "hashes": res.hashes,
                       "checksum_fields_match": bb.match,
                       "caveat": "compared at the same GekkoNet frame after 2 s of neutral input; the peer "
                                 "that is ahead may still hold speculative state (protocol doc, proposed "
                                 "change 4), so a mismatch is not proof of a desync"}
            except Exception as e:  # noqa: BLE001
                cmp = {"error": f"{type(e).__name__}: {e}"}
        out["state_compare"] = cmp
        for a_, b_ in ((sides[0], sides[1]), (sides[1], sides[0])):
            if a_.stats.freeze is not None:   # how far the peer got meanwhile
                a_.stats.freeze["peer_gekko_end"] = b_.stats.samples[-1].get("gekko") if b_.stats.samples else None
        out["sides"] = {s.name: s.stats.summary() for s in sides}
        out["frames_ahead_series"] = {s.name: s.stats.ahead_series() for s in sides}
        with contextlib.suppress(Exception):
            out["netsim"] = _netsim_summary(sess.netsim.stats())
        if any(s.stats.froze for s in sides):
            out["diagnose"] = {k: {kk: v.get(kk) for kk in ("scene", "frames", "log_key_lines")}
                               for k, v in F.diagnose(sess.instances, None, log_lines=25).items()}
        ok = True
    except Exception as e:  # noqa: BLE001
        out["error"] = f"{type(e).__name__}: {e}"
    finally:
        try:
            sess.close(success=ok and not keep)
        except Exception as e:  # noqa: BLE001
            out.setdefault("errors", []).append(f"close: {e}")
    out["wall_s"] = round(time.monotonic() - t_start, 1)
    return out


def _netsim_summary(st: Dict[str, Any]) -> Dict[str, Any]:
    out: Dict[str, Any] = {"profile": st.get("profile")}
    for d in ("up", "down"):
        x = st.get(d) or {}
        dm = x.get("delay_ms") or {}
        out[d] = {"packets_in": x.get("packets_in"), "dropped": x.get("dropped"),
                  "loss_rate": x.get("loss_rate"), "delay_p50_ms": dm.get("p50"), "delay_p99_ms": dm.get("p99")}
    return out


def text_summary(report: Dict[str, Any]) -> str:
    lines = [f"rollback scorecard  {report.get('started')}  build={report.get('build')}  "
             f"phase={report.get('phase')}  duration={report.get('duration_s')}s"]
    for r in report["sessions"]:
        head = f"[{r['preset']} seed={r['seed']}]"
        if "error" in r and "sides" not in r:
            lines.append(f"{head} ERROR {r['error']}")
            continue
        setup = r.get("setup")
        if setup:
            lines.append(f"{head} setup: reached={setup.get('reached')} step={setup.get('step')} "
                         f"{('error=' + setup['error'][:120]) if setup.get('error') else ''}")
        for name, s in r.get("sides", {}).items():
            fa = s.get("frames_ahead") or {}
            fps = s.get("fps") or {}
            fz = s.get("freeze")
            lines.append(
                f"{head} {name:6} rollbacks={s.get('rollbacks')} max_depth={s.get('max_rollback_frames')} "
                f"resim={s.get('frames_resimulated')} desyncs={s.get('desyncs_detected')} "
                f"ahead(mean/min/max)={fa.get('mean')}/{fa.get('min')}/{fa.get('max')} "
                f"fps(vi/game/gekko)={fps.get('vi')}/{fps.get('game')}/{fps.get('gekko')} "
                f"stalls>{report.get('freeze_s', 2)}s={len(s.get('stalls') or [])} "
                f"froze={'YES' if s.get('froze') else 'no'}"
                + (f" (after ~{fz.get('after_s')}s at gekko {fz.get('gekko')}, polls {fz.get('polls')}, "
                   f"{fz.get('scene')}, peer reached {fz.get('peer_gekko_end')})" if fz else ""))
        sc = r.get("state_compare") or {}
        lines.append(f"{head} state compare: " + (f"match={sc.get('match')} at gekko {sc.get('at_gekko_frame')}"
                                                 if "match" in sc else str(sc.get("skipped") or sc.get("error"))))
    return "\n".join(lines)


def main(argv: Optional[Sequence[str]] = None) -> int:
    ap = argparse.ArgumentParser(prog="python -m ppharness scorecard", description=__doc__.splitlines()[0])
    ap.add_argument("--presets", default="lan,typical,bad_wifi")
    ap.add_argument("--sessions", type=int, default=1, help="sessions per preset")
    ap.add_argument("--duration", type=float, default=60.0, help="seconds of driving per session")
    ap.add_argument("--phase", default="menus", choices=("menus", "match", "auto"),
                    help="menus: random CSS input; match: set up Fox vs Falco then random play (stops if "
                         "setup fails); auto: try the match, fall back to the menus")
    ap.add_argument("--seed", type=int, default=1)
    ap.add_argument("--freeze-s", type=float, default=2.0)
    ap.add_argument("--video", default="Null")
    ap.add_argument("--cpu-thread", default="off", choices=("on", "off", "template"))
    ap.add_argument("--exe", default=None)
    ap.add_argument("--keep", action="store_true")
    ap.add_argument("--json", type=Path, default=None, help="write the full report here")
    ap.add_argument("-v", "--verbose", action="store_true")
    args = ap.parse_args(argv)
    logging.basicConfig(level=logging.INFO if args.verbose else logging.WARNING,
                        format="%(asctime)s %(name)s %(levelname)s %(message)s")
    from . import paths
    cpu = {"on": True, "off": False, "template": None}[args.cpu_thread]
    report: Dict[str, Any] = {"started": time.strftime("%Y-%m-%d %H:%M:%S"), "phase": args.phase,
                              "duration_s": args.duration, "build": str(paths.dolphin_nogui()),
                              "cpu_thread": cpu, "freeze_s": args.freeze_s, "sessions": []}
    for preset in [p for p in args.presets.split(",") if p]:
        for i in range(args.sessions):
            r = run_session(preset, args.seed + i, args.duration, args.phase, args.freeze_s, exe=args.exe,
                            keep=args.keep, video=args.video, cpu_thread=cpu)
            report["sessions"].append(r)
            print(text_summary({**report, "sessions": [r]}).split("\n", 1)[1], flush=True)
            if args.json:
                args.json.parent.mkdir(parents=True, exist_ok=True)
                args.json.write_text(json.dumps(report, indent=1, default=str))
    report["finished"] = time.strftime("%Y-%m-%d %H:%M:%S")
    if args.json:
        args.json.write_text(json.dumps(report, indent=1, default=str))
    print()
    print(text_summary(report))
    return 0


if __name__ == "__main__":
    sys.exit(main())
