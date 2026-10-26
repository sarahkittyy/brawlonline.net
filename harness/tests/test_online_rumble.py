"""Online, a machine's controller gets its own player's rumble, by that player's own setting.

A player's report (2026-10-20): "if my controller is in port 1 but my opponent ends up being
player 1, I get their rumble", and the same with port 2. Brawl rumbles the channel of the in-game
port (PADControlMotor, which only sends to a channel with a controller plugged in), and online
the in-game ports are the session's players, not this machine's controllers. The plugin sends
the local player's port's rumble to the controller they locked in with and drops every other
port's (online.cpp, setRumblePorts).

And (Sarah's board, 2026-10-25: "atm it only works in the waiting room for me"): whether a player
rumbles at all came from the save's per-port Rumble option of their in-game port, since the match
has no name tags, while the waiting room's fighter has the tag. Now the local player's port gets
their own setting, as offline: their tag's Rumble, or with no tag (as here) the save's option of
the controller they locked in with (online_match.cpp, hkRumbleSetup).

Two instances with one controller each, in port 1 (SI port 2 empty, as for a player with one
controller), no name tags; Unranked makes the joiner P2. Each machine's save says which ports
have Rumble on, both players fight, and Dolphin's record of each local controller's motor (the
harness's rumble_status) says who got the rumble:
- game 1, both saves on for port 1 only (each one's controller's): both controllers rumble
  (before: the joiner's did not, its P2 option was off);
- game 2, the host's save on for port 1 only, the joiner's on for port 2 only (its in-game port):
  the host's controller rumbles, the joiner's does not (before: it rumbled by its P2 option).

Instances are named ``rmb-*``. Needs what ``test_online_game.py`` needs.
"""

from __future__ import annotations

import struct
import threading
import time
from typing import Callable

import pytest

from ppharness import brawl as B
from ppharness import flows as F
from ppharness.instance import DolphinInstance

from test_online_game import Game, _boot, _both, _peer_shown, online_set, ppom
from test_online_unranked import _search_unranked, backend  # noqa: F401

pytestmark = [pytest.mark.dolphin, pytest.mark.server, pytest.mark.gpu]

GAME_GLOBAL_PTR = 0x805A00E0  # GameGlobal*; +0x24 the save data
SAVE_RUMBLE = 0x820           # the save's Rumble option of in-game port p at +p (gmGlobalRecord)
PAD_SYSTEM_PTR = 0x805A0040
PAD_SYS_FLAGS_35 = 0x35       # bit 0x40: rumble allowed (startMotor does nothing without it)


def _u32(g: Game, addr: int) -> int:
    return struct.unpack(">I", g.c.read_mem(addr, 4))[0]


def _set_rumble(g: Game, ports: tuple[int, ...]) -> None:
    """The save's Rumble option on for `ports` (0 = port 1), off for the others."""
    save = _u32(g, _u32(g, GAME_GLOBAL_PTR) + 0x24)
    g.c.write_mem(save + SAVE_RUMBLE, bytes(1 if p in ports else 0 for p in range(4)))
    ps = _u32(g, PAD_SYSTEM_PTR)
    flags = g.c.read_mem(ps + PAD_SYS_FLAGS_35, 1)[0]
    g.c.write_mem(ps + PAD_SYS_FLAGS_35, bytes([flags | 0x40]))


def _starts(g: Game) -> list[int]:
    return [int(p["starts"]) for p in g.c.rumble_status()]


def _fight(players: list[Game], frames: int) -> None:
    """Both chase and hit each other (each with its controller, port 1, as its own player)."""
    def run(g: Game) -> None:
        port = online_set.local(g.c)["local_port"]
        F.fight([F.Seat(g.c, port, 0, g.name)], frames, mode="chase", seed=f"rumble-{g.name}")
    th = [threading.Thread(target=run, args=(g,)) for g in players]
    for t in th:
        t.start()
    for t in th:
        t.join(timeout=600)


def _damage(g: Game) -> dict[int, float]:
    return {p.port: p.damage for p in B.read_players(g.c.read_mem, max_entries=2) if p.damage is not None}


