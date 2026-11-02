"""Screen KOs online with a player who picked Standard (4:3) in Brawl's Options > Screen (MATT#695,
2026-11-02: "desyncs ... on star ko ... not every star ko, an occasional one"; 30 of 220 games
ended in a desync, their opponents' other games almost never).

Phase 25 (test_screen_ko_aspect.py) pinned the Wii's aspect ratio (launcher) and Brawl's screen
setting (Dolphin, PinScreenSetting, at every match start). Options > Screen also applies the
change at once through gfCameraManager's aspect setter (fn_8001A108: the cameras' aspect, the
manager's +0x738 / +0x73C, the render mode's width), and the manager is made once at boot, so the
camera stayed 4:3 in every match whatever the pinned setting said, and a screen KO, placed from the
camera, desynced. The plugin's match setup now runs that setter for 16:9 on every machine.

Two games connect in Direct (the gameplay backend). The second first does on its main menu what
Options > Screen > Standard does (the setter with 1, written here as the setter writes it, without
the VIConfigure that only changes the picture). Once the match runs, every KO off the top is made a
screen KO on both (0x8087C314, the branch that picks a star KO, a nop), and the host up smashes the
idle joiner (Final Destination, damage ratio 2.0). Both cameras must be the same 16:9 and the
confirmed checksums equal through the KOs to game set. With the plugin from before
(PPHARNESS_PLUGIN), the second game's camera stayed 4:3 and the first screen KO desynced (game
frame 3747, "gprb: desync at frame 3628" on both).
"""

from __future__ import annotations

import json
import struct
import sys
from pathlib import Path
from typing import Any, Callable, Dict, Iterator

import pytest

from ppharness.backend import OnlineBackend
from ppharness.instance import DolphinInstance

sys.path.insert(0, str(Path(__file__).resolve().parent))
from test_online import _make_backend  # noqa: E402
from test_online_game import ROOT, _connected_direct, _log  # noqa: E402
from test_screen_ko_aspect import STAR_KO_BRANCH, _top_kos  # noqa: E402
import online_set  # noqa: E402
from ppharness import brawl as B  # noqa: E402

pytestmark = [pytest.mark.dolphin, pytest.mark.server, pytest.mark.gpu]

CAMERA_MANAGER_PTR = 0x8059FFC0   # gfCameraManager (made at boot)
RENDER_APP_PTR = 0x805A0098       # the application's render mode, which the setter's VI half reads
VI_MODES = 0x80423488             # per video mode: {u16 width, f32 scale} for 4:3, then for 16:9
APP_WIDE = 0x20                   # render mode +0x4C: 16:9
SCREEN_RECORD = 0x24              # GameGlobal+0x24: gmGlobalRecord; menu data +0x810, screen byte +0x28
SCREEN_BYTE = 0x810 + 0x28
CAMERAS, CAMERA_SIZE, CAMERA_ASPECT = 6, 0x134, 0xE4
FINAL_DESTINATION = 0x02


@pytest.fixture(scope="module")
def backend(tmp_path_factory: pytest.TempPathFactory) -> Iterator[OnlineBackend]:
    """Direct on Final Destination only (nothing but the up smashes sends the idle player off the
    top) at damage ratio 2.0 (fewer up smashes to a KO), as Phase 25's reproduction."""
    rules = json.loads((ROOT / "server" / "config" / "rulesets.json").read_text(encoding="utf-8"))
    rules["direct"]["stages"] = [FINAL_DESTINATION]
    rules["direct"]["rules"]["damageRatio"] = 2.0
    path = tmp_path_factory.mktemp("rulesets") / "rulesets.json"
    path.write_text(json.dumps(rules), encoding="utf-8")
    be = _make_backend(rulesets_file=path)
    try:
        yield be
    finally:
        be.stop()


def _u32(c: Any, addr: int) -> int:
    return int.from_bytes(c.read_mem(addr, 4), "big")


def _f32(x: float) -> float:
    return struct.unpack(">f", struct.pack(">f", x))[0]


