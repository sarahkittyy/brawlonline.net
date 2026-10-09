"""Coverage sweep of gameplay-only rollback: every P+ v3.2 character, the legal stages, items,
Final Smashes, and two-instance sessions (docs/gprb-coverage.md).

Build: ``run/bin/npd-d2a443a6`` (``--build``; ``PPHARNESS_DOLPHIN_DIR`` wins if set), region
set gp-v21 (``--region-set``).

Runs (``plan()``), each written to ``<out>/runs/<id>.json`` when it finishes, so the sweep is
resumable (finished runs are skipped unless ``--rerun``; ``--retry-errors`` repeats harness errors):

- ``sync`` (single instance, ``gprb_synctest``): every frame the region set from ``--distance``
  frames back is restored and the frames are simulated again; each frame's checksum (frame
  counter, RNGs, per-port fighter instance, damage, stocks, position, status) is compared with
  its first run. Then the **ground truth**: the run's pass log (``PPR_GPRB_PASS_LOG``) is
  flattened (every frame once, with its final input, no rollback: ``gprb_passlog`` ``flat``) and
  replayed from the run's countdown savestate in a second instance; the two per-frame traces must
  be identical (``drift`` otherwise). Failed runs keep the savestate and the pass log in
  ``<out>/work/<id>/``: ``gprb_mispredict.py run --state <sav> --modes replay=<log>`` replays the
  run exactly. Groups:
  - ``vsfox``: each character as P1 against Fox, Battlefield, items off;
  - ``mirror``: each character against itself, Battlefield, items off;
  - ``stage``: Fox vs Falco on each stage of P+'s legal list, items off;
  - ``items``: all items at the highest frequency;
  - ``fs``: Smash Ball only at the highest frequency (Final Smashes, FS transformations);
  - ``ffs``: Smash Ball only, and both ports get their Final Smash at the last countdown frame
    (``PPR_GPRB_FORCE_FINAL=3``, also in the ground truth), so it runs at their first neutral B.
- ``session`` (two instances, ``gprb_connect`` through a netsim preset, single and dual core):
  independent menu paths, each side plays its own port, then the confirmed checksums and the
  per-frame traces are compared.

The match is set up as the existing tools do: Fox and Falco are picked on the CSS, the wanted
characters are written into ``gmSelCharData`` on the stage select (the CSS recipe cannot reach
every icon), and the stage is picked closed-loop. The rules are 4 stocks with a ``--minutes``
time limit, so every match ends in game set (stocks or time). Fighters play seeded random macros
that use all four specials, shields, rolls, spot dodges, grabs and throws, C-stick smashes and
aerials (``--macro-exclude`` leaves categories out, to bisect a failure).

Results per run: result (pass, drift, desync, crash, hang, error), first mismatching frame,
desync log, ground-truth comparison, notable log lines (game OSReport messages, exceptions), the
log tail, the forms each port was seen in (ftKind), a sparse timeline, and screenshots on failure
(instances run headless D3D11; a hung game presents no frame, so the latest periodic screenshot
is kept).

    python harness/tools/gprb_sweep.py run --out run/qa/sweep2 --jobs 3 --retry-errors
    python harness/tools/gprb_sweep.py run --out run/qa/sweep2 --only "^m-peach$" --rerun
    python harness/tools/gprb_sweep.py list
    python harness/tools/gprb_sweep.py report --out run/qa/sweep2 > matrix.md
"""

from __future__ import annotations

import argparse
import contextlib
import json
import os
import random
import re
import shutil
import struct
import sys
import threading
import time
import traceback
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path
from typing import Any, Dict, List, Mapping, Optional, Sequence

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
sys.path.insert(0, str(Path(__file__).resolve().parent))

import gameplay_rollback as G  # noqa: E402
import gprb_session as GS  # noqa: E402
import gprb_synctest as T  # noqa: E402
from gprb_ab import compare_traces  # noqa: E402
from ppharness import brawl as B  # noqa: E402
from ppharness import flows as F  # noqa: E402
from ppharness.client import HarnessClient, HarnessError  # noqa: E402
from ppharness.netsim import NetSim  # noqa: E402

ROOT = Path(__file__).resolve().parents[2]
DEFAULT_BUILD = ROOT / "run" / "bin" / "npd-d2a443a6"   # frozen nplayer-determinism d2a443a6 (gp-v21)
PREFIX = os.environ.get("GPRB_NAME_PREFIX", "sweep")  # instance dir prefix: `python -m ppharness clean --prefix sweep`
MAIN_THREAD = 0x804DD558         # the main OSThread (gprb_synctest's stall report)

# ------------------------------------------------------------------------------------- content

# P+ v3.2 CSS order (PPLUS_CSS_ROSTER) without Random: 42 characters. Pokemon Trainer's three
# Pokemon are independent characters in P+ v3.2, and Zelda/Sheik are separate slots.
_NAME_BY_CSS = {v: k for k, v in B.CSS_ID.items() if k not in ("none", "random")}
CHARACTERS: List[str] = [_NAME_BY_CSS[i] for i in B.PPLUS_CSS_ROSTER if i != B.CSS_RANDOM]

# Legal stages: P+'s 2024 Proposed ruleset (the preset Orca's ranked mode uses; P+ stage kinds),
# plus Final Destination. ``--extended-stages`` adds the rest of P+ v3.2's SSS page 0.
LEGAL_STAGES: Dict[str, int] = {
    "battlefield": 0x01, "final_destination": 0x02, "pokemon_stadium_2": 0x2E, "smashville": 0x21,
    "luigis_mansion": 0x04, "temple_of_time": 0x09, "green_hill_zone": 0x23, "bowsers_castle": 0x06,
    "frigate_husk": 0x0C, "dream_land": 0x2D,
}
PAGE0_OTHER: Dict[str, int] = {f"page0_{k:02x}": k for k in
                               (0x2A, 0x19, 0x43, 0x47, 0x49, 0x1D, 0x0D, 0x1C, 0x1F, 0x05, 0x03)}
STAGES: Dict[str, int] = {**LEGAL_STAGES, **PAGE0_OTHER}

# Record menu data (gmGlobalRecord+0x810): +0 item frequency (P+: 0 none .. 4 very high),
# +8 the item switch (64 bits, vanilla gmItSwitch order; bit 0 of the second word = Smash Ball).
ITEM_FREQ_MAX = 4
ITEM_SWITCH_ALL = bytes.fromhex("ffe7ffffffffffff")       # the game's default: everything
ITEM_SWITCH_SMASH_BALL = bytes.fromhex("0000000000000001")

ITEM_RUNS = [("fox", "falco", "battlefield"), ("mario", "marth", "final_destination"),
             ("peach", "diddy_kong", "smashville"), ("snake", "ice_climbers", "pokemon_stadium_2"),
             ("pikachu", "olimar", "dream_land"), ("rob", "game_and_watch", "battlefield")]
FS_RUNS = [("samus", "zero_suit_samus", "battlefield"), ("wario", "bowser", "final_destination"),
           ("zelda", "sheik", "battlefield"), ("charizard_solo", "squirtle_solo", "smashville"),
           ("ivysaur_solo", "pikachu", "battlefield"), ("ice_climbers", "peach", "final_destination"),
           ("olimar", "lucario", "battlefield")]
# Forced Final Smashes (``ffs``): both ports get their Final Smash at the last countdown frame
# (PPR_GPRB_FORCE_FINAL=3: ftManager::setFinal, as a broken Smash Ball does), in the sync test and in
# its ground-truth replay; the random fighters use it at their first neutral B. One pair per kind:
# transformations, a beam and a suit, a cutscene and a beam, cutscenes, projectiles, a transformation
# and a cutscene, arrows.
FFS_RUNS = [("wario", "bowser", "final_destination"), ("samus", "zero_suit_samus", "battlefield"),
            ("olimar", "lucario", "battlefield"), ("marth", "ike", "battlefield"),
            ("mario", "pikachu", "final_destination"), ("captain_falcon", "ganondorf", "battlefield"),
            ("zelda", "sheik", "battlefield")]
SESSION_RUNS = [("fox", "falco", "battlefield"), ("mario", "marth", "final_destination"),
                ("peach", "game_and_watch", "pokemon_stadium_2"), ("ice_climbers", "olimar", "smashville"),
                ("zelda", "sheik", "battlefield"), ("squirtle_solo", "charizard_solo", "dream_land"),
                ("samus", "zero_suit_samus", "frigate_husk"), ("snake", "rob", "luigis_mansion"),
                ("pikachu", "jigglypuff", "green_hill_zone"), ("diddy_kong", "king_dedede", "temple_of_time"),
                ("link", "toon_link", "bowsers_castle"), ("marth", "roy", "smashville"),
                ("lucario", "mewtwo", "pokemon_stadium_2"), ("sonic", "knuckles", "battlefield"),
                ("wolf", "captain_falcon", "final_destination")]

