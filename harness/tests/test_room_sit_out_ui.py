"""SIT OUT and the placed coin on the room CSS, on one instance with Dolphin's half played by
tools/gamecode/roomsim.py (as test_rooms_ui.py). The user's, 2026-10-18 (docs/design/rooms.md
"Sitting out"):

- START with the coin still in the hand readies nobody (it readied the player with the portrait
  under the hand); it puts the coin down there, as A (bug reports #63-#65, 2026-10-19). The coin
  put down goes to the others before START (a lock-in without its ready),
  and another player's placed coin shows on their card, "Choosing...".
- SIT OUT / PLAY (under RULES, level with FFA), START, or A on one's own name plate (Brawl's
  player / CPU switch offline; its pencil still opens the name tags) sits the player out or back in; sitting out takes a ready back, and
  START while sitting out plays again without readying. The host's A on anyone's plate does the
  same for them; anyone else's is refused.
- A card sitting out is a CPU's grey with "SIT OUT" on its plate and no state box.

Screenshots: run/artifacts/game-code/rooms/sit-out/.
"""

from __future__ import annotations

import struct

import pytest

from test_rooms_ui import (FOX, GMKIND_FOX, GMKIND_MARIO, PANEL_X, ROOT, Game, create_room,  # noqa: F401
                           css_area, hand_button, ppom_sym)

import ppom  # noqa: E402

pytestmark = [pytest.mark.dolphin, pytest.mark.gpu]

ART = ROOT / "run" / "artifacts" / "game-code" / "rooms" / "sit-out"
BUTTON_SIT_OUT = 0x31
BUTTON_TAGS = 0x1C                    # the local card's pencil (and plate) on the Wi-Fi CSS
PLATE_Y = -20.0                       # the cards' name plates (x a little right of the centres)
ST_NONE, ST_SELECTING, ST_READY = 0, 2, 4
GREY_BACKPLATE = 9.0                  # a CPU's card colour frame (room_css.cpp GREY_FRAMES)


@pytest.fixture
def game(dolphin_exe):
    g = Game("sit-out-ui", art=ART)
    yield g
    g.close()


def button_centre(g: Game, sym: str) -> tuple[float, float]:
    made, presses, hits, l, r, t, b = struct.unpack(">3I4f", g.c.read_mem(ppom_sym(g, sym), 0x1C))
    assert made >= 1, sym
    return (l + r) / 2, (t + b) / 2


def backplate_frame(g: Game, area: int) -> float:
    """The card's backplate colour frame (MuObject +0x14 -> +0x18 CLR0 controller, frame +0x18)."""
    obj = g.u32(g.area(area) + 0xB0)
    ctl = g.u32(g.u32(obj + 0x14) + 0x18)
    return struct.unpack(">f", g.c.read_mem(ctl + 0x18, 4))[0]


def lock(g: Game) -> dict:
    return ppom.read_local(g.c, g.sim.b)["lock"]


def sit_ops(g: Game) -> list[tuple[int, int]]:
    return [(p[1], p[2]) for p in g.sim.room_ops("sit_out")]


