"""The input display (core/ui InputDisplayUI, card 775): the controller a player plays on, drawn
over the game with what it holds, from the game's own pad records (Online/InputView.h).

- Offline (`test_offline_every_controller`): one game in a local match, the display taking
  whichever controller was pressed last: the GameCube controller in port 1, then Dolphin's
  emulated Wii Remote alone, with a Nunchuk and with a Classic Controller (its extension switched
  live). For each: the box's controller type, the buttons, sticks and triggers it shows, and a
  capture of the window. EmissaryRecomp (PPHARNESS_RECOMP) has the harness's GameCube controller
  only: the Wii parts are left out there.
- Online (`test_online_own_then_every_player`): A on a Wii Remote with a Nunchuk (PLUS locks in),
  B on a GameCube controller (Dolphin, or the recomp with PPHARNESS_RECOMP_NAMES=inp-b), in
  Unranked. Each shows its own port only; then the launcher's input-display.json (as Settings >
  Game changes it while the game runs) turns on "Every player" on B and moves its box: B draws A's
  Wii controller too, as A switches its extension to a Classic Controller and to none. A's replay
  played back on a third Dolphin shows both players' boxes.

Windows only (window captures). Captures: run/artifacts/input-display/.

    PPHARNESS_INSTANCE_PREFIX=inp- python -m pytest harness/tests/test_input_display.py
"""

from __future__ import annotations

import json
import os
import sys
import time
from pathlib import Path
from typing import Any, Callable

import pytest

from ppharness import brawl as B
from ppharness.backend import OnlineBackend
from ppharness.instance import DolphinInstance, InstanceConfig
from ppharness.recomp import RecompInstance

from test_online import _wait
from test_online_game import ROOT, _boot, _peer_shown, online_set
from test_online_unranked import _search_unranked, backend  # noqa: F401
from test_online_wii_remote import NUNCHUK, Wii, _port, _recording, _running, _search_with, _to_unranked_css
from test_replay import _viewer
from test_ui_crossplay import grab, place

pytestmark = [pytest.mark.dolphin, pytest.mark.gpu,
              pytest.mark.skipif(sys.platform != "win32", reason="window capture is Windows'")]

ART = ROOT / "run" / "artifacts" / "input-display"
# Bottom left, a little under a third of the picture's width.
SHOWN = {"InputDisplay": True, "InputDisplayX": 20, "InputDisplayY": 690, "InputDisplayWidth": 300}


def status(inst: DolphinInstance) -> dict[str, Any]:
    return inst.client.call("input_display")


def until_box(inst: DolphinInstance, what: str, ok: Callable[[list[dict[str, Any]]], bool],
              timeout: float = 20) -> list[dict[str, Any]]:
    """The boxes of the last frame drawn, once `ok` holds for them."""
    got: dict[str, Any] = {}

    def check() -> bool:
        got["s"] = status(inst)
        return bool(got["s"]["enabled"]) and ok(got["s"]["boxes"])

    try:
        _wait(check, timeout, what, interval=0.1)
    except AssertionError as e:
        raise AssertionError(f"{what}: {got.get('s')}") from e
    return got["s"]["boxes"]


def one(boxes: list[dict[str, Any]], kind: str, pressed: set[str] = frozenset()) -> bool:
    return len(boxes) == 1 and boxes[0]["type"] == kind and pressed <= set(boxes[0]["pressed"])


def write_request(inst: DolphinInstance, **values: Any) -> None:
    """input-display.json, as the launcher writes it when the setting changes mid-game."""
    online = Path(inst.user_dir) / "Online"
    online.mkdir(exist_ok=True)
    tmp = online / "input-display.json.tmp"
    tmp.write_text(json.dumps({"version": 1, **values}), encoding="utf-8")
    os.replace(tmp, online / "input-display.json")