# Lines in dolphin.log worth reporting: the game's own OSReport text (Japanese assertion
# messages, exception reports), emulator exceptions, gprb warnings.
NOTABLE = re.compile(r"存在しない|Article|changeMotion|force final|fighter change|Exception|exception|DSI|ISI|Invalid (read|write)|"
                     r"Unknown instruction|[Pp]anic|assert|ASSERT|gprb synctest: checksum|gprb: desync|"
                     r"stall|crash|HALT|Unhandled|PROGRAM|\bSRR0\b|OSPanic|Fatal|fatal")
NOISE = re.compile(r"frame_trace|rollback_timings|log_mark")


# 3 and 4 players (docs/nplayer/determinism.md): the scenarios of gprb_synctest (free-for-all,
# team battles with team attack on), then items, forced Final Smashes and 4-player mirrors.
NP_ITEM_RUNS = [(("fox", "falco", "peach", "snake"), "battlefield", None),
                (("ice_climbers", "olimar", "rob", "game_and_watch"), "final_destination", None),
                (("mario", "marth", "pikachu", "diddy_kong"), "smashville", (0, 0, 1, 1))]
NP_FFS_RUNS = [(("wario", "bowser", "zelda", "sheik"), "final_destination", None),
               (("samus", "zero_suit_samus", "olimar", "lucario"), "battlefield", None),
               (("mario", "pikachu", "marth", "ike"), "final_destination", (0, 0, 1, 1)),
               (("ice_climbers", "peach", "charizard_solo", "snake"), "smashville", None)]
NP_MIRRORS = ["ice_climbers", "olimar", "game_and_watch", "yoshi", "peach", "zelda", "meta_knight", "snake"]


def run_chars(run: Mapping[str, Any]) -> List[str]:
    """The run's characters from P1 (2-player runs have only p1 and p2)."""
    return list(run.get("chars") or [run["p1"], run["p2"]])


def run_ports(run: Mapping[str, Any]) -> int:
    return (1 << len(run_chars(run))) - 1


# Per instance (client id): each port's team in a team battle, for sweep_decide.
TEAMS: Dict[int, Dict[int, int]] = {}


def plan(args: argparse.Namespace) -> List[Dict[str, Any]]:
    d, m = args.distance, args.minutes
    runs: List[Dict[str, Any]] = []

    def sync(rid, group, p1, p2, stage="battlefield", items="off", distance=d, cpu="sc", minutes=m,
             force_final=False, chars=None, teams=None):
        r = dict(id=rid, kind="sync", group=group, p1=p1, p2=p2, stage=stage, items=items,
                 distance=distance, cpu=cpu, minutes=minutes, weight=1, force_final=force_final)
        if chars is not None and len(chars) != 2 or teams is not None:
            r.update(chars=list(chars), teams=list(teams) if teams is not None else None)
        runs.append(r)

    def nsync(rid, group, chars, stage, teams=None, **kw):
        sync(rid, group, chars[0], chars[1], stage, chars=chars, teams=teams, **kw)
    for ch in CHARACTERS:
        if ch != "fox":
            sync(f"v-{ch}", "vsfox", ch, "fox")
    for ch in CHARACTERS:
        sync(f"m-{ch}", "mirror", ch, ch)      # m-fox is also Fox's "vs Fox" cell
    stages = STAGES if args.extended_stages else LEGAL_STAGES
    for st in stages:
        sync(f"s-{st}", "stage", "fox", "falco", st)
    for p1, p2, st in ITEM_RUNS:
        sync(f"i-{p1}-{p2}", "items", p1, p2, st, items="all")
    for p1, p2, st in FS_RUNS:
        sync(f"f-{p1}-{p2}", "fs", p1, p2, st, items="smashball")
    for p1, p2, st in FFS_RUNS:
        sync(f"ff-{p1}-{p2}", "ffs", p1, p2, st, items="smashball", force_final=True)
    for name, sc in T.NP_SCENARIOS.items():
        nsync(f"np-{name}", f"np-{name[:2]}", sc.chars, sc.stage, sc.teams)
    for chars, st, teams in NP_ITEM_RUNS:
        nsync(f"npi-{'-'.join(chars)}", "np-items", chars, st, teams, items="all")
    for chars, st, teams in NP_FFS_RUNS:
        nsync(f"npff-{'-'.join(chars)}", "np-ffs", chars, st, teams, items="smashball", force_final=True)
    for ch in NP_MIRRORS:
        nsync(f"npm-{ch}", "np-mirror", (ch,) * 4, "battlefield")
    for cpu in ("sc", "dc"):
        for p1, p2, st in SESSION_RUNS:
            runs.append(dict(id=f"n-{cpu}-{p1}-{p2}", kind="session", group=f"session-{cpu}", p1=p1, p2=p2,
                             stage=st, items="off", cpu=cpu, preset=args.preset, delay=2, minutes=m, weight=2))
    return runs


# ------------------------------------------------------------------------------------- inputs


# Macro categories with their cumulative probability bounds (the order is the draw order).
MACRO_KINDS = (("walk", 0.14), ("back", 0.20), ("jump", 0.27), ("jab", 0.35), ("tilt", 0.40), ("smash", 0.45),
               ("aerial", 0.51), ("nb", 0.57), ("sb", 0.62), ("db", 0.67), ("ub", 0.70), ("shield", 0.76),
               ("dodge", 0.79), ("grab", 0.88), ("crouch", 0.93), ("idle", 1.0))
MACRO_EXCLUDE: set = set()      # --macro-exclude: categories to leave out (bisecting a failure)


def sweep_macro(rng: random.Random, toward: int) -> List[Dict[str, Any]]:
    """One plausible action: movement, jumps, jab, tilts, smashes, aerials, all four specials
    (neutral B also starts a held Final Smash), shield, rolls, spot dodge, grab + throw in one of
    four directions. Never Start, D-pad, L+R or Z (Z is a grab/item throw; R covers it)."""
    t = 255 if toward > 0 else 1
    away = 1 if toward > 0 else 255
    while True:
        r = rng.random()
        kind = next(k for k, bound in MACRO_KINDS if r < bound)
        if kind not in MACRO_EXCLUDE:
            break
    if kind == "walk":
        return [{"main": [t, 128], "hold": rng.randint(4, 18)}]
    if kind == "back":
        return [{"main": [away, 128], "hold": rng.randint(4, 12)}]
    if kind == "jump":
        return [{"buttons": ["X"], "hold": 2}, {"hold": rng.randint(6, 18)}]
    if kind == "jab":
        return [{"buttons": ["A"], "hold": 2}, {"hold": rng.randint(5, 12)}]
    if kind == "tilt":
        d = rng.choice([[t, 128], [128, 220], [128, 40]])
        return [{"main": d, "buttons": ["A"], "hold": 2}, {"hold": 16}]
    if kind == "smash":                                                     # C-stick smashes
        return [{"c": rng.choice([[t, 128], [128, 255], [128, 1]]), "hold": 3}, {"hold": 26}]
    if kind == "aerial":
        return [{"buttons": ["X"], "hold": 2}, {"hold": 4},
                {"main": rng.choice([[128, 128], [t, 128], [128, 255], [128, 1], [away, 128]]),
                 "buttons": ["A"], "hold": 2}, {"hold": 20}]
    if kind == "nb":                                                        # neutral B / Final Smash
        return [{"buttons": ["B"], "hold": rng.choice([2, 2, 30])}, {"hold": 24}]
    if kind == "sb":
        return [{"main": [t, 128], "buttons": ["B"], "hold": 2}, {"hold": 30}]
    if kind == "db":
        return [{"main": [128, 1], "buttons": ["B"], "hold": 2}, {"hold": 34}]
    if kind == "ub":
        return [{"main": [128, 255], "buttons": ["B"], "hold": 2}, {"main": [t, 200], "hold": 36}]
    if kind == "shield":
        return [{"r": 255, "buttons": ["R"], "hold": rng.randint(6, 20)}, {"hold": 3}]
    if kind == "dodge":                                                     # rolls / spot dodge
        return [{"r": 255, "buttons": ["R"], "hold": 3},
                {"r": 255, "buttons": ["R"], "main": rng.choice([[t, 128], [away, 128], [128, 1]]), "hold": 3},
                {"hold": 24}]
    if kind == "grab":                                                      # grab + throw
        throw = rng.choice([[t, 128], [away, 128], [128, 255], [128, 1]])
        return [{"main": [t, 128], "hold": 3}, {"r": 255, "buttons": ["R", "A"], "hold": 2},
                {"hold": rng.randint(4, 14)}, {"main": throw, "hold": 4}, {"hold": 30}]
    if kind == "crouch":
        return [{"main": [128, 1], "hold": rng.randint(4, 10)}]
    return [{"hold": rng.randint(4, 16)}]


# Half-widths of the main platform (x of the ledges), per P+ stage kind; others use the default.
STAGE_EDGE = {0x01: 68.0, 0x02: 85.0, 0x2E: 85.0, 0x21: 66.0, 0x2D: 76.0, 0x23: 75.0}
DEFAULT_EDGE = 62.0


