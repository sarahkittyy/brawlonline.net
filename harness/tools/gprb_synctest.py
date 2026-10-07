"""Single-instance sync test for gameplay-only rollback (phase 3, docs/gameplay-rollback-status.md).

Boots one instance, sets up a 2-player Versus match on the CSS/SSS (both ports driven by the
harness), arms ``gprb_synctest`` (a GekkoNet stress session that starts at the match's first
simulation frame: every frame the region set from ``--distance`` frames back is restored and the
frames are simulated again), then plays the match closed-loop until game set or ``--frames``.
Reports the GekkoNet checksum mismatches (gameplay: fighters, RNG, frame counter), the region-set
hash mismatches (any byte of the region set differing from the first run, with 4 KiB chunk
addresses), rollback counts and the save/load cost.

    python harness/tools/gprb_synctest.py --scenario ps2-peach-gw --distance 2 --frames 29000
    python harness/tools/gprb_synctest.py --scenario all --json run/qa/gprb2/synctest.json
"""

from __future__ import annotations

import argparse
import contextlib
import json
import sys
import time
from pathlib import Path
from typing import Any, Dict, List, Optional, Sequence

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
sys.path.insert(0, str(Path(__file__).resolve().parent))

import gameplay_rollback as G  # noqa: E402
from gprb_ab import gentle_macro  # noqa: E402
from ppharness import brawl as B  # noqa: E402
from ppharness import flows as F  # noqa: E402
from ppharness.client import HarnessClient, HarnessError  # noqa: E402

# name: (P1, P2, stage, items). P+ v3.2 has no Pokemon Trainer (Charizard, Squirtle and Ivysaur
# are standalone characters) and no Zelda/Sheik transformation (separate characters), so those
# requested cases are covered by the standalone Pokemon and Zelda.
SCENARIOS: Dict[str, tuple] = {
    "ps2-peach-gw": ("peach", "game_and_watch", "pokemon_stadium_2", False),
    "bf-fox-falco": ("fox", "falco", "battlefield", False),
    "sv-squirtle-zelda": ("squirtle_solo", "zelda", "smashville", False),
    "ps2-ics-olimar": ("ice_climbers", "olimar", "pokemon_stadium_2", False),
    "fd-mario-marth-items": ("mario", "marth", "final_destination", True),
    "fd-mario-marth": ("mario", "marth", "final_destination", False),
    "sv-ics-charizard-items": ("ice_climbers", "charizard_solo", "smashville", True),
    # Article users (projectiles/props the fighters create): bombs, grenades, Pikmin, gyro, bananas.
    "bf-link-snake": ("link", "snake", "battlefield", False),
    "bf-toonlink-diddy": ("toon_link", "diddy_kong", "battlefield", False),
    "fd-rob-olimar": ("rob", "olimar", "final_destination", False),
    "sv-peach-gw": ("peach", "game_and_watch", "smashville", False),
}


def set_items(c: HarnessClient, frequency: int) -> None:
    """Item frequency in the record's menu data (gmGlobalRecord+0x810): 0 none .. 4 very high."""
    m = B.Mem(c.read_mem)
    rec = B._try(lambda: m.chain(B.GAME_GLOBAL_PTR, B.GG_RECORD))
    if rec:
        c.write_mem(rec + 0x810, bytes([frequency]))


def write_selections(c: HarnessClient, chars: Sequence[str]) -> None:
    """On the stage select: overwrite the CSS record (gmSelCharData, players[port] = {character,
    state, ...}) that the stage select copies into gmGlobalModeMelee when it exits. This is how a
    session applies the peer's selection; here it also avoids the CSS recipe, which cannot reach
    some icons (Peach, Pokemon Trainer) on the current P+ CSS."""
    m = B.Mem(c.read_mem)
    scd = m.chain(B.GAME_GLOBAL_PTR, B.GG_SEL_CHAR_DATA)
    for port, who in enumerate(chars):
        kind = B.CSS_TO_CHAR_KIND[B.CSS_ID[who]]
        c.write_mem(scd + B.SCD_PLAYERS + port * B.MM_PLAYER_SIZE, bytes([kind]))


