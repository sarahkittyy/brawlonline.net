"""A mirror match through the game's online CSS: both players on the same character in the same
costume, so the plugin shades the second one (online_match.cpp, the colour clash). Staging found
such a game (Game & Watch, costume 2, Smashville) desyncing from frame 0 between a Mac and a
Windows Dolphin (run/staging/feedback/2026-10-10-rooms-1). Direct, game 1 on a stage from a
server list that holds only that stage.

    PPHARNESS_MIRROR_STAGE=33 PPHARNESS_MIRROR_CHAR=game_and_watch PPHARNESS_MIRROR_COSTUME=2
"""

from __future__ import annotations

import json
import os
from pathlib import Path
from typing import Any, Callable

import pytest

from ppharness.instance import DolphinInstance

from test_online import _make_backend
from test_online_game import ROOT, _connected_direct  # noqa: F401
import online_set  # noqa: E402
import ppom  # noqa: E402
from ppharness import brawl as B  # noqa: E402

pytestmark = [pytest.mark.dolphin, pytest.mark.server, pytest.mark.gpu]

STAGE = int(os.environ.get("PPHARNESS_MIRROR_STAGE", "33"))           # Smashville
CHAR = os.environ.get("PPHARNESS_MIRROR_CHAR", "game_and_watch")
COSTUME = int(os.environ.get("PPHARNESS_MIRROR_COSTUME", "2"))


def _mirror_pick(g: Any) -> None:
    """The coin's character and costume on the local panel (muSelCharPlayerArea), which the
    plugin's lock-in reads."""
    B.css_pick_character(g.c, 0, B.CSS_ID[CHAR])
    u = g.c.read_u32
    area = u(u(u(u(0x805A0060) + 4) + 0x400) + 0x44)
    g.c.write_mem(area + 0x1BC, COSTUME.to_bytes(4, "big"))
    B.step(g.c, 10)
    print("panel", g.name, online_set.css_panel(g.c))


def test_mirror_match_same_costume(dolphin: Callable[..., DolphinInstance], gpu_backend: str,
                                   tmp_path: Path) -> None:
    rules = tmp_path / "rulesets.json"
    base = json.loads((ROOT / "server" / "config" / "rulesets.json").read_text())
    base["direct"]["stages"] = [STAGE]
    rules.write_text(json.dumps(base))
    be = _make_backend(rulesets_file=rules)
    try:
        a, b, _, _ = _connected_direct(be, dolphin, gpu_backend, "mirror", ("mira", "miro"),
                                       on_css=(_mirror_pick, _mirror_pick))
        for g in (a, b):
            lock = ppom.read_local(g.c, ppom.find_block(g.c))["lock"]
            assert lock["char_kind"] == B.CHAR_KIND[CHAR] and lock["costume"] == COSTUME, lock
        rep = online_set.play_game([a, b], 1)
        print("mirror:", json.dumps({k: rep.get(k) for k in ("stage", "shades", "checksums", "trace", "end")},
                                    default=str))
        assert rep["stage"] == STAGE
        assert rep["checksums"]["mismatches"] == 0, rep
    finally:
        be.stop()


def _offline_four_player_match(g: Any) -> None:
    """From the plugin's ONLINE page: P+'s Versus with four players (Mario, Luigi, Peach, Marth),
    one stock, three walk off; the results; back out to the main menu and to the ONLINE page."""
    c = g.c
    B.step(c, 60)
    B.tap(c, 0, ["B"], hold=3, release=60)        # the main menu's top page on PLAY ONLINE
    B.tap(c, 0, ["DUP"], hold=4, release=40)      # Brawl
    B.boot_to_css(c, 0)
    B.wait_scene(c, [B.Scene.CSS], 60 * 60)
    B.wait_css_ready(c, [0, 1, 2, 3])
    B.write_rules(c, stocks=1, minutes=1, items_off=True)
    for port, ch in enumerate(("mario", "luigi", "peach", "marth")):
        for attempt in range(3):
            try:
                B.css_pick_character(c, port, B.CSS_ID[ch])
                break
            except Exception:
                if attempt == 2:
                    raise
                B.step(c, 30)
    from ppharness import flows as F
    F.start_and_pick_stage(F.Seat(c, 0), "battlefield")
    B.wait_scene(c, [B.Scene.IN_MATCH], 60 * 60)
    B.step(c, 300)
    try:
        F.fight([F.Seat(c, p) for p in (1, 2, 3)], 60 * 60, mode="selfdestruct",
                stop=lambda: B.read_match_state(c.read_mem).game_set)
    except Exception:
        pass
    B.results_to_css(c, [0, 1, 2, 3])
    B.wait_scene(c, [B.Scene.CSS], 60 * 60)
    for port in range(4):
        B.neutral(c, port)
    B.tap(c, 0, ["B"], hold=90, release=60)       # out of the CSS
    _wait_scene_name(g, "muMenuMain", 60)
    g.shot("00-after-offline")
    # Back on the ONLINE page: B to the top page, down to PLAY ONLINE, A.
    g.steps("wait 60", "tap B", "wait 60", "tap DDOWN 4", "wait 40", "tap A", "wait 90")
    g.shot("00-online-page-again")


def _wait_scene_name(g: Any, name: str, timeout: float) -> None:
    import time
    deadline = time.monotonic() + timeout
    while g.scene() != name:
        if time.monotonic() > deadline:
            raise AssertionError(f"{g.name}: not on {name} ({g.scene()})")
        time.sleep(0.2)


def test_direct_after_an_offline_four_player_match(dolphin: Callable[..., DolphinInstance],
                                                   gpu_backend: str) -> None:
    """Staging, 2026-10-10 game 8: one player had played offline 4-player Versus before going
    online, the other had not, and the online 1v1 ended at once on "desync at frame 0". Brawl
    frees the unused fighter entries of the next 2-player match but keeps their player numbers
    (2 and 3), and the session's checksum read them as fighters on that machine only. Here: A
    plays an offline 4-player match first, then a Direct game with B to game set, the confirmed
    checksums equal."""
    import test_online_game as T
    be = _make_backend()
    try:
        ua, ub = be.create_user("offa", "OFFA"), be.create_user("offb", "OFFB")
        a = T._boot(dolphin, "game-a", be, ua, gpu_backend, "offline4", "gameplay",
                    standard_controllers=(0, 1, 2, 3))
        b = T._boot(dolphin, "game-b", be, ub, gpu_backend, "offline4", "gameplay")
        T._both(a.to_main_menu, b.to_main_menu)
        _offline_four_player_match(a)
        b.to_online_page()
        T._both(lambda: a.to_css("direct"), lambda: b.to_css("direct"))
        for g in (a, b):
            B.write_rules(g.c, stocks=1, minutes=2, items_off=True)
            ppom.allow_test_rules(g.c)
        a.open_keypad()
        a.type_code(ub.connect_code)
        T._searching(a, ub.connect_code)
        import time
        time.sleep(2.2)
        b.open_keypad()
        b.type_code(ua.connect_code)
        T._peer_shown(a, ub)
        T._peer_shown(b, ua)
        rep = online_set.play_game([a, b], 1)
        print("offline4:", json.dumps({k: rep.get(k) for k in ("stage", "checksums", "end")}, default=str))
        assert rep["checksums"]["mismatches"] == 0, rep
        assert all(e == "game set" for e in rep["end"]), rep
    finally:
        be.stop()