def sweep_decide(self: F.Fighter, st: B.MatchState) -> List[Dict[str, Any]]:
    """flows.Fighter.decide for "random" with the stage in mind: random macros aim at the
    opponent in the middle of the stage and at the centre near the ledges, and off stage the
    fighter double-jumps and up-Bs back. Random play with Fox-like recoveries otherwise
    self-destructs every few seconds and the match is over before anything was tested."""
    me = next((p for p in st.players if p.port == self.seat.port), None)
    if me is None or me.x is None or me.y is None:
        return [{"hold": 2}]
    edge = STAGE_EDGE.get(getattr(self, "stage_kind", -1), DEFAULT_EDGE)
    to_c = 255 if me.x < 0 else 1
    if abs(me.x) > edge + 4 or me.y < -6:
        # Off stage: jump toward the centre, then up-B toward the centre, drift in.
        return [{"main": [to_c, 128], "buttons": ["X"], "hold": 2}, {"main": [to_c, 128], "hold": 12},
                {"main": [to_c, 255], "buttons": ["B"], "hold": 2}, {"main": [to_c, 200], "hold": 40}]
    if abs(me.x) > edge - 6:
        return [{"main": [to_c, 128], "hold": 10}]
    team_of = getattr(self, "team_of", None) or {}
    opps = [p for p in st.players if p.port != self.seat.port and p.x is not None
            and (not team_of or team_of.get(p.port) != team_of.get(self.seat.port))]
    opp = min(opps, key=lambda p: abs((p.x or 0) - me.x)) if opps else None
    dx = (opp.x - me.x) if opp is not None and opp.x is not None else -me.x
    toward = 1 if dx > 0 else -1
    if abs(me.x) > edge * 0.55:
        toward = 1 if me.x < 0 else -1           # near a ledge: face and move toward the centre
    return sweep_macro(self.rng, toward)


_orig_init = F.Fighter.__init__


def _fighter_init(self, seat, mode="chase", seed=0, stage_kind=None):
    _orig_init(self, seat, mode, seed, stage_kind)
    self.stage_kind = stage_kind
    self.team_of = TEAMS.get(id(seat.client))


F._macro = sweep_macro
F.Fighter.__init__ = _fighter_init
F.Fighter.decide = sweep_decide


# ------------------------------------------------------------------------------------- helpers


def configure_rules(c: HarnessClient, items: str, minutes: int) -> None:
    """On the CSS: 4 stocks, ``minutes`` time limit, and the item setting."""
    B.write_rules(c, stocks=4, minutes=minutes, items_off=items == "off")
    if items != "off":
        rec = B.Mem(c.read_mem).chain(B.GAME_GLOBAL_PTR, B.GG_RECORD)
        sw = ITEM_SWITCH_SMASH_BALL if items == "smashball" else ITEM_SWITCH_ALL
        c.write_mem(rec + 0x810, bytes([ITEM_FREQ_MAX]))
        c.write_mem(rec + 0x818, sw)


def write_selections(c: HarnessClient, chars: Sequence[str]) -> None:
    """On the SSS: the wanted characters into gmSelCharData (copied into the match setup when the
    stage select exits); as gprb_synctest/the session do."""
    m = B.Mem(c.read_mem)
    scd = m.chain(B.GAME_GLOBAL_PTR, B.GG_SEL_CHAR_DATA)
    for port, who in enumerate(chars):
        kind = B.CSS_TO_CHAR_KIND[B.CSS_ID[who]]
        c.write_mem(scd + B.SCD_PLAYERS + port * B.MM_PLAYER_SIZE, bytes([kind]))


def log_info(user_dir: Path, tail: int = 40) -> Dict[str, Any]:
    p = user_dir / "Logs" / "dolphin.log"
    out: Dict[str, Any] = {"log_tail": [], "notable": [], "notable_counts": {}}
    try:
        lines = p.read_text(encoding="utf-8", errors="replace").splitlines()
    except OSError:
        return out
    out["log_tail"] = [ln[:400] for ln in lines[-tail:]]
    counts: Dict[str, int] = {}
    order: List[str] = []
    for ln in lines:
        if NOISE.search(ln) or not NOTABLE.search(ln):
            continue
        key = re.sub(r"^\S+\s+\S+\s+", "", ln)                  # drop the timestamp/source prefix
        key = re.sub(r"\b(frame|game frame) \d+", r"\1 N", key)[:300]
        if key not in counts:
            order.append(ln[:400])
        counts[key] = counts.get(key, 0) + 1
    out["notable"] = order[:40]
    out["notable_counts"] = dict(sorted(counts.items(), key=lambda kv: -kv[1])[:20])
    return out


def host_crashed(user_dir: Path) -> Optional[str]:
    """The harness's crash handler line (an exception in Dolphin itself), if any."""
    with contextlib.suppress(OSError):
        with open(user_dir / "Logs" / "dolphin.log", "rb") as f:
            f.seek(0, 2)
            f.seek(max(0, f.tell() - 200_000))
            tail = f.read().decode("utf-8", errors="replace")
        i = tail.find("HARNESS CRASH")
        if i >= 0:
            return " / ".join(x.strip() for x in tail[i:].splitlines()[:3])[:300]
    return None


def first_desync(status: Dict[str, Any]) -> Dict[str, Any]:
    log = status.get("desync_log") or []
    if not log:
        return {}
    m = re.match(r"frame (\d+) \(game frame (\d+)\)", log[0])
    return {"session_frame": int(m.group(1)) if m else None, "game_frame": int(m.group(2)) if m else None,
            "line": log[0]}


VIDEO = {"backend": "D3D11"}


def rel(path: Path) -> str:
    """A path for the report: relative to the workspace root when inside it, else absolute (an
    --out outside the checkout)."""
    path = Path(path).resolve()
    return str(path.relative_to(ROOT)) if path.is_relative_to(ROOT) else str(path)


def shot(c: Optional[HarnessClient], path: Path) -> Optional[str]:
    if c is None:
        return None
    if VIDEO["backend"] == "Null":
        return None          # the Null backend renders nothing; a screenshot request would block the server
    try:
        path.parent.mkdir(parents=True, exist_ok=True)
        c.call("screenshot", path=str(path.resolve()), timeout=20)
        return rel(path)
    except Exception as e:  # noqa: BLE001
        return f"screenshot failed: {e}"


class Forms:
    """ftKinds each port was seen in (transformations: Final Smash forms, Zelda/Sheik, ...)."""

    def __init__(self) -> None:
        self.seen: Dict[int, List[int]] = {}
        self.timeline: List[Any] = []     # every ~10 s: [elapsed, [[port, ftKind, stocks, damage, action]]]
        self._last = 0.0

    def sample(self, c: HarnessClient) -> None:
        with contextlib.suppress(Exception):
            st = B.read_match_state(c.read_mem)
            for p in st.players:
                if p.ft_kind is not None and p.ft_kind not in self.seen.setdefault(p.port, []):
                    self.seen[p.port].append(p.ft_kind)
            if time.monotonic() - self._last > 10:
                self._last = time.monotonic()
                self.timeline.append([st.frames_elapsed, [[p.port, p.ft_kind, p.stocks, round(p.damage or 0, 1), p.action]
                                                           for p in st.players]])

    def named(self) -> Dict[str, List[str]]:
        rev = {v: k for k, v in B.FT_KIND.items()}
        return {f"P{p + 1}": [rev.get(k, hex(k)) for k in ks] for p, ks in sorted(self.seen.items())}


@contextlib.contextmanager
def instance(name: str, cpu: str, video: str, rtc: Optional[int] = G.FIXED_RTC, unthrottled: bool = False,
             env: Optional[Dict[str, str]] = None, controllers: Sequence[int] = (0, 1)):
    """One instance, launched with retries: on a busy machine a launch can lose its harness port
    to another process or answer too slowly."""
    for attempt in range(3):
        inst = G.make_instance(name, cpu_thread=cpu == "dc", video=video, rtc=rtc, controllers=controllers)
        if unthrottled:
            # A sync test resimulates (distance - 1) frames per frame and emulated time keeps
            # running through them; at 100 % speed that caps the game at 60 / distance fps.
            inst.config.config_args = list(inst.config.config_args or []) + ["Dolphin.Core.EmulationSpeed=0"]
        inst.env.update(env or {})
        try:
            inst.launch()
            inst.connect()
            inst.client.timeout = 30.0           # the machine is shared: allow slow answers
            inst.client.wait_state("running", timeout=120)
            for port in controllers:
                inst.client.pad_set(port)
            break
        except Exception as e:  # noqa: BLE001
            print(f"  {name}: launch attempt {attempt + 1} failed: {type(e).__name__}: {str(e)[:200]}", flush=True)
            with contextlib.suppress(Exception):
                inst.kill()
            with contextlib.suppress(Exception):
                inst.cleanup(True)
            if attempt == 2:
                raise
            time.sleep(5)
    try:
        yield inst
    finally:
        with contextlib.suppress(Exception):
            if inst.is_running():
                inst.stop()
        with contextlib.suppress(Exception):
            if inst.is_running():
                inst.kill()


