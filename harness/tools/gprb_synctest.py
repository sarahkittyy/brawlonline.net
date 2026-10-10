"""Single-instance sync test for gameplay-only rollback (phase 3, docs/gameplay-rollback-status.md).

Boots one instance, sets up a 2-, 3- or 4-player Versus match on the CSS/SSS (every port driven
by the harness; free-for-all or team battle, ``NP_SCENARIOS``), arms ``gprb_synctest`` (a GekkoNet stress session that starts at the match's first
simulation frame: every frame the region set from ``--distance`` frames back is restored and the
frames are simulated again), then plays the match closed-loop until game set or ``--frames``.
Reports the GekkoNet checksum mismatches (gameplay: fighters, RNG, frame counter), the region-set
hash mismatches (any byte of the region set differing from the first run, with 4 KiB chunk
addresses), rollback counts and the save/load cost.

    python harness/tools/gprb_synctest.py --scenario ps2-peach-gw --distance 2 --frames 29000
    python harness/tools/gprb_synctest.py --scenario all --json run/qa/gprb2/synctest.json
    python harness/tools/gprb_synctest.py --scenario f4,t4 --json run/qa/nplayer/st.json
"""

from __future__ import annotations

import argparse
import contextlib
import dataclasses
import json
import sys
import time
from dataclasses import dataclass
from pathlib import Path
from typing import Any, Callable, Dict, List, Mapping, Optional, Sequence, Tuple

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


@dataclass(frozen=True)
class Scenario:
    """A match of 2-4 players: ``chars`` per port from P1, the stage, items, and for a team battle
    each port's team (CSS team colour: 0 red, 1 blue, 2 green), else None (free-for-all)."""
    chars: Tuple[str, ...]
    stage: str
    items: bool = False
    teams: Optional[Tuple[int, ...]] = None

    @property
    def ports(self) -> int:
        """Bit mask of the ports in the match (sync test ``ports``)."""
        return (1 << len(self.chars)) - 1

    @property
    def controllers(self) -> Tuple[int, ...]:
        return tuple(range(len(self.chars)))


# 3- and 4-player matches (docs/nplayer/determinism.md): P+ competitive rules (4 stocks, 8 minutes,
# items off); team battles with team attack on (P+'s default rule). Characters with many articles,
# projectiles and transformations, on legal stages.
NP_SCENARIOS: Dict[str, Scenario] = {
    # free-for-all, 4 players
    "f4-bf-zelda-ics-olimar-peach": Scenario(("zelda", "ice_climbers", "olimar", "peach"), "battlefield"),
    "f4-ps2-gw-snake-rob-wario": Scenario(("game_and_watch", "snake", "rob", "wario"), "pokemon_stadium_2"),
    "f4-sv-bowser-sheik-ivysaur-squirtle": Scenario(("bowser", "sheik", "ivysaur_solo", "squirtle_solo"),
                                                    "smashville"),
    "f4-fd-charizard-link-diddy-pit": Scenario(("charizard_solo", "link", "diddy_kong", "pit"), "final_destination"),
    "f4-dl-tlink-samus-falco-dedede": Scenario(("toon_link", "samus", "falco", "king_dedede"), "dream_land"),
    "f4-fh-ics-ics-olimar-olimar": Scenario(("ice_climbers", "ice_climbers", "olimar", "olimar"), "frigate_husk"),
    # free-for-all, 3 players
    "f3-fd-peach-gw-snake": Scenario(("peach", "game_and_watch", "snake"), "final_destination"),
    "f3-gh-rob-zelda-wario": Scenario(("rob", "zelda", "wario"), "green_hill_zone"),
    "f3-tt-olimar-bowser-ics": Scenario(("olimar", "bowser", "ice_climbers"), "temple_of_time"),
    # team battles, 2 vs 2 (P1+P2 red, P3+P4 blue), team attack on
    "t4-bf-ics-olimar-vs-zelda-sheik": Scenario(("ice_climbers", "olimar", "zelda", "sheik"), "battlefield",
                                                teams=(0, 0, 1, 1)),
    "t4-sv-rob-wario-vs-bowser-peach": Scenario(("rob", "wario", "bowser", "peach"), "smashville", teams=(0, 0, 1, 1)),
    "t4-ps2-snake-gw-vs-charizard-squirtle": Scenario(("snake", "game_and_watch", "charizard_solo", "squirtle_solo"),
                                                      "pokemon_stadium_2", teams=(0, 0, 1, 1)),
    "t4-lm-peach-peach-vs-snake-snake": Scenario(("peach", "peach", "snake", "snake"), "luigis_mansion",
                                                 teams=(0, 0, 1, 1)),
    # team battle, 3 players (2 vs 1)
    "t3-dl-gw-ics-vs-rob": Scenario(("game_and_watch", "ice_climbers", "rob"), "dream_land", teams=(0, 0, 1)),
    # the cost series: the 4-player Battlefield match above with its first 2 and 3 players
    "f2-bf-zelda-ics": Scenario(("zelda", "ice_climbers"), "battlefield"),
    "f3-bf-zelda-ics-olimar": Scenario(("zelda", "ice_climbers", "olimar"), "battlefield"),
}


