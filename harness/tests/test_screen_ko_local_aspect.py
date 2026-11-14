"""Any screen shape (docs/game-code.md, Any screen shape): each machine draws for the shape it shows
the picture at, and the match is still played with the same 16:9 cameras on every machine.

The camera's aspect frames the fighters, and a screen KO places the fighter from the camera, so
every machine plays with the 16:9 cameras (Phase 25: test_screen_ko_aspect.py,
test_screen_ko_camera.py). What a machine draws is its own: Dolphin writes the shape it shows the
picture at into LOCAL (PPOM v12, from Graphics > Aspect Ratio), and the plugin scales the
projections the cameras hand to GX to that shape (local_aspect.cpp). The cameras are not touched.

``test_the_game_draws_for_the_screens_shape``: one game with the plugin in a local Versus match.
For Dolphin's own 16:9, a 21:9 and a 4:3 picture (Graphics > Aspect Ratio > Custom, as a player
sets it) LOCAL holds that shape, the scene's projection in the GX state has it, the screenshot has
it, the damage panels are at the screen's bottom edge (at 4:3 too, where the 16:9 picture's own
bottom is an eighth of the screen higher), and gfCameraManager's cameras stay as they were.

``test_screen_kos_between_a_21_9_and_a_4_3_screen``: a thin wrapper around
``tools/gprb_session.py``: two Dolphins through netsim, Zelda up smashing an idle Mario on Final
Destination at damage ratio 2.0, every KO off the top made a screen KO (``0x8087C314``, the branch
that picks a star KO, a nop on both), one machine drawing for 21:9 and the other for 4:3
(``--aspects``, written into LOCAL as Dolphin does). The match runs through its screen KOs with
every confirmed checksum equal. About 4 minutes.
"""

from __future__ import annotations

import json
import os
import struct
import sys
import time
from pathlib import Path
from typing import Any, Callable, Dict, List

import pytest

from ppharness import brawl as B
from ppharness import paths
from ppharness.instance import DolphinInstance, InstanceConfig

ROOT = paths.workspace_root()
sys.path.insert(0, str(ROOT / "harness" / "tools"))
sys.path.insert(0, str(ROOT / "tools" / "gamecode"))
sys.path.insert(0, str(ROOT / "tools" / "sdcard"))
sys.path.insert(0, str(Path(__file__).resolve().parent))

from test_screen_ko_aspect import STAR_KO_BRANCH, _top_kos  # noqa: E402

PLUGIN = Path(os.environ.get("PPHARNESS_PLUGIN") or ROOT / "game-code" / "PPOnline" / "PPOnline.rel")
ARTIFACTS = ROOT / "run" / "artifacts" / "local-aspect"

CAMERA_MANAGER_PTR = 0x8059FFC0   # gfCameraManager: six cameras of 0x134 (aspect at +0xE4), its own at +0x738
GAME_ASPECT = 16 / 9 * 0.974432   # what the cameras draw for: 16:9 times P+'s video mode scale
ASPECT_PM_WIDE, ASPECT_CUSTOM_STRETCH = "5", "7"   # AspectMode: P+'s 16:9 (the default), exactly the custom ratio


def _cameras(c: Any) -> List[float]:
    """What the aspect setter writes: the manager's aspect and scale and its six cameras' aspects."""
    mgr = int.from_bytes(c.read_mem(CAMERA_MANAGER_PTR, 4), "big")
    f = lambda a: round(struct.unpack(">f", c.read_mem(a, 4))[0], 6)  # noqa: E731
    return [f(mgr + 0x738), f(mgr + 0x73C)] + [f(mgr + 0xE4 + i * 0x134) for i in range(6)]


def _drawn(c: Any, samples: int = 300) -> Dict[str, Any]:
    """The scene's perspective projections seen in the GX state over some frames, as aspects
    (gprb_session.drawn_aspects: those drawn into the whole picture)."""
    import gprb_session

    seen = gprb_session.drawn_aspects(c, samples)
    return {"samples": len(seen), "min": min(seen) if seen else None, "max": max(seen) if seen else None}


def _panel_pixels(path: Path) -> int:
    """The damage digits' white pixels in the strip at the screen's bottom edge where the panels
    are (0.86 to 0.97 of the height, the middle 60% of the width): about 350 with two players at
    0%, about 40 when the panels hang an eighth of the screen higher."""
    from PIL import Image
    im = Image.open(path).convert("RGB")
    w, h = im.size
    strip = im.crop((int(0.2 * w), int(0.86 * h), int(0.8 * w), int(0.97 * h)))
    return sum(1 for r, g, b in strip.getdata() if r > 225 and g > 225 and b > 225)


def _local_aspect(c: Any) -> float:
    import ppom
    b = ppom.find_block(c)
    assert b.version >= 12 and b.local_size >= ppom.L_ASPECT + 4, (b.version, b.local_size)
    return struct.unpack(">f", c.read_mem(b.local + ppom.L_ASPECT, 4))[0]


def _show_at(c: Any, shape: tuple[int, int] | None) -> None:
    """Graphics > Aspect Ratio: P+'s 16:9 (None), or Custom (Stretch) with this ratio."""
    values = [("AspectRatio", ASPECT_PM_WIDE)] if shape is None else [
        ("CustomAspectRatioWidth", str(shape[0])), ("CustomAspectRatioHeight", str(shape[1])),
        ("AspectRatio", ASPECT_CUSTOM_STRETCH)]
    for key, value in values:
        c.call("config_set", system="Graphics", section="Settings", key=key, value=value)


