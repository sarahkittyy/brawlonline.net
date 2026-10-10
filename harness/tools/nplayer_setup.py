"""3-4 player matches on one instance (docs/nplayer/setup.md): the online match setup from a
SESSION written the way Dolphin writes it, free-for-all and teams with port gaps, a dropped
player's fighter removed at a frame, and that removal under the single-instance sync test.

One instance boots P+ with the plugin, plays Dolphin's part of the mailbox itself (GameBridge off:
logged in, a Direct search answered "connected"), then writes a 3- or 4-player SESSION. The online
CSS leaves for that match exactly as it does for a real one, and the plugin's match setup builds
it. All four ports are harness pads (neutral unless the sync test drives them).

    python harness/tools/nplayer_setup.py --scenario ffa-gap          # P1 + P3 + P4, free-for-all
    python harness/tools/nplayer_setup.py --scenario teams-2v2        # red P1 + P3, blue P2 + P4
    python harness/tools/nplayer_setup.py --scenario teams-2v1-gap    # red P1 + P3, green P4
    python harness/tools/nplayer_setup.py --scenario bad-team-byte    # refused (no match)
    python harness/tools/nplayer_setup.py --scenario ffa-gap --synctest --distance 2

Removal: SESSION's gone flag for a port is written while the match runs (what Dolphin's session
does from the agreed frame on); with --synctest the plugin's test hook (DEBUG CFG_TEST_GONE) sets
it from a game frame instead, since nothing may write the rolled-back state from outside while a
sync test runs. Results (JSON) and screenshots go to run/artifacts/nplayer/<scenario>/.
"""

from __future__ import annotations

import argparse
import json
import os
import struct
import sys
import time
from pathlib import Path
from typing import Any, Callable, Dict, List, Optional

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "harness"))
sys.path.insert(0, str(ROOT / "harness" / "tests"))
sys.path.insert(0, str(ROOT / "tools" / "gamecode"))
sys.path.insert(0, str(ROOT / "tools" / "sdcard"))

import drive  # noqa: E402
import ppboot  # noqa: E402
import ppom  # noqa: E402
from ppharness import brawl as B  # noqa: E402
from ppharness.client import HarnessClient, PadInput  # noqa: E402
from test_online_game import keypad_steps  # noqa: E402

PLUGIN = Path(os.environ.get("PPHARNESS_PLUGIN") or ROOT / "game-code" / "PPOnline" / "PPOnline.rel")
ART = ROOT / "run" / "artifacts" / "nplayer"

GG = 0x805A00E0
MM_PLAYERS, PLAYER_SIZE = 0x98, 0x5C
SEL_TEAMS = 0x33
COSTUME_TABLE, TEAM_COLOUR = 0x80585B08, 0x805A21F0   # P+'s CSS slot costume lists; team -> colour

RED, BLUE, GREEN, NONE = 0, 1, 2, 0xFF
OUT_STATUS = 0x10B   # a fighter out of stocks (as after falling off on its last stock)

