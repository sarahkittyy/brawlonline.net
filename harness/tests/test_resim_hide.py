"""Re-run passes nobody sees draw no scene layers (docs/gameplay-rollback-status.md, "Re-run passes
nobody sees hide the scene's layers").

A sync test rolls back every frame: at distance 4 each update re-runs 3 frames, and the first 2 of
them are followed by another re-run pass, so their pictures are never shown. In those passes the
session clears the scene root's per-layer draw bits (and the shadow pass's bit), and puts them back
before the next save or load. The test checks that:

- every frame's gameplay checksum (frame counter, RNGs, each fighter's instance, damage, stocks,
  position and action) equals its first run, which drew everything: 0 desyncs, with items on;
- 2 passes per update were hidden;
- once the session has ended, every bit is back as the stage left it.

Run: ``pytest -m dolphin tests/test_resim_hide.py`` (about 3 minutes).
"""

from __future__ import annotations

import sys
import time
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "harness" / "tools"))

import gameplay_rollback as G  # noqa: E402
import gprb_synctest as T  # noqa: E402
from ppharness import brawl as B  # noqa: E402

pytestmark = [pytest.mark.dolphin, pytest.mark.slow]

SCENE_ROOT = 0x8059FF80   # g_gfSceneRoot
LAYER_FLAGS = 0x399       # + 0x18 * layer
LAYERS = 9
ROOT_SHADOW = 0x464
DISTANCE = 4
FRAMES = 1500


def _flags(c) -> list:
    root = c.read_u32(SCENE_ROOT)
    return [c.read_mem(root + LAYER_FLAGS + 0x18 * k, 1)[0] for k in range(LAYERS)] + \
        [c.read_mem(root + ROOT_SHADOW, 1)[0]]


@pytest.mark.parametrize("scenario,items", [("bf-fox-falco", False), ("fd-mario-marth", True)])
def test_hidden_rerun_passes_keep_gameplay_and_restore_the_bits(scenario: str, items: bool) -> None:
    sc = T.scenario(scenario)
    with G.instances([(f"resimhide-{scenario}", dict(cpu_thread=True, controllers=sc.controllers))]) as (inst,):
        c = inst.client
        if items:
            T.set_items(c, 4)
        T.setup_match_n(c, sc)
        c.call("gprb_synctest", distance=DISTANCE, region_set="gp-v29", hash_regions=False, start_frame=120,
               ports=sc.ports)
        deadline = time.monotonic() + 300
        while c.call("gprb_status")["phase"] != "running":
            assert time.monotonic() < deadline, c.call("gprb_status")
            time.sleep(0.2)
        stop_at = time.monotonic() + 240

        def stop() -> bool:
            s = c.call("gprb_status")
            return s["phase"] != "running" or s["current_frame"] >= FRAMES or time.monotonic() > stop_at

        setup = B.read_match_setup(c.read_mem)
        while not stop():
            T.fight_n(c, sc, FRAMES, f"resimhide-{scenario}", setup.stage_kind if setup else None, stop)
        status = c.call("gprb_status")
        c.call("gprb_stop")
        deadline = time.monotonic() + 30
        while c.call("gprb_status")["phase"] == "running":
            assert time.monotonic() < deadline
            time.sleep(0.1)
        c.pause()
        after = _flags(c)

    assert status["desyncs_detected"] == 0, status["desync_log"][:4]
    assert status["current_frame"] >= FRAMES - 10, status["current_frame"]
    updates = status["rollbacks"]
    # Every update but the session's first few re-runs DISTANCE - 1 frames; all but the last of
    # them are hidden.
    assert status["resim_hidden_passes"] >= (updates - 10) * (DISTANCE - 2), (status["resim_hidden_passes"], updates)
    # Battlefield and Final Destination set every layer's draw bit (0x80) and the shadow pass's
    # (0x40); none of them may stay cleared.
    assert all(after[k] & 0x80 for k in range(LAYERS)), [hex(b) for b in after]
    assert after[LAYERS] & 0x40, [hex(b) for b in after]
