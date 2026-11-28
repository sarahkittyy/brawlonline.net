"""The input display in Other Games (Sarah's board: "input display doesnt work on 'other games'").

Brawl's display reads Brawl's own pad records (gfPadSystem, Online/InputView.h), which no other
game has, and which a whole-machine session's Brawl build never gets sampled from (its frame-end
hook is the session's). There the display shows what Dolphin's hardware gives the game instead
(dolphin Core/HW/InputDisplayFeed.cpp): the SI's GameCube controllers and the Bluetooth stack's Wii
Remotes, turned into the same records.

- `test_gamecube_game_offline`: a GameCube disc (not Brawl) played offline, the GameCube controller
  in port 1: its buttons, sticks and an analog trigger.
- `test_machine_session_own_port`: the same disc in a whole-machine session's sync test (every frame
  loaded and run again) as the player in slot 2: the box is port 2's, steady through the re-runs.
- `test_brawl_build_machine_session_wii`: Brawl's disc in a whole-machine session played with a Wii
  Remote (an Other Games room of a Brawl build): the Wii Remote alone, with a Nunchuk and with a
  Classic Controller, as Brawl's own display shows them (test_input_display.py's values).

Windows only (window captures). Captures: run/artifacts/other-games-input-display/.

    PPHARNESS_INSTANCE_PREFIX=ogi- python -m pytest harness/tests/test_other_games_input_display.py
"""

from __future__ import annotations

import json
import os
import sys
from pathlib import Path
from typing import Any, Callable

import pytest

from ppharness import paths
from ppharness.instance import DolphinInstance, InstanceConfig

from test_input_display import SHOWN, one, status, until_box
from test_online import _wait
from test_online_game import ROOT
from test_ui_crossplay import grab, place

pytestmark = [pytest.mark.dolphin, pytest.mark.gpu,
              pytest.mark.skipif(sys.platform != "win32", reason="window capture is Windows'")]

ART = ROOT / "run" / "artifacts" / "other-games-input-display"
GC_DISC = Path(os.environ.get("PPHARNESS_GC_DISC", r"E:\Roms\GCN\Mario Party 6 (USA).nkit.iso"))


def _gc_disc() -> Path:
    if not GC_DISC.is_file():
        pytest.skip(f"no GameCube disc at {GC_DISC} (PPHARNESS_GC_DISC)")
    return GC_DISC


def _boot(dolphin: Callable[..., DolphinInstance], name: str, disc: Path,
          session: dict[str, Any] | None = None, **cfg: Any) -> DolphinInstance:
    config = InstanceConfig(cpu_thread=True, video_backend="D3D11", dolphin_ini={"Online": SHOWN},
                            config_args=["Dolphin.Core.WiiSDCard=False", f"Dolphin.Core.DefaultISO={disc}"],
                            **cfg)
    env = {"PPR_MACHINE_SESSION": json.dumps(session)} if session else {}
    inst = dolphin(name, config=config, boot=str(disc), connect_timeout=120, client_timeout=60.0, env=env,
                   platform="win32")
    inst.create()
    inst.launch()
    inst.connect()
    return inst


def _place(inst: DolphinInstance) -> None:
    """On the test screen, once the game's window is up."""
    def placed() -> bool:
        try:
            place(inst, 40, 60)
            return True
        except AssertionError:
            return False
    _wait(placed, 120, "the game's window", interval=1.0)


def _frames(inst: DolphinInstance, n: int) -> None:
    c = inst.client
    c.wait_frame(c.status().frame + n, timeout_ms=120000)


def test_gamecube_game_offline(dolphin: Callable[..., DolphinInstance]) -> None:
    inst = _boot(dolphin, "ogi-gc", _gc_disc(), standard_controllers=(0,), other_ports="none")
    c = inst.client
    c.wait_state("running", timeout=120)
    _place(inst)
    _frames(inst, 300)
    # A, Z, the stick right and up, the C-stick down, L half in.
    c.pad_set(0, buttons=["A", "Z"], main=(255, 200), c=(128, 0), l=128)
    gc = until_box(inst, "the GameCube controller", lambda b: one(b, "gc", {"A", "Z"}))[0]
    assert gc["stick"] == [127, 72] and gc["sub"] == [0, -128] and gc["triggers"] == [128, 0], gc
    assert gc["port"] == -1 and gc["pad"] == 0, gc
    grab(inst, ART / "gamecube-offline.png")
    # Let go: the same controller at rest.
    c.pad_set(0)
    until_box(inst, "the controller at rest", lambda b: one(b, "gc") and not b[0]["pressed"])