SCENARIOS: Dict[str, Dict[str, Any]] = {
    # Players by port (None: empty). Removal: `gone` ports first, then `gone2`, then game set.
    "ffa-gap": {"teams": False, "stage": 0x01,
                "players": [{"c": "fox", "costume": 1}, None, {"c": "marth"}, {"c": "fox", "costume": 1}],
                "gone": 0b0100, "gone2": 0b1000, "winner": 0},
    # Two players (today's 1v1 through the same path): a baseline for the sync test.
    "ffa-2": {"teams": False, "stage": 0x01,
              "players": [{"c": "fox"}, {"c": "marth"}, None, None],
              "gone": 0, "gone2": 0b0010, "winner": 0},
    # Ice Climbers: Nana is a second fighter of the entry; both have to leave.
    "ffa-ics": {"teams": False, "stage": 0x01,
                "players": [{"c": "fox"}, {"c": "ice_climbers"}, {"c": "marth"}, None],
                "gone": 0b0010, "gone2": 0b0100, "winner": 0},
    "ffa-4": {"teams": False, "stage": 0x21,
              "players": [{"c": "mario"}, {"c": "falco"}, {"c": "peach"}, {"c": "marth"}],
              "gone": 0b0010, "gone2": 0b1100, "winner": 0},
    "teams-2v2": {"teams": True, "stage": 0x02,
                  "players": [{"c": "mario", "team": RED}, {"c": "fox", "team": BLUE},
                              {"c": "mario", "team": RED}, {"c": "falco", "team": BLUE, "costume": 2}],
                  "gone": 0b0010, "gone2": 0b1000, "winner": 0},
    "teams-2v1-gap": {"teams": True, "stage": 0x2D,
                      "players": [{"c": "peach", "team": RED}, None, {"c": "marth", "team": RED},
                                  {"c": "fox", "team": GREEN}],
                      "gone": 0b0001, "gone2": 0b1000, "winner": 2},
    # A team byte that is no colour (Dolphin never sends one; the plugin checks anyway).
    "bad-team-byte": {"teams": True, "stage": 0x01, "refused": True,
                      "players": [{"c": "fox", "team": BLUE}, {"c": "marth", "team": 7},
                                  {"c": "mario", "team": RED}, None]},
}


def wait(cond: Callable[[], Any], timeout: float, what: str, step: float = 0.1) -> Any:
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        v = cond()
        if v:
            return v
        time.sleep(step)
    raise TimeoutError(f"timed out after {timeout:.0f} s: {what}")


