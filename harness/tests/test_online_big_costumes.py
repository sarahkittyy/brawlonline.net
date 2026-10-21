"""The biggest costume files in an online match (bug #75, KUWA#443, 2026-10-21).

Brawl copies every costume PAC (P+'s CompressPAC.asm makes them all compressed) into a block of
the Network heap to decompress it, and the plugin's menus kept their texture copies there: once a
game had been online the heap had about 1,057,000 bytes in one block, and FitSamus27.pac (Samus's
costume 9, "EOD", 1,096,170 bytes) did not fit. The copy went to address 0 (memcpy, PC 0x80004354),
CXUncompressLZ read from 0 and every machine loading that fighter froze on the load screen.
FitLinkAltR.pac (Link, R held on START, 1,061,129 bytes) was over the line too. The menus' copies
are now freed before any scene that loads fighters (labels.cpp Labels::copyBlock / releaseCopies,
online_menu.cpp releaseMenuCopies; docs/game-code.md section 2, "Heap budget").

Two games on the Direct CSS with the gameplay backend (test_online_game.py's setup): A plays
Samus in costume 9, B Link with R held on START, so both machines load both files. With the
plugin before the fix (PPHARNESS_PLUGIN) the games never leave the load screen.
"""

from __future__ import annotations

import time
from typing import Callable

import pytest

from ppharness import brawl as B
from ppharness.backend import OnlineBackend
from ppharness.instance import DolphinInstance

from test_online_game import Game, _confirmed_checksums, _connected_direct, backend  # noqa: F401 (fixture)
import online_set  # noqa: E402  (test_online_game put harness/tools on the path)

pytestmark = [pytest.mark.dolphin, pytest.mark.server, pytest.mark.gpu, pytest.mark.slow]

MM_PLAYERS, PLAYER_SIZE = 0x98, 0x5C   # gmGlobalModeMelee players; +0x05 the costume file
SAMUS_EOD = 9          # Samus's costume list index of FitSamus27.pac (P+ v3.2.1)
FIT_SAMUS_EOD = 27
FIT_ALT_R = 61         # what P+'s Gecko lines set an AltR costume to (FitLinkAltR.pac)


def _mm_costumes(g: Game) -> list[int]:
    mm = g.c.read_u32(g.c.read_u32(0x805A00E0) + 0x08)
    raw = g.c.read_mem(mm + MM_PLAYERS, 4 * PLAYER_SIZE)
    return [raw[i * PLAYER_SIZE + 5] for i in range(4)]


def _samus_eod(g: Game) -> None:
    B.css_pick_character(g.c, 0, B.CSS_ID["samus"])
    for _ in range(SAMUS_EOD):   # X: the next costume
        g.c.pad_script(0, [{"buttons": ["X"], "hold": 3}, {"buttons": [], "hold": 6}])
        B.step(g.c, 12)
    area = B.read_css_area(g.c.read_mem, 0).area
    assert g.c.read_u32(area + 0x1BC) == SAMUS_EOD, g.c.read_u32(area + 0x1BC)


def _link_alt_r(g: Game) -> None:
    B.css_pick_character(g.c, 0, B.CSS_ID["link"])
    g.start_with = ("R",)


def _frames(g: Game) -> int:
    return B.read_frame_counters(g.c.read_mem)["game_frame"]


def test_biggest_costumes_load_online(backend: OnlineBackend, dolphin: Callable[..., DolphinInstance],
                                      gpu_backend: str) -> None:
    """Samus's FitSamus27.pac on A and Link's FitLinkAltR.pac on B: both machines run the match
    (the frame counters move, the confirmed frames agree) with those costumes."""
    a, b, ua, ub = _connected_direct(backend, dolphin, gpu_backend, "big-costumes", ("eoda", "altb"),
                                     on_css=(_samus_eod, _link_alt_r))
    try:
        online_set.wait(lambda: all(online_set.gstatus(g.c)["phase"] == "running" for g in (a, b)), 180,
                        "the match to run on both")
    except TimeoutError:
        # (no screenshots: a frozen game never writes one)
        before = {g.name: _frames(g) for g in (a, b)}
        time.sleep(2)
        frames = {g.name: (before[g.name], _frames(g)) for g in (a, b)}
        raise AssertionError(f"the match never ran (frame counters 2 s apart: {frames}); "
                             "a fighter's costume did not load (bug #75)") from None
    a.steps("wait 300")
    host, join = (a, b) if online_set.local(a.c)["local_port"] == 0 else (b, a)
    port_a, port_b = online_set.local(a.c)["local_port"], online_set.local(b.c)["local_port"]
    for g in (a, b):
        c = _mm_costumes(g)
        assert c[port_a] == FIT_SAMUS_EOD and c[port_b] == FIT_ALT_R, (g.name, c)
        kinds = {p.port: p.character for p in B.read_players(g.c.read_mem)}
        assert kinds.get(port_a) == B.CSS_TO_CHAR_KIND[B.CSS_ID["samus"]], (g.name, kinds)
        assert kinds.get(port_b) == B.CSS_TO_CHAR_KIND[B.CSS_ID["link"]], (g.name, kinds)
        f0 = _frames(g)
        time.sleep(1)
        assert _frames(g) > f0, (g.name, "the match is not running")
        g.shot("01-match")
    ck = _confirmed_checksums(host, join)
    assert ck["mismatches"] == 0 and ck["compared"] > 300, ck
    for g in (a, b):
        g.c.mm_cancel()