def scenario(name: str) -> Scenario:
    """A scenario of either table (the 2-player SCENARIOS tuples or NP_SCENARIOS)."""
    if name in NP_SCENARIOS:
        return NP_SCENARIOS[name]
    p1, p2, stage, items = SCENARIOS[name]
    return Scenario((p1, p2), stage, items)


def set_items(c: HarnessClient, frequency: int) -> None:
    """Item frequency in the record's menu data (gmGlobalRecord+0x810): 0 none .. 4 very high."""
    m = B.Mem(c.read_mem)
    rec = B._try(lambda: m.chain(B.GAME_GLOBAL_PTR, B.GG_RECORD))
    if rec:
        c.write_mem(rec + 0x810, bytes([frequency]))


def write_selections(c: HarnessClient, chars: Sequence[str], teams: Optional[Sequence[int]] = None) -> None:
    """On the stage select: overwrite the CSS record (gmSelCharData, players[port] = {character,
    state, ...}) that the stage select copies into gmGlobalModeMelee when it exits. This is how a
    session applies the peer's selection; here it also avoids the CSS recipe, which cannot reach
    some icons (Peach, Pokemon Trainer) on the current P+ CSS.

    ``teams``: in a team battle, each port's team (gmPlayerInitData::m_teamNo, 0 red, 1 blue,
    2 green; checked in the match as ftEntry::m_pointTeam, ``check_setup``)."""
    m = B.Mem(c.read_mem)
    scd = m.chain(B.GAME_GLOBAL_PTR, B.GG_SEL_CHAR_DATA)
    for port, who in enumerate(chars):
        kind = B.CSS_TO_CHAR_KIND[B.CSS_ID[who]]
        c.write_mem(scd + B.SCD_PLAYERS + port * B.MM_PLAYER_SIZE, bytes([kind]))
        if teams is not None:
            c.write_mem(scd + B.SCD_PLAYERS + port * B.MM_PLAYER_SIZE + SEL_TEAM, bytes([teams[port]]))


SEL_TEAM = 0x0B            # gmPlayerInitData::m_teamNo (gmSelCharData players; the setup copies it)
FTE_POINT_TEAM = 0x60      # ftEntry::m_pointTeam: the team the match plays the entry in
MM_IS_TEAMS = 0x08 + 0x0B  # gmGlobalModeMelee: gmMeleeInitData m_isTeams
CSS_TASK_TEAM_BATTLE = 0x5C8  # CSS task: 1 after the BRAWL tab switched to team battle (verified live)
CSS_BUTTON_BRAWL_TAB = 0x03   # hand button id of the BRAWL / TEAM tab (Orca's OnlineRulesTest)


def css_team_battle(c: HarnessClient, port: int = 0) -> None:
    """Switch the CSS to team battle: ``port``'s hand on the BRAWL tab (top left), A."""
    m = B.Mem(c.read_mem)
    task = m.ptr(B.read_scene(c.read_mem).scene_ptr + B.CSS_SCENE_TASK)
    if c.read_mem(task + CSS_TASK_TEAM_BATTLE, 1)[0] == 1:
        return
    B.css_join(c, port)
    for _ in range(600):
        a = B.read_css_area(c.read_mem, port)
        if a is None:
            raise B.RecipeError("left the CSS while switching to team battle", None)
        if a.hand_target == B.CSS_HAND_BUTTON and a.hand_button == CSS_BUTTON_BRAWL_TAB:
            B.neutral(c, port)
            B.step(c, 6)
            B.tap(c, port, ["A"], hold=3, release=30)
            break
        c.pad_set(port, buttons=(), main=B.steer_toward(a.hand_x, a.hand_y, -14.0, 19.5, full_tilt_at=3.0))
        B.step(c, 1)
    if c.read_mem(task + CSS_TASK_TEAM_BATTLE, 1)[0] != 1:
        raise B.RecipeError("the CSS did not switch to team battle", B.read_css_area(c.read_mem, port))


def setup_match(c: HarnessClient, p1: str, p2: str, stage: str, items: bool) -> None:
    setup_match_n(c, Scenario((p1, p2), stage, items))


