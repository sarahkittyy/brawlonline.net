"""The online character select: its status line, its rules and the connect-code keypad.

Three things the online CSS (game-code/PPOnline) has to get right, checked in real P+ with the
plugin (docs/game-code.md §6 "CSS status line", "Rules (item 2)", §7 "The keypad"):

1. The status line is readable: a text that fits is printed at the font's normal size on one
   line; a longer one is wrapped onto two smaller lines and never squeezed; every error the
   server sends fits one line (server/crates/mm/src/messages.rs).
2. Nothing in the rules is the player's choice online (Slippi): the CSS's ITEM and STAGE
   buttons (P+'s item switch, random stage switch and hazard switch) are hidden and do nothing,
   the Code Menu does not open, and the match setup forces the ruleset, so settings left over
   from offline play (or changed on another menu) never reach an online match; the player's
   own settings come back when the menus load.
3. The connect-code keypad: a "PLYR#123" placeholder in the empty field, a '#' key on the
   letters page that turns to the digits at once, erasing the '#' turns back, keys that cannot
   be valid where the cursor is are refused.

Screenshots: run/design/online-css/<test>/. Needs the real Dolphin build, a video backend, the
built plugin and the server binaries plus Postgres, as test_online_game.py.
"""

from __future__ import annotations

import re
import struct
import time
from typing import Any, Callable

import pytest

from ppharness.backend import OnlineBackend
from ppharness.instance import DolphinInstance

from test_online import _wait
from test_online_game import (ROOT, Game, _boot, _connected_direct, backend,  # noqa: F401
                              online_set, ppom)
from ppharness import brawl as B

pytestmark = [pytest.mark.dolphin, pytest.mark.server, pytest.mark.gpu]

SHOTS = ROOT / "run" / "design" / "online-css"
MESSAGES_RS = ROOT / "server" / "crates" / "mm" / "src" / "messages.rs"

# PPOM DEBUG scratch words (game-code/PPOnline, docs/game-code.md §5)
SCR_STATUS = 14   # the status line: layout << 24 | the text's width in font units
SCR_CSS = 15      # locked ITEM/STAGE presses (low byte) | keypad page << 8 | refused keys << 16
LAYOUT_ONE, LAYOUT_TWO, LAYOUT_ONE_SMALL = 1, 2, 3

# muSelCharTask (the CSS task): the ITEM and STAGE buttons' models; a ScnMdlSimple's +0xCC flags
# have 0x60 set while nwSMSetVisibility hides it.
TASK_ITEM, TASK_STAGE = 0x420, 0x424
HAND_X = 0x90
HAND_BUTTON = 0xAC
BUTTON_ITEM, BUTTON_STAGE = 0x19, 0x1A
ITEM_XY, STAGE_XY = (27.0, 20.0), (27.0, 17.0)   # hand coordinates over the two buttons
RECORD_MENU = 0x9017BE50          # gmGlobalRecord+0x810: item frequency, +8 item switch
CODE_MENU_STATE = 0x804E0034      # 4 while P+'s Code Menu is drawn
CODE_MENU_COMBO = [{"buttons": ["L", "R"], "l": 255, "r": 255, "hold": 6},
                   {"buttons": ["L", "R", "DDOWN"], "l": 255, "r": 255, "hold": 4},
                   {"hold": 4}]


def _u(c: Any, a: int) -> int:
    return c.read_u32(a)


def _task(g: Game) -> int:
    return _u(g.c, _u(g.c, _u(g.c, 0x805A0060) + 4) + 0x400)


def _hidden(g: Game, off: int) -> bool:
    mdl = _u(g.c, _u(g.c, _task(g) + off) + 0xC)
    return bool(_u(g.c, mdl + 0xCC) & 0x60)


def _hand(g: Game) -> int:
    area = _u(g.c, _task(g) + 0x44)
    return _u(g.c, area + 0x1A8)


def _scratch(g: Game, i: int) -> int:
    return g.debug_scratch()[i]


def _shots(test: str, g: Game) -> None:
    g.out = SHOTS / test
    g.out.mkdir(parents=True, exist_ok=True)


# --------------------------------------------------------------------------- 1. status line