def finish_instance(inst, rep: Dict[str, Any], out: Path, tag: str, failed: bool) -> None:
    """Copy logs of failed runs, record the log summary, remove the instance dir (no 2 GB SD
    images are kept; the logs and the screenshot are the evidence)."""
    info = log_info(inst.user_dir)
    rep.setdefault("logs", {})[tag] = {k: info[k] for k in ("notable", "notable_counts")}
    rep["logs"][tag]["tail"] = info["log_tail"][-25:]
    if failed:
        d = out / "logs"
        d.mkdir(parents=True, exist_ok=True)
        for src, suffix in (("Logs/dolphin.log", "log"), ("harness-stderr.txt", "stderr.txt")):
            with contextlib.suppress(OSError):
                shutil.copy(inst.user_dir / src, d / f"{rep['id']}-{tag}.{suffix}")
    with contextlib.suppress(Exception):
        inst.cleanup(True)
    if inst.user_dir.exists():
        with contextlib.suppress(OSError):
            (inst.user_dir / "Wii" / "sd.raw").unlink()


# ------------------------------------------------------------------------------------- sync test


def extra_flags(args: argparse.Namespace) -> str:
    f = ""
    if args.video != "D3D11":
        f += f" --video {args.video}"
    if args.throttled:
        f += " --throttled"
    if args.macro_exclude:
        f += f" --macro-exclude {args.macro_exclude}"
    if args.extended_stages:
        f += " --extended-stages"
    return f


def force_final_env(run: Dict[str, Any]) -> Dict[str, str]:
    """``ffs`` runs: every port gets its Final Smash before rollback starts (and in the ground truth)."""
    return {"PPR_GPRB_FORCE_FINAL": str(run_ports(run))} if run.get("force_final") else {}


def run_sync(run: Dict[str, Any], args: argparse.Namespace, out: Path) -> Dict[str, Any]:
    rep: Dict[str, Any] = dict(run)
    rep["started"] = time.strftime("%Y-%m-%d %H:%M:%S")
    rep["cmd"] = (f"python harness/tools/gprb_sweep.py run --out {args.out} --only \"^{re.escape(run['id'])}$\" "
                  f"--rerun --distance {run['distance']} --minutes {run['minutes']}{extra_flags(args)}")
    t0 = time.monotonic()
    forms = Forms()
    c: Optional[HarnessClient] = None
    # Ground truth: the run's passes (PPR_GPRB_PASS_LOG) and a countdown savestate; afterwards the
    # same session is replayed with every rollback removed (gprb_passlog flat) and the traces compared.
    work = out / "work" / run["id"]
    shutil.rmtree(work, ignore_errors=True)
    work.mkdir(parents=True, exist_ok=True)
    tag = (work / run["id"]).resolve()
    env = {} if args.no_ground_truth else {"PPR_GPRB_PASS_LOG": str(tag)}
    env.update(force_final_env(run))
    chars = run_chars(run)
    ports = list(range(len(chars)))
    teams = run.get("teams")
    with instance(f"{PREFIX}-{run['id']}", run["cpu"], args.video, unthrottled=not args.throttled, env=env,
                  controllers=ports) as inst:
        failed = True
        # Last resort: a driver call that never returns. Killing Dolphin makes every call fail.
        dog = threading.Timer(args.match_timeout + 900, lambda: (rep.__setitem__("watchdog", True), inst.kill()))
        dog.daemon = True
        dog.start()
        try:
            c = inst.client
            B.wait_scene(c, [B.Scene.CSS], 60 * 120)
            B.wait_css_ready(c, ports)
            configure_rules(c, run["items"], run["minutes"])
            if teams is not None:
                T.css_team_battle(c, 0)
                TEAMS[id(c)] = {p: t for p, t in enumerate(teams)}
            for p in ports:
                B.css_pick_character(c, p, B.CSS_ID["fox" if p % 2 == 0 else "falco"])
            B.css_start(c, 0)
            T.write_selections(c, chars, teams)
            B.sss_pick_stage(c, STAGES[run["stage"]], 0)
            c.call("gprb_synctest", distance=run["distance"], region_set=args.region_set, hash_regions=False,
                   start_frame=args.start_frame, ports=run_ports(run))
            with contextlib.suppress(HarnessError):
                c.call("frame_trace_config", enabled=True)
            if not args.no_ground_truth:
                deadline = time.monotonic() + 300
                while not (c.call("gprb_status")["phase"] == "countdown" and
                           (B.read_frame_counters(c.read_mem).get("game_frame") or 0) >= 60):
                    if time.monotonic() > deadline or not inst.is_running():
                        raise RuntimeError("no countdown to save")
                    time.sleep(0.05)
                c.pause()
                c.save_state(str(tag) + ".sav")
                c.resume()
            deadline = time.monotonic() + 300
            while c.call("gprb_status")["phase"] != "running":
                if time.monotonic() > deadline or not inst.is_running():
                    raise RuntimeError(f"sync test never started: {c.call('gprb_status')}")
                time.sleep(0.5)
            setup = B.read_match_setup(c.read_mem)
            rep["setup"] = {"stage_kind": setup.stage_kind if setup else None,
                            "chars": [p.character for p in setup.players][:len(chars)] if setup else None}
            if len(chars) > 2 or teams is not None:
                sc = T.Scenario(tuple(chars), run["stage"], run["items"] != "off",
                                tuple(teams) if teams is not None else None)
                problems = [x for x in T.check_setup(c, sc)
                            if not x.startswith(("stage", "time limit"))]   # STAGES kinds, --minutes
                if problems:
                    raise RuntimeError(f"match setup: {problems}")
            seats = [F.Seat(c, p) for p in ports]
            rep.update(_play_sync(inst, c, seats, run, args, forms, rep, out, setup))
        except Exception as e:  # noqa: BLE001
            rep["error"] = f"{type(e).__name__}: {e}"
            rep["traceback"] = traceback.format_exc()[-2000:]
        rep["exit_code"] = inst.process.poll() if inst.process else None
        with contextlib.suppress(Exception):
            TEAMS.pop(id(inst.client), None)
        rep["forms"] = forms.named()
        rep["timeline"] = forms.timeline
        rep["wall_s"] = round(time.monotonic() - t0, 1)
        dog.cancel()
        rep["host_crash"] = host_crashed(inst.user_dir)
        classify_sync(rep)
        failed = rep["result"] != "pass"
        if failed and rep["exit_code"] is None and "screenshot" not in rep:
            rep["screenshot"] = shot(c, out / "shots" / f"{run['id']}.png")
        lasts = sorted((p for p in (out / "shots").glob(f"{run['id']}-last?.png") if p.stat().st_size > 0),
                       key=lambda p: p.stat().st_mtime) if (out / "shots").exists() else []
        if failed and lasts:
            rep["last_screenshot"] = rel(lasts[-1])
            lasts = lasts[:-1]
        for p in lasts:
            with contextlib.suppress(OSError):
                p.unlink()
        finish_instance(inst, rep, out, "A", failed)
    trace = rep.pop("_trace", None)
    if not args.no_ground_truth and rep["result"] in ("pass", "desync") and trace:
        try:
            ground_truth(run, args, out, tag, trace, rep)
        except Exception as e:  # noqa: BLE001
            rep["ground_truth"] = {"error": f"{type(e).__name__}: {e}"}
        gt = rep.get("ground_truth") or {}
        if gt.get("error"):
            rep["result"], rep["symptom"] = "error", f"ground truth: {gt['error']}"
        elif gt.get("diverged_at") is not None:
            what = (f"differs from no-rollback play from game frame {gt['diverged_at']} "
                    f"({', '.join(sorted(gt.get('fields', [])))})")
            rep["symptom"] = f"{rep['symptom']}; {what}" if rep["result"] != "pass" else what
            if rep["result"] == "pass":
                rep["result"] = "drift"
    if (rep["result"] in ("pass", "error") and not args.keep_work) or args.no_ground_truth:
        shutil.rmtree(work, ignore_errors=True)
    else:
        # Keep the countdown state and the pass log: `gprb_mispredict.py run --state <sav>
        # --modes replay=<log>` replays this run exactly.
        rep["replay"] = {"state": rel(work / (run["id"] + ".sav")),
                         "pass_log": rel(work / (run["id"] + ".synctest.m0"))}
        for p in work.glob("*.flat"):
            with contextlib.suppress(OSError):
                p.unlink()
    return rep