def test_offline_every_controller(dolphin: Callable[..., DolphinInstance]) -> None:
    cfg = InstanceConfig(cpu_thread=True, video_backend="D3D11", dolphin_ini={"Online": SHOWN},
                         wiimote_ini={"Wiimote1": {"Source": 1, "Extension": "None"}})
    inst = dolphin("solo", config=cfg, platform="win32", client_timeout=60.0)
    recomp = isinstance(inst, RecompInstance)
    inst.start()
    place(inst, 40, 60)
    c = inst.client
    st = c.wait_state("running", timeout=120)
    c.wait_frame(st.frame + 300, timeout_ms=120000)
    B.start_match_local(c, {0: "mario", 1: "mario"}, stage="final_destination")
    out = ART / ("recomp" if recomp else "dolphin")

    def wait(frames: int = 20) -> None:
        c.wait_frame(c.status().frame + frames, timeout_ms=20000)

    # The GameCube controller in port 1: A, the stick right and up, the C-stick down, L half in.
    c.pad_set(0, buttons=["A", "Z"], main=(255, 200), c=(128, 0), l=128)
    boxes = until_box(inst, "the GameCube controller", lambda b: one(b, "gc", {"A", "Z"}))
    gc = boxes[0]
    assert gc["stick"][0] == 127 and gc["stick"][1] > 50, gc
    assert gc["sub"][1] == -128 and 100 < gc["triggers"][0] < 160, gc
    assert gc["port"] == -1 and gc["pad"] == 0, gc
    # Where the setting puts it: 30% of the picture's width, half as tall, inside the window.
    assert abs(gc["h"] * 2 - gc["w"]) < 1 and gc["x"] >= 0 and gc["y"] >= 0, gc
    grab(inst, out / "offline-gamecube.png")
    c.pad_set(0)
    wait()
    if recomp:
        return

    wii = c.wiimote_input
    # A Wii Remote alone, held sideways: A, 1, and the D-pad's up as the player holds it (the
    # remote's right, upright).
    wii(0, extension="none", wiimote={"Buttons/A": 1, "Buttons/1": 1, "D-Pad/Right": 1})
    boxes = until_box(inst, "the Wii Remote", lambda b: one(b, "wiimote", {"A", "1", "UP"}), 30)
    assert boxes[0]["pad"] == 4, boxes
    grab(inst, out / "offline-wii-remote.png")
    # With a Nunchuk (plugged in at rest: a stick held as it is plugged in is its centre): its
    # stick left, C and Z, and the Wii Remote's B.
    wii(0, extension="nunchuk")
    until_box(inst, "the Nunchuk plugged in", lambda b: one(b, "nunchuk"), 30)
    wii(0, extension="nunchuk", wiimote={"Buttons/B": 1}, nunchuk={"Stick/X": -1, "Buttons/C": 1, "Buttons/Z": 1})
    until_box(inst, "the Nunchuk", lambda b: one(b, "nunchuk", {"B", "C", "Z"}) and b[0]["stick"][0] == -100, 30)
    grab(inst, out / "offline-nunchuk.png")
    # A Classic Controller: both sticks, ZR, b, the D-pad's left and L half in (analog).
    wii(0, extension="classic")
    until_box(inst, "the Classic Controller plugged in", lambda b: one(b, "classic"), 30)
    wii(0, extension="classic", classic={"Left Stick/Y": 1, "Right Stick/X": 1, "Buttons/ZR": 1, "Buttons/B": 1,
                                         "D-Pad/Left": 1, "Triggers/L-Analog": 0.5})
    until_box(inst, "the Classic Controller",
              lambda b: (one(b, "classic", {"ZR", "b", "LEFT"}) and b[0]["stick"][1] == 100 and b[0]["sub"][0] == 100
                         and b[0]["triggers"][0] > 100), 30)
    grab(inst, out / "offline-classic.png")
    # Let go: the box stays on the controller pressed last, at rest.
    wii(0, extension="classic")
    boxes = until_box(inst, "the Classic Controller at rest", lambda b: one(b, "classic") and not b[0]["pressed"])
    # Off: nothing drawn.
    write_request(inst, enabled=False, x=20, y=690, width=300)
    _wait(lambda: not status(inst)["enabled"] and not status(inst)["boxes"], 10, "the display off")
    wii(0, clear=True)