def camera(c: Any) -> Dict[str, Any]:
    """What the aspect setter writes: the manager's aspect and scale, its cameras' aspects, and
    the render mode's 16:9 bit and scale."""
    mgr, app = _u32(c, CAMERA_MANAGER_PTR), _u32(c, RENDER_APP_PTR)
    f = lambda a: round(struct.unpack(">f", c.read_mem(a, 4))[0], 6)  # noqa: E731
    return {"aspect": f(mgr + 0x738), "scale": f(mgr + 0x73C),
            "cameras": [f(mgr + CAMERA_ASPECT + i * CAMERA_SIZE) for i in range(CAMERAS)],
            "wide": bool(c.read_mem(app + 0x4C, 1)[0] & APP_WIDE), "app_scale": f(app + 0x44)}


def options_screen_standard(g: Any) -> None:
    """Options > Screen > Standard: sora_menu_main runs gfCameraManager's aspect setter with 1
    (fn_8001A108 with 0x800384CC's render-mode half), and the record keeps the choice."""
    c = g.c
    mgr, app = _u32(c, CAMERA_MANAGER_PTR), _u32(c, RENDER_APP_PTR)
    flags = c.read_mem(app + 0x4C, 1)[0]
    assert flags & APP_WIDE, "booted 4:3: the launcher's 16:9 Wii was expected"
    row = c.read_mem(VI_MODES + _u32(c, app + 0x50) * 16, 8)   # the 4:3 entry
    width = int.from_bytes(row[0:2], "big")
    scale = struct.unpack(">f", row[4:8])[0]
    c.write_mem(app + 0x4C, bytes([flags & ~APP_WIDE]))
    c.write_mem(app + 0x44, row[4:8])
    c.write_mem(app + 0x0E, row[0:2])
    c.write_mem(app + 0x0A, ((0x2D0 - width) // 2).to_bytes(2, "big"))
    aspect = _f32(4 / 3)
    c.write_mem(mgr + 0x738, struct.pack(">f", aspect))
    c.write_mem(mgr + 0x73C, row[4:8])
    for i in range(CAMERAS):
        c.write_mem(mgr + CAMERA_ASPECT + i * CAMERA_SIZE, struct.pack(">f", aspect * scale))
    rec = _u32(c, _u32(c, B.GAME_GLOBAL_PTR) + SCREEN_RECORD) + SCREEN_BYTE
    c.write_mem(rec, bytes([c.read_mem(rec, 1)[0] & ~0xC0]))
    got = camera(c)
    assert got["aspect"] == round(aspect, 6) and not got["wide"], got


@pytest.mark.slow
def test_screen_kos_with_a_player_on_options_screen_standard(
        backend: OnlineBackend, dolphin: Callable[..., DolphinInstance], gpu_backend: str) -> None:
    def rules(g: Any) -> None:
        B.write_rules(g.c, stocks=2, minutes=4, items_off=True)

    a, b, ua, ub = _connected_direct(backend, dolphin, gpu_backend, "screen-ko-camera", ("scka", "sckb"),
                                     before_online=(None, options_screen_standard), on_css=(rules, rules))
    players = [a, b]
    online_set.wait(lambda: all(online_set.gstatus(g.c)["phase"] == "running" for g in players), 240,
                    "the game running on both")
    for g in players:
        g.c.write_mem(STAR_KO_BRANCH, bytes.fromhex("60000000"))
    cams = [camera(g.c) for g in players]
    print({g.name: cam for g, cam in zip(players, cams)}, flush=True)

    rep = online_set.play_game(players, 1, timeout=360, modes=("upsmash", "idle"))
    print({k: rep.get(k) for k in ("frames", "end", "checksums", "trace")}, flush=True)
    host, _ = online_set.roles(players)
    kos = _top_kos([r for r in host.c.call("frame_trace", since=0)["rows"] if not r[10]])
    print({"top KOs (game frames)": kos}, flush=True)
    for g in players:
        g.shot("after")
        g.c.mm_cancel()
    assert kos, "no KO off the top: the test proves nothing"
    for g in players:
        assert "desync at frame" not in _log(g) and "the game states differ" not in _log(g), g.name
    assert rep["checksums"]["mismatches"] == 0, rep["checksums"]
    assert rep["end"] == ["game set", "game set"], rep
    # Both played with the camera a fresh 16:9 boot has.
    assert cams[0] == cams[1], cams
    assert cams[0]["aspect"] == round(_f32(16 / 9), 6) and cams[0]["wide"], cams