def setup_match(c: HarnessClient, p1: str, p2: str, stage: str, items: bool) -> None:
    B.wait_scene(c, [B.Scene.CSS], 60 * 120)
    B.wait_css_ready(c, [0, 1])
    B.write_rules(c, stocks=4, minutes=8, items_off=not items)
    if items:
        set_items(c, 2)
    B.css_pick_character(c, 0, B.CSS_ID["fox"])
    B.css_pick_character(c, 1, B.CSS_ID["falco"])
    B.css_start(c, 0)
    write_selections(c, [p1, p2])
    B.sss_pick_stage(c, B.STAGE_KIND[stage], 0)


def run_scenario(name: str, args: argparse.Namespace) -> Dict[str, Any]:
    p1, p2, stage, items = SCENARIOS[name]
    extra = ["Dolphin.Core.EmulationSpeed=0"] if args.unthrottled else []
    orig = G.make_instance

    def mk(nm, **kw):
        inst = orig(nm, **kw)
        inst.config.config_args = list(inst.config.config_args or []) + extra
        return inst
    G.make_instance = mk
    rep: Dict[str, Any] = {"scenario": name, "p1": p1, "p2": p2, "stage": stage, "items": items,
                           "distance": args.distance, "region_set": args.region_set, "cpu": args.cpu}
    t0 = time.monotonic()
    try:
        with G.instances([(f"{args.name_prefix}-st-{name}", dict(cpu_thread=args.cpu == "dc"))]) as (inst,):
            c = inst.client
            setup_match(c, p1, p2, stage, items)
            st = c.call("gprb_synctest", distance=args.distance, region_set=args.region_set,
                        hash_regions=not args.no_hash, start_frame=args.start_frame,
                        suppress_resim_sounds=args.suppress_resim_sounds,
                        dedupe_resim_sounds=args.dedupe_resim_sounds)
            deadline = time.monotonic() + 300
            while c.call("gprb_status")["phase"] != "running":
                if time.monotonic() > deadline:
                    raise RuntimeError(f"sync test never started: {c.call('gprb_status')}")
                time.sleep(0.2)
            setup = B.read_match_setup(c.read_mem)
            F._macro = gentle_macro
            seats = [F.Seat(c, 0), F.Seat(c, 1)]
            stop_at = time.monotonic() + args.timeout
            last_print = 0.0
            progress = {"frame": -1, "since": time.monotonic()}

            def stop() -> bool:
                nonlocal last_print
                if time.monotonic() > stop_at:
                    return True
                if time.monotonic() - last_print > 10:
                    last_print = time.monotonic()
                    s = c.call("gprb_status")
                    if s["current_frame"] != progress["frame"]:
                        progress["frame"], progress["since"] = s["current_frame"], time.monotonic()
                    elif time.monotonic() - progress["since"] > 30:
                        rep["stalled"] = {"frame": s["current_frame"], "cpu": c.call("cpu_state")}
                        with contextlib.suppress(Exception):
                            rep["stalled"]["gpu"] = c.call("gpu_state")
                        try:
                            sys.path.insert(0, str(Path(__file__).resolve().parent))
                            from gprb_debug import thread_backtrace
                            rep["stalled"]["main_thread"] = thread_backtrace(c, 0x804DD558)
                        except Exception as e:  # noqa: BLE001
                            rep["stalled"]["bt_error"] = str(e)
                        print(f"  [{name}] STALLED at frame {s['current_frame']}: {rep['stalled']}", flush=True)
                        return True
                    if int(time.monotonic()) % 60 < 10:
                        print(f"  [{name}] frame {s['current_frame']} desyncs {s['desyncs_detected']} "
                              f"region mismatches {s['region_mismatches']} phase {s['phase']}", flush=True)
                    return s["phase"] != "running"
                return False
            # The fight loop ends on game set, but also on a transient harness error or a read that
            # did not see the match: resume until the session ends (game set) or time runs out.
            rounds = 0
            while not stop() and rounds < 50:
                rounds += 1
                with contextlib.suppress(HarnessError):
                    F.fight(seats, args.frames, mode=["random", "random"], seed=f"{name}-{rounds}",
                            stage_kind=setup.stage_kind if setup else None, stop=stop)
                with contextlib.suppress(HarnessError):
                    if c.call("gprb_status")["phase"] != "running":
                        break
            rep["fight_rounds"] = rounds
            try:
                final = c.call("gprb_status")
                rep["status"] = final
                rep["match"] = G.small_state(c)
                if __import__("os").environ.get("PPR_GPRB_CENSUS"):
                    rep["census"] = c.call("gprb_census")["ranges"]
                rep["fps"] = round(final["frames"] / max(1e-6, time.monotonic() - t0), 1)
            finally:
                rep["exit_code"] = inst.process.poll() if inst.process else None
                if args.log_dir:
                    import shutil
                    Path(args.log_dir).mkdir(parents=True, exist_ok=True)
                    stem = f"{name}-{args.cpu}"
                    n = 1
                    while (Path(args.log_dir) / f"{stem}.log").exists():
                        n += 1
                        stem = f"{name}-{args.cpu}-{n}"
                    for src, suffix in (("Logs/dolphin.log", "log"), ("harness-stderr.txt", "stderr.txt")):
                        with contextlib.suppress(OSError):
                            shutil.copy(inst.user_dir / src, Path(args.log_dir) / f"{stem}.{suffix}")
    except Exception as e:  # noqa: BLE001
        rep["error"] = f"{type(e).__name__}: {e}"
    finally:
        G.make_instance = orig
    rep["wall_s"] = round(time.monotonic() - t0, 1)
    s = rep.get("status", {})
    saves = max(1, s.get("save_count", 0))
    loads = max(1, s.get("load_count", 0))
    print(f"{name} d={args.distance} set={args.region_set} cpu={args.cpu}: frames {s.get('current_frame')}, "
          f"desyncs {s.get('desyncs_detected')}, region mismatches {s.get('region_mismatches')}, "
          f"rollbacks {s.get('rollbacks')}, save avg {s.get('save_us_total', 0) / saves:.0f} us max "
          f"{s.get('save_us_max')}, load avg {s.get('load_us_total', 0) / loads:.0f} us max {s.get('load_us_max')}, "
          f"end {s.get('end_reason')!r} error {rep.get('error')} wall {rep['wall_s']} s", flush=True)
    for line in (s.get("desync_log") or [])[:8]:
        print("    desync:", line)
    for line in (s.get("region_mismatch_log") or [])[:4]:
        print("    region:", line[:300])
    return rep


