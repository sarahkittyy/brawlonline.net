"""Anyone picks (Sarah's board, 2026-10-29: "a pick option for lobbies where anyone can pick ... sync
the cursors up on the map screen ... first to pick gets to play, they say p1 p2 p3 p4 on them";
docs/design/room-rules.md, docs/game-code.md §11).

A room whose ruleset has the stage choice `anyone-picks` (no set): every player of the next game,
game 1 too, goes to the stage select once everyone is locked in, and each sees the others' cursors
there (P+'s crosshair, labelled with the player's port: the plugin's g_sssCursors). A on a stage is
that player's pick: the stage select stays until the game's host took the first pick it saw, then
every stage select leaves and the match plays it. START picks a random stage of the room's. Players
who sit out do not pick. Ranked's strikes show the opponent's cursor the same way.

- test_room_anyone_picks: two players, four games: game 1 (A picks Final Destination with L
  held: the L layout on both), the cursors (each machine draws the other's where it is, labelled
  P1 / P2, and hides it while the two are on different pages), both pressing A at once (one pick
  is played, the same on both machines), a pick while the other player is on another page, START.
- test_room_anyone_picks_three_players: a third player sits out game 1 (never on the stage select,
  no pick, not waited for), then all three pick: each draws the other two cursors.
- test_ranked_strikes_show_the_opponents_cursor: Ranked's game 1 strikes.
- test_direct_anyone_picks: Direct, where everyone picks by default (Sarah's board, 2026-10-29):
  game 1 too (its lock-in is the search's: locked in again with the pick to come), both on the
  stage select with each other's cursors, B does nothing there, a pick with L held is played as the
  L layout; game 2 both again (the winner too), START a random stage of Direct's list.

Every stage select's cursors are deleted with it (made = deleted, none alive). Screenshots:
run/artifacts/game-code/rooms/anyone-picks/<test>/<player>/.
"""
from __future__ import annotations

import re
import struct
import threading
from typing import Any, Callable

import pytest

from ppharness import brawl as B
from ppharness.backend import OnlineBackend
from ppharness.instance import DolphinInstance

from test_online import _wait
from test_online_game import Game, _boot, _both, _connected_direct, _peer_shown, backend  # noqa: F401  (the backend fixture)
from test_online_ranked import SSS as RANKED_SSS, STARTERS, _both_on, _paired, _search_ranked, _to_ranked_css
from test_ranked_sss_portraits import _sym
from test_room_rules import session_rules
from test_room_stage_list import BF, FD, STAGES, YS, room_with, session
from test_room_sit_out import _join, _open_slot, _plate, _players, _running
from test_rooms_game import (ART, CSS, _all, launcher_answer, lock_of, pick, room_view, to_join_room, until_view,
                             write_create)
import online_set  # noqa: E402
import ppom  # noqa: E402

pytestmark = [pytest.mark.dolphin, pytest.mark.server, pytest.mark.gpu]

ANY_ART = ART / "anyone-picks"
SSS = "scSelStage"
CHOICE_ANYONE_PICKS = 4
STAGE_PENDING = 0xFFFE
L_LAYOUT = 0x40
CURRENT_PAGE = 0x80496000

# g_sssCursors: written, made, deleted, refused (u32), shown, ownLabel, alive, pad (u8),
# pos[4][2] (f32), obj[4] (u32).
CURSORS_FMT = ">4I4B8f4I"


def cursors(g: Game) -> dict[str, Any]:
    v = struct.unpack(CURSORS_FMT, g.c.read_mem(_sym(g, "g_sssCursors"), struct.calcsize(CURSORS_FMT)))
    return {"written": v[0], "made": v[1], "deleted": v[2], "refused": v[3], "shown": v[4],
            "own_label": v[5], "alive": v[6], "pos": [v[8 + 2 * p:10 + 2 * p] for p in range(4)],
            "obj": list(v[16:20])}


def anim_frames(g: Game, obj: int) -> list[float]:
    """A MuObject's VIS0, CHR0, PAT0 and CLR0 frames (its anim block +0x14, slots 8 / C / 10 / 18,
    each AnmObj's frame at +0x18)."""
    block = g.c.read_u32(obj + 0x14)
    out = []
    for slot in (0x08, 0x0C, 0x10, 0x18):
        a = g.c.read_u32(block + slot)
        out.append(struct.unpack(">f", g.c.read_mem(a + 0x18, 4))[0] if a else -1.0)
    return out


