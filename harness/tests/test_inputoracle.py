"""The input fidelity check (ppharness/inputoracle.py) on made-up machines, no Dolphin: a machine
that reads the tape once a field must pass, and each way of getting it wrong must be named."""

from __future__ import annotations

import random
from typing import Callable, Dict, List, Optional, Tuple

import pytest

from ppharness import inputoracle as O
from ppharness import inputtape as T

FIELD = T.FIELD_US
START = 1_000_000_000  # host time of the tape's start (us)
PORT = 0


def make_tape(seconds: float = 60, seed: int = 1) -> T.Tape:
    tape = T.Tape()
    T.Player(seed).chunk(tape, START, START + int(seconds * 1e6))
    return tape


def machine(tape: T.Tape, *, model: O.Model = O.Model(), phase: float = 0.3, latency_us: float = 40_000,
            delay: int = 2, first_frame: int = 1000, read: Optional[Callable[[int, float], float]] = None,
            shown: Optional[Callable[[int, int], int]] = None) -> Tuple[Dict[int, str], O.Trace]:
    """A machine whose frame f reads the controller at its own instant (``read``: a fault) and runs
    with the segment of that instant (``shown``: a fault), presents its picture ``latency_us``
    after the read, and records it all as the session would."""
    pads: Dict[int, str] = {}
    tr = O.Trace()
    end = tape.segments[-1].end_us
    t = START + phase * FIELD
    f = first_frame
    while t < end:
        data = read(f, t) if read else t
        idx = tape.index_at(model.q(data))
        if idx >= 0:
            if shown:
                idx = shown(f, idx)
            slot = O.expected_slot(tape.segments[idx])
            row = (slot + bytes([0] * 0x40 * 3)).hex() if PORT == 0 else ""
            pads[f] = row
            tr.local[f] = O.LLine(f, int(data), int(data), "now", "looptop", "decode", "", "")
            done = int(t + latency_us)
            tr.presents.append(O.PLine(f + delay, f, f, False, done - 1000, done))
        t += FIELD
        f += 1
    return pads, tr


def analyze(tape: T.Tape, a: Tuple[Dict[int, str], O.Trace], b: Optional[Tuple[Dict[int, str], O.Trace]] = None,
            model: O.Model = O.Model()):
    b = b or a
    rep = O.analyze_player("A", tape, model, PORT, a[0], b[0], a[1], b[1], limit=10**9)
    return rep, O.verdict(rep)


def test_a_machine_reading_once_a_field_passes():
    tape = make_tape()
    rep, bad = analyze(tape, machine(tape))
    assert bad == [], bad
    assert rep["cadence"]["one_field_pct"] == 100.0
    assert rep["exact"]["checked"] > 1500
    assert rep["holds"]["lost_n"] == 0
    # Taps under a frame are seen about d/T of the time.
    st = rep["holds"]["short_taps"]
    assert st["n"] > 40 and abs(st["seen"] - st["expected_seen"]) < 4 * st["n"] ** 0.5
    lat = rep["latency_ms"]["own_screen"]
    assert lat["n"] > 300 and 40 <= lat["min"] and lat["max"] <= 40 + FIELD / 1000 + 0.1


def test_adapter_reports_on_their_own_grid():
    tape = make_tape()
    model = O.Model("adapter", period_us=8000, phase_us=START + 1234)
    rep, bad = analyze(tape, machine(tape, model=model), model=model)
    assert bad == [], bad
    # Without the model the check sees the adapter's lag as wrong frames.
    rep2, bad2 = analyze(tape, machine(tape, model=model))
    assert rep2["exact"]["wrong"] > 0


def test_a_sample_taken_twice_then_one_skipped_is_named():
    tape = make_tape()
    # Every 10th frame reads the controller a field late: the same sample twice, then one skipped.
    rep, bad = analyze(tape, machine(tape, read=lambda f, t: t - FIELD if f % 10 == 0 else t))
    assert rep["cadence"]["classes"].get("0", 0) > 100
    assert rep["cadence"]["classes"].get("2", 0) > 100
    assert any("one field apart" in b for b in bad)
    assert rep["holds"]["off_by_more_than_rounding"]


def test_late_reads_lose_presses_and_stretch_holds():
    tape = make_tape()
    rng = random.Random(3)
    # Reads 0-15 ms late at random (a frame that cannot wait for its due time): uneven samples.
    rep, bad = analyze(tape, machine(tape, read=lambda f, t: t + rng.uniform(0, 15000)))
    assert rep["cadence"]["jitter_ms"]["max"] > 10
    assert rep["cadence"]["off_grid_pct"] > 50
    assert rep["holds"]["off_by_more_than_rounding"]
    assert any("off the frames' grid" in b for b in bad)


def test_early_reads_on_some_frames_are_off_the_grid():
    tape = make_tape()
    # One frame in five reads 3-7 ms early (a paced sleep that ended early for a rollback, as
    # before 4e9925fb): steps still round to one field, but the reads leave the grid.
    rng = random.Random(4)
    rep, bad = analyze(tape, machine(tape, read=lambda f, t: t - rng.uniform(3000, 7000) if f % 5 == 0 else t))
    assert rep["cadence"]["one_field_pct"] > 99
    assert 15 < rep["cadence"]["off_grid_pct"] < 25
    assert any("off the frames' grid" in b for b in bad)


def test_a_frame_that_ran_with_another_segment_than_its_read_is_named():
    tape = make_tape()
    rep, bad = analyze(tape, machine(tape, shown=lambda f, i: i - 1 if f % 50 == 0 else i))
    assert rep["exact"]["wrong"] > 20
    assert any("another segment" in b for b in bad)


def test_the_other_machine_differing_is_named():
    tape = make_tape()
    a = machine(tape)
    b_pads = dict(a[0])
    some = sorted(b_pads)[500]
    raw = bytearray(bytes.fromhex(b_pads[some]))
    raw[3] ^= 0x01
    b_pads[some] = raw.hex()
    rep, bad = analyze(tape, a, (b_pads, a[1]))
    assert rep["agreement"]["differ"] == 1
    assert any("differ between the machines" in x for x in bad)


def test_waits_excuse_the_reads_around_them():
    tape = make_tape()
    a = machine(tape, read=lambda f, t: t + (200_000 if f >= 1500 else 0))
    # The frames after 1500 came 200 ms late: a wait for the peer in between.
    t_wait = a[1].local[1499].data
    a[1].waits.append((t_wait + 1000, a[1].local[1500].data - 1000, "peer"))
    rep, bad = analyze(tape, a)
    assert rep["cadence"]["classes"].get("excused") == 1
    assert not any("one field apart" in b for b in bad)


@pytest.mark.parametrize("model", ["ideal", "mapped"])
def test_quantize_is_identity_without_an_adapter(model):
    assert T.quantize(123456.0, model) == 123456.0


def test_ids_round_trip():
    for i in range(500):
        cx, cy = T.id_to_cstick(i)
        assert T.cstick_to_id(cx, cy) == i % T.ID_MOD
        seg = T.Segment(i, 0, T.Pad())
        assert T.slot_id(O.expected_slot(seg)) == i % T.ID_MOD
