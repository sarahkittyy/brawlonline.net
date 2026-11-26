"""Offline Versus after the boot's ONLINE page, as the launcher's Play boots (bug report #346).

The plugin makes Brawl's Wi-Fi task (muWifiInterfaceTask, 0x805A02A4) on the ONLINE pages
(netmenu.cpp ensureWifiTask), and nothing removed it. Offline, the Versus results screen then ran
as a Wi-Fi one: the Wi-Fi countdown, no exit when everyone was ready, and two 600-frame network
waits ("isReadyNextGame(true/false) time out!!!"); on EmissaryRecomp every results screen held
30 s or more whatever was pressed. Now the task goes outside the Wi-Fi sequence and the menus
(NetMenu::dropWifiTaskOffline) and the ONLINE page makes it again.

Boots with the built plugin on the SD card (to the ONLINE page), B to the main menu, GROUP >
BRAWL, a 1-minute match that P2 self-destructs in once, A on the results screen (both ports),
then back to the ONLINE page. Dolphin by default; EmissaryRecomp with PPHARNESS_RECOMP.

    python -m pytest harness/tests/test_offline_versus_results.py
"""

from __future__ import annotations

import os
import sys
import time
from pathlib import Path
from typing import Any, Callable

import pytest

from ppharness import brawl as B
from ppharness import paths
from ppharness.instance import DolphinInstance, InstanceConfig
from ppharness.recomp import RecompInstance

ROOT = paths.workspace_root()
sys.path.insert(0, str(ROOT / "tools" / "sdcard"))
import patch_sd  # noqa: E402

pytestmark = [pytest.mark.dolphin, pytest.mark.gpu]

PLUGIN = Path(os.environ.get("PPHARNESS_PLUGIN") or ROOT / "game-code" / "PPOnline" / "PPOnline.rel")
WIFI_TASK = 0x805A02A4   # g_muWifiInterfaceTask


def u32(c: Any, addr: int) -> int:
    return int.from_bytes(c.read_mem(addr, 4), "big")


def test_offline_results_after_online_page(dolphin: Callable[..., DolphinInstance], gpu_backend: str) -> None:
    if not PLUGIN.exists():
        pytest.skip(f"{PLUGIN} not built (game-code/build.sh)")
    inst = dolphin("vsres", config=InstanceConfig(cpu_thread=True, video_backend=gpu_backend),
                   client_timeout=60.0)
    d = inst.create()
    patch_sd.patch_image(d / "Wii" / "sd.raw", [(PLUGIN.read_bytes(), patch_sd.plugin_sd_path(PLUGIN))])
    inst.launch()
    inst.connect()
    engine = "recomp" if isinstance(inst, RecompInstance) else "dolphin"
    c = inst.client
    c.wait_state("running", timeout=120)
    B.wait_scene(c, [B.Scene.MAIN_MENU], 60 * 60)
    B.step(c, 120)
    assert u32(c, WIFI_TASK) != 0, "the boot's ONLINE page made no Wi-Fi task"
    # B: the main menu, PLAY ONLINE highlighted; up to GROUP, A, A (BRAWL): the Versus CSS.
    for buttons in (["B"], ["DUP"], ["A"], ["A"]):
        B.tap(c, 0, buttons, hold=4, release=4)
        B.step(c, 60)
    B.wait_scene(c, [B.Scene.CSS], 60 * 20)
    B.step(c, 60)
    assert u32(c, WIFI_TASK) == 0, "the Wi-Fi task outlived the ONLINE page into offline Versus"
    B.start_match_local(c, {0: "mario", 1: "mario"}, stage="final_destination", stocks=4, minutes=1,
                        force_rules=True)
    c.pad_set(1, main=(0, 128))   # P2 walks off the edge once: the time out then has a winner
    time.sleep(4)
    c.pad_set(1)
    t0 = time.monotonic()
    while B.read_scene(c.read_mem).scene is not B.Scene.RESULTS:
        time.sleep(2)
        assert time.monotonic() - t0 < 240, "no results screen"
    time.sleep(4)
    tr = time.monotonic()
    while B.read_scene(c.read_mem).scene is B.Scene.RESULTS and time.monotonic() - tr < 90:
        for p in (0, 1):
            c.pad_set(p, buttons=["A"])
        time.sleep(0.15)
        for p in (0, 1):
            c.pad_set(p)
        time.sleep(0.6)
    left = time.monotonic() - tr
    print(f"{engine}: left the results screen {left:.1f} s after the first A")
    # Before: 30 s on EmissaryRecomp (the countdown, then the two network waits).
    assert B.read_scene(c.read_mem).scene is not B.Scene.RESULTS and left < 15, left
    # Back online: hold B from the CSS to the menus, B to the top page, down to PLAY ONLINE, A.
    B.wait_scene(c, [B.Scene.CSS], 60 * 20)
    B.step(c, 60)
    B.tap(c, 0, ["B"], hold=60, release=4)
    B.wait_scene(c, [B.Scene.MAIN_MENU], 60 * 20)
    B.step(c, 90)
    for buttons in (["B"], ["DDOWN"], ["A"]):
        B.tap(c, 0, buttons, hold=4, release=4)
        B.step(c, 60)
    B.step(c, 60)
    assert u32(c, WIFI_TASK) != 0, "the ONLINE page made no Wi-Fi task again"
