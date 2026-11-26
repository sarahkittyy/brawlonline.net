"""Quick chat (docs/design/quick-chat.md, core Online/QuickChat.h): the D-pad's messages on the CSS,
end to end through the real servers.

Two booted P+ Dolphins in a room (as test_chat.py): ines's launcher writes quick-chat.json, the
D-pad then stops moving her hand on the room CSS, Up opens the Up group (the chat window's frames
say so), Right sends its message to otto as chat, B closes a group without reaching the game, and
with every message cleared the D-pad moves the hand again.

PPHARNESS_QUICK_CHAT_SHOTS=<dir>: ines plays in a real window (D3D11) and each state is captured
with PrintWindow, ImGui included (the harness's screenshot is the game's picture only).

    PPHARNESS_INSTANCE_PREFIX=qc- python -m pytest harness/tests/test_quick_chat.py -m "dolphin and server"
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
from ppharness.backend import OnlineBackend, OnlineUser
from ppharness.instance import DolphinInstance, InstanceConfig

from test_chat import hold_screen, readers, texts, until_chat
from test_online import _wait
from test_rooms import PLUGIN, Player, _logged_in, _online, _to_versus_css, backend, patch_sd  # noqa: F401

pytestmark = [pytest.mark.dolphin, pytest.mark.server, pytest.mark.gpu]

SHOTS = os.environ.get("PPHARNESS_QUICK_CHAT_SHOTS")
W, H = 1280, 720

# Group then direction, clockwise from up (common/quick_chat.ts).
UP, RIGHT, DOWN, LEFT = range(4)
LONG = "changing my controller real quick, give me one minute and I'll be right back to keep playing ok :)"
assert len(LONG) <= 100
MESSAGES = [""] * 16
MESSAGES[4 * UP + UP] = "ggs!"
MESSAGES[4 * UP + RIGHT] = "one more?"
MESSAGES[4 * UP + DOWN] = "that was close, well played"
MESSAGES[4 * UP + LEFT] = "brb"
MESSAGES[4 * LEFT + RIGHT] = LONG
MESSAGES[4 * LEFT + LEFT] = "ありがとう！"


def write_quick_chat(p: Player, messages: list[str]) -> None:
    """As the launcher does: a temporary file renamed over it."""
    d = p.online_dir()
    d.mkdir(parents=True, exist_ok=True)
    tmp = d / "quick-chat.json.launcher.tmp"
    tmp.write_text(json.dumps({"version": 1, "messages": messages}), encoding="utf-8")
    os.replace(tmp, d / "quick-chat.json")


def boot(dolphin: Callable[..., DolphinInstance], name: str, be: OnlineBackend, user: OnlineUser,
         window: bool, video: str = "Null") -> Player:
    if not PLUGIN.exists():
        pytest.skip(f"{PLUGIN} not built (game-code/build.sh)")
    ini: dict[str, dict[str, Any]] = {"Online": {"UseDevServer": True, "MatchmakingPort": be.mm_port,
                                                 "DevAccountsUrl": be.accounts_url, "Music": False},
                                      "DSP": {"Muted": True, "Volume": 0}}
    if window:
        ini["Display"] = {"RenderWindowXPos": 60, "RenderWindowYPos": 60,
                          "RenderWindowWidth": W, "RenderWindowHeight": H}
        cfg = InstanceConfig(cpu_thread=True, video_backend="D3D11", dolphin_ini=ini, standard_controllers=(0, 1))
        inst = dolphin(name, config=cfg, client_timeout=60.0, platform="win32")
    else:
        cfg = InstanceConfig(cpu_thread=False, video_backend=video, dolphin_ini=ini, standard_controllers=(0, 1))
        inst = dolphin(name, config=cfg, client_timeout=60.0)
    d = inst.create()
    patch_sd.patch_image(d / "Wii" / "sd.raw", [(PLUGIN.read_bytes(), f"{patch_sd.PLUGIN_DIR}/{PLUGIN.name}")])
    user.write_user_json(d)
    inst.launch()
    inst.connect()
    p = Player(inst, user)
    p.c.online_session_backend("gameplay")
    _logged_in(inst, user)
    return p


def hand(p: Player) -> tuple[float, float]:
    a = B.read_css_area(p.c.read_mem, 0)
    assert a is not None
    return a.hand_x, a.hand_y


def last_frame(p: Player) -> dict[str, Any]:
    """The chat window's newest recorded frame (recording stays on)."""
    frames = _wait(lambda: p.c.call("chat_ui", record=True)["frames"] or None, 10, "a chat window frame")
    return frames[-1]