def ground_truth(run: Dict[str, Any], args: argparse.Namespace, out: Path, tag: Path,
                 trace: List[Any], rep: Dict[str, Any]) -> None:
    """Replay the run's pass log with every rollback removed (each frame once, with its final
    input) from the countdown savestate, and compare the per-frame traces."""
    import gprb_passlog as PL
    log = Path(str(tag) + ".synctest.m0")
    sav = Path(str(tag) + ".sav")
    if not log.exists() or not sav.exists():
        rep["ground_truth"] = {"error": f"missing {'pass log' if not log.exists() else 'savestate'}"}
        return
    header, ups = PL.read(str(log))
    last = PL.final_inputs(ups)
    for u in ups:
        u[0] = 0
        u[2] = [[p[0], p[1], last[p[0]]] for p in u[2][-1:]]
    flat = Path(str(tag) + ".flat")
    PL.write(str(flat), header, ups)
    gt: Dict[str, Any] = {}
    gtrace: List[Any] = []
    s: Dict[str, Any] = {}
    t0 = time.monotonic()
    with instance(f"{PREFIX}-{run['id']}-gt", run["cpu"], "Null", unthrottled=True,
                  env=force_final_env(run), controllers=range(len(run_chars(run)))) as inst:
        try:
            c = inst.client
            G.load_fixture(c, sav)
            c.call("frame_trace_config", enabled=True)
            c.call("gprb_synctest", distance=7, region_set=args.region_set, hash_regions=False,
                   start_frame=args.start_frame, replay_path=str(flat.resolve()), ports=run_ports(run))
            c.resume()
            last_frame, since = -1, time.monotonic()
            while True:
                time.sleep(1)
                s = c.call("gprb_status")
                if s["phase"] in ("ended", "error"):
                    break
                if s["current_frame"] != last_frame:
                    last_frame, since = s["current_frame"], time.monotonic()
                elif time.monotonic() - since > args.hang_s:
                    gt["error"] = f"replay stalled at session frame {last_frame}"
                    break
                if time.monotonic() - t0 > args.match_timeout:
                    gt["error"] = "replay took too long"
                    break
            time.sleep(1)
            gtrace = c.call("frame_trace", since=args.start_frame)["rows"]
            gt["end"] = {k: s.get(k) for k in ("phase", "current_frame", "end_reason", "error", "rollbacks")}
        except Exception as e:  # noqa: BLE001
            gt["error"] = f"{type(e).__name__}: {e}"
        finally:
            with contextlib.suppress(Exception):
                inst.cleanup(True)
    gt["wall_s"] = round(time.monotonic() - t0, 1)
    if gtrace and not gt.get("error"):
        hi = min(max(r[0] for r in trace), max(r[0] for r in gtrace)) - 8
        cmp = compare_traces([r for r in trace if r[0] <= hi], [r for r in gtrace if r[0] <= hi])
        gt.update({"compared": cmp["compared"], "from": cmp["from"], "to": cmp["to"],
                   "diverged_at": cmp["diverged_at"], "fields": list(cmp["first_diff"]),
                   "first_diff": cmp["first_diff"], "game_set_frame": cmp["game_set_frame"]})
    rep["ground_truth"] = gt


def _play_sync(inst, c: HarnessClient, seats, run, args, forms: Forms, rep, out: Path, setup) -> Dict[str, Any]:
    res: Dict[str, Any] = {}
    stop_at = time.monotonic() + args.match_timeout
    progress: Dict[str, Any] = {"frame": -1, "since": time.monotonic(), "last": 0.0}
    t_start = time.monotonic()
    first_bad = {"frame": None}

    def stop() -> bool:
        now = time.monotonic()
        if now > stop_at:
            res["timeout"] = True
            return True
        if not inst.is_running():
            return True
        if now - progress["last"] < 3:
            return False
        progress["last"] = now
        try:
            s = c.call("gprb_status")
        except HarnessError:
            if host_crashed(inst.user_dir):
                return True
            if now - progress["since"] > args.hang_s:
                res["hang"] = {"frame": progress["frame"], "harness": "not answering"}
                return True
            return False
        forms.sample(c)
        if now - progress.get("shot", 0.0) > args.shot_every and s["phase"] == "running":
            # A hung or crashed game presents no new frame, so a screenshot taken at the failure
            # never completes: keep the latest periodic one instead (deleted when the run passes).
            # Two alternating files: a request that times out (no new frame) can leave its file
            # deleted, and must not take the previous good screenshot with it.
            progress["shot"] = now
            progress["shot_n"] = progress.get("shot_n", 0) + 1
            p = out / "shots" / f"{run['id']}-last{progress['shot_n'] % 2}.png"
            if not (shot(c, p) or "failed").startswith("screenshot failed"):
                res["last_shot"] = {"frame": s["current_frame"], "wall_s": round(now - t_start), "file": p.name}
        if now - progress.get("logged", 0.0) > 15:
            progress["logged"] = now
            res.setdefault("progress", []).append([round(now - t_start), s["current_frame"], s["phase"],
                                                   s.get("desyncs_detected"), s.get("rollbacks")])
            if len(res["progress"]) % 8 == 0:
                print(f"  [{run['id']}] {round(now - t_start)} s: frame {s['current_frame']} phase {s['phase']} "
                      f"desyncs {s.get('desyncs_detected')}", flush=True)
        if s["current_frame"] != progress["frame"]:
            progress["frame"], progress["since"] = s["current_frame"], now
        elif now - progress["since"] > args.hang_s:
            res["hang"] = {"frame": s["current_frame"], "status": {k: s.get(k) for k in (
                "phase", "current_frame", "rollbacks", "desyncs_detected", "end_reason", "error")}}
            res["screenshot"] = shot(c, out / "shots" / f"{run['id']}.png")
            with contextlib.suppress(Exception):
                res["hang"]["cpu"] = c.call("cpu_state")
            with contextlib.suppress(Exception):
                from gprb_debug import thread_backtrace
                res["hang"]["main_thread"] = thread_backtrace(c, MAIN_THREAD)
            return True
        if s.get("desyncs_detected") and first_bad["frame"] is None:
            first_bad["frame"] = s["current_frame"]
            res["screenshot"] = shot(c, out / "shots" / f"{run['id']}.png")
            # What the fighters were doing just before the first mismatch (status kinds per port).
            gf = first_desync(s).get("game_frame")
            if gf:
                with contextlib.suppress(Exception):
                    rows = c.call("frame_trace", since=max(0, gf - 30), max=400)["rows"]
                    res["trace_before_desync"] = [
                        [r[0], r[10], r[4], [[r[11][6 * p + 5], round(struct.unpack(">f", struct.pack(">I", r[11][6 * p + 3]))[0], 1)]
                                             for p in range(len(run_chars(run)))]] for r in rows if r[0] <= gf + 2]
        if first_bad["frame"] is not None and s["current_frame"] - first_bad["frame"] > args.after_desync:
            res["stopped_after_desync"] = True
            return True
        if run["group"] == "stage" and "stage_shot" not in res and s["current_frame"] > 120:
            res["stage_shot"] = shot(c, out / "stages" / f"{run['stage']}.png")
        return s["phase"] != "running"

    # The fight loop also returns on a transient harness error or a read that did not see the
    # match: resume until the session ends, the process dies, a hang or the wall-clock limit.
    rounds = 0
    while not stop():
        rounds += 1
        t_round = time.monotonic()
        with contextlib.suppress(HarnessError):
            F.fight(seats, 10 ** 6, mode=["random"] * len(seats), seed=f"{run['id']}-{rounds}",
                    stage_kind=setup.stage_kind if setup else None, stop=stop)
        with contextlib.suppress(HarnessError):
            if c.call("gprb_status")["phase"] != "running":
                break
        if time.monotonic() - t_round < 2:
            time.sleep(2)
    res["fight_rounds"] = rounds
    if inst.is_running():
        with contextlib.suppress(Exception):
            res["status"] = c.call("gprb_status")
            st = res["status"]
            res["status"]["desync_log"] = (st.get("desync_log") or [])[:20]
            res["status"].pop("region_mismatch_log", None)
        with contextlib.suppress(Exception):
            res["final"] = G.small_state(c)
        with contextlib.suppress(Exception):
            time.sleep(1)
            res["_trace"] = c.call("frame_trace", since=args.start_frame)["rows"]
    return res


def classify_sync(rep: Dict[str, Any]) -> None:
    s = rep.get("status") or {}
    rep["frames"] = s.get("current_frame")
    rep["rollbacks"] = s.get("rollbacks")
    rep["desyncs"] = s.get("desyncs_detected")
    rep["end_reason"] = s.get("end_reason")
    rep["first_mismatch"] = first_desync(s)
    if rep.get("hang"):
        # A hang can end in a Dolphin crash later (seen while the harness reads the stuck game).
        rep["result"] = "hang"
        rep["symptom"] = f"no progress at session frame {rep['hang']['frame']}"
        if rep.get("host_crash") or rep.get("exit_code") is not None:
            rep["symptom"] += f"; later Dolphin crashed: {rep.get('host_crash') or rep.get('exit_code')}"
    elif rep.get("exit_code") is not None or rep.get("host_crash"):
        rep["result"] = "crash"
        rep["symptom"] = rep.get("host_crash") or f"Dolphin exited ({rep['exit_code']})"
    elif s.get("desyncs_detected"):
        rep["result"], rep["symptom"] = "desync", rep["first_mismatch"].get("line", "")
    elif rep.get("error"):
        rep["result"], rep["symptom"] = "error", rep["error"]
    elif s.get("end_reason") == "game set":
        rep["result"], rep["symptom"] = "pass", ""
    elif rep.get("timeout"):
        rep["result"], rep["symptom"] = "pass", "wall-clock limit before game set"
    else:
        rep["result"], rep["symptom"] = "error", f"ended without game set: phase {s.get('phase')} {s.get('end_reason')!r} {s.get('error', '')}"


