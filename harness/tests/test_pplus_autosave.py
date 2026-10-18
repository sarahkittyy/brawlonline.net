"""P+'s replay autosave is off online (bug reports #48 and #49, 2026-10-18).

With P+'s Code Menu line "Autosave Replays" ON (the netplay menu's default), P+'s Control Code
Menu sets 0x804E003C to 2 in a match and its End Match code writes the game's replay to the SD
card (/Project+/rp/rp_*.bin) on the results screen. That held every online game about 8 s
before the next one (#49), and once the card was full the write failed and P+ tried it again
for good, so the game never left the results screen (#48). Online the plugin keeps the line OFF
(online_menu.cpp codeMenuOff); Dolphin's own replay (.brep) has every online game. The player's
own value comes back with the other lines when the menus load (test_online_css.py
test_stale_settings_do_not_reach_the_match checks every line).

Needs the real Dolphin build, a video backend, the built plugin and the server binaries plus
Postgres, as test_online_game.py. PPHARNESS_PLUGIN=<the plugin before this change> shows the
write and the wait.
"""

from __future__ import annotations

import re
import struct
from typing import Any, Callable

import pytest

from ppharness.backend import OnlineBackend
from ppharness.instance import DolphinInstance

from test_online_css import _code_menu_value
from test_online_game import Game, _connected_direct, backend, online_set  # noqa: F401

pytestmark = [pytest.mark.dolphin, pytest.mark.server, pytest.mark.gpu]

AUTOSAVE = "Autosave Replays"     # 0 OFF, 1 ON
CM_AUTOSAVE_FLAG = 0x804E003C     # 2 in a match while the line is ON; End Match then saves
VF_REPLAY_WRITE = re.compile(rb"File Write \(VF\)[^\r\n]*/rp/[^\r\n]*")
GAME_SET = re.compile(rb"(\d\d):(\d\d):(\d\d\d) \S+ I\[Brawlback\]: gprb: session ended at game frame \d+ \(game set\)")
MATCH_LEFT = re.compile(rb"(\d\d):(\d\d):(\d\d\d) \S+ N\[OSREPORT\]: scMelee -> ")


def _netplay(dolphin: Callable[..., DolphinInstance]) -> Callable[..., DolphinInstance]:
    """Instances booted the way Play boots P+: its netplay launcher DOL, whose Code Menu
    (dnet.cmnu) has Autosave Replays ON by default (the offline one, data.cmnu, has it OFF)."""
    def factory(name: str | None = None, **kw: Any) -> DolphinInstance:
        kw.setdefault("boot", "netplay")
        return dolphin(name, **kw)
    return factory


def _u32(g: Game, addr: int) -> int:
    return struct.unpack(">I", g.c.read_mem(addr, 4))[0]


def _log_size(g: Game) -> int:
    return g.inst.log_path.stat().st_size


def _log_since(g: Game, since: int) -> bytes:
    with open(g.inst.log_path, "rb") as f:
        f.seek(since)
        return f.read()


def _seconds(m: re.Match[bytes]) -> float:
    return int(m.group(1)) * 60 + int(m.group(2)) + int(m.group(3)) / 1000


def _game_set_to_menus(text: bytes) -> float:
    """Seconds from game set (the session's end) to the match scene's end, from the log's
    minute:second:millisecond stamps."""
    end = GAME_SET.search(text)
    assert end, "no game set in the log"
    left = MATCH_LEFT.search(text, end.end())
    assert left, "the match scene never ended"
    return round((_seconds(left) - _seconds(end)) % 3600, 2)


@pytest.mark.slow
def test_online_game_saves_no_pplus_replay(backend: OnlineBackend,
                                           dolphin: Callable[..., DolphinInstance],
                                           gpu_backend: str) -> None:
    """Both players boot P+'s netplay launcher, as Play does, so Autosave Replays is ON. In the
    online Direct match the line is OFF and P+'s save flag stays 0 on both; after game set
    neither writes a replay to the SD card, and both are back on the CSS within a few seconds;
    the session agrees on every confirmed frame. Back in the menus, A's line is ON again."""

    def autosave_on(g: Game) -> None:
        value = _code_menu_value(g, AUTOSAVE)
        assert _u32(g, value + 8) == 1, "not P+'s netplay Code Menu (its default is ON)"
        g.c.write_mem(value, struct.pack(">I", 1))

    a, b, _ua, _ub = _connected_direct(backend, _netplay(dolphin), gpu_backend, "pplus-autosave",
                                       ("vera", "wim"), stocks=1,
                                       before_online=(autosave_on, autosave_on))
    online_set.wait(lambda: all(online_set.scene(g.c) == "scMelee" for g in (a, b)), 120,
                    "both in the match")
    online_set.wait(lambda: all(online_set.gstatus(g.c)["phase"] == "running" for g in (a, b)), 240,
                    "the match to run on both")
    a.steps("wait 120")
    facts: dict[str, Any] = {g.name: {"line": _u32(g, _code_menu_value(g, AUTOSAVE)),
                                      "flag": _u32(g, CM_AUTOSAVE_FLAG)} for g in (a, b)}
    marks = {g.name: _log_size(g) for g in (a, b)}

    rep = online_set.play_game([a, b], 1, winner=a)
    online_set.wait(lambda: all(online_set.scene(g.c) == online_set.CSS for g in (a, b)), 120,
                    "both back on the CSS")
    b.steps("wait 300")
    for g in (a, b):
        text = _log_since(g, marks[g.name])
        facts[g.name].update(to_menus_s=_game_set_to_menus(text),
                             replay_writes=[m.group(0).decode("latin1") for m in VF_REPLAY_WRITE.finditer(text)])
    facts["game"] = {k: rep[k] for k in ("frames", "end", "checksums")}
    print(facts)

    for g in (a, b):
        f = facts[g.name]
        assert f["line"] == 0 and f["flag"] == 0, (g.name, f)
        assert f["replay_writes"] == [], (g.name, f)
    assert rep["checksums"]["compared"] > 100 and rep["checksums"]["mismatches"] == 0, rep["checksums"]

    # The player's own value is back in the menus: B goes, A's set ends, A leaves its CSS.
    b.inst.kill()
    online_set.wait(lambda: online_set.scene(a.c) == online_set.CSS, 120, "A back on the CSS")
    a.steps("wait 60", "hold B 90", "until muMenuMain 900", "wait 30")
    assert _u32(a, _code_menu_value(a, AUTOSAVE)) == 1
