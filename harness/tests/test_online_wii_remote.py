"""Wii Remotes in P+'s online games (bug report #89: "I can't start a game when using an emulated
Wii Remote / Classic Controller"; the user's, 2026-10-21: support the Classic Controller, the Wii
Remote alone and the Wii Remote with a Nunchuk).

Each instance's Wii Remote 1 is Dolphin's emulated one with an extension (``wiimote_ini``), driven
through the harness's ``wiimote_input`` (Dolphin's input override, as its TAS window). The menus
are walked on the GameCube pad in port 1 as in every online test; the Wii Remote's PLUS then locks
in and searches, so the lock-in names the Wii Remote (LOCAL lockPad 4) and the match plays the
player from it:

- the player's port values say so (PV_WII), on both machines' SESSION;
- the match gives their port the Wii Remote pad (gmPlayerInitData controller = port + 5) and
  Dolphin writes their input into it, by its controller type (GameCube pad of the port empty);
- what they press moves their fighter on both machines, with no checksum mismatch;
- the game's own quit buttons for their controller end an Unranked game with no pause (L+R+A+PLUS
  on a Classic Controller, B+1+2+PLUS on a Wii Remote alone, 1+B+Z+PLUS with a Nunchuk), and with
  MINUS (a Wii Remote has no Y) they are P+'s runback;
- the replay is a version 3 file (SIM_VERSION, a Wii Remote in the game) and plays back in step.

Instances are named ``wii-*`` (``ppharness clean --prefix wii-``). Screenshots go to
``run/artifacts/game-bridge/<test>/``.
"""

from __future__ import annotations

import struct
import time
from typing import Any, Callable, Mapping

import pytest

from ppharness import brawl as B
from ppharness import replay as R
from ppharness.backend import OnlineBackend
from ppharness.instance import DolphinInstance

from test_online import _wait
from test_online_game import (Game, _boot, _both, _confirmed_checksums, _peer_shown, online_set,
                              ppom)
from test_online_unranked import _search_unranked, backend  # noqa: F401
from test_replay import _play, _viewer

pytestmark = [pytest.mark.dolphin, pytest.mark.server, pytest.mark.gpu]

PAD_STATUS = 0x805BAD00   # gfPadSystem's system pads: GameCube 0-3, Wii Remotes 4-7
MM_PLAYERS, MM_PLAYER_SIZE = 0x98, 0x5C
CLASSIC, WIIMOTE, NUNCHUK = 1, 2, 3   # gfPadType


def _boot_wii(dolphin: Callable[..., DolphinInstance], name: str, be: OnlineBackend, user: Any,
              gpu_backend: str, test: str, extension: str) -> Game:
    """A game whose Wii Remote 1 is an emulated one with `extension` ("Classic", "Nunchuk", "")."""
    ini = {"Wiimote1": {"Source": 1, "Extension": extension or "None"}}
    return _boot(dolphin, name, be, user, gpu_backend, test, "gameplay", wiimote_ini=ini)


class Wii:
    """Game `g`'s Wii Remote 1 with `ext` ("classic", "nunchuk", "none")."""

    def __init__(self, g: Game, ext: str):
        self.g, self.ext = g, ext

    def set(self, wiimote: Mapping[str, float] | None = None, **parts: Mapping[str, float]) -> None:
        self.g.c.wiimote_input(0, extension=self.ext, wiimote=wiimote or {}, **parts)

    def press(self, frames: int = 8, release: int = 8, wiimote: Mapping[str, float] | None = None,
              **parts: Mapping[str, float]) -> None:
        """Hold for `frames` game frames, then let go for `release`."""
        self.set(wiimote, **parts)
        self.g.steps(f"wait {frames}")
        self.set()
        self.g.steps(f"wait {release}")


def _search_with(g: Game, press: Callable[[], None]) -> None:
    """The Wii Remote's PLUS on the Unranked CSS locks in and searches."""
    n = g.finds()
    for _ in range(5):
        press()
        try:
            _wait(lambda: g.finds() > n, 3, "FIND_OPPONENT")
            return
        except AssertionError:
            continue
    raise AssertionError("the Wii Remote's PLUS did not search")


def _to_unranked_css(games: tuple[Game, ...]) -> None:
    _both(*(g.to_main_menu for g in games))
    _both(*(g.to_online_page for g in games))
    _both(*(lambda g=g: g.to_css("unranked") for g in games))
    for g in games:
        B.write_rules(g.c, stocks=4, minutes=8, items_off=True)
        ppom.allow_test_rules(g.c)


def _running(players: tuple[Game, ...]) -> None:
    online_set.wait(lambda: all(online_set.gstatus(g.c)["phase"] == "running" for g in players), 240,
                    "the match to run on both")


def _port(g: Game) -> int:
    return ppom.read_local(g.c, ppom.find_block(g.c))["local_port"]