def quick(p: Player) -> dict[str, Any]:
    return p.c.call("quick_chat")


class Shooter:
    def __init__(self, p: Player):
        self.dir = Path(SHOTS) if SHOTS else None
        self.hwnd = 0
        if not self.dir:
            return
        sys.path.insert(0, str(Path(__file__).resolve().parent))
        from ctypes import wintypes
        import ctypes
        u32 = ctypes.WinDLL("user32", use_last_error=True)
        self.dir.mkdir(parents=True, exist_ok=True)
        found: list[int] = []
        proc = ctypes.WINFUNCTYPE(wintypes.BOOL, wintypes.HWND, wintypes.LPARAM)

        def cb(hwnd: int, _: int) -> bool:
            pid = wintypes.DWORD()
            u32.GetWindowThreadProcessId(hwnd, ctypes.byref(pid))
            if pid.value == p.inst.process.pid and u32.IsWindowVisible(hwnd):
                found.append(hwnd)
            return True
        u32.EnumWindows(proc(cb), 0)
        self.hwnd = found[0]
        wr = wintypes.RECT(0, 0, W, H)
        u32.AdjustWindowRectEx(ctypes.byref(wr), u32.GetWindowLongW(self.hwnd, -16), False,
                               u32.GetWindowLongW(self.hwnd, -20))
        u32.SetWindowPos(self.hwnd, 0, 60, 60, wr.right - wr.left, wr.bottom - wr.top, 0x0004 | 0x0010)
        time.sleep(1.5)

    def __call__(self, name: str) -> None:
        if not self.dir:
            return
        import ctypes
        from ctypes import wintypes
        from PIL import Image
        u32 = ctypes.WinDLL("user32", use_last_error=True)
        g32 = ctypes.WinDLL("gdi32", use_last_error=True)
        r = wintypes.RECT()
        u32.GetClientRect(self.hwnd, ctypes.byref(r))
        w, h = r.right, r.bottom
        hdc = u32.GetDC(self.hwnd)
        mem = g32.CreateCompatibleDC(hdc)
        bmp = g32.CreateCompatibleBitmap(hdc, w, h)
        g32.SelectObject(mem, bmp)
        u32.PrintWindow(self.hwnd, mem, 3)

        class BMI(ctypes.Structure):
            _fields_ = [("biSize", wintypes.DWORD), ("biWidth", wintypes.LONG), ("biHeight", wintypes.LONG),
                        ("biPlanes", wintypes.WORD), ("biBitCount", wintypes.WORD),
                        ("biCompression", wintypes.DWORD), ("biSizeImage", wintypes.DWORD),
                        ("biXPelsPerMeter", wintypes.LONG), ("biYPelsPerMeter", wintypes.LONG),
                        ("biClrUsed", wintypes.DWORD), ("biClrImportant", wintypes.DWORD)]
        bmi = BMI(ctypes.sizeof(BMI), w, -h, 1, 32, 0, 0, 0, 0, 0, 0)
        buf = ctypes.create_string_buffer(w * h * 4)
        g32.GetDIBits(mem, bmp, 0, h, buf, ctypes.byref(bmi), 0)
        assert self.dir is not None
        Image.frombuffer("RGBA", (w, h), buf, "raw", "BGRA", 0, 1).convert("RGB").save(self.dir / f"{name}.png")
        g32.DeleteObject(bmp)
        g32.DeleteDC(mem)
        u32.ReleaseDC(self.hwnd, hdc)