@pytest.mark.server
def test_online_own_then_every_player(backend: OnlineBackend,  # noqa: F811
                                      dolphin: Callable[..., DolphinInstance],
                                      gpu_backend: str) -> None:
    test = "input-display"
    ua, ub = backend.create_user("ivy", "IVY"), backend.create_user("jude", "JUDE")
    windowed: Callable[..., DolphinInstance] = lambda *a, **kw: dolphin(*a, platform="win32", **kw)
    a = _boot(windowed, "inp-a", backend, ua, gpu_backend, test, "gameplay", artifacts=ART,
              dolphin_ini={"Online": SHOWN}, wiimote_ini={"Wiimote1": {"Source": 1, "Extension": "Nunchuk"}})
    b = _boot(windowed, "inp-b", backend, ub, gpu_backend, test, "gameplay", artifacts=ART,
              dolphin_ini={"Online": SHOWN})
    recomp_b = isinstance(b.inst, RecompInstance)
    place(a.inst, 40, 60)
    place(b.inst, 1000, 60)
    nunchuk = Wii(a, "nunchuk")
    nunchuk.set()
    _to_unranked_css((a, b))
    _search_with(a, lambda: nunchuk.press(wiimote={"Buttons/+": 1}))
    time.sleep(2.2)  # the server takes one ticket per account per 2 s
    _search_unranked(b)
    _peer_shown(a, ub)
    _peer_shown(b, ua)
    players = (a, b)
    _running(players)
    a.steps("wait 120")
    pa, pb = _port(a), _port(b)

    # Each its own controller, on its own port.
    nunchuk.set(wiimote={"Buttons/A": 1}, nunchuk={"Stick/Y": 1, "Buttons/Z": 1})
    b.c.pad_set(0, buttons=["B"], main=(0, 128))
    box_a = until_box(a.inst, "A's Nunchuk", lambda bx: one(bx, "nunchuk", {"A", "Z"}))[0]
    box_b = until_box(b.inst, "B's GameCube controller", lambda bx: one(bx, "gc", {"B"}))[0]
    assert box_a["port"] == pa and box_a["pad"] == 4 + pa and box_a["stick"][1] == 100, box_a
    assert box_b["port"] == pb and box_b["pad"] == pb and box_b["stick"][0] == -128, box_b
    grab(a.inst, ART / "online-a-own-nunchuk.png")
    grab(b.inst, ART / f"online-b-own-gamecube{'-recomp' if recomp_b else ''}.png")

    # The launcher turns on "Every player" on B and moves its box to the top right, mid-game.
    write_request(b.inst, enabled=True, x=640, y=40, width=340, everyone=True)
    boxes = until_box(b.inst, "both players on B", lambda bx: len(bx) == 2, 15)
    by_port = {x["port"]: x for x in boxes}
    assert by_port[pa]["type"] == "nunchuk" and by_port[pb]["type"] == "gc", boxes
    assert boxes[0]["y"] < boxes[0]["h"], boxes   # at the top now
    grab(b.inst, ART / f"online-b-every-player-nunchuk{'-recomp' if recomp_b else ''}.png")

    # A switches its Wii Remote's extension: a Classic Controller, then none. B draws them from
    # the session's records.
    for ext, kind, parts, pressed in (
            ("classic", "classic", {"classic": {"Buttons/A": 1, "Triggers/R-Analog": 1, "Left Stick/X": 1}}, {"a"}),
            ("none", "wiimote", {"wiimote": {"Buttons/2": 1, "D-Pad/Down": 1}}, {"2", "RIGHT"})):
        a.c.wiimote_input(0, extension=ext)   # plugged in at rest (a held stick would be its centre)
        until_box(a.inst, f"A's {kind} plugged in", lambda bx, k=kind: one(bx, k), 30)
        a.c.wiimote_input(0, extension=ext, **parts)
        until_box(a.inst, f"A's {kind}", lambda bx, k=kind, p=pressed: one(bx, k, p), 30)
        until_box(b.inst, f"A's {kind} on B",
                  lambda bx, k=kind, p=pressed: any(x["port"] == pa and x["type"] == k and p <= set(x["pressed"])
                                                    for x in bx), 30)
        grab(a.inst, ART / f"online-a-own-{kind}.png")
        grab(b.inst, ART / f"online-b-every-player-{kind}{'-recomp' if recomp_b else ''}.png")
    a.c.wiimote_input(0, extension="nunchuk")
    b.c.pad_set(0)

    # A quits (1+B+Z+PLUS with a Nunchuk); its replay plays back with both players' boxes.
    a.c.wiimote_input(0, extension="nunchuk", wiimote={"Buttons/1": 1, "Buttons/B": 1, "Buttons/+": 1},
                      nunchuk={"Buttons/Z": 1})
    online_set.wait(lambda: all(online_set.scene(g.c) == online_set.CSS for g in players), 120,
                    "both back on the CSS")
    a.c.wiimote_input(0, extension="nunchuk")
    online_set.wait(lambda: not a.c.call("replay_recording")["recording"], 60, "A's recording to end")
    fa = _recording(a)
    for g in players:
        g.c.mm_cancel()
    assert {s[pa * 0x40 + 0x3F] for s in fa.slots} >= {NUNCHUK}, "A's records"

    v = _viewer(lambda *x, **kw: dolphin(*x, platform="win32", **kw), backend, gpu_backend, test)
    place(v.inst, 520, 560)
    write_request(v.inst, enabled=True, x=20, y=690, width=300)
    B.write_rules(v.c, stocks=4, minutes=8, items_off=True)   # the live games' rules
    v.c.call("replay_play", path=str(fa.path))
    _wait(lambda: v.c.call("replay_status")["started"], 240, "the replay's match to start")
    boxes = until_box(v.inst, "every player in the replay", lambda bx: len(bx) == 2, 120)
    assert {x["port"] for x in boxes} == {pa, pb}, boxes
    time.sleep(2)
    grab(v.inst, ART / "replay-both-players.png")
    st = _wait(lambda: (lambda s: s if s["finished"] or s["phase"] == "error" else None)(v.c.call("replay_status")),
               600, "the replay's end")
    assert st["phase"] != "error" and st["desync_frame"] == -1, st
