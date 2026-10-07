"""A set of online games from the game's own menus under the gameplay-only (Slippi-style) session.

Shared by ``harness/tests/test_online_game.py`` and ``harness/tools/e2e_launcher.py``. Both
players are connected (Direct, from the online character select); from there everything happens
in the game, driven with controller input only:

- game 1 starts by itself once both are connected (both locked in when they searched); the host's
  Dolphin draws a random stage from the server's list (P+'s legal list by default);
- each game is played closed-loop on each side's own port (random macros) until game set, and
  both peers' confirmed-frame checksums and per-frame traces are compared;
- after game set both games go straight back to the online CSS (no results screen, as Slippi);
- the next game: START locks in again; in Direct the loser of the last game first picks a stage
  on P+'s stage select (Slippi's loser-picks), the winner just locks in.

And through it all, that no game was rebooted: the harness frame counter, the plugin's own frame
counter and the PPOM block address only move forward / stay put (a boot resets them), and no
Dolphin netplay session ever runs.

Each player is any object with ``name``, ``c`` (a HarnessClient), ``shot(name)`` and
``steps(*drive_steps)``.
"""

from __future__ import annotations

import contextlib
import sys
import threading
import time
from pathlib import Path
from typing import Any, Callable, Dict, List, Optional, Sequence

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "harness"))
sys.path.insert(0, str(ROOT / "harness" / "tools"))
sys.path.insert(0, str(ROOT / "tools" / "gamecode"))

import ppom  # noqa: E402
from ppharness import brawl as B  # noqa: E402
from ppharness import flows as F  # noqa: E402
from ppharness.client import HarnessClient, HarnessError  # noqa: E402

CONFIRM_MARGIN = 16      # frames behind the slower peer that can no longer roll back
START_FRAME = 240        # the session's start barrier (gameplay rollback begins after GO)
CSS = "scSelctCharacter"
STAGE_PICK = "final_destination"   # what the Direct loser picks on the stage select


def wait(pred: Callable[[], Any], timeout: float, what: str, every: float = 0.25) -> Any:
    deadline = time.monotonic() + timeout
    last: Optional[BaseException] = None
    while time.monotonic() < deadline:
        try:
            v = pred()
            if v:
                return v
        except (HarnessError, OSError) as e:
            last = e
        time.sleep(every)
    raise TimeoutError(f"timed out after {timeout:.0f}s waiting for {what}"
                       + (f" (last error: {last})" if last else ""))


def both(*fns: Callable[[], Any]) -> List[Any]:
    import concurrent.futures as cf
    with cf.ThreadPoolExecutor(len(fns)) as ex:
        return [f.result() for f in [ex.submit(fn) for fn in fns]]


def scene(c: HarnessClient) -> str:
    return B.read_scene(c.read_mem).name


def gstatus(c: HarnessClient) -> Dict[str, Any]:
    return c.call("gprb_status")


def block(c: HarnessClient) -> ppom.Block:
    return ppom.find_block(c)


def session(c: HarnessClient) -> Dict[str, Any]:
    return ppom.read_session(c, block(c))


def local(c: HarnessClient) -> Dict[str, Any]:
    return ppom.read_local(c, block(c))


def css_panel(c: HarnessClient) -> Dict[str, int]:
    """The local player's panel on the CSS (muSelCharPlayerArea, port 0): the character the coin
    is on (0x28 = none), the costume, the name tag (save index, 0xFFFFFFFF none) and whether the
    coin is in the hand (hand+0xA0 != 0) or placed."""
    u = c.read_u32
    area = u(u(u(u(0x805A0060) + 4) + 0x400) + 0x44)
    hand = u(area + 0x1A8)
    return {"char": u(area + 0x1B8), "costume": u(area + 0x1BC), "tag": u(area + 0x1C8),
            "coin_in_hand": int(u(hand + 0xA0) != 0)}


def check_css_remembers(p: Any, before: Dict[str, int], where: str) -> Dict[str, int]:
    """Slippi: back on the CSS (after a match or the stage select) the character is still
    selected: the coin on it, the same costume and name tag."""
    now = wait(lambda: (lambda x: x if x["coin_in_hand"] == 0 else None)(css_panel(p.c)), 10,
               f"{p.name}: the coin placed again {where}")
    assert (now["char"], now["costume"], now["tag"]) == (before["char"], before["costume"],
                                                         before["tag"]), (p.name, where, before, now)
    return now


STRIKE_TABLE = 0x8042C822   # P+'s stage striking (5 pages x 6 bytes); all zero = nothing struck


# --------------------------------------------------------------------------- no reboot


def boot_marks(c: HarnessClient) -> Dict[str, Any]:
    """What a reboot would reset: the emulated frame count, the plugin's own per-frame counter
    (PPOM DEBUG.frames, in the plugin's .data) and where the plugin's block is."""
    b = block(c)
    return {"frame": c.status().frame, "plugin_frames": ppom.read_debug(c, b)["frames"],
            "block": b.addr, "game_id": c.status().game_id,
            "netplay": bool(c.netplay_status().game_running)}