def test_sit_out_and_the_placed_coin(game: Game) -> None:
    g = game
    g.c.game_bridge_config(enabled=False)
    create_room(g)
    sim = g.sim
    me = sim.room.members[0]
    sim.room.open[2] = True
    sim.add_player(1, "alice")
    g.wait(30)

    # The coin in the hand over Fox: START puts it down there (as A) and readies nobody; the
    # others see Fox before START (bug reports #63-#65). Then START readies.
    if css_area(g).placed:
        g.tap("B")
        g.wait(20)
    g.hand_to(*FOX, tol=0.6)
    g.wait(5)
    assert css_area(g).in_hand
    assert me.char == 0xFF, "no coin placed: nothing shown"
    g.tap("START")
    g.wait(30)
    g.shot("01-start-coin-in-hand")
    assert css_area(g).placed
    assert not me.ready and not lock(g)["ready"], lock(g)
    assert (me.ready, me.char) == (False, GMKIND_FOX), (me, lock(g))
    g.tap("START")
    g.wait(30)
    assert me.ready and me.char == GMKIND_FOX
    g.shot("02-ready")

    # Alice's coin on Mario, not ready: her card shows Mario, "Choosing...".
    sim.set_pick(1, GMKIND_MARIO)
    g.wait(30)
    assert g.panel_state(1) == ST_SELECTING
    assert g.u32(g.area(1) + 0x1B8) == 0x00, hex(g.u32(g.area(1) + 0x1B8))   # Mario's CSS id
    g.shot("03-alice-picks-mario")

    # SIT OUT: the ready taken back, the card grey, "PLAY" on the button.
    g.hand_to(*button_centre(g, "g_sitOutButton"), tol=0.4)
    g.wait(5)
    assert hand_button(g) == BUTTON_SIT_OUT
    g.tap("A")
    g.wait(40)
    g.shot("04-sitting-out")
    assert sit_ops(g)[-1] == (0, 1)
    assert me.sitting_out and not me.ready and not lock(g)["ready"]
    assert backplate_frame(g, 0) == GREY_BACKPLATE
    # START plays again, without readying.
    g.tap("START")
    g.wait(40)
    assert sit_ops(g)[-1] == (0, 0) and not me.sitting_out and not me.ready
    assert backplate_frame(g, 0) != GREY_BACKPLATE
    g.shot("05-start-plays-again")

    # A on one's own plate: sits out; again: plays.
    hand_mode = g.u32(g.u32(g.area(0) + 0x1A8) + 0xA4)
    g.hand_to(PANEL_X[0] + 1.5, PLATE_Y, tol=0.6)
    g.wait(5)
    g.tap("A")
    g.wait(40)
    assert me.sitting_out
    assert g.u32(g.u32(g.area(0) + 0x1A8) + 0xA4) == hand_mode, "the plate opened no name tag list"
    g.tap("A")
    g.wait(40)
    assert not me.sitting_out
    # The pencil left of it still opens the name tags (and their controls), no SIT OUT.
    n = len(sit_ops(g))
    g.hand_to(PANEL_X[0] - 7.0, PLATE_Y, tol=0.5)
    g.wait(5)
    assert hand_button(g) == BUTTON_TAGS, hex(hand_button(g))
    g.tap("A")
    g.wait(40)
    g.shot("05b-pencil-opens-tags")
    assert len(sit_ops(g)) == n
    assert g.u32(g.u32(g.area(0) + 0x1A8) + 0xA4) != hand_mode, "the name tag list opened"
    g.tap("B")
    g.wait(40)
    assert g.u32(g.u32(g.area(0) + 0x1A8) + 0xA4) == hand_mode

    # The host's A on alice's plate: she sits out (grey, "SIT OUT", no state box), and back.
    alice = sim.room.members[1]
    g.hand_to(PANEL_X[1] + 1.5, PLATE_Y, tol=0.6)
    g.wait(5)
    g.tap("A")
    g.wait(40)
    g.shot("06-host-sits-alice-out")
    assert sit_ops(g)[-1] == (1, 1) and alice.sitting_out
    assert sim.room.members[1] is not None, "a plate press is no kick"
    assert backplate_frame(g, 1) == GREY_BACKPLATE
    assert g.panel_state(1) == ST_NONE
    g.tap("A")
    g.wait(40)
    assert not alice.sitting_out and backplate_frame(g, 1) != GREY_BACKPLATE
    assert g.panel_state(1) == ST_SELECTING

    # Not the host: another's plate is refused (the buzzer, nothing sent), one's own works.
    sim.room.host = 1
    g.wait(30)
    n = len(sit_ops(g))
    g.tap("A")
    g.wait(40)
    assert len(sit_ops(g)) == n and not alice.sitting_out
    g.hand_to(PANEL_X[0] + 1.5, PLATE_Y, tol=0.6)
    g.wait(5)
    g.tap("A")
    g.wait(40)
    assert me.sitting_out
    g.shot("07-guest-sits-out")
