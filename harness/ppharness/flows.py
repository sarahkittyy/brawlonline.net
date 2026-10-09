"""Higher-level game flows built on ``brawl`` recipes: get N instances into a match, play it with
scripted or seeded pseudo-random inputs, and collect artifacts.

Every flow drives a :class:`Seat`: one instance (``client``) and the player it controls. A seat
injects on ``pad_port`` (the *local* controller port the harness overrides) and reads game memory
for ``port`` (the *in-game* port). Offline both are the same; in netplay the joiner's local port 0
is in-game port 1 (``netplay_status().raw["local_port_to_ingame_port"]``).

Inputs never include Start (pause; with the P+ Code Menu's Debug Mode on it toggles a frame
advance freeze), the D-pad, or L+R together (L+R+D-pad Down opens the Code Menu).
"""

from __future__ import annotations

import concurrent.futures as cf
import contextlib
import logging
import math
import random
import time
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any, Callable, Dict, List, Mapping, Optional, Sequence

from . import brawl as B
from .client import HarnessClient, HarnessError

log = logging.getLogger("ppharness.flows")


@dataclass
class Seat:
    client: HarnessClient
    port: int                    # in-game port (panel / fighter whose memory is read)
    pad_port: int | None = None  # local port injected (default: same as port)
    name: str = ""

    @property
    def pp(self) -> int:
        return self.port if self.pad_port is None else self.pad_port

    def __post_init__(self) -> None:
        if not self.name:
            self.name = f"P{self.port + 1}"


def netplay_seat(client: HarnessClient, name: str = "", local_port: int = 0) -> Seat:
    """The seat a netplay instance drives with its local ``local_port``."""
    raw = client.netplay_status().raw
    mapping = raw.get("local_port_to_ingame_port") or {}
    ingame = None
    if isinstance(mapping, Mapping):
        ingame = mapping.get(str(local_port), mapping.get(local_port))  # type: ignore[call-overload]
    elif isinstance(mapping, Sequence) and local_port < len(mapping):
        ingame = mapping[local_port]
    if ingame is None and not mapping and local_port == 0:
        # Servers without the mapping: the default pad map gives the host P1, the joiner P2.
        ingame = 0 if raw.get("role") == "host" else 1
    if ingame is None or int(ingame) < 0:
        raise HarnessError(f"{name or client}: local port {local_port} drives no in-game port ({mapping!r})")
    return Seat(client, int(ingame), local_port, name)


# --------------------------------------------------------------------------- artifacts


class Artifacts:
    """Screenshots and notes for one test/run. ``shoot`` never raises."""

    def __init__(self, root: Path | str):
        self.root = Path(root)
        self.files: List[Path] = []
        self.notes: List[str] = []

    def shoot(self, client: HarnessClient, name: str, timeout: float = 15.0) -> Optional[Path]:
        p = self.root / f"{len(self.files):02d}-{name}.png"
        try:
            self.root.mkdir(parents=True, exist_ok=True)
            st = client.status()
            if st.state == "paused":
                self.note(f"{name}: not shot (paused)")
                return None
            client.screenshot(p, timeout=timeout)
            self.files.append(p)
            return p
        except Exception as e:  # noqa: BLE001 - artifacts are best effort
            self.note(f"{name}: screenshot failed: {e}")
            return None

    def note(self, text: str) -> None:
        self.notes.append(text)
        log.info("%s", text)

    def write_notes(self, name: str = "notes.txt") -> Optional[Path]:
        if not self.notes:
            return None
        self.root.mkdir(parents=True, exist_ok=True)
        p = self.root / name
        p.write_text("\n".join(self.notes) + "\n", encoding="utf-8")
        return p


def run_parallel(fns: Sequence[Callable[[], Any]], timeout: float | None = None) -> List[Any]:
    """Run callables in threads; re-raise the first exception after all have finished."""
    with cf.ThreadPoolExecutor(max_workers=max(1, len(fns))) as pool:
        futs = [pool.submit(f) for f in fns]
        done, _ = cf.wait(futs, timeout=timeout)
        out, err = [], None
        for f in futs:
            if f not in done:
                err = err or TimeoutError("parallel step timed out")
                out.append(None)
                continue
            try:
                out.append(f.result())
            except BaseException as e:  # noqa: BLE001
                err = err or e
                out.append(None)
        if err is not None:
            raise err
        return out


# --------------------------------------------------------------------------- into a match


