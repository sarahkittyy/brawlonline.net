"""Ranked's stage select shows who plays whom (game-code/PPOnline/source/sss_portraits.cpp;
docs/game-code.md §11, "Ranked's stage select shows both players").

Two instances, Fox (A) and Falco (B), are paired from the Ranked queue. On game 1's stage select
each machine shows both players' CSS portraits left and right of the stage preview, in panels of
their in-game port's colour, with their names: the plugin's debug block (g_sssPortraits, through
PPOnlineMain.map) says both portraits were read and copied out, this player's on the left, and the
two machines agree on who has which port. Game 1 is then played to its end under the gameplay
session (online_set.play_game compares both machines' states); the portraits are freed with the
stage select. Game 2's stage select shows them again.

Screenshots go to run/artifacts/game-bridge/ranked-sss-portraits/. Needs what test_online_ranked.py
needs.
"""

from __future__ import annotations

import struct
from typing import Any, Callable

import pytest

from ppharness import brawl as B
from ppharness.instance import DolphinInstance

from test_online_game import PLUGIN, Game, _boot, _both, _peer_shown, online_set
from test_online_ranked import (SSS, STARTERS, _both_on, _paired, _search_ranked, _to_ranked_css,
                                _wait, _wait_view, backend)  # noqa: F401

pytestmark = [pytest.mark.dolphin, pytest.mark.server, pytest.mark.gpu]

# g_sssPortraits: shown, loads, loaded, failed, freed, drawn (u32s), texNo[2] (u16), port[2] (u8).
DEBUG_FMT = ">6I2H2B"


def _sym(g: Game, name: str) -> int:
    """A plugin symbol's address: its offset in PPOnlineMain.map plus its section's address."""
    secidx = {".text": 1, ".ctors": 2, ".dtors": 3, ".rodata": 4, ".data": 5, ".bss": 6}
    u32 = lambda a: struct.unpack(">I", g.c.read_mem(a, 4))[0]
    m = u32(0x800030C8)
    addrs = None
    while m:
        i, nxt, _p, nsec, so = struct.unpack(">5I", g.c.read_mem(m, 0x14))
        if i == 20560:
            secs = g.c.read_mem(so, 8 * nsec)
            addrs = [struct.unpack_from(">I", secs, 8 * k)[0] & ~1 for k in range(nsec)]
            break
        m = nxt
    assert addrs, "the plugin is not loaded"
    cur = None
    for line in (PLUGIN.parent / "PPOnlineMain.map").read_text().splitlines():
        parts = line.split()
        if len(parts) == 5 and parts[4].startswith("."):
            cur = parts[4]
        elif len(parts) == 5 and parts[4] == name:
            return addrs[secidx[cur]] + int(parts[0], 16)
    raise KeyError(name)


def _portraits(g: Game) -> dict[str, Any]:
    v = struct.unpack(DEBUG_FMT, g.c.read_mem(_sym(g, "g_sssPortraits"), struct.calcsize(DEBUG_FMT)))
    return {"shown": v[0], "loads": v[1], "loaded": v[2], "failed": v[3], "freed": v[4], "drawn": v[5],
            "texNo": v[6:8], "port": v[8:10]}


def _both_shown(g: Game, loaded_before: int) -> dict[str, Any]:
    """Both portraits copied out on this stage select and drawn."""
    def done() -> dict[str, Any] | None:
        f = _portraits(g)
        return f if f["loaded"] >= loaded_before + 2 and all(f["texNo"]) and f["drawn"] > 0 else None
    return _wait(done, 30, f"{g.inst.name}: both portraits")


@pytest.mark.slow
def test_ranked_stage_select_shows_both_players(backend, dolphin: Callable[..., DolphinInstance],
                                                gpu_backend: str) -> None:
    test = "ranked-sss-portraits"
    ua, ub = backend.create_user("rspa", "RSPA"), backend.create_user("rspb", "RSPB")
    a = _boot(dolphin, "rsp-a", backend, ua, gpu_backend, test, "gameplay")
    b = _boot(dolphin, "rsp-b", backend, ub, gpu_backend, test, "gameplay")
    _both(a.to_main_menu, b.to_main_menu)
    _both(a.to_online_page, b.to_online_page)
    _both(lambda: a.to_css("ranked"), lambda: b.to_css("ranked"))
    B.css_pick_character(b.c, 0, B.CSS_ID["falco"])
    B.step(b.c, 10)
    _to_ranked_css(a, b)
    _search_ranked(a)
    _search_ranked(b)
    _peer_shown(a, ub)
    _peer_shown(b, ua)
    _paired(a, b)

    _both_on([a, b], SSS, "game 1: both on the stage select")
    for g in (a, b):
        _wait_view(g, lambda v: v["type"] == 1 and sorted(v["selectable"]) == STARTERS, "the starters")
    fa, fb = _both_shown(a, 0), _both_shown(b, 0)
    assert fa["failed"] == 0 and fb["failed"] == 0, (fa, fb)
    # The two machines agree: A's left (its own) port is B's right one, and they are P1 and P2.
    assert sorted(fa["port"]) == [0, 1] and fa["port"] == tuple(reversed(fb["port"])), (fa, fb)
    # Each sees the same two portraits, its own on the left.
    assert fa["texNo"] == tuple(reversed(fb["texNo"])), (fa, fb)
    a.shot("g1-sss")
    b.shot("g1-sss")

    online_set.wait(lambda: online_set.scene(a.c) == "scMelee", 300, "game 1")
    B.step(a.c, 5)
    B.step(b.c, 5)
    for g in (a, b):
        f = _portraits(g)
        assert f["freed"] == f["loaded"] and f["texNo"] == (0, 0), f   # freed with the stage select
    online_set.play_game([a, b], 1)

    _both_on([a, b], SSS, "game 2: both on the stage select")
    for g in (a, b):
        _both_shown(g, 2)
        g.shot("g2-sss")