def setup_match_n(c: HarnessClient, sc: Scenario) -> None:
    """CSS (team battle if ``sc.teams``), Fox/Falco tokens on every port, Start, the wanted
    characters and teams written on the stage select, the stage."""
    ports = list(range(len(sc.chars)))
    B.wait_scene(c, [B.Scene.CSS], 60 * 120)
    B.wait_css_ready(c, ports)
    B.write_rules(c, stocks=4, minutes=8, items_off=not sc.items)
    if sc.items:
        set_items(c, 2)
    for port in ports:
        B.css_pick_character(c, port, B.CSS_ID["fox" if port % 2 == 0 else "falco"])
    if sc.teams is not None:
        css_team_battle(c, 0)
    B.css_start(c, 0)
    write_selections(c, sc.chars, sc.teams)
    B.sss_pick_stage(c, B.STAGE_KIND[sc.stage], 0)


def check_setup(c: HarnessClient, sc: Scenario) -> List[str]:
    """The match built from the setup (gmGlobalModeMelee): players, characters, teams, rules."""
    m = B.Mem(c.read_mem)
    mm = m.chain(B.GAME_GLOBAL_PTR, B.GG_MODE_MELEE)
    setup = B.read_match_setup(c.read_mem)
    want = {p: B.CSS_TO_CHAR_KIND[B.CSS_ID[who]] for p, who in enumerate(sc.chars)}
    out = B.verify_match_setup(c.read_mem, want, B.STAGE_KIND[sc.stage], 4, 8)
    if setup is not None and setup.num_players != len(sc.chars):
        out.append(f"{setup.num_players} players (want {len(sc.chars)})")
    for p in range(len(sc.chars), 4):
        if setup is not None and setup.players[p].present:
            out.append(f"port {p + 1} present")
    is_teams = c.read_mem(mm + MM_IS_TEAMS, 1)[0]
    if is_teams != (1 if sc.teams is not None else 0):
        out.append(f"m_isTeams {is_teams}")
    # The teams the match plays with: each fighter entry's ftEntry::m_pointTeam (+0x60), by port.
    # Free-for-all: every port its own; a team battle: the scenario's partition.
    want_teams = list(sc.teams) if sc.teams is not None else list(range(len(sc.chars)))
    got = fighter_teams(c)
    got_l = [got.get(p) for p in range(len(sc.chars))]
    if None in got_l or [got_l.index(t) for t in got_l] != [want_teams.index(t) for t in want_teams]:
        out.append(f"fighter teams {got_l} (want the partition of {want_teams})")
    if sc.teams is not None and not c.read_mem(mm + 0x08 + 0x02, 1)[0] & 1:
        out.append("team attack off")
    return out


def fighter_teams(c: HarnessClient) -> Dict[int, int]:
    """Port -> ftEntry::m_pointTeam (+0x60) of every fighter entry of the running match."""
    m = B.Mem(c.read_mem)
    out: Dict[int, int] = {}
    with contextlib.suppress(B.BadPointer):
        entries = m.ptr(B.FT_ENTRY_MANAGER)
        for i in range(4):
            e = entries + i * B.FTE_SIZE
            port = m.s32(e + B.FTE_PLAYER_NO)
            if 0 <= port < 4:
                out[port] = m.s32(e + FTE_POINT_TEAM)
    return out


class TeamFighter(F.Fighter):
    """flows.Fighter that, in a team battle, only goes for the other team."""

    def __init__(self, *a: Any, team_of: Optional[Mapping[int, int]] = None, **kw: Any):
        super().__init__(*a, **kw)
        self.team_of = dict(team_of or {})

    def decide(self, st: B.MatchState) -> List[Dict[str, Any]]:
        if self.team_of:
            mine = self.team_of.get(self.seat.port)
            players = [p for p in st.players if p.port == self.seat.port or self.team_of.get(p.port) != mine]
            st = dataclasses.replace(st, players=players)
        return super().decide(st)


def fight_n(c: HarnessClient, sc: Scenario, frames: int, seed: Any, stage_kind: Optional[int],
            stop: Callable[[], bool]) -> None:
    """flows.fight for every port of the scenario (random macros), team-aware."""
    team_of = {p: t for p, t in enumerate(sc.teams)} if sc.teams is not None else None
    if team_of is None:
        F.fight([F.Seat(c, p) for p in range(len(sc.chars))], frames, mode="random", seed=seed,
                stage_kind=stage_kind, stop=stop)
        return
    orig = F.Fighter
    F.Fighter = lambda seat, mode, seed_, sk: TeamFighter(seat, mode, seed_, sk, team_of=team_of)  # type: ignore
    try:
        F.fight([F.Seat(c, p) for p in range(len(sc.chars))], frames, mode="random", seed=seed,
                stage_kind=stage_kind, stop=stop)
    finally:
        F.Fighter = orig