def check_no_reboot(before: Dict[str, Any], after: Dict[str, Any], who: str) -> None:
    assert after["game_id"] == before["game_id"] == "RSBE01", (who, before, after)
    assert after["frame"] > before["frame"], (who, "emulated frames went back", before, after)
    assert after["plugin_frames"] > before["plugin_frames"], (who, "the plugin restarted", before, after)
    assert after["block"] == before["block"], (who, "the plugin was loaded again", before, after)
    assert not after["netplay"], (who, "a netplay session ran")


# --------------------------------------------------------------------------- one game


def roles(players: Sequence[Any]) -> tuple:
    st = [gstatus(p.c) for p in players]
    host = players[0] if st[0]["role"] == "host" else players[1]
    join = players[1] if host is players[0] else players[0]
    return host, join


def play_game(players: Sequence[Any], game: int, timeout: float = 900,
              log: Callable[[str], None] = print) -> Dict[str, Any]:
    """Wait for the match to run under rollback on both, play it to game set closed-loop (each
    side drives its own in-game port with its own controller, port 0), and compare."""
    wait(lambda: all(gstatus(p.c)["phase"] in ("running", "error", "ended") for p in players), 240,
         f"game {game}: both sessions running")
    st = [gstatus(p.c) for p in players]
    assert all(s["phase"] == "running" for s in st), [(s["phase"], s.get("error")) for s in st]
    host, join = roles(players)
    setups = [B.read_match_setup(p.c.read_mem) for p in (host, join)]
    rep: Dict[str, Any] = {"game": game, "host": host.name, "join": join.name,
                           "setup": str(setups[0]), "stage": setups[0].stage_kind if setups[0] else None,
                           "match_index": [s["match_index"] for s in st],
                           "setup_hash": [s.get("setup_hash") for s in st]}
    assert setups[0] == setups[1], ("the two games set up different matches", setups)
    assert rep["setup_hash"][0] == rep["setup_hash"][1], rep
    assert len({s["match_index"] for s in st}) == 1, rep
    for p in players:
        p.c.call("game_pads", record=True)
        p.shot(f"g{game}-match")
    log(f"game {game}: running on both, stage {rep['stage']:#x}, host {host.name}")
    F._macro = _gentle_macro()
    seats = [F.Seat(host.c, 0, 0, "H"), F.Seat(join.c, 1, 0, "J")]
    stop_flag = threading.Event()
    stop_at = time.monotonic() + timeout

    def stop() -> bool:
        return stop_flag.is_set() or time.monotonic() > stop_at

    def play(seat: Any, seed: str) -> None:
        with contextlib.suppress(HarnessError):
            F.fight([seat], 60 * 60 * 9, mode="random", seed=seed,
                    stage_kind=rep["stage"], stop=stop)
        stop_flag.set()

    th = [threading.Thread(target=play, args=(s, f"{game}-{s.name}")) for s in seats]
    for t in th:
        t.start()
    while any(t.is_alive() for t in th):
        time.sleep(1)
        with contextlib.suppress(HarnessError):
            if any(gstatus(p.c)["phase"] != "running" for p in (host, join)):
                stop_flag.set()
    for t in th:
        t.join()
    wait(lambda: all(gstatus(p.c)["phase"] != "running" for p in players), 60, f"game {game} to end")
    st = [gstatus(p.c) for p in (host, join)]
    rep.update(frames=[s["current_frame"] for s in st], end=[s["end_reason"] for s in st],
               rollbacks=[s["rollbacks"] for s in st], max_rollback=[s["max_rollback_frames"] for s in st])
    limit = min(s["current_frame"] for s in st) - CONFIRM_MARGIN
    cks = [p.c.call("gprb_checksums", since=0)["rows"] for p in (host, join)]
    ma = {r[0]: r[1] for r in cks[0] if r[0] <= limit}
    mb = {r[0]: r[1] for r in cks[1] if r[0] <= limit}
    common = sorted(set(ma) & set(mb))
    bad = [f for f in common if ma[f] != mb[f]]
    rep["checksums"] = {"compared": len(common), "mismatches": len(bad), "first_mismatch": bad[0] if bad else None}
    tr = [p.c.call("frame_trace", since=0)["rows"] for p in (host, join)]
    gf_limit = limit + START_FRAME
    from gprb_ab import compare_traces
    rep["trace"] = compare_traces([r for r in tr[0] if r[0] <= gf_limit], [r for r in tr[1] if r[0] <= gf_limit])
    log(f"game {game}: {rep['frames']} frames, end {rep['end']}, rollbacks {rep['rollbacks']}, "
        f"confirmed checksums {rep['checksums']}, trace diverged at {rep['trace'].get('diverged_at')}")
    return rep


def _gentle_macro():
    from gprb_ab import gentle_macro
    return gentle_macro