# ------------------------------------------------------------------------------------- sessions


def _history_a(c: HarnessClient, chars, stage: str, items: str, minutes: int, notes: List[str]) -> None:
    B.wait_scene(c, [B.Scene.CSS], 60 * 90)
    B.wait_css_ready(c, [0, 1])
    notes.append(f"CSS at poll {c.status().input_polls}")
    configure_rules(c, items, minutes)
    B.css_pick_character(c, 0, B.CSS_ID["fox"])
    B.css_pick_character(c, 1, B.CSS_ID["falco"])
    B.css_start(c, 0)
    write_selections(c, chars)
    B.sss_pick_stage(c, STAGES[stage], 0)


def _history_b(c: HarnessClient, chars, stage: str, items: str, minutes: int, notes: List[str]) -> None:
    """G.history_b's detour (other picks, back to the main menu, Rules, back), then as A."""
    B.wait_scene(c, [B.Scene.CSS], 60 * 90)
    B.wait_css_ready(c, [0, 1])
    B.step(c, 137)
    B.css_pick_character(c, 0, B.CSS_ID["mario"])
    B.css_pick_character(c, 1, B.CSS_ID["ike"])
    G.css_drop_token(c, 0)
    G.css_drop_token(c, 1)
    B.neutral(c, 0)
    c.pad_script(0, [{"buttons": ["B"], "hold": 90}, {"hold": 5}])
    B.wait_scene(c, [B.Scene.MAIN_MENU], 600, every=4)
    notes.append(f"main menu at poll {c.status().input_polls}")
    B.step(c, 60)
    c.pad_script(0, [dict(G.NEUTRAL, main=[255, 128], hold=4), dict(G.NEUTRAL, hold=20)])
    B.step(c, 30)
    B.tap(c, 0, ["A"], 3, 90)
    B.tap(c, 0, ["B"], 3, 40)
    c.pad_script(0, [dict(G.NEUTRAL, main=[1, 128], hold=4), dict(G.NEUTRAL, hold=20)])
    B.step(c, 41)
    B.tap(c, 0, ["A"], 3, 10)
    B.wait_scene(c, [B.Scene.CSS], 1200, every=4)
    B.wait_css_ready(c, [0, 1])
    B.step(c, 53)
    configure_rules(c, items, minutes)
    for port in (1, 0):
        a = B.read_css_area(c.read_mem, port)
        if a is not None and a.placed:
            G.css_drop_token(c, port)
    B.css_pick_character(c, 1, B.CSS_ID["falco"])
    B.css_pick_character(c, 0, B.CSS_ID["fox"])
    B.css_start(c, 0)
    B.step(c, 77)
    write_selections(c, chars)
    B.sss_pick_stage(c, STAGES[stage], 0)


def run_session(run: Dict[str, Any], args: argparse.Namespace, out: Path) -> Dict[str, Any]:
    rep: Dict[str, Any] = dict(run)
    rep["started"] = time.strftime("%Y-%m-%d %H:%M:%S")
    rep["cmd"] = (f"python harness/tools/gprb_sweep.py run --out {args.out} --only \"^{re.escape(run['id'])}$\" "
                  f"--rerun --minutes {run['minutes']} --preset {run['preset']}{extra_flags(args)}")
    t0 = time.monotonic()
    sim: Optional[NetSim] = None
    forms = Forms()
    clients: List[Optional[HarnessClient]] = [None, None]
    stack = contextlib.ExitStack()
    insts = []
    try:
        insts = [stack.enter_context(instance(f"{PREFIX}-{run['id']}-a", run["cpu"], args.video)),
                 stack.enter_context(instance(f"{PREFIX}-{run['id']}-b", run["cpu"], args.video, rtc=0x69C9A3D0))]
    except Exception as e:  # noqa: BLE001
        rep["error"] = f"launch: {type(e).__name__}: {e}"
    dog = threading.Timer(args.match_timeout + 1200, lambda: (rep.__setitem__("watchdog", True),
                                                              [i.kill() for i in insts]))
    dog.daemon = True
    dog.start()
    try:
        if len(insts) == 2:
            ca, cb = clients[0], clients[1] = insts[0].client, insts[1].client
            G.par([lambda: B.wait_scene(ca, [B.Scene.CSS], 60 * 120), lambda: B.wait_scene(cb, [B.Scene.CSS], 60 * 120)])
            host_port = GS.free_udp_port()
            ca.call("gprb_connect", role="host", port=host_port, region_set=args.region_set, delay=run["delay"],
                    name="A", hash_regions=False, start_frame=args.start_frame)
            sim = NetSim(("127.0.0.1", host_port), ("127.0.0.1", 0), run["preset"], seed=f"sweep-{run['id']}").start()
            cb.call("gprb_connect", role="join", host="127.0.0.1", remote_port=sim.listen_port,
                    region_set=args.region_set, delay=run["delay"], name="B", hash_regions=False,
                    start_frame=args.start_frame)
            GS.wait(lambda: all(c.call("gprb_status")["phase"] == "connected" for c in (ca, cb)), 30, "connect")
            ca.call("gprb_set_selections", selections={"character": run["p1"], "stage": run["stage"], "match": 0})
            cb.call("gprb_set_selections", selections={"character": run["p2"], "match": 0})
            GS.wait(lambda: all(c.call("gprb_status")["peer"]["selections"].get("match") == 0 for c in (ca, cb)),
                    10, "selections")
            chars = [run["p1"], run["p2"]]
            notes: Dict[str, List[str]] = {"A": [], "B": []}
            G.par([lambda: _history_a(ca, chars, run["stage"], run["items"], run["minutes"], notes["A"]),
                   lambda: _history_b(cb, chars, run["stage"], run["items"], run["minutes"], notes["B"])])
            rep["notes"] = notes
            for c in (ca, cb):
                for port in (0, 1):
                    c.pad_set(port)
            GS.wait(lambda: all(c.call("gprb_status")["phase"] in ("running", "ended", "error") for c in (ca, cb)),
                    180, "both sessions to start")
            st = [c.call("gprb_status") for c in (ca, cb)]
            if any(s["phase"] != "running" for s in st):
                raise RuntimeError(f"session did not start: {[s['phase'] + ' ' + str(s.get('error')) for s in st]}")
            sim.reset_epoch()
            setup = B.read_match_setup(ca.read_mem)
            rep["setup"] = {"stage_kind": setup.stage_kind if setup else None,
                            "chars": [p.character for p in setup.players][:2] if setup else None}
            _play_session(insts, ca, cb, run, args, forms, rep, setup)
    except Exception as e:  # noqa: BLE001
        rep["error"] = f"{type(e).__name__}: {e}"
        rep["traceback"] = traceback.format_exc()[-2000:]
    finally:
        if sim is not None:
            with contextlib.suppress(Exception):
                rep["netsim"] = sim.format_stats()
            with contextlib.suppress(Exception):
                sim.stop()
    dog.cancel()
    rep["exit_codes"] = [i.process.poll() if i.process else None for i in insts]
    rep["forms"] = forms.named()
    rep["timeline"] = forms.timeline
    fb = rep.pop("_formsB", None)
    rep["timeline_B"] = fb.timeline if fb else []
    rep["wall_s"] = round(time.monotonic() - t0, 1)
    rep["host_crash"] = [host_crashed(i.user_dir) for i in insts]
    classify_session(rep)
    failed = rep["result"] != "pass"
    if failed:
        for tag, inst, c in zip("AB", insts, clients):
            if inst.process and inst.process.poll() is None and f"screenshot_{tag}" not in rep:
                rep[f"screenshot_{tag}"] = shot(c, out / "shots" / f"{run['id']}-{tag}.png")
    for c in clients:
        if c is not None:
            with contextlib.suppress(Exception):
                c.call("gprb_stop")
    stack.close()
    for tag, inst in zip("AB", insts):
        finish_instance(inst, rep, out, tag, failed)
    return rep


