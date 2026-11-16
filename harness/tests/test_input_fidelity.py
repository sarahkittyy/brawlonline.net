"""Every press reaches the game once a field, on both machines, through a rollback session
(tools/input_fidelity.py: two instances, direct connect through netsim, no backend).

Each player's controller plays a tape of player-like presses at any instant of the host clock
(pad_tape), not on the game's reads, and the check (ppharness/inputoracle.py) asks of every frame
which press it ran with. The pacing changes that broke this before (bug report #70; e0e8f3c2's
rollback in the paced sleep, fixed by 4e9925fb a week after it shipped) show up here as reads off
the frames' grid, holds a frame too long or too short, and presses never seen.

Cases (90 s of tape, about 2.5 minutes each):
- `lan`, `r114j1` (57 ms each way, test_diag_bug70's link: about a fifth of frames rolled back):
  `ideal`, a controller with a past, as the Wii U adapter's read thread keeps one;
- `r114j1` with the joiner hitching 60 ms every 300 frames (PPR_GPRB_TEST_HITCH): the frames that
  catch up after a hitch;
- `r114j1`, `mapped`: a pad mapped in Dolphin (SDL, XInput, a keyboard), read when called, with no
  past.

Asserted: no frame ran with another press than the read it stood for, both machines agree on every
confirmed frame, reads on the frames' grid (99%), no press held longer than a frame unseen, no hold
off by more than rounding.
"""

from __future__ import annotations

import json
import sys
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))

CASES = [
    pytest.param("lan", "ideal", [], id="lan-ideal"),
    pytest.param("r114j1", "ideal", [], id="r114j1-ideal"),
    pytest.param("r114j1", "ideal", ["--env-b", "PPR_GPRB_TEST_HITCH=300:60"], id="r114j1-ideal-hitch",
                 marks=pytest.mark.xfail(reason="main: the frames that catch up after a hitch read the "
                                                "controller now (input-fix-hitch-catch-up)", strict=False)),
    pytest.param("r114j1", "mapped", [], id="r114j1-mapped",
                 # A pad with no past: the read after a rollback comes late by up to the rest of the re-runs
                 # on main (input-fix-deferred-read-due takes it to one re-run pass), and nothing can catch
                 # up after a hitch. Informative, not a gate.
                 marks=pytest.mark.xfail(reason="a mapped pad has no past: late reads after rollbacks and "
                                                "hitches", strict=False)),
]


@pytest.mark.dolphin
@pytest.mark.slow
@pytest.mark.parametrize("preset, model, extra", CASES)
def test_every_press_reaches_the_game_once_a_field(dolphin_exe, tmp_path, preset, model, extra):
    import input_fidelity as IF

    out = tmp_path / f"{preset}-{model}"
    code = IF.main(["--out", str(out), "--model", model, "--seconds", "90", "--preset", preset, "--cpu", "dc",
                    "--name-prefix", f"if-{preset}-{model}", *extra])
    reports = json.loads((out / "reports.json").read_text())
    assert reports, "no run"
    r = reports[0]
    assert not r.get("error"), r.get("error")
    for name, p in r["players"].items():
        print(name, json.dumps({k: p.get(k) for k in ("exact", "agreement", "cadence", "holds", "latency_ms")})[:3000])
        assert p["frames"] > 4000, f"{name}: too few frames"
        assert not p["verdict"], f"{name}: {p['verdict']}"
    assert code == 0