def own_cursor(g: Game) -> tuple[float, float, int]:
    v = B.read_sss(g.c.read_mem)
    assert v is not None, f"{g.name}: not on the stage select"
    return v.cursor_x, v.cursor_y, g.c.read_u8(CURRENT_PAGE)


def sees(viewer: Game, mover: Game, port: int, what: str, tol: float = 0.6) -> None:
    """`viewer` draws `mover`'s cursor (in-game port `port`) where it is: `mover` stays still."""
    B.neutral(mover.c, 0)
    B.step(mover.c, 20)
    mx, my, _ = own_cursor(mover)

    def there() -> bool:
        cu = cursors(viewer)
        gx, gy = cu["pos"][port]
        return bool(cu["shown"] & (1 << port)) and abs(gx - mx) < tol and abs(gy - my) < tol
    try:
        _wait(there, 10, f"{viewer.name} draws {mover.name}'s cursor ({what})")
    except AssertionError as e:
        raise AssertionError(f"{e}: {mover.name} at ({mx:.2f}, {my:.2f}), {viewer.name} {cursors(viewer)}, "
                             f"LOCAL {ppom.read_local(viewer.c, ppom.find_block(viewer.c)).get('cursors')}") from None
    # It looks as the viewer's own: the same visibility frame (P+'s coin-battle parts hidden), the
    # mover's label (PAT0 frame port + 1).
    task = B.read_sss(viewer.c.read_mem).task
    mine = anim_frames(viewer, viewer.c.read_u32(task + 0x200))
    theirs = anim_frames(viewer, cursors(viewer)["obj"][port])
    assert theirs[0] == mine[0] and theirs[2] == port + 1, (what, mine, theirs)


def all_ready_to_sss(gs: list[Game], game: int) -> None:
    for g in gs:
        g.steps("tap START 8")
    for g in gs:
        _wait(lambda g=g: lock_of(g)["ready"] and lock_of(g)["stage_pick"] == STAGE_PENDING, 30,
              f"{g.name} locked in with its pick to come")
    _wait(lambda: all(g.scene() == SSS for g in gs), 90, f"game {game}: every player on the stage select")
    for g in gs:
        B.step(g.c, 40)


def cursors_gone(gs: list[Game]) -> None:
    """The stage select's cursors went with it: every one made is deleted, none alive."""
    for g in gs:
        cu = cursors(g)
        assert cu["alive"] == 0 and cu["made"] == cu["deleted"] and cu["refused"] == 0, (g.name, cu)


def back_in_the_room(gs: list[Game], game: int) -> None:
    _wait(lambda: all(g.scene() == CSS for g in gs), 180, "back on the room's CSS")
    for g in gs:
        until_view(g, lambda v: v["flags"] & ppom.RF_IN and v["status"] == 0 and v["game"] == game + 1,
                   "the room waiting for its next game", timeout=60)
        g.steps("wait 30")


def host_pick_line(gs: list[Game], game_in_session: int = 1) -> tuple[int, int]:
    """The room game's host's log: the stage it set the game up with and whose pick it was."""
    for g in gs:
        text = g.inst.log_path.read_text(errors="replace")
        m = re.findall(rf"gprb lobby: game {game_in_session} setup: stage (0x[0-9a-f]+) \(picked by P(\d)\)", text)
        if m:
            return int(m[-1][0], 16), int(m[-1][1]) - 1
    raise AssertionError("no host's 'picked by' line")


def leaves_by_itself(g: Game, what: str) -> None:
    _wait(lambda: g.scene() != SSS, 30, f"{g.name}'s stage select leaves by itself ({what})")


