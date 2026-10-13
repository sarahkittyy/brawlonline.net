"""The chat key (launcher Settings > Game > Chat Key, launcher/src/common/chat_key.ts): Dolphin.ini
`[Online] ChatKey` replaces the "Activate NetPlay Chat" hotkey's binding (HotkeyManagerEmu::LoadConfig).

The hotkey lives in DolphinQt (HotkeyScheduler), so the player who presses it runs Dolphin.exe
with a window; the other is the usual DolphinNoGUI. The key is pressed for real (SendInput) with
`[General] HotkeysRequireFocus = False`, so the test doesn't need the window focused. F13 is used
as the chat key and F14 as a key that isn't: few keyboards have them, so the presses type nothing
into whatever window has focus. (Keys typed on the machine during the test reach the Dolphins
too: T would open the box if the override were lost, which F14 checks for.)
Windows only (the DirectInput key names).

    PPHARNESS_INSTANCE_PREFIX=chatkey- python -m pytest harness/tests/test_chat_key.py -m "dolphin and server"
"""

from __future__ import annotations

import ctypes
import os
import sys
import time
from pathlib import Path
from typing import Any, Callable

import pytest

from ppharness import paths
from ppharness.backend import OnlineBackend
from ppharness.instance import DolphinInstance, InstanceConfig

from test_chat import chat, hold_screen, readers, until_chat
from test_online import _logged_in, _wait
from test_rooms import PLUGIN, Player, _boot, _online, _to_versus_css, backend  # noqa: F401

import patch_sd  # noqa: E402  (tools/, on sys.path from test_rooms)

pytestmark = [pytest.mark.dolphin, pytest.mark.server,
              pytest.mark.skipif(sys.platform != "win32", reason="DirectInput key names")]

VK_F13, VK_F14 = 0x7C, 0x7D
KEYEVENTF_KEYUP = 0x2


class QtInstance(DolphinInstance):
    """Dolphin.exe: no `-p` (a DolphinNoGUI option)."""

    def build_args(self) -> list[str]:
        args = super().build_args()
        i = args.index("-p")
        return args[:i] + args[i + 2:]


def press(vk: int, hold: float = 0.15) -> None:
    # DirectInput reads the scan code.
    user32 = ctypes.windll.user32
    scan = user32.MapVirtualKeyW(vk, 0)
    user32.keybd_event(vk, scan, 0, 0)
    time.sleep(hold)
    user32.keybd_event(vk, scan, KEYEVENTF_KEYUP, 0)


def boot_qt(be: OnlineBackend, name: str, chat_key: str) -> Player:
    if not PLUGIN.exists():
        pytest.skip(f"{PLUGIN} not built (game-code/build.sh)")
    user = be.create_user(name, name[:4].upper())
    ini = {"Online": {"UseDevServer": True, "MatchmakingPort": be.mm_port,
                      "DevAccountsUrl": be.accounts_url, "ChatKey": chat_key},
           "General": {"HotkeysRequireFocus": False}}
    cfg = InstanceConfig(cpu_thread=False, video_backend="D3D11", dolphin_ini=ini,
                         standard_controllers=(0, 1))
    inst = QtInstance(os.environ.get("PPHARNESS_INSTANCE_PREFIX", "") + name, exe=paths.dolphin_gui(),
                      config=cfg, client_timeout=60.0)
    d = inst.create()
    patch_sd.patch_image(d / "Wii" / "sd.raw",
                         [(PLUGIN.read_bytes(), f"{patch_sd.PLUGIN_DIR}/{PLUGIN.name}")])
    user.write_user_json(d)
    inst.launch()
    inst.connect()
    p = Player(inst, user)
    _logged_in(inst, user)
    return p


def typing(p: Player) -> list[dict[str, Any]]:
    return [f for f in p.c.call("chat_ui")["frames"] if f.get("typing")]


def test_chat_key_from_dolphin_ini(backend: OnlineBackend,
                                   dolphin: Callable[..., DolphinInstance]) -> None:
    a = boot_qt(backend, "kira", "`F13`")
    try:
        b = _boot(dolphin, "chatkey-lou", backend, backend.create_user("lou", "LOU"))
        for p in (a, b):
            _to_versus_css(p)
            _online(p)
            hold_screen(p, "room")
        a.c.call("rooms_request", op="create", public=False)
        code = _wait(lambda: (a.rooms().get("view") or {}).get("code"), 15, "the room's code")
        b.c.call("rooms_request", op="join", code=code)
        until_chat(a, lambda s: s["kind"] == "room" and readers(s) == 1, "the room's chat")

        a.c.call("chat_ui", record=True)
        time.sleep(1)
        assert typing(a) == [], "typing before the key"
        press(VK_F14)
        time.sleep(1.5)
        assert typing(a) == [], "F14 opened the box"
        press(VK_F13)
        _wait(lambda: typing(a) or None, 5, "the box taking the keyboard after F13")
        # Hotkeys.ini is not rewritten: the rest of the player's hotkeys stay.
        assert not (a.inst._need_dir() / "Config" / "Hotkeys.ini").exists()
        assert chat(a)["kind"] == "room"
    finally:
        a.inst.cleanup(True)
