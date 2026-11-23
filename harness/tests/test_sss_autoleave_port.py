"""The stage select's automatic A has a port (Sarah's board, 2026-11-18: "Stage select's automatic A
reads past the pad table (port -1)"; kalomaze's a3393d88c on his fork; docs/game-code.md "That A's
port").

The stage select that leaves by itself (anyone picks, Ranked) decides with the port the plugin
gives it (task+0x278 = LOCAL lockPad) instead of scanning for a pad that pressed (-1). Direct, where
everyone picks: B's stage select leaves when A picks. Game 1 B locked in with the GameCube pad
(lockPad 0), game 2 with its Wii Remote's PLUS (lockPad 4). Each time the probe reads B's task as
it leaves: +0x278 (controller in charge), +0x25C (sub stage kind), +0x260 (the port that decided),
and the stage select data's sub stage kind afterwards. Both games' setups and setup hashes agree
and the confirmed frames match. With the plugin before the fix (PPHARNESS_PLUGIN) the task leaves
with 0xF0 in charge and port -1.
"""
from __future__ import annotations

import time
from typing import Any, Callable

import pytest

from ppharness import brawl as B
from ppharness.backend import OnlineBackend
from ppharness.instance import DolphinInstance

from test_online import _wait
from test_online_game import (Game, _boot, _both, _confirmed_checksums, _peer_shown, _searching,
                              backend, online_set, ppom)  # noqa: F401  (the backend fixture)
from test_room_anyone_picks import FD, SSS, lock_of
from test_room_stage_list import BF

pytestmark = [pytest.mark.dolphin, pytest.mark.server, pytest.mark.gpu]

GAME_GLOBAL = 0x805A00E0


def probe_leave(g: Game, timeout: float = 60) -> dict[str, Any]:
    """Read g's stage select task until it is gone; the last values seen."""
    last: dict[str, Any] | None = None
    end = time.monotonic() + timeout
    while time.monotonic() < end:
        v = B.read_sss(g.c.read_mem)
        if v is None:
            if last is not None:
                break
            time.sleep(0.005)
            continue
        t = v.task
        raw = g.c.read_mem(t + 0x25C, 8)
        last = {"ctrl": g.c.read_u32(t + 0x278), "sub": raw[0], "port": int.from_bytes(raw[4:8], "big", signed=True)}
    assert last is not None, f"{g.name}: never saw the stage select"
    gg = g.c.read_u32(GAME_GLOBAL)
    sel = g.c.read_u32(gg + 0x14)
    last["sel_sub"] = g.c.read_u8(sel + 0x24)
    last["lock_pad"] = ppom.read_local(g.c, ppom.find_block(g.c))["lock_pad"]
    return last


def _connect(backend: OnlineBackend, dolphin: Callable[..., DolphinInstance], gpu: str) -> tuple[Game, Game]:
    ua = backend.create_user("alva", "ALVA")
    ub = backend.create_user("bria", "BRIA")
    a = _boot(dolphin, "al-a", backend, ua, gpu, "sss-autoleave", "gameplay")
    b = _boot(dolphin, "al-b", backend, ub, gpu, "sss-autoleave", "gameplay",
              wiimote_ini={"Wiimote1": {"Source": 1, "Extension": "None"}})
    b.c.wiimote_input(0, extension="none", wiimote={})
    _both(a.to_main_menu, b.to_main_menu)
    _both(a.to_online_page, b.to_online_page)
    _both(lambda: a.to_css("direct"), lambda: b.to_css("direct"))
    for g in (a, b):
        B.write_rules(g.c, stocks=1, minutes=2, items_off=True)
        ppom.allow_test_rules(g.c)
        g.panel = online_set.css_panel(g.c)
    a.open_keypad()
    a.type_code(ub.connect_code)
    _searching(a, ub.connect_code)
    time.sleep(2.2)
    b.open_keypad()
    b.type_code(ua.connect_code)
    _peer_shown(a, ub)
    _peer_shown(b, ua)
    for g in (a, b):
        _wait(lambda g=g: g.c.mm_status()["handoff"] == "started", 30, "the hand-off")
    return a, b


@pytest.mark.slow
def test_automatic_leave_takes_the_locked_in_controller(backend: OnlineBackend, dolphin: Callable[..., DolphinInstance], gpu_backend: str) -> None:
    a, b = _connect(backend, dolphin, gpu_backend)
    gs = [a, b]
    out: dict[str, Any] = {}

    # Game 1: B on its GameCube pad (the keypad's search).
    _wait(lambda: all(g.scene() == SSS for g in gs), 90, "game 1: both on the stage select")
    for g in gs:
        B.step(g.c, 40)
    B.sss_pick_stage(a.c, FD, 0)
    out["g1"] = g1 = probe_leave(b)
    print("game 1 B leaves:", g1)
    assert g1["lock_pad"] == 0 and g1["ctrl"] == 0 and g1["port"] == 0, g1
    rep = online_set.play_game(gs, 1)
    print("game 1:", {k: rep[k] for k in ("stage", "setup_hash", "checksums")})
    assert rep["checksums"]["mismatches"] == 0, rep
    assert rep["stage"] == FD, rep
    online_set.back_on_css(gs, 1)

    # Game 2: B locks in with its Wii Remote's PLUS (lockPad 4).
    a.steps("tap START 8", "wait 10")
    for _ in range(6):
        b.c.wiimote_input(0, extension="none", wiimote={"Buttons/+": 1})
        b.steps("wait 8")
        b.c.wiimote_input(0, extension="none", wiimote={})
        b.steps("wait 8")
        try:
            _wait(lambda: lock_of(b)["ready"], 3, "B locked in with PLUS")
            break
        except AssertionError:
            continue
    assert ppom.read_local(b.c, ppom.find_block(b.c))["lock_pad"] == 4, lock_of(b)
    _wait(lambda: all(g.scene() == SSS for g in gs), 90, "game 2: both on the stage select")
    for g in gs:
        B.step(g.c, 40)
    B.sss_pick_stage(a.c, BF, 0)
    out["g2"] = g2 = probe_leave(b)
    print("game 2 B leaves:", g2)
    assert g2["lock_pad"] == 4 and g2["ctrl"] == 4 and g2["port"] == 4, g2
    online_set.wait(lambda: all(online_set.gstatus(g.c)["phase"] == "running" for g in gs), 240, "game 2 running")
    for g in gs:
        g.steps("wait 600")
    st = [online_set.gstatus(g.c) for g in gs]
    setups = [B.read_match_setup(g.c.read_mem) for g in gs]
    ck = _confirmed_checksums(a, b)
    print("game 2:", setups[0], [s.get("setup_hash") for s in st], ck)
    assert setups[0] == setups[1], setups
    assert st[0].get("setup_hash") == st[1].get("setup_hash"), st
    assert ck["compared"] > 300 and ck["mismatches"] == 0, ck
    a.shot("g2-match")
    print("leaves:", out)