def main(argv: Optional[Sequence[str]] = None) -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--scenario", default="ps2-peach-gw", help=f"comma list or 'all' ({', '.join(SCENARIOS)})")
    ap.add_argument("--distance", type=int, default=2)
    ap.add_argument("--region-set", default="gp-v11")
    ap.add_argument("--frames", type=int, default=30000)
    ap.add_argument("--start-frame", type=int, default=240)
    ap.add_argument("--cpu", default="sc", choices=("sc", "dc"))
    ap.add_argument("--no-hash", action="store_true")
    ap.add_argument("--suppress-resim-sounds", action="store_true")
    ap.add_argument("--dedupe-resim-sounds", action="store_true")
    ap.add_argument("--unthrottled", action="store_true")
    ap.add_argument("--timeout", type=float, default=3600)
    ap.add_argument("--json", default=None)
    ap.add_argument("--log-dir", default=None, help="copy each instance's dolphin.log here")
    ap.add_argument("--name-prefix", default="gprb", help="instance name prefix (ppharness clean --prefix)")
    ap.add_argument("--runs", type=int, default=1, help="run every scenario this many times")
    args = ap.parse_args(argv)
    names = list(SCENARIOS) if args.scenario == "all" else args.scenario.split(",")
    out = []
    for n in [n for n in names for _ in range(args.runs)]:
        out.append(run_scenario(n, args))
        if args.json:
            p = Path(args.json)
            prev = json.loads(p.read_text()) if p.exists() else []
            prev.append(out[-1])
            p.write_text(json.dumps(prev, indent=1))
    return 0


if __name__ == "__main__":
    sys.exit(main())
