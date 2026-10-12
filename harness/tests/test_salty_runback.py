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

from test_online_game import _connected_direct, backend  # noqa: E402,F401  (also puts tools on sys.path)
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
    # A plain quit of the runback's match: both back on the CSS, which says who quit.
    _walk(b)
    a.steps("wait 240")
    quitter = online_set.local(a.c)["local_port"]
    _quit(a, "L+R+A+START")
    _quit_shown_on_both(a, b, quitter)
    for g in (a, b):
        g.shot("02-quit-notice")


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
    # The next game: both START on the CSS (no winner, so nobody picks a stage); it runs on both.
    for g in (a, b):
        g.steps("tap START 8", "wait 30")
    online_set.wait(lambda: all(online_set.gstatus(g.c)["phase"] == "running" for g in (a, b)), 120,
                    "the next game to run on both")
    a.steps("wait 60")
    for g in (a, b):
        g.shot("02-next-game")
        assert online_set.gstatus(g.c)["phase"] == "running", g.name