def _server_messages() -> dict[str, str]:
    """Every error text of server/crates/mm/src/messages.rs, the format functions filled with
    the widest values they get (a connect code of four W's)."""
    src = MESSAGES_RS.read_text(encoding="utf-8")
    out = {m.group(1).lower(): m.group(2) for m in re.finditer(r'pub const (\w+): &str = "([^"]*)";', src)}
    fills = {"mode": "Ranked", "latest": "10.10.10", "code": "WWWW#999"}
    for m in re.finditer(r'pub fn (\w+)\((\w+): &str\) -> String \{\s*format!\("([^"]*)"\)', src):
        out[m.group(1)] = m.group(3).replace("{" + m.group(2) + "}", fills[m.group(2)])
    for mode in ("Teams", "Party"):
        out[f"not_available_{mode.lower()}"] = out["not_available"].replace("Ranked", mode)
    return out


# Dolphin's own errors (Source/Core/Core/Online/Matchmaking.cpp, Slippi's texts) and the longest
# the line can get (Teams' "Could not connect to players: ..." with three names).
DOLPHIN_ERRORS = {
    "dolphin-mm-connect": "Failed to connect to mm server",
    "dolphin-mm-lost": "Lost connection to the mm server",
    "dolphin-mm-status": "Invalid response when getting mm status",
    "dolphin-mm-queue": "Invalid response when joining mm queue",
    "dolphin-teams-connect": "Could not connect to players: WWWWWWWWWWWWWWW, WWWWWWWWWWWWWWW, WWWWWWWWWWWWWWW",
}


def _serve_error(g: Game, text: str, timeout: float = 10) -> None:
    """Answer the game's next FIND_OPPONENT with mmState 5 and `text` (Dolphin's servicing off),
    as Dolphin answers a refused search."""
    b = ppom.find_block(g.c)
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        (_wr, rd, _rc, _rs), reqs = ppom.read_requests(g.c, b)
        done = False
        for seq, cmd, _pl in sorted(reqs):
            if not seq or seq <= rd:
                continue
            if cmd == 0xB4:
                ppom.write_response(g.c, b, seq, 0xB3, ppom.match_state_payload(5, error=text))
                done = True
            ppom.consume(g.c, b, seq)
        if done:
            return
        time.sleep(0.05)
    raise AssertionError("no FIND_OPPONENT")


def test_status_line_is_never_squeezed(backend: OnlineBackend,
                                       dolphin: Callable[..., DolphinInstance],
                                       gpu_backend: str) -> None:
    """Every error the server can send fits the status line at the font's normal size; longer
    texts (Dolphin's own) wrap onto two smaller lines inside the bar or are cut with "...".
    The errors are served the way Dolphin answers a refused search (servicing off), on the
    Unranked CSS, where START searches at once."""
    u = backend.create_user("mona", "MONA")
    g = _boot(dolphin, "game-s", backend, u, gpu_backend, "status-line", "record")
    _shots("status-line", g)
    g.to_main_menu()
    g.to_online_page()
    g.to_css("unranked")
    g.c.game_bridge_config(enabled=False)
    g.steps("wait 20")
    assert _scratch(g, SCR_STATUS) >> 24 == LAYOUT_ONE   # "Press START to search"
    g.shot("00-idle")

    def requests() -> int:
        return ppom.read_requests(g.c, ppom.find_block(g.c))[0][0]   # reqWrite

    def phase() -> int:
        return (_scratch(g, 1) >> 4) & 0xF   # PH_IDLE 0 ... PH_ERROR 4

    results = {}
    cases = [(f"server-{k}", v, LAYOUT_ONE) for k, v in _server_messages().items()]
    cases += [(k, v, None) for k, v in DOLPHIN_ERRORS.items()]
    for name, text, want in cases:
        if phase() == 4:
            g.press_until("Z", lambda: phase() == 0, "Z to clear the error")
        n = requests()
        g.press_until("START", lambda: requests() > n, "START to search", tries=3)
        _serve_error(g, text)
        _wait(lambda: phase() == 4, 5, f"the error {name!r}")
        g.steps("wait 10")
        lay = _scratch(g, SCR_STATUS)
        g.shot(name)
        results[name] = (lay >> 24, lay & 0xFFFFFF, text)
        if want:
            assert lay >> 24 == want, (name, text, lay >> 24, lay & 0xFFFFFF)
        else:
            assert lay >> 24 in (LAYOUT_ONE, LAYOUT_TWO, LAYOUT_ONE_SMALL), (name, lay)
    for k, v in results.items():
        print(f"{k}: layout {v[0]}, {v[1]} units: {v[2]!r}")
    assert results["dolphin-teams-connect"][0] == LAYOUT_TWO   # too long even for two: cut