def to_css(seats: Sequence[Seat], timeout_frames: int = 60 * 90) -> None:
    """Every instance (once per client) from boot or results to a ready CSS."""
    by_client: Dict[int, List[Seat]] = {}
    for s in seats:
        by_client.setdefault(id(s.client), []).append(s)

    def one(group: List[Seat]) -> None:
        c = group[0].client
        c.wait_state("running", timeout=120)
        info = B.read_scene(c.read_mem)
        if info.scene is B.Scene.RESULTS:
            B.results_to_css(c, [s.pp for s in group], timeout_frames)
        elif info.scene is not B.Scene.CSS:
            B.boot_to_css(c, group[0].pp, timeout_frames)
        B.wait_css_ready(c, [s.port for s in group])

    run_parallel([lambda g=g: one(g) for g in by_client.values()])


def pick_characters(picks: Mapping[int, tuple[Seat, str]] | Sequence[tuple[Seat, str]]) -> None:
    """Each seat picks its character. Seats on different instances pick concurrently; seats on the
    same instance pick one after the other."""
    items = list(picks.values()) if isinstance(picks, Mapping) else list(picks)
    by_client: Dict[int, List[tuple[Seat, str]]] = {}
    for seat, who in items:
        by_client.setdefault(id(seat.client), []).append((seat, who))

    def one(group: List[tuple[Seat, str]]) -> None:
        for seat, who in group:
            B.css_pick_character(seat.client, seat.port, B.CSS_ID[who], pad_port=seat.pp)

    run_parallel([lambda g=g: one(g) for g in by_client.values()])


def start_and_pick_stage(host: Seat, stage: str) -> None:
    """Only the host presses Start and picks the stage (the SSS cursor obeys any port)."""
    B.css_start(host.client, host.pp)
    B.sss_pick_stage(host.client, B.STAGE_KIND[stage], host.pp)


def wait_match_started(clients: Sequence[HarnessClient]) -> List[B.MatchState]:
    return run_parallel([lambda c=c: B.wait_match_start(c) for c in clients])


def expected_char_kinds(picks: Sequence[tuple[Seat, str]]) -> Dict[int, int]:
    return {seat.port: B.CSS_TO_CHAR_KIND[B.CSS_ID[who]] for seat, who in picks}


# --------------------------------------------------------------------------- playing


@dataclass
class FightLog:
    seat: str
    frames: int = 0
    actions: int = 0
    samples: List[Dict[str, Any]] = field(default_factory=list)  # {poll, damage{port: %}, stocks}

    def damage_series(self, port: int) -> List[float]:
        return [s["damage"].get(port) for s in self.samples if s["damage"].get(port) is not None]


# Stage half-widths (main platform edge) for the recovery logic.
STAGE_EDGE = {B.STAGE_KIND["battlefield"]: 68.0, B.STAGE_KIND["final_destination"]: 85.0}


def _macro(rng: random.Random, toward: int) -> List[Dict[str, Any]]:
    """One plausible action as a pad_script (no Start, no D-pad, never L and R together)."""
    t = 255 if toward > 0 else 1
    away = 1 if toward > 0 else 255
    r = rng.random()
    if r < 0.18:
        return [{"main": [t, 128], "hold": rng.randint(6, 24)}]                         # walk/dash toward
    if r < 0.26:
        return [{"main": [away, 128], "hold": rng.randint(4, 14)}]                      # back off
    if r < 0.36:
        return [{"buttons": ["X"], "hold": 2}, {"hold": rng.randint(6, 20)}]            # jump
    if r < 0.52:
        return [{"buttons": ["A"], "hold": 2}, {"hold": rng.randint(4, 10)}]            # jab
    if r < 0.62:
        return [{"main": [t, 128], "buttons": ["A"], "hold": 2}, {"hold": 14}]          # forward tilt / dash attack
    if r < 0.70:
        return [{"c": [t, 128], "hold": 3}, {"hold": 24}]                              # smash (c-stick)
    if r < 0.78:
        return [{"buttons": ["X"], "hold": 2}, {"hold": 4}, {"main": [t, 128], "buttons": ["A"], "hold": 2},
                {"hold": 18}]                                                           # short hop aerial
    if r < 0.84:
        return [{"buttons": ["B"], "hold": 2}, {"hold": 20}]                            # neutral special
    if r < 0.90:
        return [{"r": 255, "buttons": ["R"], "hold": rng.randint(4, 12)}, {"hold": 2}]  # shield
    if r < 0.95:
        return [{"main": [128, 1], "hold": rng.randint(4, 10)}]                         # crouch
    return [{"hold": rng.randint(4, 16)}]                                              # idle