class Run:
    def __init__(self, name: str, cpu_thread: bool, out: Path, video: str = ppboot.DEFAULT_VIDEO):
        self.out = out
        out.mkdir(parents=True, exist_ok=True)
        self.video = video
        self.inst = ppboot.make_instance(name, [str(PLUGIN)], boot="offline", cpu_thread=cpu_thread,
                                         video=video, ports=(0, 1, 2, 3))
        self.inst.launch()
        self.c: HarnessClient = self.inst.connect()
        self.b: Optional[ppom.Block] = None
        self.rep: Dict[str, Any] = {}

    def close(self) -> None:
        try:
            self.inst.stop()
        except Exception:  # noqa: BLE001
            self.inst.kill()
        d = self.inst.user_dir
        if d and d.exists():
            from ppharness.instance import remove_tree
            log = d / "Logs" / "dolphin.log"
            if log.exists():
                (self.out / "dolphin.log").write_bytes(log.read_bytes())
            remove_tree(d, self.inst.linked_dirs)

    # -- game memory
    def u32(self, a: int) -> int:
        return struct.unpack(">I", self.c.read_mem(a, 4))[0]

    def scratch(self, i: int) -> int:
        return ppom.read_debug(self.c, self.block())["scratch"][i]

    def block(self) -> ppom.Block:
        if self.b is None:
            self.b = ppom.find_block(self.c)
        return self.b

    def scene(self) -> str:
        return drive.scene(self.c).name

    def steps(self, *steps: str) -> None:
        drive.run(self.c, list(steps), out=str(self.out))

    def shot(self, name: str) -> None:
        if self.video == "Null":
            return
        p = self.out / f"{name}.png"
        self.c.screenshot(str(p.resolve()))

    # -- Dolphin's part of the mailbox (GameBridge off)
    def serve(self) -> List[int]:
        b = self.block()
        (_wr, rd, _rc, rs), reqs = ppom.read_requests(self.c, b)
        seen = []
        for seq, cmd, pl in sorted(reqs):
            if not seq or seq <= rd:
                continue
            rc = struct.unpack(">I", self.c.read_mem(b.mailbox + ppom.MB_RESP_COUNT, 4))[0]
            rs = struct.unpack(">I", self.c.read_mem(b.mailbox + ppom.MB_RESP_SEEN, 4))[0]
            if rc != rs:
                break   # the game has not taken the last answer yet
            if cmd == 0xB9:
                payload = struct.pack(">BB", 1, 0) + ppom.u16s("Sarah", ppom.NAME_LEN) + \
                    ppom.u16s("SARA#001", ppom.CODE_LEN)
                ppom.write_response(self.c, b, seq, 0xB9, payload)
            elif cmd in (0xB4, 0xB3):
                ppom.write_response(self.c, b, seq, 0xB3,
                                    ppom.match_state_payload(4, "Room", "ABCD#123", role=1))
            elif cmd == 0xBE:
                ppom.write_response(self.c, b, seq, 0xBE, ppom.code_suggestion_payload(False, "", 0))
            elif cmd != 0xBA:
                ppom.write_response(self.c, b, seq, cmd, b"", status=0xFF)
            ppom.consume(self.c, b, seq)
            seen.append(cmd)
        return seen

    def to_connected_css(self) -> None:
        """Boot -> ONLINE page -> WITH FRIENDS -> BASIC VERSUS (Direct) -> Fox -> code -> START,
        the search answered "connected" (mmState 4)."""
        self.c.wait_state("running", timeout=180)
        wait(lambda: self.scene() == "muMenuMain", 180, "the menus")
        self.c.game_bridge_config(enabled=False)
        self.steps("wait 60")
        wait(lambda: 0xB9 in self.serve() or None, 20, "GET_ONLINE_STATUS")
        self.steps("tap B", "wait 60", "tap A", "wait 60")
        self.serve()
        self.steps("wait 30")
        self.serve()
        self.steps("tap A", "wait 90")
        self.serve()
        self.steps("tap A", "wait 400", "until scSelctCharacter 600")
        self.serve()
        self.steps("stick up 30", "wait 5", "tap A 8", "wait 30")
        for _ in range(6):
            self.steps("tap START 8", "wait 40")
            self.serve()
            s = ppom.read_debug(self.c, self.block())["scratch"]
            if s[4] == 1 and s[5] == 8:
                break
            self.steps("wait 60")
        else:
            raise RuntimeError("the code keypad did not open")
        self.steps(*keypad_steps("ABCD#123"))
        for _ in range(4):
            self.steps("tap START 8", "wait 20")
            if 0xB4 in self.serve():
                break
        else:
            raise RuntimeError("no FIND_OPPONENT")

        def connected() -> bool:
            self.serve()
            return (self.scratch(1) >> 4) & 0xF == 3
        wait(connected, 20, "connected on the CSS")
        self.shot("01-css-connected")

    # -- the match
    def write_session(self, sc: Dict[str, Any]) -> None:
        players = []
        for p in sc["players"]:
            if p is None:
                players.append(None)
                continue
            players.append({"char_kind": B.CHAR_KIND[p["c"]], "costume": p.get("costume", 0),
                            "team": p.get("team", NONE), "name": p["c"][:15]})
        ppom.write_session(self.c, self.block(), game=1, stage=sc["stage"], players=players,
                           teams=sc["teams"], mode=2)

    def setup_seen(self) -> Dict[str, Any]:
        gg = self.u32(GG)
        mm, sel = self.u32(gg + 0x08), self.u32(gg + 0x10)
        d = self.c.read_mem(mm, 0x98 + 4 * PLAYER_SIZE)
        players = []
        for i in range(4):
            p = d[MM_PLAYERS + i * PLAYER_SIZE:MM_PLAYERS + (i + 1) * PLAYER_SIZE]
            players.append({"char": p[0], "state": p[1], "colour_file": p[5], "costume": p[6],
                            "controller": p[7], "shade": p[0x0A], "team": p[0x0B]})
        return {"teams": d[0x08 + 0x0B], "team_attack": d[0x08 + 0x02] & 1,
                "num_players": (d[0x09] >> 2) & 7, "stage": struct.unpack_from(">H", d, 0x08 + 0x12)[0],
                "sel_teams": self.c.read_mem(sel + SEL_TEAMS, 1)[0], "players": players}

    def team_colour_of(self, char_kind: int, costume: int) -> Optional[int]:
        """The colour byte of a character's costume in P+'s CSS slot table (via its CSS id)."""
        css = [i for i, k in enumerate(B.CSS_TO_CHAR_KIND) if k == char_kind]
        if not css:
            return None
        lst = self.u32(COSTUME_TABLE + css[0] * 0x10)
        return self.c.read_mem(lst + 2 * costume, 1)[0]

    def players(self) -> Dict[int, Dict[str, Any]]:
        out = {}
        for ps in B.read_players(self.c.read_mem):
            out[ps.port] = {"stocks": ps.stocks, "damage": ps.damage, "x": ps.x, "y": ps.y,
                            "action": ps.action}
        return out

    def session(self) -> Dict[str, Any]:
        return ppom.read_session(self.c, self.block())

    def game_frame(self) -> int:
        return self.u32(B.GAME_FRAME + 4)


