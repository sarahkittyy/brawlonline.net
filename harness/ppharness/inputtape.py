"""Controller tapes for input timing tests (harness ``pad_tape``; tools/input_fidelity.py).

A tape is a player's controller as a function of host time: segments, each a whole pad state held
from its start until the next segment's. Starts fall anywhere in time (microseconds of the host's
steady clock, Python's ``time.monotonic_ns() // 1000`` on Linux), not on the game's reads, as a
player's presses do. The generator makes what a player does: taps (some shorter than a frame),
holds, mashing, light and full shield presses, dashes and flicks, short hops into air dodges,
neutral gaps.

Every segment carries an id in the C-stick, a small offset inside its dead zone (|x|, |y| <= 6, so
the game does nothing with it): ``id = index % 169``. The pad slot every frame consumed carries the
id on (game_pads), so the check (ppharness/inputoracle.py) can tell which segment each frame ran with.

No START (pause, Hold to Pause, X+Y+START recalibration, B+X+START reset) and none of the SI's
origin bits.
"""

from __future__ import annotations

import bisect
import random
from dataclasses import dataclass, field
from typing import Iterable, List, Optional, Sequence, Tuple

FIELD_US = 1_000_000 * 1001 / 60000  # 16683.3: one 59.94 Hz field

# GCPadStatus buttons.
LEFT, RIGHT, DOWN, UP = 0x0001, 0x0002, 0x0004, 0x0008
Z, R, L = 0x0010, 0x0020, 0x0040
A, B, X, Y = 0x0100, 0x0200, 0x0400, 0x0800
START = 0x1000
BUTTON_MASK = 0x1F7F  # what a PADStatus keeps (HostDecodePadSlot)

ID_SPAN = 13  # C-stick offsets -6..6 on each axis
ID_MOD = ID_SPAN * ID_SPAN  # 169
LIGHT_PRESS = 45  # HOST_DECODE_LIGHT_PRESS: P+'s light L/R press from this analog value on


