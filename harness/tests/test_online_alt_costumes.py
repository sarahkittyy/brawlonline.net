"""P+'s "Hold Z for AltZ / R (C on a Nunchuk) for AltR" alt costumes in an online match.

Offline, P+ (LegacyTE/AltCostume.asm) reads the controller when its CSS is left and stores a word
per slot at 0x901C4618 + slot * 0x5C (1 AltZ, 2 AltR); its Gecko lines then set that slot's costume
in gmGlobalModeMelee to 62 / 61 every frame, and the loader takes Fit<Char>AltZ/AltR.pac. Online
the plugin carries the buttons held on the START that locked in with the lock-in's costume
(0x40 AltZ, 0x80 AltR) and the match setup writes all four words from SESSION on every machine
(game-code online_menu.cpp altHeld, online_match.cpp setupMatch; docs/game-code.md).

Two games on the Direct CSS with the gameplay backend (test_online_game.py's setup). Screenshots
under ``run/artifacts/game-bridge/<test>/``.
"""

from __future__ import annotations

import struct
from typing import Any, Callable

import pytest

from ppharness.backend import OnlineBackend
from ppharness.instance import DolphinInstance

from test_online_game import Game, _confirmed_checksums, _connected_direct, backend  # noqa: F401 (fixture)
import online_set  # noqa: E402  (test_online_game put harness/tools on the path)

pytestmark = [pytest.mark.dolphin, pytest.mark.server, pytest.mark.gpu, pytest.mark.slow]

ALT_WORDS = 0x901C4618   # P+'s alt word per slot (+ slot * 0x5C)
PLAYER_SIZE = 0x5C
MM_PLAYERS = 0x98        # gmGlobalModeMelee players; +0x05 the costume
ALT_Z, ALT_R = 0x40, 0x80
COSTUME_ALT_Z, COSTUME_ALT_R = 62, 61   # what P+'s Gecko lines set the costume to


def _alt_words(g: Game) -> list[int]:
    return [g.c.read_u32(ALT_WORDS + i * PLAYER_SIZE) for i in range(4)]


def _mm_costumes(g: Game) -> list[int]:
    mm = g.c.read_u32(g.c.read_u32(0x805A00E0) + 0x08)
    raw = g.c.read_mem(mm + MM_PLAYERS, 4 * PLAYER_SIZE)
    return [raw[i * PLAYER_SIZE + 5] for i in range(4)]


def _stale_words(words: list[int]) -> Callable[[Game], None]:
    """What offline play leaves behind (P+'s CSS: the last pick's word per slot, 2 in an empty
    slot), written on the main menu before the game goes online."""
    def write(g: Game) -> None:
        for i, w in enumerate(words):
            g.c.write_mem(ALT_WORDS + i * PLAYER_SIZE, struct.pack(">I", w))
        assert _alt_words(g) == words
    return write


def _running(a: Game, b: Game) -> tuple[Game, Game]:
    """Both in the match; (host, joiner)."""
    online_set.wait(lambda: all(online_set.gstatus(g.c)["phase"] == "running" for g in (a, b)), 240,
                    "the match to run on both")
    a.steps("wait 120")
    return (a, b) if online_set.local(a.c)["local_port"] == 0 else (b, a)


def _check_match(a: Game, b: Game, want: dict[int, int]) -> dict[str, Any]:
    """`want`: per port, the costume byte SESSION carries (colour | alt bits). Both machines have
    the same SESSION, the same alt words and the same match costumes, and the frames agree."""
    host, join = _running(a, b)
    se = [online_set.session(g.c) for g in (a, b)]
    picks = [[(p["char_kind"], p["costume"], p["name"]) for p in s["players"]] for s in se]
    assert picks[0] == picks[1], picks
    words = {g.name: _alt_words(g) for g in (a, b)}
    costumes = {g.name: _mm_costumes(g) for g in (a, b)}
    for g, s in zip((a, b), se):
        for port, pl in enumerate(s["players"]):
            assert pl["costume"] & 0xC0 == want[port] & 0xC0, (g.name, port, pl, want)
        expect = [{ALT_Z: 1, ALT_R: 2}.get(want.get(i, 0) & 0xC0, 0) for i in range(4)]
        assert words[g.name] == expect, (g.name, words, expect)
        for port in want:
            c = costumes[g.name][port]
            if want[port] & ALT_Z:
                assert c == COSTUME_ALT_Z, (g.name, port, costumes)
            elif want[port] & ALT_R:
                assert c == COSTUME_ALT_R, (g.name, port, costumes)
            else:
                assert c < 0x20, (g.name, port, costumes)
    assert costumes[a.name][:2] == costumes[b.name][:2], costumes
    for g in (a, b):
        g.shot("01-match")
    a.steps("wait 300")
    ck = _confirmed_checksums(host, join)
    assert ck["mismatches"] == 0 and ck["compared"] > 300, ck
    for g in (a, b):
        g.shot("02-match-later")
    for g in (a, b):
        st = online_set.gstatus(g.c)
        assert st["phase"] == "running", (g.name, st)
        assert not online_set.local(g.c)["disconnected"], (g.name, online_set.local(g.c))
    return {"words": words, "costumes": costumes, "checksums": ck}


def test_alt_costumes_held_on_start(backend: OnlineBackend, dolphin: Callable[..., DolphinInstance],
                                    gpu_backend: str) -> None:
    """A holds Z on the CSS's START, B holds R (each over Fox): the lock-ins carry AltZ / AltR, and
    on both machines A's port plays costume 62 (FitFoxAltZ) and B's 61 (FitFoxAltR), with the
    confirmed frames of both peers agreeing. Each machine starts with the other's alt words left
    from offline play; they do not survive into the match."""
    def hold(*buttons: str) -> Callable[[Game], None]:
        def f(g: Game) -> None:
            g.start_with = buttons
        return f

    a, b, ua, ub = _connected_direct(backend, dolphin, gpu_backend, "alt-costumes", ("zora", "rhea"),
                                     before_online=(_stale_words([2, 1, 2, 2]), _stale_words([1, 2, 0, 0])),
                                     on_css=(hold("Z"), hold("R")))
    assert online_set.local(a.c)["lock"]["costume"] & 0xC0 == ALT_Z, online_set.local(a.c)
    assert online_set.local(b.c)["lock"]["costume"] & 0xC0 == ALT_R, online_set.local(b.c)
    port_a = online_set.local(a.c)["local_port"]
    port_b = online_set.local(b.c)["local_port"]
    _check_match(a, b, {port_a: ALT_Z, port_b: ALT_R})
    for g in (a, b):
        g.c.mm_cancel()


def test_stale_offline_alt_words_do_not_reach_the_match(backend: OnlineBackend,
                                                        dolphin: Callable[..., DolphinInstance],
                                                        gpu_backend: str) -> None:
    """Nobody holds anything; A's machine has AltZ / AltR words left from offline play (P1 held Z,
    then P+'s 2 in the empty slots), B's none. Before, A alone played P1 (and any P3/P4) in an alt
    costume, which B never saw; now both machines write SESSION's words (all 0) and play the same
    costumes."""
    a, b, ua, ub = _connected_direct(backend, dolphin, gpu_backend, "alt-costumes-stale", ("stan", "tess"),
                                     before_online=(_stale_words([1, 2, 2, 2]), None))
    for g in (a, b):
        assert online_set.local(g.c)["lock"]["costume"] & 0xC0 == 0, online_set.local(g.c)
    _check_match(a, b, {0: 0, 1: 0})
    for g in (a, b):
        g.c.mm_cancel()