def _controller_numbers(g: Game) -> list[int]:
    """gmPlayerInitData +0x07 of P1-P4 in the match setup (the pad + 1, 0 none)."""
    mm = g.c.read_u32(g.c.read_u32(0x805A00E0) + 0x08)
    return [g.c.read_mem(mm + MM_PLAYERS + i * MM_PLAYER_SIZE + 0x07, 1)[0] for i in range(4)]


def _pad(g: Game, pad: int) -> bytes:
    return g.c.read_mem(PAD_STATUS + 0x40 * pad, 0x40)


def _x(g: Game, port: int) -> float:
    for p in B.read_players(g.c.read_mem):
        if p.port == port and p.x is not None:
            return p.x
    raise AssertionError(f"no fighter on P{port + 1}")


def _check_wii_port(players: tuple[Game, ...], port: int, kind: int) -> None:
    """Every machine: the player on `port` plays the port's Wii Remote pad (controller port + 5),
    whose record has their controller type; the port's GameCube pad has no controller."""
    for g in players:
        se = ppom.read_session(g.c, ppom.find_block(g.c))
        assert se["players"][port]["pv"]["wii"], (g.name, se["players"][port])
        assert _controller_numbers(g)[port] == port + 5, (g.name, _controller_numbers(g))
        # A read can catch the pads as Brawl's pad thread made them from this machine's own
        # controllers (its Wii Remote is channel 0 whatever its port, the harness's GameCube
        # controller is in port 1), before the session writes the frame's inputs over them in
        # the same pass: the game never reads them there, but `wait` ends at an input poll, just
        # before that pass. Read again.
        for _ in range(5):
            wii, gc = _pad(g, 4 + port), _pad(g, port)
            if wii[0x38] == 0 and wii[0x3F] == kind and gc[0x38] == 0xFF:
                break
            time.sleep(0.03)
        assert wii[0x38] == 0 and wii[0x3F] == kind, (g.name, wii.hex())
        assert gc[0x38] == 0xFF, (g.name, gc.hex())


def _moves(players: tuple[Game, ...], port: int, hold: Callable[[], None], release: Callable[[], None],
           frames: int = 40) -> None:
    """Holding the stick (or D-pad) moves the fighter on `port` on every machine."""
    before = [_x(g, port) for g in players]
    hold()
    players[0].steps(f"wait {frames}")
    release()
    players[0].steps("wait 30")
    after = [_x(g, port) for g in players]
    for g, x0, x1 in zip(players, before, after):
        assert abs(x1 - x0) > 5, (g.name, x0, x1)


def _recording(g: Game) -> R.Replay:
    st = g.c.call("replay_recording", flush=True)
    return R.read(st["path"])


@pytest.mark.slow
def test_classic_controller_plays_unranked(backend: OnlineBackend,  # noqa: F811
                                           dolphin: Callable[..., DolphinInstance],
                                           gpu_backend: str) -> None:
    """A on a Classic Controller, B on a GameCube controller: A's PLUS locks in from the Wii
    Remote, the match gives A's port the Wii Remote pad on both machines, A's stick moves A's
    fighter on both with every confirmed frame the same, and L+R+A+PLUS (the game's own quit for a
    Classic Controller) ends the game with no pause. A's replay is a version 3 file whose records
    for A are Classic Controller ones, and a fresh Dolphin plays it to the end in step."""
    test = "wii-classic"
    ua, ub = backend.create_user("cleo", "CLEO"), backend.create_user("dax", "DAX")
    a = _boot_wii(dolphin, "wii-ca", backend, ua, gpu_backend, test, "Classic")
    b = _boot(dolphin, "wii-cb", backend, ub, gpu_backend, test, "gameplay")
    classic = Wii(a, "classic")
    classic.set()
    _to_unranked_css((a, b))
    _search_with(a, lambda: classic.press(classic={"Buttons/+": 1}))
    lo = ppom.read_local(a.c, ppom.find_block(a.c))
    assert lo["lock_pad"] == 4 and lo["lock"]["ready"], lo
    a.inst.wait_for_log(r"gprb lobby: local lock-in ready .*controller Wii Remote 1", timeout=10)
    time.sleep(2.2)  # the server takes one ticket per account per 2 s
    _search_unranked(b)
    _peer_shown(a, ub)
    _peer_shown(b, ua)
    players = (a, b)
    _running(players)
    a.steps("wait 120")
    pa = _port(a)
    _check_wii_port(players, pa, CLASSIC)
    pb = _port(b)
    assert all(_controller_numbers(g)[pb] == pb + 1 for g in players)
    a.shot("01-running")
    _moves(players, pa, lambda: classic.set(classic={"Left Stick/X": 1}), classic.set)
    a.shot("02-classic-moved")
    ck = _confirmed_checksums(a, b)
    assert ck["compared"] > 200 and ck["mismatches"] == 0, ck
    # The game's own quit for a Classic Controller, no pause first.
    classic.press(frames=20, classic={"Triggers/L": 1, "Triggers/R": 1, "Buttons/A": 1, "Buttons/+": 1})
    online_set.wait(lambda: all(online_set.scene(g.c) == online_set.CSS for g in players), 90,
                    "both back on the CSS")
    for g in players:
        assert online_set.gstatus(g.c)["end_reason"] == "quit", g.name
        assert g.debug_scratch()[10] & 0xC0000 == 0xC0000, (g.name, hex(g.debug_scratch()[10]))
    online_set.wait(lambda: not any(g.c.call("replay_recording")["recording"] for g in players), 60,
                    "both recordings to end")
    fa = _recording(a)
    assert fa.info["build"]["sim"] == 3 and fa.setup["sim"] == 3, (fa.info.get("build"), fa.setup.get("sim"))
    kinds = {s[pa * 0x40 + 0x3F] for s in fa.slots}
    assert kinds == {CLASSIC}, kinds
    assert {s[pb * 0x40 + 0x3F] for s in fa.slots} == {0}
    for g in players:
        g.c.mm_cancel()
    v = _viewer(dolphin, backend, gpu_backend, test)
    B.write_rules(v.c, stocks=4, minutes=8, items_off=True)   # the live games' rules
    st = _play(v, fa.path)
    assert st["phase"] != "error", st
    assert st["desync_frame"] == -1, st


