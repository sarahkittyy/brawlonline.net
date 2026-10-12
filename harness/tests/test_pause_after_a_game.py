"""Pausing in an online match under rollback (the user's report, prod 2026-10-12: "i paused and it
crashed!", game 2 of a room set, 180 ms ping, rollbacks on 40% of the frames).

Every pause makes sora_melee preload Brawl's HOME Menu archive (/system/homebutton/home_eng.pac,
0xD0260 bytes and a 0x1D000 buffer, an asynchronous read of about 40 frames into the Network heap);
the unpause unloads it, and the match's end loads it again. gfHomeMenu, the Network heap and the
file thread are outside the rollback set while the pause is rolled back, so rollbacks across a
pause or an unpause left a loaded archive the menu no longer knew of. The next pause's load found
no room ("File Read (DVD): Alloc Buffer failed /system/homebutton/home_eng.pac", the Network heap
dumped with 0x1D000 free at most): the player's game then crashed (invalid writes at 0x3Axx from
0x801D7E9C, a jump to 0x42480000), the harness's game hung. Under bad_wifi here the third pause of
game 1 did it. Now the plugin bans the HOME Menu (ban bit 6) for the whole online match
(online_match.cpp banHomeMenu): no pause and no match end loads the archive, and the ban is lifted
on the CSS.
"""
from __future__ import annotations

import struct
from typing import Callable

import pytest

from ppharness.backend import OnlineBackend
from ppharness.client import PadInput
from ppharness.instance import DolphinInstance

from test_online_game import _connected_direct, _p2p_through_netsim, backend  # noqa: E402,F401
import online_set  # noqa: E402

HBM_READ = "/system/homebutton/"
HBM_FAIL = "Alloc Buffer failed /system/homebutton"
CRASH = "Non-recoverable Exception"
HOME_MENU = 0x805A0090   # gfHomeMenu* g_gfHomeMenu; +0xB0 bits 12-18 the ban mask (setBan 0x80037D44)
PLUGIN_BAN = 1 << 6


def _ban_mask(g) -> int:
    hm = struct.unpack(">I", g.c.read_mem(HOME_MENU, 4))[0]
    return (struct.unpack(">I", g.c.read_mem(hm + 0xB0, 4))[0] >> 12) & 0x7F


def _walk(g, n: int = 200) -> None:
    walk = [PadInput(main=(40, 128), hold=7), PadInput(main=(216, 128), hold=7)]
    g.c.pad_script(0, walk * n)


def _log(g) -> str:
    return (g.inst.user_dir / "Logs" / "dolphin.log").read_text(errors="replace")


def _hbm_lines(g) -> list[str]:
    return [ln[ln.find("File Read"):] if "File Read" in ln else ln[-60:] for ln in _log(g).splitlines()
            if HBM_READ in ln or CRASH in ln]


def _check(a, b, where: str) -> None:
    for g in (a, b):
        print(g.name, where, _hbm_lines(g), "rollbacks", online_set.gstatus(g.c).get("rollbacks"))
    for g in (a, b):
        log = _log(g)
        assert HBM_FAIL not in log and CRASH not in log, (g.name, where, _hbm_lines(g))
        # Not even one load: in an online match the archive is never read.
        assert HBM_READ not in log, (g.name, where, _hbm_lines(g))


def _pauses(a, b, where: str, cycles: int = 4) -> None:
    """A pauses and unpauses `cycles` times while B moves (B's inputs keep both rolling back
    through the pause and the unpause)."""
    _walk(b)
    _walk(a, 20)
    a.steps("wait 120")
    for g in (a, b):
        assert _ban_mask(g) & PLUGIN_BAN, (g.name, where, hex(_ban_mask(g)))
    for i in range(cycles):
        a.steps("tap START 8", "wait 90")
        if i == 0:
            for g in (a, b):
                g.shot(f"{where.replace(' ', '')}-paused")
            assert all(online_set.scene(g.c) == "scMelee" for g in (a, b))
        a.steps("tap START 8", "wait 60")
        _check(a, b, f"{where} pause {i + 1}")


@pytest.mark.slow
@pytest.mark.parametrize("preset", ["lan", "bad_wifi"])
def test_pauses_under_rollback(backend: OnlineBackend, dolphin: Callable[..., DolphinInstance],
                               gpu_backend: str, preset: str) -> None:
    """Game 1: four pauses while the other player moves, then the pause screen's quit; game 2:
    four more pauses. No pause loads the HOME Menu archive (ban bit 6 is set on both in the
    match), no load fails, nothing crashes or hangs, and both are still in game 2's match at the
    end. Between the games, on the CSS, the plugin's ban is lifted. Screenshots:
    run/artifacts/game-bridge/pause-rollback-<preset>/."""
    sims, inis = _p2p_through_netsim(preset) if preset != "lan" else ([], (None, None))
    try:
        a, b, ua, ub = _connected_direct(backend, dolphin, gpu_backend, f"pause-rollback-{preset}",
                                         ("uma", "vera"), stocks=4, dolphin_ini=inis)
        online_set.wait(lambda: all(online_set.gstatus(g.c)["phase"] == "running" for g in (a, b)), 240,
                        "game 1 to run on both")
        _pauses(a, b, "game 1")
        a.steps("tap START 8", "wait 60", "hold L+R+A+START 90")
        online_set.wait(lambda: all(online_set.scene(g.c) == online_set.CSS for g in (a, b)), 45,
                        "both back on the CSS")
        for g in (a, b):
            g.steps("wait 60")
            assert not _ban_mask(g) & PLUGIN_BAN, (g.name, "banned on the CSS", hex(_ban_mask(g)))
        _check(a, b, "after game 1")
        for g in (a, b):
            g.steps("tap START 8", "wait 30")
        online_set.wait(lambda: all(online_set.gstatus(g.c)["phase"] == "running" for g in (a, b)), 120,
                        "game 2 to run on both")
        _pauses(a, b, "game 2")
        st = [online_set.gstatus(g.c) for g in (a, b)]
        print("rollbacks", [s["rollbacks"] for s in st])
        for g, s in zip((a, b), st):
            assert online_set.scene(g.c) == "scMelee", g.name
            assert s["phase"] == "running", (g.name, s["phase"], s.get("end_reason"))
        if preset != "lan":
            assert all(s["rollbacks"] > 0 for s in st), [s["rollbacks"] for s in st]
    finally:
        for sim in sims:
            sim.stop()