def back_on_css(players: Sequence[Any], game: int,
                panels: Optional[Dict[str, Dict[str, int]]] = None) -> Dict[str, Any]:
    """After game set: both straight back on the online CSS, still connected, lobby at the next
    game (Slippi: no results screen, connection kept), each with the character still selected
    (`panels`: what each panel showed before the set)."""
    wait(lambda: all(scene(p.c) == CSS for p in players), 120, f"after game {game}: both on the CSS")
    for p in players:
        p.steps("wait 60")
        p.shot(f"g{game}-back-on-css")
        if panels and p.name in panels:
            check_css_remembers(p, panels[p.name], f"after game {game}")
    out = {}
    for p in players:
        lo, se, mm = local(p.c), session(p.c), p.c.mm_status()
        assert mm["state"] == "connection_success" and mm["handoff"] == "started", (p.name, mm["state"])
        assert lo["state"] == 2 and not lo["disconnected"], (p.name, lo)
        assert se["state"] == 1 and se["game"] == game + 1, (p.name, se)
        out[p.name] = {"local_port": lo["local_port"], "last_winner": se["last_winner"]}
    winners = {v["last_winner"] for v in out.values()}
    assert len(winners) == 1, ("the two games disagree on who won", out)
    return out


def lock_in_next(players: Sequence[Any], game: int, mode: str = "direct",
                 log: Callable[[str], None] = print,
                 panels: Optional[Dict[str, Dict[str, int]]] = None) -> Dict[str, Any]:
    """START on both CSSs. Direct: the loser of the last game picks the stage (a draw: both
    pick) and comes back to the CSS locked in, the character still selected; then the winner
    locks in with START."""
    info: Dict[str, Any] = {}
    se = session(players[0].c)
    winner = se["last_winner"]
    order = sorted(players, key=lambda p: local(p.c)["local_port"] == winner)
    for p in order:   # the loser first: it waits on the CSS for the winner
        lo = local(p.c)
        picks = mode == "direct" and winner != 0xFF and lo["local_port"] != winner
        n = lo["lock"]["seq"]
        p.steps("tap START 8", "wait 30")
        if picks:
            wait(lambda: scene(p.c) == "scSelStage", 60, f"{p.name}: the stage select (loser picks)")
            p.steps("wait 40")
            p.shot(f"g{game}-loser-stage-select")
            # Slippi parity: Direct's loser may pick any stage, nothing is struck.
            assert p.c.read_mem(STRIKE_TABLE, 30) == bytes(30), (p.name, "stages struck on the SSS")
            B.sss_pick_stage(p.c, B.STAGE_KIND[STAGE_PICK], 0)
            wait(lambda: scene(p.c) in (CSS, "scMemoryChange", "scMelee"), 60, f"{p.name}: back from the stage select")
            info["picked_by"] = p.name
            if panels and p.name in panels and all(scene(q.c) == CSS for q in players):
                wait(lambda: scene(p.c) == CSS, 30, f"{p.name}: on the CSS")
                p.steps("wait 30")
                info["after_sss"] = check_css_remembers(p, panels[p.name], "after the stage select")
                p.shot(f"g{game}-back-from-stage-select")
            log(f"game {game}: {p.name} lost the last game and picked {STAGE_PICK}")
        wait(lambda: local(p.c)["lock"]["seq"] > n and local(p.c)["lock"]["ready"]
             and local(p.c)["lock"]["game"] == game, 30, f"{p.name}: locked in for game {game}")
        if scene(p.c) == CSS:
            p.shot(f"g{game}-locked-in")
    return info


def play_set(players: Sequence[Any], games: int = 2, mode: str = "direct",
             log: Callable[[str], None] = print,
             panels: Optional[Dict[str, Dict[str, int]]] = None) -> Dict[str, Any]:
    """Games 1..`games` on the session the players are connected in; returns the report.
    Raises AssertionError / TimeoutError on the first thing that is not as expected."""
    rep: Dict[str, Any] = {"games": []}
    marks = {p.name: boot_marks(p.c) for p in players}
    # What each player's panel showed before game 1 (css_panel after picking): the CSS must show
    # it again after every game and after the stage select.
    panels = panels or {}
    rep["panels"] = panels
    rep["marks_start"] = marks
    for game in range(1, games + 1):
        if game > 1:
            rep.setdefault("lock_ins", []).append(lock_in_next(players, game, mode, log, panels))
        g = play_game(players, game, log=log)
        rep["games"].append(g)
        assert g["checksums"]["mismatches"] == 0 and g["checksums"]["compared"] > 300, g
        assert g["trace"].get("diverged_at") is None, g["trace"]
        assert all(e == "game set" for e in g["end"]), g
        g["after"] = back_on_css(players, game, panels)
        if game > 1 and mode == "direct" and rep["lock_ins"][-1].get("picked_by"):
            assert g["stage"] == B.STAGE_KIND[STAGE_PICK], ("the loser's pick was not played", g)
    for p in players:
        m = boot_marks(p.c)
        check_no_reboot(marks[p.name], m, p.name)
        rep.setdefault("marks_end", {})[p.name] = m
    return rep