def _play_session(insts, ca, cb, run, args, forms: Forms, rep, setup) -> None:
    seats = [F.Seat(ca, 0, 0, "A"), F.Seat(cb, 1, 0, "B")]
    stop_at = time.monotonic() + args.match_timeout
    stop_flag = threading.Event()

    def stop() -> bool:
        return stop_flag.is_set() or time.monotonic() > stop_at

    def play(seat: F.Seat, seed: str) -> None:
        rounds = 0
        while not stop() and rounds < 30:
            rounds += 1
            with contextlib.suppress(HarnessError):
                F.fight([seat], 10 ** 6, mode="random", seed=f"{seed}-{rounds}",
                        stage_kind=setup.stage_kind if setup else None, stop=stop)
            with contextlib.suppress(HarnessError):
                if seat.client.call("gprb_status")["phase"] != "running":
                    break
        stop_flag.set()

    th = [threading.Thread(target=play, args=(s, f"{run['id']}-{s.name}"), daemon=True) for s in seats]
    for t in th:
        t.start()
    prog = {"frames": None, "since": time.monotonic()}
    while any(t.is_alive() for t in th):
        time.sleep(3)
        if not all(i.is_running() for i in insts):
            stop_flag.set()
            break
        try:
            st = [c.call("gprb_status") for c in (ca, cb)]
        except HarnessError:
            if any(host_crashed(i.user_dir) for i in insts):
                stop_flag.set()
                break
            if time.monotonic() - prog["since"] > args.hang_s:
                rep["hang"] = {"frames": prog["frames"], "harness": "not answering"}
                stop_flag.set()
                break
            continue
        forms.sample(ca)
        rep.setdefault("_formsB", Forms()).sample(cb)
        fr = [s["current_frame"] for s in st]
        if fr != prog["frames"]:
            prog["frames"], prog["since"] = fr, time.monotonic()
        elif time.monotonic() - prog["since"] > args.hang_s:
            rep["hang"] = {"frames": fr, "phases": [s["phase"] for s in st]}
            for tag, c in zip("AB", (ca, cb)):
                rep[f"screenshot_{tag}"] = shot(c, Path(args.out_abs) / "shots" / f"{run['id']}-{tag}.png")
            for tag, c in zip("AB", (ca, cb)):
                with contextlib.suppress(Exception):
                    from gprb_debug import thread_backtrace
                    rep["hang"][f"main_thread_{tag}"] = thread_backtrace(c, MAIN_THREAD)
            stop_flag.set()
        if any(s["phase"] != "running" for s in st):
            stop_flag.set()
    stop_flag.set()
    for t in th:
        t.join(timeout=60)
    if not all(i.is_running() for i in insts) or rep.get("hang") or any(host_crashed(i.user_dir) for i in insts):
        return
    with contextlib.suppress(RuntimeError):
        GS.wait(lambda: all(c.call("gprb_status")["phase"] != "running" for c in (ca, cb)), 60, "both to end")
    st = [c.call("gprb_status") for c in (ca, cb)]
    for s in st:
        s.pop("region_mismatch_log", None)
        s["desync_log"] = (s.get("desync_log") or [])[:10]
    limit = min(s["current_frame"] for s in st) - GS.CONFIRM_MARGIN
    cks = [c.call("gprb_checksums", since=0)["rows"] for c in (ca, cb)]
    rep["checksums"] = GS.compare_checksums(cks[0], cks[1], limit)
    tr = [c.call("frame_trace", since=0)["rows"] for c in (ca, cb)]
    gf_limit = limit + args.start_frame
    rep["trace"] = compare_traces([r for r in tr[0] if r[0] <= gf_limit], [r for r in tr[1] if r[0] <= gf_limit])
    rep["status"] = st
    with contextlib.suppress(Exception):
        rep["final"] = [G.small_state(c) for c in (ca, cb)]


def classify_session(rep: Dict[str, Any]) -> None:
    st = rep.get("status") or [{}, {}]
    ck = rep.get("checksums") or {}
    trc = rep.get("trace") or {}
    rep["frames"] = [s.get("current_frame") for s in st]
    rep["rollbacks"] = [s.get("rollbacks") for s in st]
    rep["max_rollback"] = [s.get("max_rollback_frames") for s in st]
    rep["end_reason"] = [s.get("end_reason") for s in st]
    rep["first_mismatch"] = {"confirmed_checksum_frame": ck.get("first_mismatch"),
                             "trace_game_frame": trc.get("diverged_at"),
                             "trace_fields": trc.get("first_diff")}
    codes = rep.get("exit_codes") or []
    crashed = any(c is not None for c in codes) or any(rep.get("host_crash") or [])
    if rep.get("hang"):
        rep["result"], rep["symptom"] = "hang", f"no progress at frames {rep['hang']['frames']}"
        if crashed:
            rep["symptom"] += f"; later Dolphin crashed {codes} {[h for h in rep.get('host_crash') or [] if h]}"
    elif crashed:
        rep["result"] = "crash"
        rep["symptom"] = f"Dolphin exited {codes} {[h for h in rep.get('host_crash') or [] if h]}"
    elif ck.get("mismatches") or (trc.get("diverged_at") is not None):
        rep["result"] = "desync"
        rep["symptom"] = (f"{ck.get('mismatches')} of {ck.get('compared')} confirmed checksums differ (first frame "
                          f"{ck.get('first_mismatch')}); trace diverged at game frame {trc.get('diverged_at')} "
                          f"in {sorted((trc.get('first_diff') or {}).keys())}")
    elif rep.get("error"):
        rep["result"], rep["symptom"] = "error", rep["error"]
    elif not ck.get("compared"):
        rep["result"], rep["symptom"] = "error", "nothing compared"
    elif all(r == "game set" for r in rep["end_reason"]):
        rep["result"], rep["symptom"] = "pass", ""
    else:
        rep["result"], rep["symptom"] = "pass", f"compared up to the wall-clock limit (end {rep['end_reason']})"


# ------------------------------------------------------------------------------------- driver


class Slots:
    """At most ``n`` Dolphin instances at a time (a session takes two)."""

    def __init__(self, n: int):
        self.n, self.used = n, 0
        self.cv = threading.Condition()
        self.queue: List[int] = []           # first come, first served: a session is not starved
        self.ticket = 0

    @contextlib.contextmanager
    def take(self, w: int):
        w = min(w, self.n)
        with self.cv:
            self.ticket += 1
            me = self.ticket
            self.queue.append(me)
            self.cv.wait_for(lambda: self.queue[0] == me and self.used + w <= self.n)
            self.queue.pop(0)
            self.used += w
            self.cv.notify_all()
        try:
            yield
        finally:
            with self.cv:
                self.used -= w
                self.cv.notify_all()


def _stack_dumper(out: Path) -> None:
    """Touch ``<out>/dump-stacks`` to get every driver thread's stack in ``<out>/stacks.txt``."""
    trigger = out / "dump-stacks"
    while True:
        time.sleep(5)
        if trigger.exists():
            with contextlib.suppress(OSError):
                trigger.unlink()
            names = {t.ident: t.name for t in threading.enumerate()}
            with open(out / "stacks.txt", "a", encoding="utf-8") as f:
                f.write(f"==== {time.strftime('%H:%M:%S')}\n")
                for ident, frame in sys._current_frames().items():
                    stack = "".join(traceback.format_stack(frame))
                    f.write(f"--- thread {names.get(ident, ident)}\n{stack}\n")


def cmd_run(args: argparse.Namespace) -> int:
    out = Path(args.out)
    if not out.is_absolute():
        out = ROOT / out
    args.out_abs = str(out)
    (out / "runs").mkdir(parents=True, exist_ok=True)
    runs = plan(args)
    if args.only:
        runs = [r for r in runs if re.search(args.only, r["id"])]
    if args.groups:
        gs = set(args.groups.split(","))
        runs = [r for r in runs if r["group"] in gs or r["kind"] in gs
                or ("np" in gs and r["group"].startswith("np-"))]
    todo = []
    for r in runs:
        p = out / "runs" / f"{r['id']}.json"
        if p.exists() and not args.rerun:
            prev = json.loads(p.read_text(encoding="utf-8"))
            if not (args.retry_errors and prev.get("result") == "error"):
                continue
        todo.append(r)
    print(f"{len(runs)} runs selected, {len(todo)} to do, {args.jobs} instance slots", flush=True)
    if args.dry_run:
        for r in todo:
            print(" ", r["id"])
        return 0
    threading.Thread(target=_stack_dumper, args=(out,), daemon=True).start()
    slots = Slots(args.jobs)
    lock = threading.Lock()
    done = [0]

    def one(r: Dict[str, Any]) -> None:
        with slots.take(r["weight"]):
            print(f"[{time.strftime('%H:%M:%S')}] start {r['id']}", flush=True)
            fn = run_sync if r["kind"] == "sync" else run_session
            try:
                rep = fn(r, args, out)
            except Exception as e:  # noqa: BLE001
                rep = dict(r, result="error", symptom=f"driver: {type(e).__name__}: {e}",
                           traceback=traceback.format_exc()[-2000:])
            (out / "runs" / f"{r['id']}.json").write_text(json.dumps(rep, indent=1, ensure_ascii=False, default=str),
                                                          encoding="utf-8")
            with lock:
                done[0] += 1
                print(f"[{time.strftime('%H:%M:%S')}] {done[0]}/{len(todo)} {r['id']}: {rep['result'].upper()} "
                      f"frames {rep.get('frames')} {rep.get('symptom', '')[:160]} ({rep.get('wall_s')} s)", flush=True)

    # Sessions first in their own phase would idle slots; interleave by weight instead.
    with ThreadPoolExecutor(args.jobs) as pool:
        list(pool.map(one, todo))
    return 0


