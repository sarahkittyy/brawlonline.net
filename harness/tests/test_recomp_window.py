"""EmissaryRecomp's window under the harness (PPHARNESS_RECOMP): things a player does to the window that
the game must live through. docs/design/brawl-online-core.md §10.2.

- Minimized and restored, then its display scale changing: aurora recreates ImGui's device objects
  on every display scale change, and ImGui 1.92.2b's SDL_GPU backend then uploaded the font atlas
  through a released transfer buffer (memcpy to address 0 on the render thread), which crashed a
  recomp in a room test after SDL reported a display scale of 0 (EmissaryRecomp
  aurora-main/cmake/imgui-sdlgpu3-transfer-buffer.cmake).
"""

from __future__ import annotations

import os
import time
from typing import Callable

import pytest

from ppharness.backend import OnlineBackend
from ppharness.instance import DolphinInstance

from test_online_game import _boot, backend  # noqa: F401  (the backend fixture)

pytestmark = [pytest.mark.dolphin, pytest.mark.server, pytest.mark.gpu]


def _windows(pid: int) -> list[int]:
    import ctypes
    from ctypes import wintypes
    user32 = ctypes.windll.user32
    found: list[int] = []
    proc = ctypes.WINFUNCTYPE(wintypes.BOOL, wintypes.HWND, wintypes.LPARAM)

    def each(hwnd: int, _lp: int) -> bool:
        owner = wintypes.DWORD()
        user32.GetWindowThreadProcessId(hwnd, ctypes.byref(owner))
        if owner.value == pid and user32.IsWindowVisible(hwnd):
            found.append(hwnd)
        return True

    user32.EnumWindows(proc(each), 0)
    return found


@pytest.mark.skipif(os.name != "nt", reason="Windows window messages")
def test_recomp_window_minimized_and_restored(backend: OnlineBackend, dolphin: Callable[..., DolphinInstance],
                                             gpu_backend: str, monkeypatch: pytest.MonkeyPatch) -> None:
    """The recomp's window minimized and restored three times on the main menu (the chat's and the
    on-screen messages' ImGui drawn meanwhile): the program lives on and the game keeps running."""
    if not os.environ.get("PPHARNESS_RECOMP"):
        pytest.skip("PPHARNESS_RECOMP is not set")
    import ctypes
    monkeypatch.setenv("PPHARNESS_RECOMP_NAMES", "win-a$")
    ua = backend.create_user("wina", "WINA")
    a = _boot(dolphin, "win-a", backend, ua, gpu_backend, "recomp-window", "gameplay")
    a.to_main_menu()
    hwnds = _windows(a.inst.process.pid)
    assert hwnds, "no recomp window"
    user32 = ctypes.windll.user32
    for _ in range(3):
        for h in hwnds:
            user32.ShowWindow(h, 6)   # SW_MINIMIZE
        time.sleep(2.0)
        for h in hwnds:
            user32.ShowWindow(h, 9)   # SW_RESTORE
        time.sleep(2.0)
    assert a.inst.process.poll() is None, "the recomp ended"
    polls = a.c.status().input_polls
    a.steps("wait 60")
    assert a.c.status().input_polls > polls, "the game stopped"
    err = (a.inst.user_dir / "harness-stderr.txt")
    text = err.read_text(errors="replace") if err.exists() else ""
    assert "exception 0xc0000005" not in text, text[-3000:]
    print("display scale lines:", [l for l in text.splitlines() if "Display scale" in l])


@pytest.mark.skipif(os.name != "nt", reason="Windows window messages")
def test_recomp_display_scale_changes(backend: OnlineBackend, dolphin: Callable[..., DolphinInstance],
                                      gpu_backend: str, monkeypatch: pytest.MonkeyPatch) -> None:
    """The display scale changing again and again (aurora's AURORA_TEST_SCALE_FLIP: as the window
    moving between monitors of different DPI) on the main menu: each change recreates ImGui's
    device objects and uploads its font atlas again, and the program and its UI live on. Before EmissaryRecomp's imgui patch the first upload after a change crashed."""
    if not os.environ.get("PPHARNESS_RECOMP"):
        pytest.skip("PPHARNESS_RECOMP is not set")
    monkeypatch.setenv("PPHARNESS_RECOMP_NAMES", "scale-a$")
    monkeypatch.setenv("PPHARNESS_RECOMP_ENV", "AURORA_TEST_SCALE_FLIP=45")
    ua = backend.create_user("scla", "SCLA")
    a = _boot(dolphin, "scale-a", backend, ua, gpu_backend, "recomp-scale", "gameplay")
    a.to_main_menu()
    a.c.call("chat_ui", record=True)   # the UI's frames recorded meanwhile
    a.steps("wait 600")                 # about 13 scale changes
    assert a.inst.process.poll() is None, "the recomp ended"
    frames = a.c.call("chat_ui", record=False)["frames"]
    assert len(frames) > 300, ("the UI stopped drawing", len(frames))
    err = a.inst.user_dir / "harness-stderr.txt"
    text = err.read_text(errors="replace") if err.exists() else ""
    assert "exception 0xc0000005" not in text, text[-3000:]
    changes = [l for l in text.splitlines() if "ImGui's device objects recreated" in l]
    assert len(changes) >= 4, ("the display scale did not change", changes)
