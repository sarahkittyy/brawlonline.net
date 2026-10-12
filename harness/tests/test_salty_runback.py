"""P+'s salty runback (L+R+A+Y+START on the pause screen) and results skip (L+R+A+X+START) are off
online (docs/game-code.md §11, the pause-screen quit).

P+'s Code Menu "End Match" control code (Net-CodeMenu.asm, hook at 0x806D4850) reads this
machine's own pads when the match scene ends and, with the Code Menu header's buttons held (L+R+Y
at 0x804E000A, L+R+X at 0x804E000E), sets the sequence's next state to 1 or 13 instead of 10. In
the online sequence that skipped OnlineMatch's state 10 on the quitter's machine only: its CSS kept
the old match armed, and its next START set up a match of its own, with no session, while the
other game waited on the CSS ("Waiting on opponent"). Now the plugin switches both off online
(header bytes 0x804E0008 / 0x804E000C) and puts the player's values back with the menus.
"""
from __future__ import annotations

from typing import Callable

import pytest

from ppharness.backend import OnlineBackend
from ppharness.client import PadInput
from ppharness.instance import DolphinInstance

from test_online_game import _connected_direct, backend  # noqa: E402,F401  (also puts tools on sys.path)
import online_set  # noqa: E402

AFTER_MATCH = 10   # debug scratch[10] low byte: OnlineMatch's state 10 ran (online_match.cpp afterMatch)


@pytest.mark.slow
@pytest.mark.parametrize("buttons", ["L+R+A+Y+START", "L+R+A+X+START"])
def test_runback_combo_on_the_pause_quit(backend: OnlineBackend,
                                         dolphin: Callable[..., DolphinInstance],
                                         gpu_backend: str, buttons: str) -> None:
    tag = "runback" if "Y" in buttons else "skipresults"
    names = ("rhea", "otto") if tag == "runback" else ("sami", "tova")
    a, b, ua, ub = _connected_direct(backend, dolphin, gpu_backend, f"salty-{tag}", names, stocks=4)
    online_set.wait(lambda: all(online_set.gstatus(g.c)["phase"] == "running" for g in (a, b)), 240,
                    "the match to run on both")
    for g in (a, b):
        assert g.c.read_mem(0x804E0008, 1) == b"\x01" and g.c.read_mem(0x804E000C, 1) == b"\x01", g.name
    walk = [PadInput(main=(40, 128), hold=7), PadInput(main=(216, 128), hold=7)]
    b.c.pad_script(0, walk * 80)
    a.steps("wait 240")
    a.steps("tap START 8", "wait 60")
    # Held well past the session's end and the scene change, as a player keeps holding it.
    a.steps(f"hold {buttons} 360")
    online_set.wait(lambda: all(online_set.scene(g.c) == online_set.CSS for g in (a, b)), 45,
                    "both back on the CSS")
    for g in (a, b):
        g.shot("01-after-quit")
        st, lo, se = online_set.gstatus(g.c), online_set.local(g.c), online_set.session(g.c)
        assert st["end_reason"] == "quit" and st["phase"] == "connected", (g.name, st["phase"], st.get("end_reason"))
        assert g.debug_scratch()[10] & 0xFF == AFTER_MATCH, (g.name, hex(g.debug_scratch()[10]))
        assert lo["state"] == 2 and se["game"] == 2, (g.name, lo, se)
    # The next game: both START on the CSS (no winner, so nobody picks a stage); it runs on both.
    for g in (a, b):
        g.steps("tap START 8", "wait 30")
    online_set.wait(lambda: all(online_set.gstatus(g.c)["phase"] == "running" for g in (a, b)), 120,
                    "the next game to run on both")
    st = [online_set.gstatus(g.c) for g in (a, b)]
    assert len({s["match_index"] for s in st}) == 1, st
    a.steps("wait 120")
    for g in (a, b):
        g.shot("02-next-game")
        assert online_set.gstatus(g.c)["phase"] == "running", g.name