# --------------------------------------------------------------------------- 2. rules


def test_online_css_settings_are_locked(backend: OnlineBackend,
                                        dolphin: Callable[..., DolphinInstance],
                                        gpu_backend: str) -> None:
    """On the online CSS the ITEM and STAGE buttons are hidden and A where they are opens
    nothing (the item switch, the random stage and hazard switches stay as they are); L+R+Down
    does not open P+'s Code Menu. Checked on the Direct and the Unranked CSS."""
    u = backend.create_user("nils", "NILS")
    g = _boot(dolphin, "game-l", backend, u, gpu_backend, "css-locked", "record")
    _shots("css-locked", g)
    g.to_main_menu()
    g.to_online_page()
    for mode in ("direct", "unranked"):
        if mode == "unranked":
            g.steps("hold B 90", "until muMenuMain 900", "wait 60", "tap B", "wait 60")
        g.to_css(mode)
        g.steps("wait 20")
        assert _hidden(g, TASK_ITEM) and _hidden(g, TASK_STAGE), mode
        before = (g.c.read_mem(RECORD_MENU, 0x10), g.c.read_mem(B.RSS_EXDATA, 0x40))
        g.shot(f"{mode}-01-css")
        for i, (xy, button) in enumerate(((ITEM_XY, BUTTON_ITEM), (STAGE_XY, BUTTON_STAGE))):
            presses = _scratch(g, SCR_CSS) & 0xFF
            g.c.write_mem(_hand(g) + HAND_X, struct.pack(">ff", *xy))
            g.steps("wait 4")
            # Hidden, the button is usually not even hit; if it is, the lock takes the press.
            hit = _u(g.c, _hand(g) + HAND_BUTTON) == button
            g.steps("tap A 8", "wait 60")
            assert _scratch(g, SCR_CSS) & 0xFF == presses + (1 if hit else 0)
            g.shot(f"{mode}-0{2 + i}-A-on-{'item' if button == BUTTON_ITEM else 'stage'}")
            assert g.scene() == "scSelctCharacter"
        assert (g.c.read_mem(RECORD_MENU, 0x10), g.c.read_mem(B.RSS_EXDATA, 0x40)) == before
        refused = _scratch(g, SCR_CSS) >> 24
        g.c.pad_script(0, CODE_MENU_COMBO)
        g.steps("wait 40")
        assert _u(g.c, CODE_MENU_STATE) != 4, "P+'s Code Menu opened on the online CSS"
        assert _scratch(g, SCR_CSS) >> 24 > refused, "the combination did not reach the menu"
        g.shot(f"{mode}-04-code-menu-combo")
        # The CSS is not left frozen (the Code Menu stops the menus while it is open).
        x0 = struct.unpack(">f", g.c.read_mem(_hand(g) + HAND_X, 4))[0]
        g.steps("stick left 20")
        assert struct.unpack(">f", g.c.read_mem(_hand(g) + HAND_X, 4))[0] < x0 - 1
        # The READY TO FIGHT banner: hidden, and A where it is starts nothing, idle and searching.
        _a_on_banner(g, f"{mode}-05-A-on-banner-idle")
        if mode == "direct":
            assert _scratch(g, 12) & 0x80000000, "the own connect code is not shown"
            g.shot("direct-06-own-code")
            g.open_keypad()
            g.type_code("ZZZZ#999")
        else:
            n = g.finds()
            g.press_until("START", lambda: g.finds() > n, "FIND_OPPONENT")
        _wait(g.locked, 10, "the search")
        _a_on_banner(g, f"{mode}-07-A-on-banner-searching")
        g.press_until("Z", lambda: g.c.mm_status()["state"] == "idle", "Z to cancel")


TASK_READY = 0x3C4          # MenSelchrReady: the READY TO FIGHT banner
HAND_TARGET = 0x80          # 5 = over the banner
BANNER_XY = (0.0, -3.0)


def _a_on_banner(g: Game, shot: str) -> None:
    """A with the hand where Brawl's READY TO FIGHT banner is (all characters chosen): the
    banner is hidden, the hand is not over it, and the CSS stays (its own start would leave for
    Brawl's network stage vote, which hangs the game)."""
    assert _hidden(g, TASK_READY)
    g.c.write_mem(_hand(g) + HAND_X, struct.pack(">ff", *BANNER_XY))
    g.steps("wait 4")
    assert _u(g.c, _hand(g) + HAND_TARGET) != 5
    polls = g.c.status().input_polls
    g.steps("tap A 8", "wait 90")
    assert g.scene() == "scSelctCharacter"
    assert g.c.status().input_polls > polls + 60, "the game stopped"
    assert _scratch(g, 10) & 0xFF != 0x3F, "the CSS was left by its own start"
    g.shot(shot)