class Fighter:
    """Decides one seat's next input macro from the match state (see ``fight``)."""

    def __init__(self, seat: Seat, mode: str = "chase", seed: Any = 0, stage_kind: int | None = None):
        if mode not in ("chase", "random", "idle", "selfdestruct"):
            raise ValueError(f"unknown fight mode {mode!r}")
        self.seat, self.mode = seat, mode
        self.rng = random.Random(f"{seed}:{seat.port}")
        self.edge = STAGE_EDGE.get(stage_kind if stage_kind is not None else -1, 65.0)
        self.busy_until = -1
        self.log = FightLog(seat.name)

    def decide(self, st: B.MatchState) -> List[Dict[str, Any]]:
        me = next((p for p in st.players if p.port == self.seat.port), None)
        if me is None or me.x is None or me.y is None:
            return [{"hold": 2}]
        opps = [p for p in st.players if p.port != self.seat.port and p.x is not None]
        opp = min(opps, key=lambda p: abs((p.x or 0) - me.x)) if opps else None
        dx = (opp.x - me.x) if opp is not None and opp.x is not None else -me.x
        dy = (opp.y - me.y) if opp is not None and opp.y is not None else 0.0
        toward = 1 if dx > 0 else -1
        t = 255 if toward > 0 else 1
        if self.mode == "selfdestruct":                            # walk off the nearer edge
            return [{"main": [255 if me.x >= 0 else 1, 128], "hold": 10}]
        if abs(me.x) > self.edge - 2 or me.y < -8:                # off stage: recover
            to_c = 255 if me.x < 0 else 1
            if me.y < -8:
                return [{"main": [to_c, 128], "buttons": ["X"], "hold": 2}, {"main": [to_c, 128], "hold": 10},
                        {"main": [to_c, 255], "buttons": ["B"], "hold": 2}, {"main": [to_c, 160], "hold": 30}]
            return [{"main": [to_c, 128], "hold": 8}]
        if self.mode == "idle":
            return [{"hold": 10}]
        if self.mode == "chase":
            if abs(dx) < 14 and abs(dy) < 10:
                return [{"main": [t, 128], "hold": 1}, {"buttons": ["A"], "hold": 2}, {"hold": 6}]
            if dy > 15 and abs(dx) < 30:
                return [{"buttons": ["X"], "hold": 2}, {"main": [t, 128], "hold": 12}]
            if dy < -15 and abs(dx) < 30:
                return [{"main": [128, 1], "hold": 4}, {"hold": 10}]   # drop through a platform
            return [{"main": [t, 128], "hold": 4}]
        return _macro(self.rng, toward)


def fight(seats: Seat | Sequence[Seat], frames: int, *, mode: str | Sequence[str] = "chase", seed: Any = 0,
          stage_kind: int | None = None, sample_every: int = 30,
          stop: Callable[[], bool] | None = None) -> List[FightLog]:
    """Drive ``seats`` (all on the same instance) for ``frames`` game frames of a running match.

    mode (one, or one per seat): "chase" walks to the nearest opponent and jabs/tilts in range
    (reliably deals damage); "random" plays seeded random macros (``_macro``) biased toward the
    opponent; "idle" stands still; "selfdestruct" walks off the nearer edge (to lose on purpose).
    All modes but "selfdestruct" recover when off stage. Never presses Start or the
    D-pad. For netplay, run one ``fight`` per instance in its own thread (``play``).
    """
    seats = [seats] if isinstance(seats, Seat) else list(seats)
    if not seats:
        return []
    c = seats[0].client
    if any(s.client is not c for s in seats):
        raise ValueError("fight() drives seats of one instance; use play() for several")
    modes = [mode] * len(seats) if isinstance(mode, str) else list(mode)
    fighters = [Fighter(s, m, seed, stage_kind) for s, m in zip(seats, modes)]
    start = B.polls(c)
    last_sample = -10 ** 9
    for f in fighters:
        B.neutral(c, f.seat.pp)
    while True:
        now = B.polls(c)
        if now - start >= frames or (stop is not None and stop()):
            break
        st = B.read_match_state(c.read_mem)
        if not st.in_match or st.game_set:
            break
        if now - last_sample >= sample_every:
            last_sample = now
            sample = {"poll": now, "damage": {p.port: p.damage for p in st.players},
                      "stocks": {p.port: p.stocks for p in st.players}}
            for f in fighters:
                f.log.samples.append(sample)
        for f in fighters:
            if f.busy_until <= now:
                seq = f.decide(st)
                f.busy_until = c.pad_script(f.seat.pp, seq).ends_at
                f.log.actions += 1
        wake = min(f.busy_until for f in fighters)
        try:
            c.wait_frame(input_polls=max(wake, now + 1), timeout_ms=30000)
        except HarnessError:
            break
    for f in fighters:
        with contextlib.suppress(HarnessError):
            B.neutral(c, f.seat.pp)
        f.log.frames = B.polls(c) - start
    return [f.log for f in fighters]


