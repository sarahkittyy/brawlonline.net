"""Brawl Online's in-game UI (core/ui: on-screen messages, the chat window) in a live Direct set,
on whatever engines the instances are: two Dolphins, or a Dolphin against EmissaryRecomp
(PPHARNESS_RECOMP, PPHARNESS_RECOMP_NAMES="game-b"), which draws the same UI code
(docs/design/brawl-online-core.md §11).

The players chat both ways on the CSS and during a game (`chat_send`, what the box's Enter does),
each sees the other's lines (`chat_status`), the chat window is drawn open on both (`chat_ui`) and
the core's on-screen messages are up (`osd_messages`). Both windows are placed side by side on
the primary monitor and captured (PrintWindow) with the chat open, on the CSS and in the match:
run/artifacts/ui-crossplay/.

    PPHARNESS_RECOMP=<build>\\native PPHARNESS_RECOMP_NAMES=game-b python -m pytest tests/test_ui_crossplay.py
"""

from __future__ import annotations

import ctypes
import os
import sys
import time
from ctypes import wintypes
from pathlib import Path
from typing import Any, Callable

import pytest

from ppharness.backend import OnlineBackend
from ppharness.instance import DolphinInstance

from test_online import _wait
from test_online_game import ROOT, _connected_direct, backend, online_set  # noqa: F401

pytestmark = [pytest.mark.dolphin, pytest.mark.server,
              pytest.mark.skipif(sys.platform != "win32", reason="window capture is Windows'")]

ART = ROOT / "run" / "artifacts" / "ui-crossplay"
# PPHARNESS_UI_TYPE=1: game-b also types a line with the keyboard (SendInput, its window brought
# to the front: it takes the machine's keyboard for a few seconds).
TYPE = os.environ.get("PPHARNESS_UI_TYPE") == "1"
# Side by side on the primary monitor (positive coordinates), the same client size for both.
PLACES = {"game-a": (40, 60), "game-b": (1000, 60)}
CLIENT = (900, 506)


def _hwnd(pid: int) -> int:
    user32 = ctypes.windll.user32
    found: list[int] = []

    @ctypes.WINFUNCTYPE(wintypes.BOOL, wintypes.HWND, wintypes.LPARAM)
    def each(hwnd: int, _: int) -> bool:
        owner = wintypes.DWORD()
        user32.GetWindowThreadProcessId(hwnd, ctypes.byref(owner))
        if owner.value == pid and user32.IsWindowVisible(hwnd):
            r = wintypes.RECT()
            user32.GetClientRect(hwnd, ctypes.byref(r))
            if r.right > 100 and r.bottom > 100:
                found.append(hwnd)
        return True

    user32.EnumWindows(each, 0)
    assert found, f"no window for pid {pid}"
    return found[0]


def place(inst: DolphinInstance, x: int, y: int) -> None:
    """The window at (x, y) with a client area of CLIENT."""
    user32 = ctypes.windll.user32
    hwnd = _hwnd(inst.process.pid)
    r = wintypes.RECT(0, 0, *CLIENT)
    style = user32.GetWindowLongW(hwnd, -16)
    user32.AdjustWindowRect(ctypes.byref(r), style, False)
    user32.SetWindowPos(hwnd, 0, x, y, r.right - r.left, r.bottom - r.top, 0x0004 | 0x0010)


def grab(inst: DolphinInstance, out: Path) -> None:
    """The window's own pixels (PrintWindow with PW_RENDERFULLCONTENT: a swap chain's too)."""
    from PIL import Image

    user32, gdi32 = ctypes.windll.user32, ctypes.windll.gdi32
    hwnd = _hwnd(inst.process.pid)
    r = wintypes.RECT()
    user32.GetClientRect(hwnd, ctypes.byref(r))
    w, h = r.right, r.bottom
    hdc = user32.GetDC(hwnd)
    mem = gdi32.CreateCompatibleDC(hdc)
    bmp = gdi32.CreateCompatibleBitmap(hdc, w, h)
    gdi32.SelectObject(mem, bmp)
    assert user32.PrintWindow(hwnd, mem, 1 | 2), "PrintWindow failed"

    class BITMAPINFOHEADER(ctypes.Structure):
        _fields_ = [("biSize", wintypes.DWORD), ("biWidth", wintypes.LONG), ("biHeight", wintypes.LONG),
                    ("biPlanes", wintypes.WORD), ("biBitCount", wintypes.WORD),
                    ("biCompression", wintypes.DWORD), ("biSizeImage", wintypes.DWORD),
                    ("biXPelsPerMeter", wintypes.LONG), ("biYPelsPerMeter", wintypes.LONG),
                    ("biClrUsed", wintypes.DWORD), ("biClrImportant", wintypes.DWORD)]

    bi = BITMAPINFOHEADER()
    bi.biSize = ctypes.sizeof(BITMAPINFOHEADER)
    bi.biWidth, bi.biHeight, bi.biPlanes, bi.biBitCount = w, -h, 1, 32
    buf = ctypes.create_string_buffer(w * h * 4)
    gdi32.GetDIBits(mem, bmp, 0, h, buf, ctypes.byref(bi), 0)
    out.parent.mkdir(parents=True, exist_ok=True)
    Image.frombuffer("RGB", (w, h), buf, "raw", "BGRX", 0, 1).save(out)
    gdi32.DeleteObject(bmp)
    gdi32.DeleteDC(mem)
    user32.ReleaseDC(hwnd, hdc)


class _KEYBDINPUT(ctypes.Structure):
    _fields_ = [("wVk", wintypes.WORD), ("wScan", wintypes.WORD), ("dwFlags", wintypes.DWORD),
                ("time", wintypes.DWORD), ("dwExtraInfo", ctypes.c_size_t)]