def check_setup(r: Run, sc: Dict[str, Any]) -> List[str]:
    """The match as built against the SESSION: characters, empty ports, teams and costumes."""
    errs = []
    st = r.setup_seen()
    r.rep["setup"] = st
    if st["teams"] != (1 if sc["teams"] else 0):
        errs.append(f"isTeams {st['teams']}")
    if st["team_attack"] != 1:
        errs.append("team attack off")
    if st["stage"] != sc["stage"]:
        errs.append(f"stage {st['stage']:#x}")
    seen_costumes: Dict[int, List[tuple]] = {}
    for i, p in enumerate(sc["players"]):
        m = st["players"][i]
        if p is None:
            if m["state"] != 3:
                errs.append(f"P{i + 1} should be empty: {m}")
            continue
        kind = B.CHAR_KIND[p["c"]]
        if m["char"] != kind or m["state"] != 0 or m["controller"] != i + 1:
            errs.append(f"P{i + 1}: {m}")
        if sc["teams"]:
            if m["team"] != p["team"]:
                errs.append(f"P{i + 1} team {m['team']} != {p['team']}")
            colour = r.team_colour_of(kind, m["costume"])
            want = r.c.read_mem(TEAM_COLOUR + p["team"], 1)[0]
            if colour != want:
                errs.append(f"P{i + 1} costume {m['costume']} has colour {colour}, team wants {want}")
        elif m["costume"] != p.get("costume", 0):
            errs.append(f"P{i + 1} costume {m['costume']} != {p.get('costume', 0)}")
        # Colour clash: a second fighter of the same character and costume is shaded.
        key = (kind, m["costume"])
        n = len(seen_costumes.setdefault(key, []))
        seen_costumes[key].append(i)
        want_shade = [0, 3, 1, 2][n]
        if sc["teams"]:
            # A team battle: the game shades a second one of a character on a team itself.
            if (m["shade"] != 0) != (n > 0):
                errs.append(f"P{i + 1} shade {m['shade']} (same character and costume before: {n})")
            continue
        if m["shade"] != want_shade:
            errs.append(f"P{i + 1} shade {m['shade']} != {want_shade}")
    return errs