@pytest.mark.slow
def test_wii_remote_and_nunchuk_play_unranked(backend: OnlineBackend,  # noqa: F811
                                              dolphin: Callable[..., DolphinInstance],
                                              gpu_backend: str) -> None:
    """A on a Wii Remote alone, B on a Wii Remote with a Nunchuk: both lock in with PLUS, both
    ports get their Wii Remote pads on both machines with their controller types, the Wii
    Remote's D-pad and the Nunchuk's stick move their fighters on both machines with no checksum
    mismatch. A's B+1+2+PLUS with MINUS is P+'s runback (the same match again on both); then B's
    1+B+Z+PLUS (the game's quit with a Nunchuk) ends the game."""
    test = "wii-remote"
    ua, ub = backend.create_user("wren", "WREN"), backend.create_user("nell", "NELL")
    a = _boot_wii(dolphin, "wii-ra", backend, ua, gpu_backend, test, "")
    b = _boot_wii(dolphin, "wii-rb", backend, ub, gpu_backend, test, "Nunchuk")
    remote, nunchuk = Wii(a, "none"), Wii(b, "nunchuk")
    remote.set()
    nunchuk.set()
    _to_unranked_css((a, b))
    _search_with(a, lambda: remote.press(wiimote={"Buttons/+": 1}))
    time.sleep(2.2)
    _search_with(b, lambda: nunchuk.press(wiimote={"Buttons/+": 1}))
    for g in (a, b):
        assert ppom.read_local(g.c, ppom.find_block(g.c))["lock_pad"] == 4, g.name
    _peer_shown(a, ub)
    _peer_shown(b, ua)
    players = (a, b)
    _running(players)
    a.steps("wait 120")
    pa, pb = _port(a), _port(b)
    _check_wii_port(players, pa, WIIMOTE)
    _check_wii_port(players, pb, NUNCHUK)
    a.shot("01-running")
    # A Wii Remote alone is held sideways: its D-pad is the stick (Dolphin's emulated one, upright,
    # reports the same buttons: the game turns them).
    _moves(players, pa, lambda: remote.set(wiimote={"D-Pad/Down": 1}), remote.set)
    _moves(players, pb, lambda: nunchuk.set(nunchuk={"Stick/X": -1}), nunchuk.set)
    b.shot("02-both-moved")
    ck = _confirmed_checksums(a, b)
    assert ck["compared"] > 200 and ck["mismatches"] == 0, ck
    # A's runback: the Wii Remote's quit buttons and MINUS.
    first = online_set.gstatus(a.c)["match_index"]
    remote.press(frames=20, wiimote={"Buttons/B": 1, "Buttons/1": 1, "Buttons/2": 1, "Buttons/+": 1,
                                     "Buttons/-": 1})
    online_set.wait(lambda: all(online_set.gstatus(g.c)["phase"] == "running" and
                                online_set.gstatus(g.c)["match_index"] == first + 1 for g in players),
                    90, "the same match again on both")
    for g in players:
        assert g.debug_scratch()[10] & 0x1000000, (g.name, hex(g.debug_scratch()[10]))
    a.shot("03-runback")
    a.steps("wait 240")
    _check_wii_port(players, pa, WIIMOTE)
    # B's quit with a Nunchuk.
    nunchuk.press(frames=20, wiimote={"Buttons/1": 1, "Buttons/B": 1, "Buttons/+": 1}, nunchuk={"Buttons/Z": 1})
    online_set.wait(lambda: all(online_set.scene(g.c) == online_set.CSS for g in players), 90,
                    "both back on the CSS")
    for g in players:
        assert online_set.gstatus(g.c)["end_reason"] == "quit", g.name
        g.c.mm_cancel()
