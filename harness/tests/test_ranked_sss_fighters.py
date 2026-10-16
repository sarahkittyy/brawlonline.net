"""Ranked's stage select shows both players' fighters (game-code/PPOnline/source/sss_fighters.cpp;
docs/game-code.md §11, "Ranked's stage select shows both fighters").

Two instances, Fox (A) and Falco (B), are paired from the Ranked queue. On game 1's stage select
each machine stands this player's fighter on the left and the opponent's on the right, each in its
idle animation with its name and rating under it: the plugin's debug block (g_sssFighters, through
PPOnlineMain.map) says both were made, from two different resource heaps, with both name lines.
Game 1 is then played to its end under the gameplay session (online_set.play_game compares both
machines' states), so the fighters loaded on the stage select reach the match on both. Game 2's
stage select shows the last game's fighters again.

Screenshots go to run/artifacts/game-bridge/ranked-sss-fighters/. Needs what test_online_ranked.py
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

# g_sssFighters: preloads, made, deleted, scans, state[2], model[2], tex[2], motion[2], heap[2],
# obj[2] (u32s), then floats the tests do not read.
DEBUG_FMT = ">4I12I"


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


def _fighters(g: Game) -> dict[str, Any]:
    v = struct.unpack(DEBUG_FMT, g.c.read_mem(_sym(g, "g_sssFighters"), struct.calcsize(DEBUG_FMT)))
    return {"made": v[1], "deleted": v[2], "state": v[4:6], "heap": v[12:14]}


def _labels(g: Game) -> int:
    a = _sym(g, "g_sssFighters")
    return struct.unpack(">I", g.c.read_mem(a + 0xB0, 4))[0]


def _both_shown(g: Game, made_before: int) -> dict[str, Any]:
    """Both fighters made on this stage select (state 0xF: model and motion found, made, animated)."""
    return _wait(lambda: (lambda f: f if f["made"] >= made_before + 2 and all(s == 0xF for s in f["state"])
                          else None)(_fighters(g)), 30, f"{g.inst.name}: both fighters")


@pytest.mark.slow
def test_ranked_stage_select_shows_both_fighters(backend, dolphin: Callable[..., DolphinInstance],
                                                 gpu_backend: str) -> None:
    test = "ranked-sss-fighters"
    ua, ub = backend.create_user("rsfa", "RSFA"), backend.create_user("rsfb", "RSFB")
    a = _boot(dolphin, "rsf-a", backend, ua, gpu_backend, test, "gameplay")
    b = _boot(dolphin, "rsf-b", backend, ub, gpu_backend, test, "gameplay")
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
    for g in (a, b):
        f = _both_shown(g, 0)
        assert f["heap"][0] != f["heap"][1], f   # each from its own slot's files
        assert _labels(g) == 2, _labels(g)
        g.shot("g1-sss")

    online_set.wait(lambda: online_set.scene(a.c) == "scMelee", 300, "game 1")
    for g in (a, b):
        f = _fighters(g)
        assert f["deleted"] == f["made"], f   # deleted with the stage select, before the match
    online_set.play_game([a, b], 1)

    _both_on([a, b], SSS, "game 2: both on the stage select")
    for g in (a, b):
        _both_shown(g, 2)
        g.shot("g2-sss")