def run_scenario(name: str, args: argparse.Namespace) -> Dict[str, Any]:
    sc = SCENARIOS[name]
    out = ART / (name + ("-synctest" if args.synctest else ""))
    r = Run(f"npsetup-{name}", cpu_thread=args.cpu == "dc", out=out,
            video=args.video or ("Null" if args.synctest else ppboot.DEFAULT_VIDEO))
    rep = r.rep
    rep.update(scenario=name, with_synctest=args.synctest)
    errs: List[str] = []
    try:
        log = r.inst.user_dir / "Logs" / "dolphin.log"
        r.to_connected_css()
        text = log.read_text(errors="replace") if log.exists() else ""
        rep["plugin_loaded"] = "Loaded plugin (PPOnline" in text
        if not rep["plugin_loaded"]:
            errs.append("the plugin did not load")
        ports = sum(1 << i for i, p in enumerate(sc["players"]) if p)
        if args.synctest:
            # The removal from a game frame (the test hook), written before the match: g_GameFrame
            # restarts with the match; `gone` 20 frames after the session's rollback starts, `gone2`
            # 100 frames later (inside the first 200 frames, which the mismatch log keeps).
            if r.block().version >= 4:   # (an older plugin, for a baseline: no removal)
                ppom.set_test_gone(r.c, r.block(), args.start_frame + 20, sc.get("gone", 0),
                                   sc.get("gone2", 0), 100)
            st = r.c.call("gprb_synctest", distance=args.distance, region_set=args.region_set,
                          hash_regions=True, start_frame=args.start_frame, ports=ports,
                          dedupe_resim_sounds=True)
            rep["armed"] = st.get("phase")
        r.write_session(sc)
        if sc.get("refused"):
            # Dolphin never sends such a setup; the plugin refuses it anyway: no match, the CSS.
            deadline = time.monotonic() + 15
            while time.monotonic() < deadline:
                if r.scene() == "scMelee":
                    errs.append("a match started")
                    break
                time.sleep(0.2)
            rep["scene"] = r.scene()
            rep["last_error"] = hex(ppom.read_debug(r.c, r.block())["lastError"])
            if rep["last_error"] != "0x5e72":
                errs.append(f"lastError {rep['last_error']}")
            r.shot("02-refused")
            return rep
        wait(lambda: r.scene() == "scMelee", 60, "the match")
        r.steps("wait 30")
        errs += check_setup(r, sc)
        wait(lambda: (B.read_match_state(r.c.read_mem).started) or None, 60, "GO")
        r.steps("wait 30")
        r.shot("02-match")
        before = r.players()
        rep["players_start"] = before
        rep["game_frame_start"] = r.game_frame()
        if not args.synctest:
            # What Dolphin's session does from the agreed frame on: the gone flag in SESSION.
            gone = sc["gone"]
            for i in range(4):
                if gone & (1 << i):
                    r.c.write_mem(r.block().session + ppom.S_GONE + i, b"\x01")
        else:
            wait(lambda: r.game_frame() >= args.start_frame + 30, 120, "the first removal")
        r.steps("wait 20")
        mid = r.players()
        se = r.session()
        rep["players_after_first"] = mid
        rep["session_after_first"] = {k: se.get(k) for k in ("gone", "out_count")}
        rep["out_after_first"] = [p.get("out") for p in se["players"]]
        ents, cnt = struct.unpack(">II", r.c.read_mem(0x80624780, 8))
        rep["diag"] = {"entries": hex(ents), "count": cnt, "ft_manager": hex(r.u32(0x80B87C28)),
                       "player_no": [r.u32(ents + k * 0x244 + 0x58) for k in range(min(cnt, 7))],
                       "entry_id": [r.u32(ents + k * 0x244 + 4) for k in range(min(cnt, 7))],
                       "cfg": hex(ppom.read_debug(r.c, r.block())["cfg"])}
        r.shot("03-after-first-removal")
        for i in range(4):
            if not sc["players"][i]:
                continue
            if sc["gone"] & (1 << i):
                if (mid.get(i) or {}).get("action") != OUT_STATUS:
                    errs.append(f"P{i + 1} not in the out status: {mid.get(i)}")
                if (mid.get(i) or {}).get("stocks") not in (0, None):
                    errs.append(f"P{i + 1} still has stocks after removal: {mid.get(i)}")
                if se["players"][i].get("out", 0) != 1:
                    errs.append(f"P{i + 1} out {se['players'][i]['out']} != 1")
            else:
                if mid.get(i, {}).get("stocks") != before.get(i, {}).get("stocks"):
                    errs.append(f"P{i + 1} stocks changed: {before.get(i)} -> {mid.get(i)}")
                if se["players"][i].get("out", 0) != 0:
                    errs.append(f"P{i + 1} marked out")
        if B.read_match_state(r.c.read_mem).game_set:
            errs.append("game set after the first removal")
        if errs:
            raise RuntimeError("the first removal failed")
        r.steps("wait 120")
        r.shot("04-match-goes-on")
        if not args.synctest:
            for i in range(4):
                if sc["gone2"] & (1 << i):
                    r.c.write_mem(r.block().session + ppom.S_GONE + i, b"\x01")
        wait(lambda: B.read_match_state(r.c.read_mem).game_set or None, 180, "game set")
        se = r.session()
        rep["out_at_game_set"] = [p.get("out") for p in se["players"]]
        rep["players_at_game_set"] = r.players()
        # As Dolphin does after a game (once the session has ended: SESSION is rolled-back state):
        # the lobby, waiting for the next game (else the CSS would start this setup again).
        lobby = lambda: ppom.write_session(r.c, r.block(), game=2, stage=sc["stage"], state=1, players=[
            None if p is None else {"char_kind": B.CHAR_KIND[p["c"]], "costume": p.get("costume", 0),
                                    "team": p.get("team", NONE)} for p in sc["players"]],
            teams=sc["teams"], last_winner=sc["winner"])
        if not args.synctest:
            lobby()
        r.shot("05-game-set")
        if args.synctest:
            def ended() -> Any:
                s = r.c.call("gprb_status")
                return s if s["phase"] != "running" else None
            st = wait(ended, 300, "the sync test's end")
            lobby()
            rep["synctest"] = {k: st.get(k) for k in ("phase", "end_reason", "current_frame",
                                                      "desyncs_detected", "region_mismatches",
                                                      "rollbacks", "desync_log", "region_mismatch_log")}
            if st.get("desyncs_detected") or st.get("region_mismatches"):
                errs.append(f"sync test: {st.get('desyncs_detected')} desyncs, "
                            f"{st.get('region_mismatches')} region mismatches")
        winner = sc["winner"]
        left = [i for i, p in rep["players_at_game_set"].items() if (p.get("stocks") or 0) > 0]
        rep["left"] = left
        if winner not in left:
            errs.append(f"the expected winner P{winner + 1} is not left: {left}")
        wait(lambda: r.scene() == "scSelctCharacter", 120, "back on the CSS")
        r.steps("wait 60")
        r.shot("06-back-on-css")
        rep["sel_teams_after"] = r.c.read_mem(r.u32(r.u32(GG) + 0x10) + SEL_TEAMS, 1)[0]
        if rep["sel_teams_after"] != 0:
            errs.append("the CSS's team switch is still on after the match")
    except Exception as e:  # noqa: BLE001
        errs.append(f"{type(e).__name__}: {e}")
        if args.synctest and "synctest" not in rep:
            try:
                st = r.c.call("gprb_status")
                rep["synctest"] = {k: st.get(k) for k in ("phase", "end_reason", "current_frame",
                                                          "desyncs_detected", "region_mismatches",
                                                          "rollbacks", "desync_log", "region_mismatch_log")}
            except Exception:  # noqa: BLE001
                pass
        try:
            r.shot("99-error")
        except Exception:  # noqa: BLE001
            pass
    finally:
        rep["errors"] = errs
        r.close()
        (out / "result.json").write_text(json.dumps(rep, indent=1, default=str))
    return rep


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--scenario", default="ffa-gap", help=f"comma list or 'all' ({', '.join(SCENARIOS)})")
    ap.add_argument("--synctest", action="store_true")
    ap.add_argument("--distance", type=int, default=2)
    ap.add_argument("--start-frame", type=int, default=240)
    ap.add_argument("--region-set", default="gp-v19")
    ap.add_argument("--cpu", default="dc", choices=("sc", "dc"))
    ap.add_argument("--video", default=None,
                    help="video backend (default: D3D11 for screenshots, Null with --synctest: the EFB "
                         "copies a renderer writes into game memory are not gameplay state)")
    a = ap.parse_args()
    names = list(SCENARIOS) if a.scenario == "all" else a.scenario.split(",")
    bad = 0
    for n in names:
        rep = run_scenario(n, a)
        print(json.dumps({k: rep.get(k) for k in ("scenario", "errors", "setup", "out_at_game_set",
                                                  "left", "synctest", "last_error", "scene")},
                         default=str), flush=True)
        bad += bool(rep["errors"])
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