@pytest.mark.slow
def test_ready_banner_between_games(backend: OnlineBackend,
                                    dolphin: Callable[..., DolphinInstance],
                                    gpu_backend: str) -> None:
    """Connected, between games of a Direct set (both back on the CSS after game 1): A where the
    READY TO FIGHT banner would be starts nothing on either side; both stay connected."""
    a, b, ua, ub = _connected_direct(backend, dolphin, gpu_backend, "banner-between", ("tara", "udo"))
    for g in (a, b):
        _shots("banner-between", g)
    online_set.play_game([a, b], 1)
    online_set.wait(lambda: all(online_set.scene(g.c) == online_set.CSS for g in (a, b)), 120,
                    "both back on the CSS")
    for g in (a, b):
        g.steps("wait 60")
        _a_on_banner(g, f"{g.name}-A-on-banner-between-games")
        assert g.c.mm_status()["state"] == "connection_success"


@pytest.mark.slow
def test_stale_settings_do_not_reach_the_match(backend: OnlineBackend,
                                               dolphin: Callable[..., DolphinInstance],
                                               gpu_backend: str) -> None:
    """Player A's machine carries settings of its own into an online Direct match: items on
    (frequency high, every switch on, as set offline or on the CSS's item switch), the hazard
    switch off (P+'s stage select keeps it in GameGlobal+0x14 +0x25 from the last offline pick)
    and 1 stock. The match setup forces the online ruleset on both machines: the same item
    frequency (none), hazards, stocks and pause, the session runs with agreeing confirmed
    frames, and A's own settings come back when its menus load.
    With the plugin before this change (PPHARNESS_PLUGIN) the same steps show what reached the
    match: see docs/game-code.md §6 "Rules (item 2)"."""
    stale: dict[str, Any] = {}

    def make_stale(g: Game) -> None:
        """What player A's machine brings: the item switch's frequency High and every switch on
        (the bytes the CSS's ITEM screen writes, docs/game-code.md §6), P+'s hazard switch off
        for the stage (the last offline stage select's pick) and a 1-stock rule."""
        m = B.Mem(g.c.read_mem)
        g.c.write_mem(RECORD_MENU, b"\x03")
        g.c.write_mem(RECORD_MENU + 8, b"\xff" * 8)
        st = m.chain(B.GAME_GLOBAL_PTR, B.GG_SEL_STAGE_DATA)
        g.c.write_mem(st + 0x25, b"\x01")
        rule = m.chain(B.GAME_GLOBAL_PTR, B.GG_SET_RULE)
        g.c.write_mem(rule + 4, b"\x01")
        # P+'s Code Menu: Big Head Mode on, hitstun x0.5, P1 infinite shield.
        for name, value in CODE_MENU_STALE.items():
            g.c.write_mem(_code_menu_value(g, name), value)
        stale.update(record=g.c.read_mem(RECORD_MENU, 0x10), hazard=1, stocks=1,
                     code_menu={k: v for k, (v, _d) in _code_menu_values(g).items()})

    a, b, ua, ub = _connected_direct(backend, dolphin, gpu_backend, "stale-settings", ("olga", "piet"),
                                     test_rules=False, before_online=(make_stale, None))
    for g in (a, b):
        _shots("stale-settings", g)
    facts: dict[str, Any] = {}
    online_set.wait(lambda: all(online_set.scene(g.c) == "scMelee" for g in (a, b)), 120,
                    "both in the match")
    online_set.wait(lambda: all(online_set.gstatus(g.c)["phase"] == "running" for g in (a, b)), 240,
                    "the match to run on both")
    a.steps("wait 600")
    for g in (a, b):
        mm = B.Mem(g.c.read_mem).chain(B.GAME_GLOBAL_PTR, B.GG_MODE_MELEE)
        raw = g.c.read_mem(mm, 0x98 + 2 * 0x5C)
        facts[g.name] = {"item_frequency": raw[B.MM_ITEM_FREQUENCY],
                         "hazards_off": bool(raw[0x29] & 0x20),
                         "stocks": [raw[0x98 + 4], raw[0x98 + 0x5C + 4]],
                         "rule": raw[0x08:0x28].hex(),
                         "code_menu": {n: v for n, (v, _d) in _code_menu_values(g).items()}}
        g.shot("01-in-match")
    cks = _confirm_checksums(a, b)
    facts["checksums"] = cks
    facts["gprb"] = {g.name: {k: online_set.gstatus(g.c).get(k) for k in ("phase", "error")}
                     for g in (a, b)}
    print(facts)
    fa, fb = facts[a.name], facts[b.name]
    assert fa == fb, facts
    assert fa["item_frequency"] == 0 and not fa["hazards_off"] and fa["stocks"] == [4, 4], facts
    # The Code Menu: the same on both, the stale lines back at P+'s defaults (B never changed).
    for name in CODE_MENU_STALE:
        k = next(k for k in fa["code_menu"] if k.startswith(name))
        assert fa["code_menu"][k] == 0 or name == "Hitstun Multiplier", (k, fa["code_menu"][k])
        assert struct.pack(">I", fa["code_menu"][k]) != CODE_MENU_STALE[name], k
    assert cks["compared"] > 100 and cks["mismatches"] == 0, cks
    # A's own settings are back once it leaves for the menus: B goes, A's match ends (the
    # disconnect flow), A holds B on its CSS.
    b.inst.kill()
    online_set.wait(lambda: online_set.scene(a.c) == online_set.CSS, 120, "A back on the CSS")
    a.steps("wait 60", "hold B 90", "until muMenuMain 900", "wait 30")
    m = B.Mem(a.c.read_mem)
    assert a.c.read_mem(RECORD_MENU, 0x10) == stale["record"]
    assert a.c.read_mem(m.chain(B.GAME_GLOBAL_PTR, B.GG_SEL_STAGE_DATA) + 0x25, 1)[0] == stale["hazard"]
    assert a.c.read_mem(m.chain(B.GAME_GLOBAL_PTR, B.GG_SET_RULE) + 4, 1)[0] == stale["stocks"]
    assert {k: v for k, (v, _d) in _code_menu_values(a).items()} == stale["code_menu"]
    a.shot("02-menus-own-settings-back")


