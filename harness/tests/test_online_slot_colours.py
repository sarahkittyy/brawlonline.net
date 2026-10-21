"""P+'s slot colours (its Code Menu's PlayerSlotColorChangers) in an online match.

Offline, L / R on a CSS panel's player box steps that port's colour (a Code Menu value line,
found through the pointer at 0x804E02F4 + port * 4: value at +8, 0-12, default at +0x10, the
port's own 0-3) and the CSS and the match HUD are drawn in it. Online every Code Menu line went to
its default, so everyone played red / blue whatever they picked. Now the player's colour is P1's
line on the online CSS (the local panel; what P1 had offline, then L / R there): the lock-in's
port values carry it (+ 1, 0 none) and the match setup writes all four lines from SESSION on every
machine, a player without a pick in their port's colour or, when a pick has it, the first of red,
blue, yellow, green nobody has (game-code online_menu.cpp, online_match.cpp slotColours).

Two games on the Direct CSS with the gameplay backend (test_online_game.py's setup). Screenshots
under ``run/artifacts/game-bridge/<test>/``.
"""

from __future__ import annotations

import struct
from typing import Any, Callable

import pytest

from ppharness.backend import OnlineBackend
from ppharness.client import PadInput
from ppharness.instance import DolphinInstance

from test_online_alt_costumes import _running
from test_online_game import Game, _confirmed_checksums, _connected_direct, backend  # noqa: F401 (fixture)
from test_rooms_game import hand, hand_to
import online_set  # noqa: E402  (test_online_game put harness/tools on the path)

pytestmark = [pytest.mark.dolphin, pytest.mark.server, pytest.mark.gpu, pytest.mark.slow]

SLOT_COLOUR_LINES = 0x804E02F4
PLAYER_KIND_BUTTON = 0x1C    # the CSS hand's +0xAC over the local panel's player box
PLAYER_BOX = (-26.0, -20.0)  # the online CSS's local panel's player box (hand coordinates)


def _line(g: Game, port: int) -> int:
    return g.c.read_u32(SLOT_COLOUR_LINES + 4 * port)


def lines(g: Game) -> list[int]:
    return [g.c.read_u32(_line(g, p) + 8) for p in range(4)]


def defaults(g: Game) -> list[int]:
    return [g.c.read_u32(_line(g, p) + 0x10) for p in range(4)]


def _offline_lines(values: dict[int, int]) -> Callable[[Game], None]:
    """What offline play leaves: these ports' colours set on P+'s CSS."""
    def write(g: Game) -> None:
        for port, v in values.items():
            g.c.write_mem(_line(g, port) + 8, struct.pack(">I", v))
        assert all(lines(g)[p] == v for p, v in values.items()), lines(g)
    return write


def _press_on_player_box(trigger: str, times: int) -> Callable[[Game], None]:
    """P+'s own control on the online CSS: the hand on the local panel's player box, then the
    trigger `times` times, each until P1's line steps (L down, R up, 0-12 wrapping)."""
    def press(g: Game) -> None:
        hand_to(g, *PLAYER_BOX)
        area = g.c.read_u32(g.c.read_u32(g.c.read_u32(g.c.read_u32(0x805A0060) + 4) + 0x400) + 0x44)
        h = g.c.read_u32(area + 0x1A8)
        for _ in range(times):
            want = (lines(g)[0] + (-1 if trigger == "L" else 1)) % 13
            for _ in range(10):
                assert g.c.read_u32(h + 0xAC) in (0, PLAYER_KIND_BUTTON), hex(g.c.read_u32(h + 0xAC))
                pad = PadInput(l=255, hold=4) if trigger == "L" else PadInput(r=255, hold=4)
                g.c.pad_script(0, [pad, PadInput(hold=8)])
                g.steps("wait 14")
                if lines(g)[0] == want:
                    break
            assert lines(g)[0] == want, (g.name, lines(g), want, hand(g))
        g.shot("css-colour")
    return press


def _match_colours(a: Game, b: Game, want: dict[str, int]) -> dict[str, Any]:
    """`want`: per game, the colour its player's port has in the match. Both machines have the
    same four lines (the other ports at their defaults) and the frames agree."""
    host, join = _running(a, b)
    port = {g.name: online_set.local(g.c)["local_port"] for g in (a, b)}
    se = [online_set.session(g.c) for g in (a, b)]
    pv = [[p.get("pv", {}).get("slot_colour") for p in s["players"]] for s in se]
    assert pv[0] == pv[1], pv
    expect = defaults(a)
    for name, c in want.items():
        expect[port[name]] = c
    got = {g.name: lines(g) for g in (a, b)}
    for g in (a, b):
        assert got[g.name] == expect, (g.name, got, expect, port)
        g.shot("01-match")
    a.steps("wait 300")
    ck = _confirmed_checksums(host, join)
    assert ck["mismatches"] == 0 and ck["compared"] > 300, ck
    for g in (a, b):
        assert online_set.gstatus(g.c)["phase"] == "running", g.name
        assert not online_set.local(g.c)["disconnected"], g.name
    return {"lines": got, "ports": port, "pv": pv[0], "checksums": ck}


def test_slot_colours_picked_on_the_css(backend: OnlineBackend, dolphin: Callable[..., DolphinInstance],
                                        gpu_backend: str) -> None:
    """A had colour 7 as P1 offline and steps it down once with L on the online CSS (6); B had 5 on
    P2 offline (not P1: not its colour) and steps P1's red up once with R (1, blue). Each lock-in
    carries its colour + 1, and on both machines A's port has 6 and B's 1 in the match, the empty
    ports their defaults, with the confirmed frames of both peers agreeing."""
    a, b, ua, ub = _connected_direct(backend, dolphin, gpu_backend, "slot-colours", ("cora", "bess"),
                                     before_online=(_offline_lines({0: 7}), _offline_lines({1: 5})),
                                     on_css=(_press_on_player_box("L", 1), _press_on_player_box("R", 1)))
    assert online_set.local(a.c)["own"]["slot_colour"] == 7, online_set.local(a.c)["own"]
    assert online_set.local(b.c)["own"]["slot_colour"] == 2, online_set.local(b.c)["own"]
    r = _match_colours(a, b, {a.name: 6, b.name: 1})
    print(r)
    for g in (a, b):
        g.c.mm_cancel()


def test_unpicked_colour_moves_off_a_pick(backend: OnlineBackend, dolphin: Callable[..., DolphinInstance],
                                          gpu_backend: str) -> None:
    """A picks blue (R once); B picks nothing (its lock-in says 0). Whichever port each gets, A is
    blue and B red: as P2, B's own blue is A's pick, so B gets the first free one, red."""
    a, b, ua, ub = _connected_direct(backend, dolphin, gpu_backend, "slot-colours-clash", ("dora", "edna"),
                                     on_css=(_press_on_player_box("R", 1), None))
    assert online_set.local(a.c)["own"]["slot_colour"] == 2, online_set.local(a.c)["own"]
    assert online_set.local(b.c)["own"]["slot_colour"] == 0, online_set.local(b.c)["own"]
    r = _match_colours(a, b, {a.name: 1, b.name: 0})
    print(r)
    for g in (a, b):
        g.c.mm_cancel()