def play(plan: Sequence[tuple[Seat, str]], frames: int, *, seed: Any = 0, stage_kind: int | None = None,
         stop: Callable[[], bool] | None = None) -> List[FightLog]:
    """``fight`` for every (seat, mode), one thread per instance. Returns logs in plan order."""
    groups: Dict[int, List[int]] = {}
    for i, (seat, _) in enumerate(plan):
        groups.setdefault(id(seat.client), []).append(i)
    out: List[Any] = [None] * len(plan)

    def run(idx: List[int]) -> None:
        logs = fight([plan[i][0] for i in idx], frames, mode=[plan[i][1] for i in idx], seed=seed,
                     stage_kind=stage_kind, stop=stop)
        for i, lg in zip(idx, logs):
            out[i] = lg

    run_parallel([lambda g=g: run(g) for g in groups.values()])
    return out


def menu_wander(seat: Seat, frames: int, seed: Any = 0, stop: Callable[[], bool] | None = None) -> int:
    """Seeded random CSS hand movement (stick only, plus A only over the character grid). Returns
    the number of actions. Used to load the rollback code with menu traffic."""
    c, pp = seat.client, seat.pp
    rng = random.Random(f"menu:{seed}:{seat.port}")
    start, n = B.polls(c), 0
    while B.polls(c) - start < frames and not (stop is not None and stop()):
        a = B.read_css_area(c.read_mem, seat.port)
        if a is None:
            B.step(c, 4)
            continue
        if a.hand_target in (B.CSS_HAND_GRID, B.CSS_HAND_GRID_HOLDING) and rng.random() < 0.15:
            seq = [{"buttons": ["A"], "hold": 2}, {"hold": 6}]
        else:
            ang = rng.uniform(0, 2 * math.pi)
            # Keep the hand away from the BACK button (top left) by biasing toward the grid centre.
            if math.isfinite(a.hand_x) and (a.hand_y > 14 or a.hand_y < -10 or abs(a.hand_x) > 25):
                ang = math.atan2(4 - a.hand_y, 0 - a.hand_x)
            seq = [{"main": [int(128 + 100 * math.cos(ang)), int(128 + 100 * math.sin(ang))],
                    "hold": rng.randint(4, 20)}]
        end = c.pad_script(pp, seq).ends_at
        n += 1
        try:
            c.wait_frame(input_polls=end, timeout_ms=30000)
        except HarnessError:
            break
    with contextlib.suppress(HarnessError):
        B.neutral(c, pp)
    return n


# --------------------------------------------------------------------------- diagnostics


def diagnose(instances: Sequence[Any], art: Artifacts | None = None, label: str = "fail",
             log_lines: int = 40) -> Dict[str, Any]:
    """Status, netplay_status, scene, rollback counters and dolphin.log tail of each instance
    (``DolphinInstance``), plus a screenshot each when ``art`` is given. Never raises."""
    out: Dict[str, Any] = {}
    for inst in instances:
        d: Dict[str, Any] = {}
        c = getattr(inst, "client", None)
        for key, fn in (("status", lambda: c.status().raw),
                        ("netplay_status", lambda: c.netplay_status().raw),
                        ("scene", lambda: B.read_scene(c.read_mem).name),
                        ("frames", lambda: B.read_frame_counters(c.read_mem))):
            try:
                d[key] = fn() if c is not None else None
            except Exception as e:  # noqa: BLE001
                d[key] = f"error: {e}"
        with contextlib.suppress(Exception):
            d["log_tail"] = inst.log.tail(log_lines)
        with contextlib.suppress(Exception):
            # The interesting lines are often far back behind ENet chatter.
            keep = ("Brawlback", "OSREPORT", "HARNESS", " E[", " W[", "-> sc", "-> sq")
            lines = [ln for ln in inst.log.lines() if any(k in ln for k in keep)
                     and "dirty granules" not in ln]
            d["log_key_lines"] = "\n".join(lines[-log_lines:])
        if art is not None and c is not None:
            p = art.shoot(c, f"{inst.name}-{label}", timeout=10.0)
            d["screenshot"] = str(p) if p else None
        out[inst.name] = d
    if art is not None:
        with contextlib.suppress(Exception):
            import json
            art.root.mkdir(parents=True, exist_ok=True)
            (art.root / f"diagnose-{label}.json").write_text(json.dumps(out, indent=2, default=str))
    return out
