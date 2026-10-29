"""The pause screen's quit and P+'s salty runback in Direct (docs/game-code.md §11, the quit per
mode).

P+'s own runback (its Code Menu "End Match" control code, hook at 0x806D4850) reads this machine's
own pads when the match scene ends and sets the sequence's next state to 1 with L+R+Y held (13 with
L+R+X): online that skipped OnlineMatch's state 10 on the quitter's machine only, which then set up
a match of its own with no session. It stays off online (OnlineMenu::runbackOff). Instead the
plugin marks the quit with L+R+Y in SESSION from the pauser's buttons in the session's inputs
(quitBy, quitRunback), and in Unranked / Direct Dolphin sets the next game up as the same match on
every machine, so both games go from the match straight into it. A plain quit (L+R+X is one now)
goes back to the CSS, which says who quit.
"""
from __future__ import annotations

from typing import Callable

import pytest

from ppharness.backend import OnlineBackend
from ppharness.client import PadInput
from ppharness.instance import DolphinInstance

from test_online_game import _boot, _connected_direct, backend  # noqa: E402,F401  (also puts tools on sys.path)
import online_set  # noqa: E402
from ppharness import brawl as B  # noqa: E402

AFTER_MATCH = 10     # debug scratch[10] low byte: OnlineMatch's state 10 went back to the CSS
RUNBACK_SETUP = 0x1B  # ... state 10 went straight into the runback's match
RUNBACK_MARKED = 0x1000000   # scratch[10]: the quit was marked as a runback (pponline_pauseResult)
QUIT_NOTICE = 0x2000000      # scratch[10]: "<name> quit" on the CSS, the quitter's port in bits 26-27


def _quit(a, buttons: str) -> None:
    a.steps("tap START 8", "wait 60")
    # Held past the session's end (the game stays paused until then), as a player holds it; not
    # into the next match, whose first frame after GO would take the held START as a new pause.
    a.steps(f"hold {buttons} 90")


def _quit_late_y(a, late: int) -> None:
    """The quit with Y `late` frames after L+R+A+START (one pad read a frame in a match)."""
    a.steps("tap START 8", "wait 60")
    lras = PadInput(buttons=("L", "R", "A", "START"), hold=late)
    a.c.pad_script(a.pad, [lras, PadInput(buttons=("L", "R", "A", "Y", "START"), hold=90)])
    a.steps(f"wait {late + 90}")


def _walk(b) -> None:
    walk = [PadInput(main=(40, 128), hold=7), PadInput(main=(216, 128), hold=7)]
    b.c.pad_script(0, walk * 80)


def _quit_shown_on_both(a, b, quitter_port: int) -> None:
    online_set.wait(lambda: all(online_set.scene(g.c) == online_set.CSS for g in (a, b)), 45,
                    "both back on the CSS")
    online_set.wait(lambda: all(g.debug_scratch()[10] & QUIT_NOTICE for g in (a, b)), 10,
                    "the quit notice on both CSSs")
    for g in (a, b):
        sc = g.debug_scratch()[10]
        assert (sc >> 26) & 3 == quitter_port, (g.name, hex(sc))
        assert sc & 0xFF == AFTER_MATCH, (g.name, hex(sc))
        st = online_set.gstatus(g.c)
        assert st["end_reason"] == "quit" and st["phase"] == "connected", (g.name, st["phase"], st.get("end_reason"))


@pytest.mark.slow
def test_runback_restarts_the_same_match_in_direct(backend: OnlineBackend,
                                                   dolphin: Callable[..., DolphinInstance],
                                                   gpu_backend: str) -> None:
    a, b, ua, ub = _connected_direct(backend, dolphin, gpu_backend, "salty-runback", ("rhea", "otto"),
                                     stocks=4)
    online_set.wait(lambda: all(online_set.gstatus(g.c)["phase"] == "running" for g in (a, b)), 240,
                    "the match to run on both")
    for g in (a, b):   # P+'s own runback stays off online
        assert g.c.read_mem(0x804E0008, 1) == b"\x01" and g.c.read_mem(0x804E000C, 1) == b"\x01", g.name
    first = [online_set.gstatus(g.c)["match_index"] for g in (a, b)]
    setup = B.read_match_setup(a.c.read_mem)
    _walk(b)
    a.steps("wait 240")
    _quit(a, "L+R+A+Y+START")
    # Straight into the same match on both: never the CSS, the next game, the same setup.
    online_set.wait(lambda: all(online_set.gstatus(g.c)["phase"] == "running" and
                                online_set.gstatus(g.c)["match_index"] == first[0] + 1 for g in (a, b)),
                    90, "the same match again on both")
    for g in (a, b):
        g.shot("01-runback")
        sc = g.debug_scratch()[10]
        assert sc & RUNBACK_MARKED and sc & 0xFF == RUNBACK_SETUP, (g.name, hex(sc))
        again = B.read_match_setup(g.c.read_mem)
        assert again == setup, (g.name, "a different match", setup, again)
        assert g.inst.log.read().count("the last game again (runback)") == 1, g.name
    # Y a few frames after L+R+A+START runs back too, as P+'s own runback (it reads the buttons
    # when the match scene ends); online it had to be down on the quit's frame (bug report #181).
    _walk(b)
    a.steps("wait 240")
    _quit_late_y(a, 6)
    online_set.wait(lambda: all(online_set.gstatus(g.c)["phase"] == "running" and
                                online_set.gstatus(g.c)["match_index"] == first[0] + 2 for g in (a, b)),
                    90, "the same match again on both (Y late)")
    for g in (a, b):
        assert B.read_match_setup(g.c.read_mem) == setup, (g.name, "a different match")
        assert g.inst.log.read().count("the last game again (runback)") == 2, g.name
    # A plain quit of the runback's match: both back on the CSS, which says who quit.
    _walk(b)
    a.steps("wait 240")
    quitter = online_set.local(a.c)["local_port"]
    _quit(a, "L+R+A+START")
    _quit_shown_on_both(a, b, quitter)
    for g in (a, b):
        g.shot("02-quit-notice")