def test_quick_chat_in_a_room(backend: OnlineBackend, dolphin: Callable[..., DolphinInstance]) -> None:
    ua = backend.create_user("ines", "INES")
    ub = backend.create_user("otto", "OTTO")
    # ines draws ImGui (the chat window and quick chat's menu need a video backend).
    a = boot(dolphin, "qc-ines", backend, ua, window=bool(SHOTS), video="D3D11")
    b = boot(dolphin, "qc-otto", backend, ub, window=False)
    for p in (a, b):
        _to_versus_css(p)
        _online(p)
        hold_screen(p, "room")
    a.c.call("rooms_request", op="create", public=False)
    code = _wait(lambda: (a.rooms().get("view") or {}).get("code"), 15, "the room's code")
    b.c.call("rooms_request", op="join", code=code)
    for p in (a, b):
        until_chat(p, lambda s: s["kind"] == "room" and readers(s) == 1, "the room's chat")
    shoot = Shooter(a)
    a.c.call("chat_ui", record=True)

    # No messages (the default): the D-pad moves the hand as always.
    assert not quick(a)["usable"]
    before = hand(a)
    B.tap(a.c, 0, ["DUP"], hold=20, release=10)
    assert hand(a)[1] != before[1], "the D-pad should move the hand with no quick chat set"

    # The launcher's messages: read within a second; the D-pad is quick chat's now.
    write_quick_chat(a, MESSAGES)
    st = _wait(lambda: (lambda q: q if q["usable"] else None)(quick(a)), 10, "quick chat usable")
    assert st["messages"][4 * LEFT + RIGHT] == LONG and st["messages"][4 * LEFT + LEFT] == "ありがとう！"
    shoot("00-css")

    # Up: the Up group opens, the hand stays where it is.
    before = hand(a)
    B.tap(a.c, 0, ["DUP"], hold=4, release=12)
    f = last_frame(a)
    assert f["quick_chat"] == 1 and f["quick_chat_group"] == UP, f
    assert hand(a) == before
    shoot("01-up-open")

    # Right: "one more?" goes to otto as a chat message; ines's menu shows it sent.
    B.tap(a.c, 0, ["DRIGHT"], hold=4, release=4)
    shoot("02-up-right-sent")
    f = last_frame(a)
    assert f["quick_chat"] == 2, f
    until_chat(b, lambda s: "one more?" in texts(s, "message"), "ines's quick chat")
    assert "one more?" in texts(a.c.call("chat_status"), "own")
    B.step(a.c, 90)
    assert last_frame(a)["quick_chat"] == 0

    # Left: the longest message beside the D-pad, and kana; Left again sends the kana.
    B.tap(a.c, 0, ["DLEFT"], hold=4, release=12)
    f = last_frame(a)
    assert f["quick_chat"] == 1 and f["quick_chat_group"] == LEFT, f
    shoot("03-left-open-long")
    B.tap(a.c, 0, ["DLEFT"], hold=4, release=4)
    until_chat(b, lambda s: "ありがとう！" in texts(s, "message"), "the kana message")
    B.step(a.c, 90)

    # B closes an open group and never reaches the game (the CSS's B would take the token back).
    B.tap(a.c, 0, ["DUP"], hold=4, release=12)
    assert last_frame(a)["quick_chat"] == 1
    area = B.read_css_area(a.c.read_mem, 0)
    B.tap(a.c, 0, ["B"], hold=4, release=12)
    assert last_frame(a)["quick_chat"] == 0
    after = B.read_css_area(a.c.read_mem, 0)
    assert area is not None and after is not None and after.in_hand == area.in_hand

    # An empty group doesn't open (Right and Down have nothing).
    B.tap(a.c, 0, ["DDOWN"], hold=4, release=12)
    assert last_frame(a)["quick_chat"] == 0

    # Every message cleared: the D-pad moves the hand again.
    write_quick_chat(a, [""] * 16)
    _wait(lambda: (lambda q: q if not q["usable"] else None)(quick(a)), 10, "quick chat off")
    before = hand(a)
    B.tap(a.c, 0, ["DDOWN"], hold=20, release=10)
    assert hand(a)[1] != before[1]
    assert texts(b.c.call("chat_status"), "message") == ["one more?", "ありがとう！"]