@pytest.mark.slow
def test_room_anyone_picks(backend: OnlineBackend, dolphin: Callable[..., DolphinInstance], gpu_backend: str) -> None:
    a, b = room_with(backend, dolphin, gpu_backend, "anyp", "anyone-picks", alternates=True)
    gs = [a, b]
    for g in gs:
        g.out = ANY_ART / "two-players" / g.name
        se = session(g)
        assert se["play"]["stage_choice"] == CHOICE_ANYONE_PICKS, se["play"]
        until_view(g, lambda v: [s["picks_stage"] for s in v["slots"][:2]] == [1, 1], "game 1: both pick")
    a.shot("01-room")
    game = room_view(a)["game"]

    # Game 1: both on the stage select; each draws the other's cursor (P2 on A's, P1 on B's) and
    # labels its own.
    all_ready_to_sss(gs, game)
    sees(a, b, 1, "B where its stage select starts")
    sees(b, a, 0, "A where its stage select starts")
    assert (cursors(a)["own_label"], cursors(b)["own_label"]) == (1, 2), (cursors(a), cursors(b))
    lo = ppom.read_local(a.c, ppom.find_block(a.c))
    assert lo["own_cursor"] and lo["own_cursor"]["page"] == 0, lo
    # B moves over Battlefield (X: nothing here, so no pick): A's copy follows.
    B.sss_pick_stage(b.c, BF, 0, button="X")
    sees(a, b, 1, "B over Battlefield")
    assert b.scene() == SSS and lock_of(b)["stage_pick"] == STAGE_PENDING, "X picked"
    a.shot("02-two-cursors")
    b.shot("02-two-cursors")
    # A picks Final Destination holding L: B's stage select leaves with it; the L layout on both.
    B.sss_pick_stage(a.c, FD, 0, held=["L"])
    leaves_by_itself(b, "A picked")
    rep = online_set.play_game(gs, game)
    assert rep["checksums"]["mismatches"] == 0, rep
    assert rep["stage"] == FD and rep["asl"] == [L_LAYOUT, L_LAYOUT], ("A's pick was not played", rep)
    assert rep["asl_loaded"] == [L_LAYOUT, L_LAYOUT], ("P+ did not load the L layout", rep)
    assert host_pick_line(gs) == (FD, 0), host_pick_line(gs)
    cursors_gone(gs)
    back_in_the_room(gs, game)
    game += 1

    # Game 2: both press A at once, A on Battlefield and B on Final Destination. One pick is
    # played, the same on both machines, and both stage selects leave.
    all_ready_to_sss(gs, game)
    _all(lambda: B.sss_pick_stage(a.c, BF, 0, button="X"), lambda: B.sss_pick_stage(b.c, FD, 0, button="X"))
    B.step(a.c, 20)
    go = threading.Barrier(2)

    def press(g: Game) -> None:
        go.wait()
        B.tap(g.c, 0, ["A"], hold=3, release=3)
    _all(lambda: press(a), lambda: press(b))
    for g in gs:
        leaves_by_itself(g, "both picked")
    picks = [lock_of(g)["stage_pick"] for g in gs]
    rep = online_set.play_game(gs, game)
    assert rep["checksums"]["mismatches"] == 0, rep
    stage, port = host_pick_line(gs)
    print("both at once:", {"locks": picks, "played": hex(rep["stage"]), "host took P": port + 1})
    assert rep["stage"] == stage and stage == (BF, FD)[port], (rep, stage, port)
    assert picks == [BF, FD] or STAGE_PENDING in picks, ("the picks", picks)
    cursors_gone(gs)
    back_in_the_room(gs, game)
    game += 1

    # Game 3: A turns to P+'s second page (each hides the other's cursor there), B picks
    # Battlefield on the first: A's stage select leaves by itself, without A's pick.
    all_ready_to_sss(gs, game)
    B.sss_hover_item(a.c, B.SSS_ITEM_PAGE, 0)
    B.tap(a.c, 0, ["A"], hold=3, release=3)
    _wait(lambda: a.c.read_u8(CURRENT_PAGE) == 1, 10, "A on page 2")
    _wait(lambda: not cursors(a)["shown"] & 2 and not cursors(b)["shown"] & 1, 10,
          "the other's cursor hidden on another page")
    a.shot("03-another-page")
    B.sss_pick_stage(b.c, BF, 0)
    leaves_by_itself(a, "B picked on another page")
    assert lock_of(a)["stage_pick"] == STAGE_PENDING, lock_of(a)
    rep = online_set.play_game(gs, game)
    assert rep["checksums"]["mismatches"] == 0, rep
    assert rep["stage"] == BF and rep["asl"] == [0, 0], rep
    cursors_gone(gs)
    back_in_the_room(gs, game)
    game += 1

    # Game 4: START is a random stage of the room's.
    all_ready_to_sss(gs, game)
    b.steps("tap START 8")
    for g in gs:
        leaves_by_itself(g, "B's START")
    rep = online_set.play_game(gs, game)
    assert rep["checksums"]["mismatches"] == 0, rep
    assert rep["stage"] in (BF, FD, YS), ("START left the room's stages", rep)
    cursors_gone(gs)


