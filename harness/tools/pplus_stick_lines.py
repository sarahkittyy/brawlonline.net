"""P+'s stick and trigger lines, measured in the game (Gprb::Suggest's LINES: which controller changes
go out as input suggestions).

One offline Dolphin, Mario (P1, facing right) vs Mario (P2, idle) on Final Destination. From idle, each
test puts one control at decoded value v (raw = 128 +- v, the harness pad's origin is neutral) and
records the action states of the next frames. The situation tests start elsewhere and also record
each frame's vertical move: falling after a jump's apex (y down: fast fall from 65), shielding with
the stick still (x: roll from 73; y down: spot dodge from 73), running (y up with x 66: tap jump
from 62, against 68 standing). cdiag puts the C-stick at (v, v): an input from 36 (a radius over
50). Run with the repo's .venv python:
    python -I harness/tools/pplus_stick_lines.py [test ...]
"""
from __future__ import annotations

import json
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path[:0] = [str(ROOT / "harness"), str(ROOT / "harness" / "tools")]

from ppharness import brawl as B  # noqa: E402
import gameplay_rollback as G  # noqa: E402

FRAMES = 10
OUT = ROOT / "run" / "scratch" / "pplus_stick_lines"


def p1(c):
    for p in B.read_players(c.read_mem):
        if p.port == 0:
            return p
    return None


def trial(c, entries: list[dict]) -> list[int]:
    c.pad_script(0, entries + [{"buttons": [], "main": [128, 128], "hold": 2}])
    seen = []
    for _ in range(FRAMES):
        B.step(c, 1)
        st = p1(c)
        seen.append(st.action if st else -1)
    return seen


TESTS = {
    "x": lambda v: [{"main": [128 + v, 128], "hold": FRAMES}],
    "y_up": lambda v: [{"main": [128, 128 + v], "hold": FRAMES}],
    "y_down": lambda v: [{"main": [128, 128 - v], "hold": FRAMES}],
    "x_A": lambda v: [{"main": [128 + v, 128], "buttons": ["A"], "hold": 2}],
    "yu_A": lambda v: [{"main": [128, 128 + v], "buttons": ["A"], "hold": 2}],
    "yd_A": lambda v: [{"main": [128, 128 - v], "buttons": ["A"], "hold": 2}],
    "cx": lambda v: [{"c": [128 + v, 128], "hold": 4}],
    "cy_up": lambda v: [{"c": [128, 128 + v], "hold": 4}],
    "cy_down": lambda v: [{"c": [128, 128 - v], "hold": 4}],
    "r": lambda v: [{"r": v, "hold": FRAMES}],
    "cdiag": lambda v: [{"c": [128 + v, 128 + v], "hold": 4}],
}


def to_falling(c) -> None:
    """Jump (X) and step to the first frame that falls."""
    c.pad_script(0, [{"buttons": ["X"], "main": [128, 128], "hold": 3}, {"main": [128, 128], "hold": 200}])
    prev = p1(c).y
    for _ in range(120):
        B.step(c, 1)
        if p1(c).y < prev:
            return
        prev = p1(c).y


def to_shield(c) -> None:
    c.pad_script(0, [{"buttons": ["R"], "r": 255, "main": [128, 128], "hold": 30}])
    B.step(c, 10)


def to_running(c) -> None:
    c.pad_script(0, [{"main": [228, 128], "hold": 2}, {"main": [194, 128], "hold": 60}])
    B.step(c, 20)


# name: (how to get there, the input at v), from idle.
SITUATIONS = {
    "fall_y_down": (to_falling, lambda v: [{"main": [128, 128 - v], "hold": 6}]),
    "shield_x": (to_shield, lambda v: [{"buttons": ["R"], "r": 255, "main": [128 + v, 128], "hold": 6}]),
    "shield_y_down": (to_shield, lambda v: [{"buttons": ["R"], "r": 255, "main": [128, 128 - v], "hold": 6}]),
    "run_y_up": (to_running, lambda v: [{"main": [194, 128 + v], "hold": 6}]),
}


def situation_trial(c, entries: list[dict]) -> list[list]:
    c.pad_script(0, entries + [{"buttons": [], "main": [128, 128], "hold": 2}])
    seen = []
    prev = p1(c).y
    for _ in range(6):
        B.step(c, 1)
        st = p1(c)
        seen.append([st.action, round(st.y - prev, 3)])
        prev = st.y
    return seen


def main() -> int:
    OUT.mkdir(parents=True, exist_ok=True)
    out = OUT / "lines.json"
    only = sys.argv[1:]
    res: dict = {}
    with G.instances([("stick-lines", dict(video="Null"))]) as insts:
        c = insts[0].client
        B.start_match_local(c, {0: "mario", 1: "mario"}, stage="final_destination")
        B.step(c, 200)
        idle = p1(c).action
        res["idle"] = idle
        print("idle action", idle, "facing", p1(c).facing, flush=True)
        state = OUT / "idle.sav"
        c.save_state(state)
        for name, make in TESTS.items():
            if only and name not in only:
                continue
            rows = []
            for v in list(range(0, 101)) + [110, 120, 127]:
                c.load_state(state)
                B.step(c, 2)
                seen = trial(c, make(v))
                rows.append([v, seen])
                print(name, v, seen, flush=True)
            res[name] = rows
            out.write_text(json.dumps(res, indent=1))
        for name, (setup, make) in SITUATIONS.items():
            if only and name not in only:
                continue
            c.load_state(state)
            B.step(c, 2)
            setup(c)
            there = OUT / f"{name}.sav"
            c.save_state(there)
            rows = []
            for v in range(0, 101):
                c.load_state(there)
                seen = situation_trial(c, make(v))
                rows.append([v, seen])
                print(name, v, seen, flush=True)
            res[name] = rows
            out.write_text(json.dumps(res, indent=1))
    print("wrote", out)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