def cmd_list(args: argparse.Namespace) -> int:
    for r in plan(args):
        print(f"{r['id']:45} {r['kind']:8} {r['group']:11} {r['p1']:>16} vs {r['p2']:<16} {r['stage']:18} items={r['items']}")
    print(len(plan(args)), "runs")
    return 0


def load_results(out: Path) -> Dict[str, Dict[str, Any]]:
    res = {}
    for p in sorted((out / "runs").glob("*.json")):
        with contextlib.suppress(Exception):
            r = json.loads(p.read_text(encoding="utf-8"))
            res[r["id"]] = r
    return res


def _abs(p: str) -> Path:
    q = Path(p)
    return q if q.is_absolute() else ROOT / q


def cmd_report(args: argparse.Namespace) -> int:
    """Markdown matrices: the main sweep (``--out``) and, if given, a second pass of the same
    runs with other inputs (``--compare``, e.g. the no-C-stick pass) as an extra column."""
    res = load_results(_abs(args.out))
    cmp_ = load_results(_abs(args.compare)) if args.compare else {}
    mark = {"pass": "pass", "desync": "**DESYNC**", "crash": "**CRASH**", "hang": "**HANG**", "error": "error",
            "drift": "**DRIFT**"}

    def cell(rid: str, src: Dict[str, Dict[str, Any]] = res) -> str:
        r = src.get(rid)
        if r is None:
            return "-"
        s = mark.get(r["result"], r["result"])
        if r["kind"] == "sync" and r["result"] != "pass":
            fm = (r.get("first_mismatch") or {}).get("game_frame") or (r.get("ground_truth") or {}).get("diverged_at")
            fr = (r.get("hang") or {}).get("frame")
            if fm:
                s += f" gf {fm}"
            elif fr:
                s += f" @{fr}"
        return s
    cmp_hdr = f" | vs Fox, {args.compare_label}" if cmp_ else ""
    print(f"| Character | vs Fox{cmp_hdr} | Mirror | Forms seen |\n|---|---{'|---' if cmp_ else ''}|---|---|")
    for ch in CHARACTERS:
        v = "m-fox" if ch == "fox" else f"v-{ch}"
        forms = set()
        for src in (res, cmp_):
            for rid in (v, f"m-{ch}"):
                forms.update(((src.get(rid) or {}).get("forms") or {}).get("P1") or [])
        extra = f" | {cell(v, cmp_)}" if cmp_ else ""
        print(f"| {ch} | {cell(v)}{extra} | {cell('m-' + ch)} | {', '.join(sorted(forms))} |")
    print(f"\n| Stage | kind | Fox vs Falco{cmp_hdr.replace('vs Fox, ', '')} |\n|---|---|---{'|---' if cmp_ else ''}|")
    for st, k in STAGES.items():
        if f"s-{st}" in res or f"s-{st}" in cmp_:
            extra = f" | {cell('s-' + st, cmp_)}" if cmp_ else ""
            print(f"| {st} | 0x{k:02X} | {cell('s-' + st)}{extra} |")
    print(f"\n| Items run | items | result{cmp_hdr.replace('vs Fox, ', '')} | forms seen |\n|---|---|---{'|---' if cmp_ else ''}|---|")
    for rid, r in res.items():
        if r.get("group") in ("items", "fs", "ffs"):
            forms = sorted({f for src in (res, cmp_) for v in ((src.get(rid) or {}).get("forms") or {}).values() for f in v})
            extra = f" | {cell(rid, cmp_)}" if cmp_ else ""
            items = r["items"] + (", Final Smash forced" if r.get("force_final") else "")
            print(f"| {r['p1']} vs {r['p2']}, {r['stage']} | {items} | {cell(rid)}{extra} | {', '.join(forms)} |")
    print("\n| Session | stage | single core | dual core |\n|---|---|---|---|")
    for p1, p2, st in SESSION_RUNS:
        print(f"| {p1} vs {p2} | {st} | {cell(f'n-sc-{p1}-{p2}')} | {cell(f'n-dc-{p1}-{p2}')} |")
    nps = [r for r in res.values() if str(r.get("group", "")).startswith("np-")]
    if nps:
        print("\n| 3-4 players | stage | teams | items | result | frames | forms seen |\n|---|---|---|---|---|---|---|")
        for r in sorted(nps, key=lambda r: (r["group"], r["id"])):
            forms = sorted({f for v in (r.get("forms") or {}).values() for f in v})
            items = r["items"] + (", Final Smash forced" if r.get("force_final") else "")
            teams = "-" if not r.get("teams") else " ".join(str(t) for t in r["teams"])
            print(f"| {', '.join(run_chars(r))} | {r['stage']} | {teams} | {items} | {cell(r['id'])} | "
                  f"{r.get('frames')} | {', '.join(forms)} |")
    for label, src in (("main", res), (args.compare_label, cmp_)):
        if not src:
            continue
        n = len(src)
        by = {}
        for r in src.values():
            by.setdefault(r.get("group"), []).append(r["result"])
        print(f"\n{label}: {sum(1 for r in src.values() if r['result'] == 'pass')}/{n} pass; " +
              "; ".join(f"{g} {v.count('pass')}/{len(v)}" for g, v in sorted(by.items())))
    return 0


def main(argv: Optional[Sequence[str]] = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("command", choices=("run", "list", "report"))
    ap.add_argument("--out", default="run/qa/sweep")
    ap.add_argument("--only", default=None, help="regex on run ids")
    ap.add_argument("--groups", default=None,
                    help="comma list: vsfox,mirror,stage,items,fs,ffs,session-sc,session-dc,sync,session; 3-4 "
                         "players: np-f4,np-f3,np-t4,np-t3,np-items,np-ffs,np-mirror, or np for all of them")
    ap.add_argument("--rerun", action="store_true", help="run again even if a result exists")
    ap.add_argument("--retry-errors", action="store_true", help="run again runs whose result is 'error'")
    ap.add_argument("--dry-run", action="store_true")
    ap.add_argument("--jobs", type=int, default=3, help="Dolphin instances at a time (a session uses 2)")
    ap.add_argument("--distance", type=int, default=7, help="sync test: restore the state this many frames back")
    ap.add_argument("--minutes", type=int, default=2, help="time limit of every match (4 stocks)")
    ap.add_argument("--region-set", default="gp-v21")
    ap.add_argument("--build", default=str(DEFAULT_BUILD),
                    help="Dolphin binaries dir (PPHARNESS_DOLPHIN_DIR wins if set)")
    ap.add_argument("--keep-work", action="store_true",
                    help="keep every sync run's countdown state and pass log, passed runs too (replays)")
    ap.add_argument("--no-ground-truth", action="store_true",
                    help="sync tests: skip the no-rollback replay and the trace comparison")
    ap.add_argument("--start-frame", type=int, default=240)
    ap.add_argument("--preset", default="typical", help="netsim preset of the sessions")
    ap.add_argument("--compare", default=None, help="report: a second result dir shown as an extra column")
    ap.add_argument("--compare-label", default="no C-stick", help="report: the extra column's label")
    ap.add_argument("--extended-stages", action="store_true", help="also the rest of SSS page 0")
    ap.add_argument("--throttled", action="store_true", help="sync tests at 100 %% emulation speed (default unthrottled)")
    ap.add_argument("--video", default="D3D11", help="video backend (screenshots need a real one)")
    ap.add_argument("--match-timeout", type=float, default=1500, help="wall-clock limit of one match (s)")
    ap.add_argument("--macro-exclude", default="", help=f"leave out input categories: {','.join(k for k, _ in MACRO_KINDS)}")
    ap.add_argument("--shot-every", type=float, default=40, help="periodic screenshot interval (s), kept on failure")
    ap.add_argument("--hang-s", type=float, default=45, help="no frame progress for this long = hang")
    ap.add_argument("--after-desync", type=int, default=300, help="frames to keep running after a desync")
    args = ap.parse_args(argv)
    os.environ.setdefault("PPHARNESS_DOLPHIN_DIR", args.build)
    args.build = os.environ["PPHARNESS_DOLPHIN_DIR"]
    VIDEO["backend"] = args.video
    MACRO_EXCLUDE.update(k for k in (args.macro_exclude or "").split(",") if k)
    return {"run": cmd_run, "list": cmd_list, "report": cmd_report}[args.command](args)


if __name__ == "__main__":
    sys.exit(main())
