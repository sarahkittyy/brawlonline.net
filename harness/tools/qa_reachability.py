"""QA probe: can a player reach banned states with controller input in vanilla P+ netplay?

Runs one two-instance fixed-delay netplay session of the P+ Netplay Launcher and, using only the
joiner's (or host's) own controller, tries to reach:

1. the main menu (and so every non-Versus mode) by holding B on the CSS;
2. the P+ Code Menu (L + R + D-pad Down) on the CSS and in a match, and Debug Mode inside it;
3. Giga Bowser / Wario-Man via "hold L while going from the character screen to the stage
   screen" (P+ features page) with Bowser / Wario picked.

It reports what each instance's memory says (``brawl.check_banned``, Code Menu state word, debug
bytes, match setup) and screenshots, as JSON on stdout and in ``--out``. It never asserts: the
answer is a QA finding.

    python harness/tools/qa_reachability.py --out run/artifacts/qa-reachability
"""

from __future__ import annotations

import argparse
import json
import os
import sys
import time
from pathlib import Path
from typing import Any, Callable, Dict

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from ppharness import brawl as B  # noqa: E402
from ppharness import flows as F  # noqa: E402
from ppharness.instance import InstanceConfig  # noqa: E402
from ppharness.session import two_player_netplay  # noqa: E402

CODE_MENU_COMBO = [{"buttons": ["L", "R"], "l": 255, "r": 255, "hold": 6},
                   {"buttons": ["L", "R", "DDOWN"], "l": 255, "r": 255, "hold": 4},
                   {"hold": 4}]


def both(hc, jc, fn: Callable[[Any], Any]) -> Dict[str, Any]:
    return {"host": fn(hc), "joiner": fn(jc)}


def menu_state(c) -> Dict[str, Any]:
    m = B.Mem(c.read_mem)
    return {"code_menu_state": B._try(lambda: m.u32(B.CODE_MENU_STATE)),
            "debug_flags": (B.read_debug_flags(c.read_mem) or b"").hex(),
            "scene": B.read_scene(c.read_mem).name, "sequence": B.read_scene(c.read_mem).sequence,
            "banned": B.check_banned(c.read_mem)}


