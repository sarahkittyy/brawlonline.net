"""Screen KOs between players whose games are set to different aspect ratios (prod 2026-10-12:
three Ranked games between two players ended DESYNC DETECTED on the frame after a KO off the top;
all three were screen KOs). Brawl frames its camera with its screen setting (Options > Screen,
which the game copies from the Wii's aspect ratio at boot), and a screen KO places the fighter from
the camera, so with one player on 4:3 and the other on 16:9 every screen KO desynced. The gameplay
session now plays every match with the fresh-install 16:9 (``PinScreenSetting``), and the launcher
pins the Wii's aspect ratio (``WII_SETTINGS_CONFIG_ARGS``).

A thin wrapper around ``tools/gprb_session.py``: two Dolphins through netsim, Zelda up smashing an
idle Mario on Final Destination at damage ratio 2.0, the joiner's screen setting set to 4:3 on its
character select, and every KO off the top made a screen KO (``0x8087C314``, the branch that picks a
star KO, is a nop on both). On a build without the pin the first screen KO desynced; with it, the
match runs through its screen KOs with every confirmed checksum equal. About 4 minutes.
"""

from __future__ import annotations

import json
import struct
import sys
from pathlib import Path

import pytest

from ppharness import paths

ROOT = paths.workspace_root()
sys.path.insert(0, str(ROOT / "harness" / "tools"))

pytestmark = [pytest.mark.dolphin, pytest.mark.slow]

SCREEN_SETTING = 0x9017BE78  # gmGlobalRecord menu data +0x28: 0xC0 with 16:9, 0x00 with 4:3
STAR_KO_BRANCH = 0x8087C314  # sora_melee: blt to "star KO" after the screen-KO roll
DEAD = 0xBD
TOP = 100.0


def _f32(u: int) -> float:
    return struct.unpack(">f", struct.pack(">I", u & 0xFFFFFFFF))[0]


def _top_kos(rows: list) -> list[int]:
    """Game frames on which a fighter entered the dead status above the stage."""
    out = []
    prev = None
    for r in rows:
        for port in range(4):
            f = r[11][port * 6:port * 6 + 6]
            was = prev[11][port * 6 + 5] if prev else None
            if f[5] == DEAD and was is not None and was != DEAD and _f32(f[4]) > TOP:
                out.append(r[0])
        prev = r
    return out


def test_screen_kos_with_a_4_3_player_stay_in_sync(tmp_path: Path) -> None:
    import gprb_session

    report, traces = tmp_path / "session.json", tmp_path / "traces"
    rc = gprb_session.main([
        "--preset", "typical", "--cpu", "dc", "--p1", "zelda", "--p2", "mario",
        "--stage", "final_destination", "--frames", "7000", "--timeout", "400",
        "--modes", "upsmash,idle", "--damage-ratio", "2.0",
        "--poke-b", f"{SCREEN_SETTING:#x}=00",
        "--poke-match", f"{STAR_KO_BRANCH:#x}=60000000",
        "--name-prefix", "pytest-screenko", "--save-traces", str(traces), "--json", str(report),
    ])
    runs = json.loads(report.read_text())
    match = runs[0]["matches"][0]
    print(json.dumps({k: match.get(k) for k in ("checksums", "trace", "notes")}, indent=1)[:3000])
    assert rc == 0 and not runs[0].get("error"), runs[0].get("error")
    rows = json.loads(next(traces.glob("*.json")).read_text())["a"]
    kos = _top_kos(rows)
    assert kos, "no KO off the top: the test proves nothing"
    assert match["checksums"]["compared"] > kos[0] - 120 + 200, match["checksums"]
    assert match["checksums"]["mismatches"] == 0, match["checksums"]
    # A live desync ends the session before its frames are confirmed: each peer's own report.
    for st in match["status"]:
        assert st["desyncs_detected"] == 0 and st["end_reason"] != "desync", st