def _own_rumble_applied(players: list[Game]) -> None:
    """The match setup gave each player's port their own setting (DEBUG scratch[13] 0x100 << port)."""
    for g in players:
        port = online_set.local(g.c)["local_port"]
        assert g.debug_scratch()[13] & (0x100 << port), (g.name, "own rumble not applied", hex(g.debug_scratch()[13]))


def _play(players: list[Game], game: int) -> dict[str, list[int]]:
    """The running game `game`: both fight; each instance's motor starts per local controller port
    during the fight."""
    online_set.wait(lambda: all(online_set.gstatus(g.c)["phase"] == "running" for g in players), 240,
                    f"game {game} running on both")
    before = {g.name: _starts(g) for g in players}
    _fight(players, 900)
    got = {g.name: [n - b for n, b in zip(_starts(g), before[g.name])] for g in players}
    dmg = {g.name: _damage(g) for g in players}
    print(f"game {game}: motor starts per local port {got}, "
          f"damage {dmg}", flush=True)
    for g in players:
        g.shot(f"g{game}-fought")
        # A hit landed: it rumbles both the fighter hit and the one hitting.
        assert any(d > 0 for d in dmg[g.name].values()), (g.name, "nobody was hit", dmg)
    return got


@pytest.mark.slow
def test_only_the_local_players_rumble(backend, dolphin: Callable[..., DolphinInstance],  # noqa: F811
                                       gpu_backend: str) -> None:
    test = "online-rumble"
    ua, ub = backend.create_user("rumh", "RUMH"), backend.create_user("rumj", "RUMJ")
    a = _boot(dolphin, "rmb-a", backend, ua, gpu_backend, test, "gameplay", standard_controllers=(0,))
    b = _boot(dolphin, "rmb-b", backend, ub, gpu_backend, test, "gameplay", standard_controllers=(0,))
    players = [a, b]
    _both(a.to_main_menu, b.to_main_menu)
    _both(a.to_online_page, b.to_online_page)
    _both(lambda: a.to_css("unranked"), lambda: b.to_css("unranked"))
    for g in players:
        B.write_rules(g.c, stocks=4, minutes=8, items_off=True)
        ppom.allow_test_rules(g.c)
    # Game 1: both saves have Rumble on for port 1 only, each one's controller's (the joiner is
    # P2, whose option is off).
    _set_rumble(a, (0,))
    _set_rumble(b, (0,))
    _search_unranked(a)
    time.sleep(2.2)  # the server takes one ticket per account per 2 s
    _search_unranked(b)
    _peer_shown(a, ub)
    _peer_shown(b, ua)
    online_set.wait(lambda: all(online_set.gstatus(g.c)["phase"] == "running" for g in players), 240,
                    "game 1 running on both")
    host, join = online_set.roles(players)
    assert online_set.local(host.c)["local_port"] == 0 and online_set.local(join.c)["local_port"] == 1
    for g in players:
        assert online_set.local(g.c)["lock_pad"] == 0, (g.name, online_set.local(g.c))

    got = _play(players, 1)
    for g in players:
        assert got[g.name][0] > 0, (g.name, "its controller never rumbled by its own setting", got)
        assert got[g.name][1:] == [0, 0, 0], got
    _own_rumble_applied(players)

    # Unranked's quit (L+R+A+START), back on the CSS; game 2: the host on for port 1, the joiner
    # on for port 2 only (its in-game port, not its controller's).
    host.steps("hold L+R+A+START 20")
    online_set.wait(lambda: all(online_set.scene(g.c) == online_set.CSS for g in players), 60,
                    "both back on the CSS")
    for g in players:
        g.steps("wait 60")
    _set_rumble(host, (0,))
    _set_rumble(join, (1,))
    online_set.lock_in_next(players, 2, mode="unranked")
    got = _play(players, 2)
    assert got[host.name][0] > 0, ("the host's controller never rumbled by its own setting", got)
    assert got[join.name] == [0, 0, 0, 0], ("the joiner's controller rumbled by its P2 option", got)
    assert got[host.name][1:] == [0, 0, 0], got
    _own_rumble_applied(players)
    for g in players:
        assert g.bridge()["lost"] == 0
        g.c.mm_cancel()