@pytest.mark.slow
def test_runback_restarts_the_same_match_in_a_room_without_a_set(backend: OnlineBackend,
                                                                 dolphin: Callable[..., DolphinInstance],
                                                                 gpu_backend: str) -> None:
    """A 1v1 room without a set (prod 2026-10-13, DUCK#96 against sosa on Wario Land): the session
    set the runback up, but the room ended its game as soon as the match scene ended, so both games
    played the runback alone, each local pad driving P1. The room's game now goes on in the same
    session; a plain quit of the runback then ends it and both are back on the room's CSS."""
    from test_rooms_game import ART as ROOM_ART, _all, create_room, pick, room_view, to_join_room, \
        type_room_code, until_view
    import ppom
    test = "room-runback"
    ua = backend.create_user("rrba", "RRBA")
    ub = backend.create_user("rrbb", "RRBB")
    a = _boot(dolphin, "room-runback-a", backend, ua, gpu_backend, test, "gameplay", artifacts=ROOM_ART)
    b = _boot(dolphin, "room-runback-b", backend, ub, gpu_backend, test, "gameplay", artifacts=ROOM_ART)
    _all(a.to_main_menu, b.to_main_menu)
    _all(a.to_online_page, b.to_online_page)
    code = create_room(a)
    to_join_room(b)
    pick(a)
    pick(b, right=2)
    for g in (a, b):
        B.write_rules(g.c, stocks=4, minutes=8, items_off=True)
        ppom.allow_test_rules(g.c)
    type_room_code(b, code)
    until_view(b, lambda v: v["flags"] & ppom.RF_IN, "B in the room")
    until_view(a, lambda v: v["slots"][1]["bits"] & ppom.SLOT_TAKEN, "A sees B")
    game = room_view(a)["game"]
    a.steps("tap START 8", "wait 40")
    until_view(b, lambda v: v["slots"][0]["bits"] & ppom.SLOT_READY, "B sees A ready")
    b.steps("tap START 8", "wait 10")
    online_set.wait(lambda: all(online_set.gstatus(g.c)["phase"] == "running" for g in (a, b)), 240,
                    "the room's match to run on both")
    first = [online_set.gstatus(g.c)["match_index"] for g in (a, b)]
    setup = B.read_match_setup(a.c.read_mem)
    _walk(b)
    a.steps("wait 240")
    _quit(a, "L+R+A+Y+START")
    # Straight into the same match on both, still under the session (the bug: no session, phase
    # ended, each game alone).
    online_set.wait(lambda: all(online_set.gstatus(g.c)["phase"] == "running" and
                                online_set.gstatus(g.c)["match_index"] == first[0] + 1 for g in (a, b)),
                    90, "the same match again on both")
    _walk(b)
    a.steps("wait 240")
    for g in (a, b):
        g.shot("01-room-runback")
        sc = g.debug_scratch()[10]
        assert sc & RUNBACK_MARKED and sc & 0xFF == RUNBACK_SETUP, (g.name, hex(sc))
        assert B.read_match_setup(g.c.read_mem) == setup, (g.name, "a different match")
        log = g.inst.log.read()
        assert log.count("the last game again (runback)") == 1, g.name
        assert "game run back: the session plays it again" in log, g.name
        st = online_set.gstatus(g.c)
        assert st["phase"] == "running", (g.name, st["phase"])
        assert room_view(g)["status"] != 0, (g.name, "the room is no longer in its game", room_view(g))
    # A plain quit of the runback's match ends the room's game: both on the room's CSS, the room's
    # game numbers past both games.
    _quit(a, "L+R+A+START")
    for g in (a, b):
        until_view(g, lambda v: v["flags"] & ppom.RF_IN and v["status"] == 0 and v["game"] == game + 2,
                   "the room waiting for its next game", timeout=90)
        g.steps("wait 60")
        g.shot("02-back-in-the-room")


@pytest.mark.slow
def test_lrax_is_a_plain_quit_and_the_css_says_who_quit(backend: OnlineBackend,
                                                         dolphin: Callable[..., DolphinInstance],
                                                         gpu_backend: str) -> None:
    a, b, ua, ub = _connected_direct(backend, dolphin, gpu_backend, "salty-skipresults", ("sami", "tova"),
                                     stocks=4)
    online_set.wait(lambda: all(online_set.gstatus(g.c)["phase"] == "running" for g in (a, b)), 240,
                    "the match to run on both")
    _walk(b)
    a.steps("wait 240")
    quitter = online_set.local(a.c)["local_port"]
    _quit(a, "L+R+A+X+START")
    _quit_shown_on_both(a, b, quitter)
    for g in (a, b):
        g.shot("01-quit-notice")
        assert online_set.session(g.c)["game"] == 2, g.name
    # The next game: both START on the CSS (no winner; everyone picks in Direct since 2026-10-29,
    # so both go on to the stage select, where one's START picks a random stage); it runs on both.
    for g in (a, b):
        g.steps("tap START 8", "wait 30")
    online_set.random_pick_where_anyone_picks([a, b], 2)
    online_set.wait(lambda: all(online_set.gstatus(g.c)["phase"] == "running" for g in (a, b)), 120,
                    "the next game to run on both")
    a.steps("wait 60")
    for g in (a, b):
        g.shot("02-next-game")
        assert online_set.gstatus(g.c)["phase"] == "running", g.name
