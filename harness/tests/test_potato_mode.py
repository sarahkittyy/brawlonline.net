"""Potato mode (docs/game-code.md, Potato mode): with the launcher's setting ([Online] PotatoMode) a match
draws no shadows and none of the listed stage pieces (backgrounds, scenery); everything else is
drawn as usual, and what the machine simulates does not change.

- One instance booted with our plugin and ``-C Dolphin.Online.PotatoMode``: GameBridge writes LOCAL.potato
  (PPOM v11) and the plugin, in a match on Battlefield, clears the scene root's shadow bit and hides
  exactly Battlefield's seven background models; with the setting off nothing is touched.
- Two machines in a gameplay rollback session (``harness/tools/gprb_session.py --potato-ports``), one in
  potato mode and one not, on a bad link (hundreds of rollbacks up to 7 frames deep): the match plays
  to game set with every confirmed frame's checksum equal on both, while each machine draws its own
  way (read back in the match).

Run: ``pytest -m dolphin tests/test_potato_mode.py`` (about 6 minutes).
"""

from __future__ import annotations

import json
import os
import sys
import time
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "harness" / "tools"))
sys.path.insert(0, str(ROOT / "tools" / "gamecode"))
sys.path.insert(0, str(ROOT / "tools" / "sdcard"))

import gameplay_rollback as G  # noqa: E402
import gprb_synctest as T  # noqa: E402
import patch_sd  # noqa: E402
import ppom  # noqa: E402
from ppharness import brawl as B  # noqa: E402

pytestmark = [pytest.mark.dolphin, pytest.mark.slow]

PLUGIN = Path(os.environ.get("PPHARNESS_PLUGIN") or ROOT / "game-code" / "PPOnline" / "PPOnline.rel")

# potato.cpp's Battlefield rows: the sky, the sun, moon, stars, clouds, the distant land, the flare.
BATTLEFIELD_HIDDEN = ["grBattleFieldSky[0]", "grBattleFieldSun[0]", "grBattleFieldMoon[0]", "grBattleFieldStar[0]",
                      "grBattleFieldCloud[0]", "grBattleFieldMainBg[0]", "grBattleFieldFlare[0]"]
SHADOW_ON = 0x40


def _local_match_potato_state(potato: bool, stage: str) -> dict:
    """Boot P+ with the plugin and [Online] PotatoMode, play a local Versus match on `stage` (GameBridge
    on, as on an install) and read the potato state 5 s into the match."""
    inst = G.make_instance(f"potato-{'on' if potato else 'off'}", cpu_thread=True, video="Null", controllers=(0, 1),
                           config=[f"Dolphin.Online.PotatoMode={'True' if potato else 'False'}"])
    patch_sd.patch_image(inst.user_dir / "Wii" / "sd.raw", [(PLUGIN.read_bytes(), patch_sd.plugin_sd_path(PLUGIN))])
    try:
        inst.start()
        c = inst.client
        deadline = time.monotonic() + 120
        while c.status().game_id != "RSBE01" or c.status().state != "running":
            assert time.monotonic() < deadline, "the game never booted"
            time.sleep(0.2)
        # With the plugin P+ boots to the ONLINE page: B to the main menu's top page, up to Brawl.
        B.wait_scene(c, [B.Scene.MAIN_MENU, B.Scene.CSS], 60 * 120)
        if B.read_scene(c.read_mem).scene is B.Scene.MAIN_MENU:
            B.step(c, 60)
            B.tap(c, 0, ["B"], hold=3, release=60)
            B.tap(c, 0, ["DUP"], hold=4, release=40)
            B.boot_to_css(c, 0)
        T.setup_match_n(c, T.Scenario(("mario", "marth"), stage, False))
        B.wait_scene(c, [B.Scene.IN_MATCH], 600)
        B.step(c, 300)
        b = ppom.find_block(c)
        assert b.version >= 11, f"plugin speaks PPOM v{b.version}: no potato mode"
        return ppom.potato_state(c, b)
    finally:
        inst.stop()


def test_potato_mode_draws_less_and_nothing_else() -> None:
    on = _local_match_potato_state(True, "battlefield")
    assert on == {"potato": 1, "shadow_bit": 0, "no_draw_models": BATTLEFIELD_HIDDEN}, on
    off = _local_match_potato_state(False, "battlefield")
    assert off == {"potato": 0, "shadow_bit": SHADOW_ON, "no_draw_models": []}, off


def test_potato_mode_fountain_keeps_its_sky() -> None:
    # Fountain of Dreams keeps its Sky model: without it the stage's clear colour (mint) shows as a
    # flat void. Hidden are only the water's reflection, BG and MainRing (the scenery ring), as
    # grIzumiMainBg[1] on the first ground and [3]/[4] on the second; Sky is [2] and still draws.
    on = _local_match_potato_state(True, "fountain_of_dreams")
    assert on == {"potato": 1, "shadow_bit": 0,
                  "no_draw_models": ["grIzumiMainBg[1]", "grIzumiMainBg[3]", "grIzumiMainBg[4]"]}, on


def test_potato_player_against_a_normal_player_stays_in_sync(tmp_path: Path) -> None:
    import gprb_session

    out = tmp_path / "potato.json"
    rc = gprb_session.main([
        "--ports", "1,2", "--chars", "peach,game_and_watch", "--plugin", str(PLUGIN), "--potato-ports", "1",
        "--stage", "pokemon_stadium_2", "--preset", "bad_wifi", "--cpu", "dc", "--minutes", "2",
        "--name-prefix", "potato", "--json", str(out),
    ])
    assert rc == 0
    rep = json.loads(out.read_text())[0]
    assert rep.get("error") is None, rep.get("error")
    assert [s["end_reason"] for s in rep["status"]] == ["game set", "game set"], [s["end_reason"] for s in rep["status"]]
    ck = rep["checksums"][0]
    assert ck["compared"] > 5000 and ck["mismatches"] == 0, ck
    assert all(s["rollbacks"] > 100 for s in rep["status"]), [s["rollbacks"] for s in rep["status"]]
    # Each machine drew its own way: P1 without shadows, the big screen and the searchlights.
    assert rep["potato_state"] == [
        {"potato": 1, "shadow_bit": 0,
         "no_draw_models": ["grStadiumAuroraVision[0]", "grStadiumSearchLight[0]"]},
        {"potato": 0, "shadow_bit": SHADOW_ON, "no_draw_models": []},
    ], rep["potato_state"]