@pytest.mark.dolphin
@pytest.mark.gpu
@pytest.mark.slow
def test_the_game_draws_for_the_screens_shape(dolphin: Callable[..., DolphinInstance], gpu_backend: str) -> None:
    import gameplay_rollback as G
    import patch_sd

    if not PLUGIN.exists():
        pytest.skip(f"{PLUGIN} not built (game-code/build.sh)")
    inst = dolphin("local-aspect", config=InstanceConfig(cpu_thread=True, video_backend=gpu_backend,
                                                         standard_controllers=(0, 1)), client_timeout=60.0)
    d = inst.create()
    patch_sd.patch_image(d / "Wii" / "sd.raw", [(PLUGIN.read_bytes(), patch_sd.plugin_sd_path(PLUGIN))])
    inst.launch()
    c = inst.connect()
    c.wait_state("running", timeout=120)
    for port in (0, 1):
        c.pad_set(port)
    # With the plugin P+ boots to the ONLINE page: B to the main menu's top page, up to VERSUS.
    B.wait_scene(c, [B.Scene.MAIN_MENU, B.Scene.CSS], 60 * 120)
    if B.read_scene(c.read_mem).scene is B.Scene.MAIN_MENU:
        B.step(c, 60)
        B.tap(c, 0, ["B"], hold=3, release=60)
        B.tap(c, 0, ["DUP"], hold=4, release=40)
        B.boot_to_css(c, 0)
    G.history_a(c, ["mario", "fox"], "final_destination", [])
    B.wait_scene(c, [B.Scene.IN_MATCH], 60 * 120)
    deadline = time.monotonic() + 120
    while (B.read_frame_counters(c.read_mem).get("game_frame") or 0) < 400:   # the fighters are in
        assert time.monotonic() < deadline, "the match never started"
        time.sleep(0.5)

    cameras = _cameras(c)
    assert cameras[0] == round(struct.unpack(">f", struct.pack(">f", 16 / 9))[0], 6), cameras
    seen: Dict[str, Any] = {}
    for name, shape in (("16-9", None), ("21-9", (21, 9)), ("4-3", (4, 3)), ("16-9-again", None)):
        _show_at(c, shape)
        time.sleep(1.5)
        ARTIFACTS.mkdir(parents=True, exist_ok=True)
        shot = c.screenshot(str((ARTIFACTS / f"final-destination-{name}.png").resolve()))
        seen[name] = {"local": _local_aspect(c), "drawn": _drawn(c), "shot": (shot.width, shot.height),
                      "panels": _panel_pixels(ARTIFACTS / f"final-destination-{name}.png"), "cameras": _cameras(c)}
    print(json.dumps(seen, indent=1), flush=True)
    for name, want in (("16-9", None), ("21-9", 21 / 9), ("4-3", 4 / 3), ("16-9-again", None)):
        s = seen[name]
        # Dolphin says how it shows the picture ...
        assert abs(s["local"] - (want or 69 / 40)) < 0.005, (name, s)
        assert abs(s["shot"][0] / s["shot"][1] - (want or 69 / 40)) < 0.01, (name, s)
        # ... the scene is drawn for that shape (Dolphin's own 16:9: as the game draws it) ...
        assert s["drawn"]["samples"] > 50, (name, s)
        for edge in ("min", "max"):
            assert abs(s["drawn"][edge] - (want or GAME_ASPECT)) < 0.005, (name, s)
        # ... with the damage panels at the screen's bottom edge ...
        assert s["panels"] > 200, (name, s)
        # ... and the cameras the match is played with are the 16:9 ones throughout.
        assert s["cameras"] == cameras, (name, s, cameras)


@pytest.mark.dolphin
@pytest.mark.slow
def test_screen_kos_between_a_21_9_and_a_4_3_screen(tmp_path: Path) -> None:
    import gprb_session

    if not PLUGIN.exists():
        pytest.skip(f"{PLUGIN} not built (game-code/build.sh)")
    shapes = (21 / 9, 4 / 3)
    report, traces = tmp_path / "session.json", tmp_path / "traces"
    rc = gprb_session.main([
        "--preset", "typical", "--cpu", "dc", "--ports", "1,2", "--chars", "zelda,mario",
        "--stage", "final_destination", "--frames", "7000", "--timeout", "400",
        "--modes", "upsmash,idle", "--damage-ratio", "2.0",
        "--plugin", str(PLUGIN), "--aspects", ",".join(f"{s:.4f}" for s in shapes),
        "--poke-match", f"{STAR_KO_BRANCH:#x}=60000000",
        "--name-prefix", "pytest-screenko-shape", "--save-traces", str(traces), "--json", str(report),
    ])
    run = json.loads(report.read_text())[0]
    print(json.dumps({k: run.get(k) for k in ("aspect_state", "checksums", "trace")}, indent=1)[:3000])
    assert rc == 0 and not run.get("error"), run.get("error")
    # Each machine had its own shape and drew the scene for it.
    for state, shape in zip(run["aspect_state"], shapes):
        assert abs(state["local"] - shape) < 0.001 and state["samples"] > 50, run["aspect_state"]
        assert abs(state["drawn_min"] - shape) < 0.005 and abs(state["drawn_max"] - shape) < 0.005, run["aspect_state"]
    rows = json.loads(next(traces.glob("*.json")).read_text())["p1"]
    kos = _top_kos([r for r in rows if not r[10]])
    assert kos, "no KO off the top: the test proves nothing"
    checksums = run["checksums"][0]
    assert checksums["compared"] > kos[0] - 120 + 200, checksums
    assert checksums["mismatches"] == 0, checksums
    assert run["trace"][0]["diverged_at"] is None, run["trace"]
    # A live desync ends the session before its frames are confirmed: each machine's own report.
    for st in run["status"]:
        assert st["desyncs_detected"] == 0 and st["end_reason"] != "desync", st
