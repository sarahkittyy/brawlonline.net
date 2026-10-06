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


def _press(drv: HarnessClient, pp: int, frames: List[Dict[str, Any]]) -> int:
    win = drv.pad_script(pp, frames)
    return win.ends_at


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


def fight(seat: Seat, frames: int, *, mode: str = "chase", seed: Any = 0, stage_kind: int | None = None,
          sample_every: int = 30, stop: Callable[[], bool] | None = None) -> FightLog:
    """Drive ``seat`` for ``frames`` game frames of a running match.

    mode "chase": walk to the nearest opponent and jab/tilt when in range (reliably deals damage).
    mode "random": seeded random macros (``_macro``) biased toward the opponent.
    mode "idle":   stand still (but still recover to the stage).
    Both recover when off stage (steer to the centre, jump, up-B). Never presses Start or the D-pad.
    """
    c, pp = seat.client, seat.pp
    rng = random.Random(f"{seed}:{seat.port}")
    edge = STAGE_EDGE.get(stage_kind or -1, 65.0)
    lg = FightLog(seat.name)
    start = B.polls(c)
    last_sample = -10 ** 9
    B.neutral(c, pp)
    while True:
        now = B.polls(c)
        if now - start >= frames or (stop is not None and stop()):
            break
        st = B.read_match_state(c.read_mem)
        if not st.in_match or st.game_set:
            break
        if now - last_sample >= sample_every:
            last_sample = now
            lg.samples.append({"poll": now, "damage": {p.port: p.damage for p in st.players},
                               "stocks": {p.port: p.stocks for p in st.players}})
        me = next((p for p in st.players if p.port == seat.port), None)
        opps = [p for p in st.players if p.port != seat.port and p.x is not None]
        if me is None or me.x is None or me.y is None:
            B.step(c, 2)
            continue
        opp = min(opps, key=lambda p: abs((p.x or 0) - me.x)) if opps else None
        dx = (opp.x - me.x) if opp is not None and opp.x is not None else -me.x
        dy = (opp.y - me.y) if opp is not None and opp.y is not None else 0.0
        toward = 1 if dx > 0 else -1
        if abs(me.x) > edge - 2 or me.y < -8:                 # off stage: recover
            to_c = 255 if me.x < 0 else 1
            if me.y < -8:
                seq = [{"main": [to_c, 128], "buttons": ["X"], "hold": 2}, {"main": [to_c, 128], "hold": 10},
                       {"main": [to_c, 255], "buttons": ["B"], "hold": 2}, {"main": [to_c, 160], "hold": 30}]
            else:
                seq = [{"main": [to_c, 128], "hold": 8}]
        elif mode == "idle":
            seq = [{"hold": 10}]
        elif mode == "chase":
            if abs(dx) < 14 and abs(dy) < 10:
                seq = [{"main": [255 if toward > 0 else 1, 128], "hold": 1}, {"buttons": ["A"], "hold": 2},
                       {"hold": 6}]
            elif dy > 15 and abs(dx) < 30:
                seq = [{"buttons": ["X"], "hold": 2}, {"main": [255 if toward > 0 else 1, 128], "hold": 12}]
            elif dy < -15 and abs(dx) < 30:
                seq = [{"main": [128, 1], "hold": 4}, {"hold": 10}]   # drop through a platform
            else:
                seq = [{"main": [255 if toward > 0 else 1, 128], "hold": 4}]
        else:
            seq = _macro(rng, toward)
        end = _press(c, pp, seq)
        lg.actions += 1
        try:
            c.wait_frame(input_polls=end, timeout_ms=30000)
        except HarnessError:
            break
    with contextlib.suppress(HarnessError):
        B.neutral(c, pp)
    lg.frames = B.polls(c) - start
    return lg


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