@pytest.mark.slow
def test_room_anyone_picks_three_players(backend: OnlineBackend, dolphin: Callable[..., DolphinInstance],
                                         gpu_backend: str) -> None:
    test = "anyone-three"
    a, b, c = gs = _players(backend, dolphin, gpu_backend, test, "anp", 3)
    for g in gs:
        g.out = ANY_ART / test / g.name
    code = backend._http("POST", "/v1/rulesets", {"ruleset": {
        "format": 1, "name": "Anyone Three", "rules": {"stocks": 1, "timeMinutes": 2}, "bestOf": 0,
        "stageChoice": "anyone-picks", "stages": STAGES}}, token=a.user.session_token)["code"]
    rid = write_create(a, ruleset=code)
    assert launcher_answer(a, rid)["result"] == "accepted"
    _wait(lambda: a.scene() == CSS, 60, "A on the room's CSS")
    room = until_view(a, lambda v: v["flags"] & ppom.RF_IN, "A in its room", timeout=60)["code"]
    pick(a)
    _open_slot(a, 2)
    for k, g in ((1, b), (2, c)):
        to_join_room(g)
        pick(g, right=2 * k)
        _join(g, room, k)
    until_view(a, lambda v: all(v["slots"][p]["bits"] & ppom.SLOT_TAKEN for p in range(3)), "all players")
    for g in gs:
        _wait(lambda g=g: (session_rules(g) or {}).get("stocks") == 1, 30, f"{g.name}: the ruleset's rules")
        _wait(lambda g=g: session(g)["play"]["stage_choice"] == CHOICE_ANYONE_PICKS, 30, f"{g.name}: anyone picks")

    # C sits out game 1: no pick, not on the stage select, not waited for.
    _plate(c, 2)
    for g in gs:
        until_view(g, lambda v: [s["picks_stage"] for s in v["slots"][:3]] == [1, 1, 0], f"{g.name}: A and B pick")
    game = room_view(a)["game"]
    all_ready_to_sss([a, b], game)
    assert c.scene() == CSS, "who sits out went to the stage select"
    sees(a, b, 1, "B")
    sees(b, a, 0, "A")
    assert not cursors(a)["shown"] & 4 and not cursors(b)["shown"] & 4, "a cursor for who sits out"
    B.sss_pick_stage(a.c, FD, 0)
    leaves_by_itself(b, "A picked")
    rep = online_set.play_game([a, b], game)
    assert rep["checksums"]["mismatches"] == 0 and rep["stage"] == FD, rep
    cursors_gone([a, b])
    back_in_the_room([a, b], game)
    game += 1

    # All three: each draws the other two, labelled with their ports; C's pick plays.
    _plate(c, 2)
    for g in gs:
        until_view(g, lambda v: [s["picks_stage"] for s in v["slots"][:3]] == [1, 1, 1], f"{g.name}: everyone picks")
    all_ready_to_sss(gs, game)
    sees(a, b, 1, "B")
    sees(a, c, 2, "C")
    sees(b, c, 2, "C")
    sees(c, a, 0, "A")
    sees(c, b, 1, "B")
    assert [cursors(g)["own_label"] for g in gs] == [1, 2, 3], [cursors(g) for g in gs]
    # Apart: B over Battlefield, C over Final Destination (X picks nothing here).
    _all(lambda: B.sss_pick_stage(b.c, BF, 0, button="X"), lambda: B.sss_pick_stage(c.c, FD, 0, button="X"))
    sees(a, b, 1, "B over Battlefield")
    sees(a, c, 2, "C over Final Destination")
    for g in gs:
        g.shot("02-three-cursors")
    B.sss_pick_stage(c.c, BF, 0)
    leaves_by_itself(a, "C picked")
    leaves_by_itself(b, "C picked")
    _running(gs, "game 2 with all three")
    setups = [B.read_match_setup(g.c.read_mem) for g in gs]
    assert all(s == setups[0] for s in setups) and setups[0].stage_kind == BF, setups
    cursors_gone(gs)