def test_machine_session_own_port(dolphin: Callable[..., DolphinInstance]) -> None:
    inst = _boot(dolphin, "ogi-ms", _gc_disc(), {"slot": 1, "synctest": 2},
                 standard_controllers=(0,), other_ports="none")
    c = inst.client
    ms = lambda: c.call("machine_status")  # noqa: E731
    until = lambda what, ok, t: until_box(inst, what, ok, t)  # noqa: E731
    _wait(lambda: ms().get("phase") == "running" and (ms().get("frame") or 0) >= 300, 240,
          "the session running", interval=1.0)
    _place(inst)
    # The local controller plays slot 2: port 2's box, the session's input (delay and all).
    c.pad_set(0, buttons=["B", "START"], main=(0, 128), r=255)
    box = until("port 2's controller", lambda b: one(b, "gc", {"B", "START"}), 30)[0]
    assert box["port"] == 1 and box["pad"] == 1 and box["stick"][0] == -128 and box["triggers"][1] == 255, box
    # The re-runs never show: a hundred frames' boxes all hold it.
    for _ in range(20):
        _frames(inst, 5)
        b = status(inst)["boxes"]
        assert one(b, "gc", {"B", "START"}) and b[0]["port"] == 1, b
    grab(inst, ART / "gamecube-machine-session.png")
    c.pad_set(0)
    until("at rest", lambda b: one(b, "gc") and not b[0]["pressed"], 30)
    assert ms().get("desyncs") == 0, ms()


def test_brawl_build_machine_session_wii(dolphin: Callable[..., DolphinInstance]) -> None:
    iso: Path = paths.game_iso() or paths.fallback_iso()
    inst = _boot(dolphin, "ogi-wii", iso, {"slot": 0, "synctest": 2, "controller": "wiimote"},
                 wiimote_ini={"Wiimote1": {"Source": 1, "Extension": "None"}})
    c = inst.client
    _wait(lambda: (c.call("machine_status").get("frame") or 0) >= 600, 240, "the session's frame 600",
          interval=1.0)
    _place(inst)
    wii = c.wiimote_input
    # Alone, held sideways: A, 1, the D-pad's up as the player holds it (the remote's right).
    wii(0, extension="none", wiimote={"Buttons/A": 1, "Buttons/1": 1, "D-Pad/Right": 1})
    boxes = until_box(inst, "the Wii Remote", lambda b: one(b, "wiimote", {"A", "1", "UP"}), 30)
    assert boxes[0]["pad"] == 4 and boxes[0]["port"] == 0, boxes
    grab(inst, ART / "wii-remote.png")
    # A Nunchuk (plugged in at rest): its stick left, C and Z, the Wii Remote's B.
    wii(0, extension="nunchuk")
    until_box(inst, "the Nunchuk plugged in", lambda b: one(b, "nunchuk"), 30)
    wii(0, extension="nunchuk", wiimote={"Buttons/B": 1}, nunchuk={"Stick/X": -1, "Buttons/C": 1, "Buttons/Z": 1})
    until_box(inst, "the Nunchuk", lambda b: one(b, "nunchuk", {"B", "C", "Z"}) and b[0]["stick"] == [-100, 0], 30)
    grab(inst, ART / "nunchuk.png")
    # A Classic Controller: both sticks, ZR, b, the D-pad's left, L half in.
    wii(0, extension="classic")
    until_box(inst, "the Classic Controller plugged in", lambda b: one(b, "classic"), 30)
    wii(0, extension="classic", classic={"Left Stick/Y": 1, "Right Stick/X": 1, "Buttons/ZR": 1, "Buttons/B": 1,
                                         "D-Pad/Left": 1, "Triggers/L-Analog": 0.5})
    until_box(inst, "the Classic Controller",
              lambda b: (one(b, "classic", {"ZR", "b", "LEFT"}) and b[0]["stick"] == [0, 100]
                         and b[0]["sub"] == [100, 0] and 100 < b[0]["triggers"][0] < 160), 30)
    grab(inst, ART / "classic.png")
    wii(0, clear=True)
    assert c.call("machine_status").get("desyncs") == 0