def id_to_cstick(i: int) -> Tuple[int, int]:
    i %= ID_MOD
    return 128 + (i % ID_SPAN) - 6, 128 + (i // ID_SPAN) - 6


def cstick_to_id(cx: int, cy: int) -> Optional[int]:
    """The id of a C-stick (raw bytes, centred at 128), None outside the id band."""
    dx, dy = cx - 128 + 6, cy - 128 + 6
    if 0 <= dx < ID_SPAN and 0 <= dy < ID_SPAN:
        return dx + dy * ID_SPAN
    return None


def slot_id(slot: bytes) -> Optional[int]:
    """The id a game pad slot (gfPadStatus, 0x40 bytes) carries: its C-stick at +0x32/+0x33 is the
    raw value less the origin (128 for a harness pad), as a signed byte."""
    sx = slot[0x32] - 256 if slot[0x32] >= 128 else slot[0x32]
    sy = slot[0x33] - 256 if slot[0x33] >= 128 else slot[0x33]
    return cstick_to_id(sx + 128, sy + 128)


@dataclass
class Pad:
    buttons: int = 0
    sx: int = 128
    sy: int = 128
    l: int = 0
    r: int = 0

    def key(self) -> Tuple[int, int, int, int, int]:
        return (self.buttons, self.sx, self.sy, self.l, self.r)


@dataclass
class Segment:
    index: int
    start_us: int
    pad: Pad
    end_us: int = 0  # the next segment's start (set by Tape)

    @property
    def id(self) -> int:
        return self.index % ID_MOD

    @property
    def duration_us(self) -> int:
        return self.end_us - self.start_us

    def row(self) -> List[int]:
        cx, cy = id_to_cstick(self.index)
        p = self.pad
        return [self.start_us, p.buttons, p.sx, p.sy, cx, cy, p.l, p.r]


@dataclass
class Tape:
    segments: List[Segment] = field(default_factory=list)
    _starts: List[int] = field(default_factory=list)

    def add(self, start_us: int, pad: Pad) -> Segment:
        if self.segments:
            if start_us <= self.segments[-1].start_us:
                raise ValueError("segments must start in order")
            self.segments[-1].end_us = start_us
        seg = Segment(len(self.segments), int(start_us), pad, int(start_us))
        self.segments.append(seg)
        self._starts.append(seg.start_us)
        return seg

    def close(self, end_us: int) -> None:
        if self.segments:
            self.segments[-1].end_us = max(int(end_us), self.segments[-1].start_us)

    def at(self, t_us: float) -> Optional[Segment]:
        """The segment in force at host time ``t_us`` (None before the first)."""
        i = bisect.bisect_right(self._starts, t_us) - 1
        return self.segments[i] if i >= 0 else None

    def index_at(self, t_us: float) -> int:
        return bisect.bisect_right(self._starts, t_us) - 1

    def rows(self, since: int = 0) -> List[List[int]]:
        return [s.row() for s in self.segments[since:]]

    def resolve(self, ident: int, near_index: int) -> Optional[int]:
        """The index of the segment with id ``ident`` closest to ``near_index``."""
        base = near_index - ((near_index - ident) % ID_MOD)
        best = None
        for cand in (base, base + ID_MOD):
            if 0 <= cand < len(self.segments):
                if best is None or abs(cand - near_index) < abs(best - near_index):
                    best = cand
        return best


class Player:
    """A seeded, human-like player. ``chunk(t0, t1, bias)`` makes the segments from t0 to t1;
    ``bias`` (-1 left, 0 none, 1 right) leans the stick's walks and dashes back towards the stage's
    middle (the driver reads the fighter's position between chunks)."""

    def __init__(self, seed: object, *, taps_under_a_frame: float = 0.15):
        self.rng = random.Random(str(seed))
        self.taps_under_a_frame = taps_under_a_frame
        self.t = 0

    def _ms(self, lo: float, hi: float) -> int:
        return int(self.rng.uniform(lo, hi) * 1000)

    def _tap_ms(self) -> int:
        """A press's length: some under a frame (2-16 ms), most 1-3 frames, a few longer."""
        r = self.rng.random()
        if r < self.taps_under_a_frame:
            return self._ms(2, 16)
        if r < 0.75:
            return self._ms(16, 55)
        return self._ms(55, 140)

    def _gap_ms(self) -> int:
        r = self.rng.random()
        if r < 0.15:
            return self._ms(3, 16)
        if r < 0.7:
            return self._ms(16, 90)
        return self._ms(90, 400)

    def _stick_x(self, bias: int, dash: bool) -> int:
        right = self.rng.random() < (0.5 + 0.35 * bias)
        mag = self.rng.randint(90, 127) if dash else self.rng.randint(35, 70)
        return 128 + mag if right else 128 - mag

    def actions(self, bias: int) -> List[Tuple[int, Pad]]:
        """One action: (length us, pad) pieces, each held for its length."""
        rng = self.rng
        k = rng.random()
        neutral = Pad()
        if k < 0.22:  # a button tap
            b = rng.choice([A, A, B, B, X, Y, Z])
            return [(self._tap_ms(), Pad(buttons=b)), (self._gap_ms(), neutral)]
        if k < 0.32:  # a tilt or smash with the stick held
            sx = self._stick_x(bias, rng.random() < 0.5)
            hold = self._ms(30, 120)
            return [(self._ms(5, 40), Pad(sx=sx)), (self._tap_ms(), Pad(buttons=A, sx=sx)), (hold, Pad(sx=sx)),
                    (self._gap_ms(), neutral)]
        if k < 0.44:  # walk or dash
            sx = self._stick_x(bias, rng.random() < 0.6)
            return [(self._ms(40, 260), Pad(sx=sx)), (self._gap_ms(), neutral)]
        if k < 0.52:  # a flick (smash turn, dash dance)
            out = []
            for _ in range(rng.randint(1, 4)):
                out.append((self._ms(12, 50), Pad(sx=self._stick_x(bias, True))))
            return out + [(self._gap_ms(), neutral)]
        if k < 0.62:  # light shield (analog only), or a full press (digital + 255)
            trig = rng.choice(["l", "r"])
            if rng.random() < 0.5:
                v = rng.randint(LIGHT_PRESS, 110)
                pad = Pad(l=v) if trig == "l" else Pad(r=v)
            else:
                pad = Pad(buttons=L, l=255) if trig == "l" else Pad(buttons=R, r=255)
            return [(self._tap_ms() + self._ms(0, 200), pad), (self._gap_ms(), neutral)]
        if k < 0.72:  # short hop into an air dodge (wavedash-like): jump, then L with a diagonal
            jump = rng.choice([X, Y])
            sx = self._stick_x(bias, True)
            return [(self._ms(8, 45), Pad(buttons=jump)), (self._ms(30, 70), neutral),
                    (self._ms(12, 40), Pad(buttons=L, l=255, sx=sx, sy=40)), (self._gap_ms(), neutral)]
        if k < 0.80:  # mashing a button (8-14 Hz)
            b = rng.choice([A, B])
            out = []
            for _ in range(rng.randint(4, 12)):
                period = 1000 / rng.uniform(8, 14)
                down = period * rng.uniform(0.3, 0.6)
                out.append((int(down * 1000), Pad(buttons=b)))
                out.append((int((period - down) * 1000), neutral))
            return out
        if k < 0.86:  # crouch / down-B / up-B with the stick
            sy = rng.choice([20, 236])
            b = rng.choice([0, B, A])
            return [(self._ms(20, 200), Pad(sy=sy)), (self._tap_ms(), Pad(buttons=b, sy=sy)),
                    (self._gap_ms(), neutral)]
        if k < 0.92:  # a grab (Z) or a jump held long
            b = rng.choice([Z, X])
            return [(self._ms(30, 250), Pad(buttons=b)), (self._gap_ms(), neutral)]
        # idle
        return [(self._ms(100, 600), neutral)]

    def chunk(self, tape: Tape, t0: int, t1: int, bias: int = 0) -> int:
        """Add segments from ``max(t0, the tape's end)`` until past ``t1``; returns the end time."""
        t = max(t0, self.t)
        while t < t1:
            for length, pad in self.actions(bias):
                # At least 1.5 ms: two rows never share an instant. The same state twice in a row is
                # still a new segment (a new id), as a stick's noise is.
                length = max(length, 1500)
                tape.add(t, pad)
                t += length
        self.t = t
        tape.close(t)
        return t


def quantize(t_us: float, model: str, period_us: float = 0.0, phase_us: float = 0.0,
             latency_us: float = 0.0) -> float:
    """The instant whose controller state a device model presents at host time ``t_us``
    (Harness.cpp TapeDataTime): ``t_us`` itself, or an adapter's last report by then."""
    if model != "adapter" or period_us <= 0:
        return t_us
    read = t_us - latency_us
    k = (read - phase_us) // period_us
    return phase_us + k * period_us


def expected_slot_buttons(pad: Pad) -> int:
    return pad.buttons & BUTTON_MASK


def describe(seg: Segment) -> str:
    p = seg.pad
    parts = []
    names = [(A, "A"), (B, "B"), (X, "X"), (Y, "Y"), (Z, "Z"), (L, "L"), (R, "R"),
             (UP, "DU"), (DOWN, "DD"), (LEFT, "DL"), (RIGHT, "DR")]
    parts += [n for m, n in names if p.buttons & m]
    if (p.sx, p.sy) != (128, 128):
        parts.append(f"stick {p.sx},{p.sy}")
    if p.l:
        parts.append(f"l{p.l}")
    if p.r:
        parts.append(f"r{p.r}")
    return f"#{seg.index} {' '.join(parts) or 'neutral'} {seg.duration_us / 1000:.1f}ms"


def rows_json(segments: Iterable[Segment]) -> List[List[int]]:
    return [s.row() for s in segments]


__all__: Sequence[str] = ("Tape", "Segment", "Pad", "Player", "FIELD_US", "slot_id", "id_to_cstick",
                          "cstick_to_id", "quantize", "describe", "ID_MOD", "LIGHT_PRESS")