CODE_MENU_BASE, CODE_MENU_SIZE = 0x804E0000, 0x2520
CODE_MENU_STALE = {"Big Head Mode": struct.pack(">I", 1),
                   "Hitstun Multiplier": struct.pack(">f", 0.5),
                   "Infinite Shield": struct.pack(">I", 1)}


def _code_menu_lines(g: Game) -> list[tuple[str, int]]:
    """P+'s Code Menu value lines as (text, address), found the way the plugin finds them
    (online_menu.cpp codeMenuLine): value at +8, default at +0x10."""
    d = g.c.read_mem(CODE_MENU_BASE, CODE_MENU_SIZE)
    out = []
    for o in range(0, len(d) - 0x20, 4):
        size, typ, text = struct.unpack_from(">H", d, o)[0], d[o + 2], d[o + 6]
        if size & 3 or not 0x20 <= size <= 0x400 or o + size > len(d) or typ > 2 or not 0x1C <= text < size:
            continue
        if typ == 0:
            q = struct.unpack_from(">I", d, o + 0x18)[0]
            if not CODE_MENU_BASE <= q < CODE_MENU_BASE + CODE_MENU_SIZE:
                continue
        elif text != 0x20:
            continue
        t = d[o + text:o + size].split(b"\0")[0]
        if t[:1].isalpha() and b"%" in t and all(0x20 <= c < 0x7F for c in t):
            out.append((t.decode(), CODE_MENU_BASE + o))
    return out


def _code_menu_value(g: Game, name: str) -> int:
    """The value address of the first line whose text starts with `name`."""
    return next(a for t, a in _code_menu_lines(g) if t.startswith(name)) + 8


def _code_menu_values(g: Game) -> dict[str, tuple[int, int]]:
    """{text@address: (value, default)} of every value line."""
    out = {}
    for t, a in _code_menu_lines(g):
        v, dflt = struct.unpack(">I4xI", g.c.read_mem(a + 8, 12))
        out[f"{t}@{a:#x}"] = (v, dflt)
    return out