@pytest.mark.slow
def test_ranked_strikes_show_the_opponents_cursor(backend, dolphin: Callable[..., DolphinInstance],
                                                  gpu_backend: str) -> None:
    test = "anyone-picks"
    ua, ub = backend.create_user("rcua", "RCUA"), backend.create_user("rcub", "RCUB")
    a = _boot(dolphin, "rcur-a", backend, ua, gpu_backend, test, "gameplay", artifacts=ANY_ART)
    b = _boot(dolphin, "rcur-b", backend, ub, gpu_backend, test, "gameplay", artifacts=ANY_ART)
    _both(a.to_main_menu, b.to_main_menu)
    _both(a.to_online_page, b.to_online_page)
    _both(lambda: a.to_css("ranked"), lambda: b.to_css("ranked"))
    _to_ranked_css(a, b)
    _search_ranked(a)
    _search_ranked(b)
    _peer_shown(a, ub)
    _peer_shown(b, ua)
    _paired(a, b)
    _both_on([a, b], RANKED_SSS, "game 1: both on the stage select")
    B.step(a.c, 60)
    port_a = ppom.read_local(a.c, ppom.find_block(a.c))["local_port"]
    port_b = 1 - port_a
    sees(a, b, port_b, "the opponent")
    sees(b, a, port_a, "the opponent")
    assert (cursors(a)["own_label"], cursors(b)["own_label"]) == (port_a + 1, port_b + 1)
    # B over a starter (Z: masked on a ranked stage select, so no strike either way).
    B.sss_pick_stage(b.c, STARTERS[2], 0, button="Z")
    sees(a, b, port_b, "the opponent over a starter")
    a.shot("ranked-opponents-cursor")
    online_set.wait(lambda: online_set.scene(a.c) == "scMelee", 300, "game 1")
    B.step(a.c, 5)
    cursors_gone([a, b])


@pytest.mark.slow
def test_direct_anyone_picks(backend, dolphin: Callable[..., DolphinInstance], gpu_backend: str) -> None:
    a, b, _, _ = _connected_direct(backend, dolphin, gpu_backend, "anyone-direct", ("dapa", "dapb"), stocks=1,
                                  pick_game_1=False)
    gs = [a, b]
    for g in gs:
        g.out = ANY_ART / "direct" / g.name
    # Game 1: the search's lock-in, then locked in again with the pick to come; both stage selects.
    _wait(lambda: all(g.scene() == SSS for g in gs), 90, "game 1: both on the stage select")
    for g in gs:
        se = session(g)
        assert se["anyone_picks"] and [p["picks_stage"] for p in se["players"][:2]] == [1, 1], se
        assert lock_of(g)["stage_pick"] == STAGE_PENDING, lock_of(g)
        B.step(g.c, 40)
    port_a = ppom.read_local(a.c, ppom.find_block(a.c))["local_port"]
    port_b = 1 - port_a
    sees(a, b, port_b, "B in Direct")
    sees(b, a, port_a, "A in Direct")
    # B: no way back to the CSS where everyone picks.
    b.steps("tap B 8", "wait 40")
    assert b.scene() == SSS and lock_of(b)["ready"], ("B left the stage select", b.scene(), lock_of(b))
    a.shot("01-direct-game-1")
    B.sss_pick_stage(a.c, FD, 0, held=["L"])
    leaves_by_itself(b, "A picked")
    rep = online_set.play_game(gs, 1)
    assert rep["checksums"]["mismatches"] == 0, rep
    assert rep["stage"] == FD and rep["asl"] == [L_LAYOUT, L_LAYOUT], ("A's pick was not played", rep)
    cursors_gone(gs)
    online_set.back_on_css(gs, 1)

    # Game 2: both again, the winner too; START: a random stage of Direct's list.
    for g in gs:
        g.steps("tap START 8", "wait 10")
    _wait(lambda: all(g.scene() == SSS for g in gs), 90, "game 2: both on the stage select")
    starters = session(a)["stages"]["starters"]
    assert starters, ("Direct's stages not in SESSION", session(a)["stages"])
    b.steps("wait 40", "tap START 8")
    for g in gs:
        leaves_by_itself(g, "B's START")
    rep = online_set.play_game(gs, 2)
    assert rep["checksums"]["mismatches"] == 0, rep
    assert rep["stage"] in starters, ("START left Direct's stages", rep, starters)
    cursors_gone(gs)