def press(c, frames, wait_extra: int = 20) -> None:
    end = c.pad_script(0, frames).ends_at
    c.wait_frame(input_polls=end + wait_extra, timeout_ms=60000)


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--out", default="run/artifacts/qa-reachability")
    ap.add_argument("--preset", default="lan")
    ap.add_argument("--keep", action="store_true")
    args = ap.parse_args(argv)
    art = F.Artifacts(Path(args.out))
    report: Dict[str, Any] = {"started": time.strftime("%Y-%m-%d %H:%M:%S"), "mode": "fixed-delay netplay",
                              "launcher": "Project+ Netplay Launcher.dol", "steps": {}}
    steps = report["steps"]
    with two_player_netplay(args.preset, rollback=False, keep=args.keep, name="qa-reach",
                            config=InstanceConfig(video_backend="D3D11", cpu_thread=False)) as s:
        hc, jc = s.clients
        host, joiner = F.netplay_seat(hc, "host"), F.netplay_seat(jc, "joiner")

        def step(name: str, fn: Callable[[], Any]) -> None:
            t0 = time.monotonic()
            try:
                steps[name] = {"ok": True, "result": fn()}
            except Exception as e:  # noqa: BLE001 - a QA probe records and goes on
                steps[name] = {"ok": False, "error": f"{type(e).__name__}: {e}",
                               "state": both(hc, jc, lambda c: B._try(lambda: menu_state(c)))}
            steps[name]["seconds"] = round(time.monotonic() - t0, 1)
            print(name, json.dumps(steps[name], default=str)[:600], flush=True)

        step("boot_to_css", lambda: (F.to_css([host, joiner]), both(hc, jc, menu_state))[1])

        # 1. Code Menu on the CSS, opened by the joiner's controller.
        def code_menu_css():
            press(jc, CODE_MENU_COMBO)
            r = both(hc, jc, menu_state)
            art.shoot(jc, "code-menu-css-joiner")
            art.shoot(hc, "code-menu-css-host")
            for _ in range(4):
                press(jc, [{"buttons": ["B"], "hold": 3}, {"hold": 6}], 10)
            r["after_closing"] = both(hc, jc, menu_state)
            return r
        step("code_menu_on_css", code_menu_css)

        # 2. Giga Bowser / Wario-Man: joiner Bowser, host Wario, both hold L while Start is pressed.
        def specials():
            F.pick_characters([(host, "wario"), (joiner, "bowser")])
            hold_l = {"buttons": ["L"], "l": 255, "hold": 1}
            for c in (hc, jc):
                c.pad_set(0, buttons=["L"], l=255)
            B.step(hc, 20)
            # Start with L still held on the host.
            hc.pad_script(0, [{"buttons": ["L", "START"], "l": 255, "hold": 3}] + [dict(hold_l, hold=60)])
            B.wait_scene(hc, [B.Scene.SSS, B.Scene.LOADING, B.Scene.IN_MATCH], 900)
            B.step(hc, 30)
            for c in (hc, jc):
                B.neutral(c, 0)
            art.shoot(hc, "sss-after-hold-L")
            B.sss_pick_stage(hc, B.STAGE_KIND["final_destination"], 0)
            F.wait_match_started([hc, jc])
            B.step(hc, 30)
            art.shoot(hc, "match-host")
            art.shoot(jc, "match-joiner")

            def setup(c):
                ms = B.read_match_setup(c.read_mem)
                return {"characters": {p.port: hex(p.character) for p in ms.players if p.present} if ms else None,
                        "fighters": {p.port: hex(p.ft_kind) for p in B.read_players(c.read_mem)},
                        "banned": B.check_banned(c.read_mem)}
            return both(hc, jc, setup)
        step("hold_L_special_fighters", specials)

        # 3. Code Menu in a match, then Debug Mode on (first line of the first submenu).
        def code_menu_match():
            if B.read_scene(jc.read_mem).scene is not B.Scene.IN_MATCH:
                raise RuntimeError("not in a match")
            press(jc, CODE_MENU_COMBO)
            r = {"opened": both(hc, jc, menu_state)}
            art.shoot(jc, "code-menu-match-joiner")
            press(jc, [{"buttons": ["A"], "hold": 3}, {"hold": 10}])           # Debug Mode Settings >
            press(jc, [{"buttons": ["DRIGHT"], "hold": 3}, {"hold": 10}])      # Debug Mode: ON
            art.shoot(jc, "code-menu-debug-joiner")
            for _ in range(3):
                press(jc, [{"buttons": ["B"], "hold": 3}, {"hold": 10}], 10)
            B.step(jc, 30)
            r["after_toggle_and_close"] = both(hc, jc, menu_state)
            art.shoot(hc, "after-debug-host")
            # Turn it back off so Start is not a freeze toggle for the rest of the session.
            press(jc, CODE_MENU_COMBO)
            press(jc, [{"buttons": ["A"], "hold": 3}, {"hold": 10}])
            press(jc, [{"buttons": ["DLEFT"], "hold": 3}, {"hold": 10}])
            for _ in range(3):
                press(jc, [{"buttons": ["B"], "hold": 3}, {"hold": 10}], 10)
            r["after_restore"] = both(hc, jc, menu_state)
            return r
        step("code_menu_in_match", code_menu_match)

        # 4. Back out of the CSS to the main menu: forfeit the match with nothing; instead check
        #    from the results/CSS. Holding B on the CSS with the token held backs out (~31 frames).
        def back_to_main_menu():
            # End the match quickly: both players self-destruct (L/R-free inputs).
            deadline = time.monotonic() + 240
            while B.read_scene(hc.read_mem).scene is B.Scene.IN_MATCH and time.monotonic() < deadline:
                for c, d in ((hc, 255), (jc, 1)):
                    c.pad_set(0, main=(d, 128))
                B.step(hc, 30)
            for c in (hc, jc):
                B.neutral(c, 0)
            B.wait_scene(hc, [B.Scene.RESULTS, B.Scene.CSS], 1800)
            if B.read_scene(hc.read_mem).scene is B.Scene.RESULTS:
                F.run_parallel([lambda: B.results_to_css(hc, [0]), lambda: B.results_to_css(jc, [0])])
            B.wait_css_ready(jc, [1])
            a = B.read_css_area(jc.read_mem, 1)
            if a is not None and a.placed:
                press(jc, [{"buttons": ["B"], "hold": 1}, {"hold": 10}])       # pick the token up
            press(jc, [{"buttons": ["B"], "hold": 90}, {"hold": 10}], 120)     # hold B
            B.step(jc, 120)
            art.shoot(jc, "after-hold-B")
            return both(hc, jc, menu_state)
        step("hold_B_on_css", back_to_main_menu)

    report["finished"] = time.strftime("%Y-%m-%d %H:%M:%S")
    report["screenshots"] = [str(p) for p in art.files]
    art.root.mkdir(parents=True, exist_ok=True)
    (art.root / "report.json").write_text(json.dumps(report, indent=2, default=str))
    print(json.dumps(report, indent=2, default=str))
    return 0


if __name__ == "__main__":
    sys.exit(main())