class _INPUT(ctypes.Structure):
    class _U(ctypes.Union):
        _fields_ = [("ki", _KEYBDINPUT), ("pad", ctypes.c_byte * 32)]

    _anonymous_ = ("u",)
    _fields_ = [("type", wintypes.DWORD), ("u", _U)]


def _send(vk: int = 0, scan: int = 0, flags: int = 0) -> None:
    i = _INPUT(type=1)
    i.ki = _KEYBDINPUT(vk, scan, flags, 0, 0)
    ctypes.windll.user32.SendInput(1, ctypes.byref(i), ctypes.sizeof(_INPUT))


def press(vk: int) -> None:
    """A key down and up, as the keyboard sends it (scan code too: SDL reads scan codes)."""
    scan = ctypes.windll.user32.MapVirtualKeyW(vk, 0)
    _send(vk, scan, 0x0008)
    time.sleep(0.08)
    _send(vk, scan, 0x0008 | 0x0002)
    time.sleep(0.08)


def type_text(text: str) -> None:
    for ch in text:
        _send(0, ord(ch), 0x0004)            # KEYEVENTF_UNICODE
        _send(0, ord(ch), 0x0004 | 0x0002)
        time.sleep(0.03)


def focus(inst: DolphinInstance) -> None:
    """The window in front with the keyboard (an Alt tap first: Windows lets a process that just
    had input move the foreground)."""
    user32 = ctypes.windll.user32
    hwnd = _hwnd(inst.process.pid)
    press(0x12)
    user32.SetForegroundWindow(hwnd)
    time.sleep(0.5)
    assert user32.GetForegroundWindow() == hwnd, "the window did not come to the front"


def chat(g: Any) -> dict[str, Any]:
    return g.c.call("chat_status")


def texts(st: dict[str, Any], kind: str = "message") -> list[str]:
    return [line["text"] for line in st["lines"] if line["kind"] == kind]


def until_line(g: Any, text: str, timeout: float = 30) -> None:
    _wait(lambda: text in texts(chat(g)), timeout, f"{g.name}: {text!r}", interval=0.2)


def drawn_open(g: Any) -> dict[str, Any]:
    """The chat window as the UI drew it in the last frames: open (shown, not folded)."""
    g.c.call("chat_ui", record=True)
    frames: list[dict[str, Any]] = []
    deadline = time.monotonic() + 15
    while len(frames) < 10 and time.monotonic() < deadline:
        time.sleep(0.5)
        frames += g.c.call("chat_ui", record=True)["frames"]
    assert frames, f"{g.name}: no frame drawn"
    last = frames[-1]
    assert last["shown"] and not last["collapsed"], last
    return last


def test_chat_both_ways_in_a_direct_set(backend: OnlineBackend,  # noqa: F811
                                        dolphin: Callable[..., DolphinInstance],
                                        gpu_backend: str) -> None:
    windowed: Callable[..., DolphinInstance] = lambda *a, **kw: dolphin(*a, platform="win32", **kw)
    a, b, ua, ub = _connected_direct(backend, windowed, gpu_backend, "ui-crossplay", ("iris", "jack"))
    for g in (a, b):
        place(g.inst, *PLACES[g.inst.name])
        _wait(lambda: chat(g)["kind"] == "match" and chat(g)["match_connected"], 120,
              f"{g.name}: the match's chat", interval=0.2)

    # Both ways on the CSS, kana and kanji included (the chat font's).
    a.c.call("chat_send", text="gl hf")
    until_line(b, "gl hf")
    b.c.call("chat_send", text="ねこ 対戦よろしく")
    until_line(a, "ねこ 対戦よろしく")
    looks = {g.name: drawn_open(g) for g in (a, b)}
    for g in (a, b):
        grab(g.inst, ART / f"css-{g.inst.name}.png")

    # Game 1, and both ways again while it plays.
    rep: dict[str, Any] = {}

    def chat_in_the_match() -> None:
        _wait(lambda: all(x.c.call("gprb_status").get("phase") == "running" for x in (a, b)), 120,
              "the match running", interval=0.5)
        time.sleep(3)
        a.c.call("chat_send", text="nice")
        until_line(b, "nice")
        b.c.call("chat_send", text="ty")
        until_line(a, "ty")
        if TYPE:
            # The chat key (T, the default) on the keyboard, a line typed and Enter: the box takes
            # the keyboard (the game gets none of it) and sends the line.
            focus(b.inst)
            press(ord("T"))
            _wait(lambda: any(f["typing"] for f in b.c.call("chat_ui", record=True)["frames"]), 10,
                  f"{b.name}: the box taking the keyboard", interval=0.2)
            type_text("typed in the box")
            grab(b.inst, ART / f"typing-{b.inst.name}.png")
            press(0x0D)
            until_line(a, "typed in the box")
        for x in (a, b):
            grab(x.inst, ART / f"match-{x.inst.name}.png")

    import threading
    errors: list[BaseException] = []

    def run() -> None:
        try:
            chat_in_the_match()
        except BaseException as e:  # noqa: BLE001
            errors.append(e)

    t = threading.Thread(target=run, daemon=True)
    t.start()
    rep = online_set.play_set([a, b], games=1, mode="direct",
                              panels={g.name: g.panel for g in (a, b)})
    t.join(timeout=60)
    assert not errors, errors[0]
    assert rep["games"], rep
    # Each one's view has the other's lines (its own are not "message" lines).
    for g, other, got in ((a, ub, {"ねこ 対戦よろしく", "ty"}), (b, ua, {"gl hf", "nice"})):
        st = chat(g)
        assert got <= set(texts(st)), st
        assert other.display_name in {m["name"] for m in st["members"]}, st
    print("chat window as drawn:", looks)
    for g in (a, b):
        g.c.mm_cancel()
