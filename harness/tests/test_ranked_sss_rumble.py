"""The ranked stage select that leaves by itself keeps the controllers alive (rumble on port -1).

Players' controllers died for good after game 1 of a ranked set (logs 2026-10-11/12): the stage
select left by itself (the opponent picked, or the pick's time ran out), and from that frame
Brawl's pad thread made no pass again. The plugin leaves that stage select with an A that no pad
pressed (stage_legal.cpp), and with any controller in charge (0xF0) the stage select looks for the
port that pressed it by scanning the pads: -1. It then rumbles that port (sel_stage +0x5454:
rumble(-1, 10 frames) when the opponent's record, the human one without a controller, has rumble
on), and gfPadSystem::startMotor stores the 10 to m_padMotorMasks[-1], the low half of the
pointer to the pad thread the pad alarm resumes (+0xB54: 0x805BA100 became 0x805B000A, the value
in both players' logs). Rumble for a record without a tag is the save's byte before port 0's
(+0x81F), 0 on the harness's fresh save, so the harness never saw it; the test sets it. The plugin
now ignores a motor port outside 0-7 (online.cpp): the pointer stays and the winner locks in.
"""

from __future__ import annotations

import struct
import time
from typing import Callable

import pytest

from ppharness import brawl as B
from ppharness.instance import DolphinInstance

from test_online_game import Game, _boot, _both, _peer_shown, online_set, ppom
from test_online_ranked import (SSS, STARTERS, _both_on, _locks, _ranked, _search_ranked, _strike,
                                _wait, _wait_view, backend)  # noqa: F401

pytestmark = [pytest.mark.dolphin, pytest.mark.server, pytest.mark.gpu]

PAD_SYSTEM_PTR = 0x805A0040
PAD_SYS_THREAD_OFF = 0xB54    # gfPadSystem::m_controllerThread, the gfThread the pad alarm resumes
PAD_SYS_FLAGS_35 = 0x35       # bit 0x40: rumble allowed (startMotor does nothing without it)
PAD_THREAD_OBJ = 0x805BA100   # the pad thread's gfThread (its OSThread at +8)
GAME_GLOBAL_PTR = 0x805A00E0  # GameGlobal*; +0x24 the save data, whose +0x820 + port is the port's rumble
SAVE_RUMBLE_BEFORE_PORT0 = 0x81F


def _u32(g: Game, addr: int) -> int:
    return struct.unpack(">I", g.c.read_mem(addr, 4))[0]


def _rumble_for_port_minus_one(g: Game) -> None:
    """Make the rumble lookup for port -1 say yes, as on the players' saves."""
    save = _u32(g, _u32(g, GAME_GLOBAL_PTR) + 0x24)
    g.c.write_mem(save + SAVE_RUMBLE_BEFORE_PORT0, b"\x01")
    ps = _u32(g, PAD_SYSTEM_PTR)
    flags = g.c.read_mem(ps + PAD_SYS_FLAGS_35, 1)[0]
    g.c.write_mem(ps + PAD_SYS_FLAGS_35, bytes([flags | 0x40]))


@pytest.mark.slow
def test_stage_select_leaving_by_itself_keeps_the_pad_thread(backend, dolphin: Callable[..., DolphinInstance],
                                                             gpu_backend: str) -> None:
    test = "ranked-sss-rumble"
    ua, ub = backend.create_user("rma", "RMA"), backend.create_user("rmb", "RMB")
    a = _boot(dolphin, "rm-a", backend, ua, gpu_backend, test, "gameplay")
    b = _boot(dolphin, "rm-b", backend, ub, gpu_backend, test, "gameplay")
    _both(a.to_main_menu, b.to_main_menu)
    _both(a.to_online_page, b.to_online_page)
    _both(lambda: a.to_css("ranked"), lambda: b.to_css("ranked"))
    for g in (a, b):
        B.write_rules(g.c, stocks=1, minutes=2, items_off=True)
        ppom.allow_test_rules(g.c)
    _search_ranked(a)
    _search_ranked(b)
    _peer_shown(a, ub)
    _peer_shown(b, ua)
    players = [a, b]
    for g in players:
        _wait(lambda g=g: g.c.mm_status()["handoff"] == "started", 30, "the hand-off")
    host = a if _ranked(a)["local_port"] == 0 else b
    join = b if host is a else a

    _both_on(players, SSS, "game 1: both on the stage select")
    for g in players:
        _wait_view(g, lambda v: v["type"] == 1 and sorted(v["selectable"]) == STARTERS, "the starters")
    _strike(host, 0x02, join)
    _wait_view(join, lambda v: v["my_turn"], "the joiner's turn")
    _strike(join, 0x21, host)
    _strike(join, 0x2E, host)
    _strike(host, 0x2D, join)
    online_set.play_game(players, 1, winner=join)   # the guest wins, as in the players' reports
    winner, loser = join, host

    _both_on(players, SSS, "game 2: both on the stage select")
    _wait_view(winner, lambda v: v["game"] == 2 and v["type"] == 1 and v["my_turn"], "the ban")
    _strike(winner, 0x06, loser)
    _wait_view(winner, lambda v: v["type"] == 1 and v["my_turn"] and v["count"] == 1, "the second ban")
    _strike(winner, 0x02, loser)
    v = _wait_view(loser, lambda v: v["type"] == 2 and v["my_turn"], "the pick")
    n = _locks(winner)
    ps = _u32(winner, PAD_SYSTEM_PTR)
    assert _u32(winner, ps + PAD_SYS_THREAD_OFF) == PAD_THREAD_OBJ
    _rumble_for_port_minus_one(winner)
    since = len(winner.inst.log.read())

    # The loser picks; the winner's stage select leaves by itself.
    B.sss_pick_stage(loser.c, v["selectable"][0], 0)
    _both_on(players, online_set.CSS, "game 2: both on the CSS")
    # Read before waiting on frames: with the pointer broken every OS alarm stops too, and the
    # harness's frame waits time out.
    time.sleep(0.5)
    thread = _u32(winner, ps + PAD_SYS_THREAD_OFF)
    assert thread == PAD_THREAD_OBJ, f"gfPadSystem+0xB54 is {thread:08x}, not the pad thread's {PAD_THREAD_OBJ:08x}"
    winner.steps("wait 120")
    time.sleep(0.3)
    log = winner.inst.log.read()[since:]
    assert "pad watchdog: the pad thread made no pass" not in log, log[-3000:]

    # The winner's controller still works: START locks in.
    winner.steps("tap START 8", "wait 40")
    assert _locks(winner) > n, "the winner could not lock in"