def _confirm_checksums(host: Game, join: Game) -> dict[str, Any]:
    st = [online_set.gstatus(g.c) for g in (host, join)]
    limit = min(s["current_frame"] for s in st) - online_set.CONFIRM_MARGIN
    cks = [g.c.call("gprb_checksums", since=0)["rows"] for g in (host, join)]
    ma = {r[0]: r[1] for r in cks[0] if r[0] <= limit}
    mb = {r[0]: r[1] for r in cks[1] if r[0] <= limit}
    common = sorted(set(ma) & set(mb))
    bad = [f for f in common if ma[f] != mb[f]]
    return {"compared": len(common), "mismatches": len(bad), "first_mismatch": bad[0] if bad else None}


# --------------------------------------------------------------------------- 3. keypad


def _keypad_text(g: Game) -> str:
    """The keypad's text (MuSelctChrNameEntry+0x04, full-width UTF-8) as ASCII."""
    helper = _u(g.c, _task(g) + 0x44) + 0x370
    raw = g.c.read_mem(_u(g.c, helper + 4), 32).split(b"\0")[0].decode("utf-8", "replace")
    return "".join(chr(ord(ch) - 0xFEE0) if 0xFF01 <= ord(ch) <= 0xFF5E else ch for ch in raw)


def test_code_keypad_hash_key_and_placeholder(backend: OnlineBackend,
                                              dolphin: Callable[..., DolphinInstance],
                                              gpu_backend: str) -> None:
    """The keypad opens on the letters page with the "PLYR#123" placeholder (no recent codes for
    this account). '#' on an empty field is refused; letters, then '#' turns to the digits page
    at once; a digit key pressed twice types two digits; erasing the '#' turns back to the
    letters; a fifth letter is refused."""
    u = backend.create_user("rosa", "ROSA")
    g = _boot(dolphin, "game-k", backend, u, gpu_backend, "keypad", "record")
    _shots("keypad", g)
    g.to_main_menu()
    g.to_online_page()
    g.to_css("direct")
    g.open_keypad()
    g.steps("wait 30")
    sc = _scratch(g, SCR_CSS)
    assert (sc >> 8) & 0xFF == 0 and _keypad_text(g) == ""
    g.shot("01-empty-placeholder")

    def move(d: str) -> None:
        g.steps(f"tap {d} 8", "wait 6")

    def press() -> None:
        g.steps("tap A 8", "wait 12")

    def refused() -> int:
        return (_scratch(g, SCR_CSS) >> 16) & 0xFF

    # The cursor starts on the first key, which is '#': refused on an empty field.
    r0 = refused()
    press()
    assert refused() == r0 + 1 and _keypad_text(g) == ""
    # A (ABC once), D (DEF once)
    move("DRIGHT"); press(); move("DRIGHT"); press()
    assert _keypad_text(g) == "AD"
    g.shot("02-letters-typed")
    # '#': the digits page at once
    move("DLEFT"); move("DLEFT"); press()
    assert _keypad_text(g) == "AD#"
    _wait(lambda: (_scratch(g, SCR_CSS) >> 8) & 0xFF == 1, 3, "the digits page")
    g.shot("03-hash-digits-page")
    # the first key is now '1': twice without moving types two digits
    press(); press()
    assert _keypad_text(g) == "AD#11"
    g.shot("04-digits-typed")
    # erase the digits and the '#': back to the letters
    g.steps("tap B 8", "wait 12", "tap B 8", "wait 12")
    assert _keypad_text(g) == "AD#" and (_scratch(g, SCR_CSS) >> 8) & 0xFF == 1
    g.steps("tap B 8", "wait 12")
    assert _keypad_text(g) == "AD"
    _wait(lambda: (_scratch(g, SCR_CSS) >> 8) & 0xFF == 0, 3, "the letters page again")
    g.shot("05-hash-erased-letters-page")
    # letters up to four, the fifth refused (codes have 2-4 letters): A, D again
    move("DRIGHT"); press(); move("DRIGHT"); press()
    assert _keypad_text(g) == "ADAD"
    r1 = refused()
    move("DLEFT"); press()
    assert refused() == r1 + 1 and _keypad_text(g) == "ADAD"
    g.shot("06-fifth-letter-refused")
    # B on the keypad until it closes, then back to the menus: nothing of ours is left in the
    # CSS's heap (the menus would not load).
    for _ in range(6):
        g.steps("tap B 8", "wait 12")
    _wait(lambda: g.debug_scratch()[4] == 3, 5, "the keypad closed")
    g.steps("hold B 90", "until muMenuMain 900", "wait 30")