def run_scenario(name: str, args: argparse.Namespace) -> Dict[str, Any]:
    sc = scenario(name)
    p1, p2, stage, items = sc.chars[0], sc.chars[1], sc.stage, sc.items
    extra = ["Dolphin.Core.EmulationSpeed=0"] if args.unthrottled else []
    orig = G.make_instance

    def mk(nm, **kw):
        inst = orig(nm, **kw)
        inst.config.config_args = list(inst.config.config_args or []) + extra
        return inst
    G.make_instance = mk
    rep: Dict[str, Any] = {"scenario": name, "p1": p1, "p2": p2, "chars": list(sc.chars), "stage": stage,
                           "items": items, "teams": list(sc.teams) if sc.teams is not None else None,
                           "distance": args.distance, "region_set": args.region_set, "cpu": args.cpu}
    t0 = time.monotonic()
    tag = None
    if args.pass_log:
        # Diagnostics: the passes (PPR_GPRB_PASS_LOG) and a countdown savestate, so that this run
        # can be replayed exactly (gprb_mispredict.py run --state <sav> --modes replay=<log>).
        Path(args.pass_log).mkdir(parents=True, exist_ok=True)
        n = 1
        while (Path(args.pass_log) / f"{name}-{n}.sav").exists():
            n += 1
        tag = Path(args.pass_log, f"{name}-{n}").resolve()
        __import__("os").environ["PPR_GPRB_PASS_LOG"] = str(tag)
        rep["pass_log"] = str(tag) + ".synctest.m0"
    try:
        with G.instances([(f"{args.name_prefix}-st-{name}",
                           dict(cpu_thread=args.cpu == "dc", controllers=sc.controllers))]) as (inst,):
            c = inst.client
            setup_match_n(c, sc)
            st = c.call("gprb_synctest", distance=args.distance, region_set=args.region_set,
                        hash_regions=not args.no_hash, start_frame=args.start_frame, ports=sc.ports,
                        suppress_resim_sounds=args.suppress_resim_sounds,
                        dedupe_resim_sounds=args.dedupe_resim_sounds)
            if tag is not None:
                deadline = time.monotonic() + 300
                while not (c.call("gprb_status")["phase"] == "countdown" and
                           (B.read_frame_counters(c.read_mem).get("game_frame") or 0) >= 60):
                    if time.monotonic() > deadline:
                        raise RuntimeError("no countdown to save")
                    time.sleep(0.05)
                c.pause()
                c.save_state(str(tag) + ".sav")
                rep["countdown_state"] = str(tag) + ".sav"
                c.resume()
            deadline = time.monotonic() + 300
            while c.call("gprb_status")["phase"] != "running":
                if time.monotonic() > deadline:
                    raise RuntimeError(f"sync test never started: {c.call('gprb_status')}")
                time.sleep(0.2)
            setup = B.read_match_setup(c.read_mem)
            rep["setup_problems"] = check_setup(c, sc)
            if rep["setup_problems"]:
                raise RuntimeError(f"match setup: {rep['setup_problems']}")
            F._macro = gentle_macro
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
                    fight_n(c, sc, args.frames, f"{name}-{rounds}", setup.stage_kind if setup else None, stop)
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
    ap.add_argument("--scenario", default="ps2-peach-gw",
                    help=f"comma list of names or groups: 'all' (the 2-player ones: {', '.join(SCENARIOS)}), "
                         f"'np' (3-4 players: {', '.join(NP_SCENARIOS)}), 'f4', 'f3', 't4', 't3'")
    ap.add_argument("--distance", type=int, default=2)
    ap.add_argument("--region-set", default="gp-v21")
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
    ap.add_argument("--pass-log", default=None,
                    help="diagnostics: per run, the passes and a countdown savestate in this directory")
    args = ap.parse_args(argv)
    groups = {"all": list(SCENARIOS), "np": list(NP_SCENARIOS),
              "f4": [n for n in NP_SCENARIOS if n.startswith("f4-")],
              "f3": [n for n in NP_SCENARIOS if n.startswith("f3-")],
              "f2": [n for n in NP_SCENARIOS if n.startswith("f2-")],
              "t4": [n for n in NP_SCENARIOS if n.startswith("t4-")],
              "t3": [n for n in NP_SCENARIOS if n.startswith("t3-")]}
    names = [n for x in args.scenario.split(",") for n in groups.get(x, [x])]
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
