"""P+'s special modes in a room (docs/design/special-rules.md): a published ruleset with one Special
Versus row on (and P+'s item frequencies past HIGH), the room created with it from the launcher's
ruleset box, a whole game between two Dolphins under rollback through the real server: both
machines set up the same match with Brawl's Special setup (the set rule's special byte, what the
setup made of it for each player), and play it to the end with no mismatch.

Screenshots: run/artifacts/game-code/rooms/special/<test>/<player>/.
"""
from __future__ import annotations

import struct
import threading
import time
from typing import Any, Callable

import pytest

from ppharness import brawl as B
from ppharness.backend import OnlineBackend
from ppharness.instance import DolphinInstance

from test_online import _wait
from test_online_game import Game, _boot, backend  # noqa: F401  (the backend fixture)
from test_room_rules import session_rules
from test_rooms_game import ART, CSS, _all, launcher_answer, pick, to_join_room, type_room_code, until_view, write_create
import online_set  # noqa: E402
import ppom  # noqa: E402

pytestmark = [pytest.mark.dolphin, pytest.mark.server, pytest.mark.gpu, pytest.mark.slow]

SPECIAL_ART = ART / "special"
GG = 0x805A00E0                   # g_GameGlobal
SET_RULE, MODE_MELEE = 0x1C, 0x08  # its set rule and gmGlobalModeMelee
MM_PLAYERS, PLAYER_SIZE = 0x98, 0x5C


def rooms(g: Game) -> dict[str, Any]:
    return g.c.call("rooms_status")


def u32(g: Game, a: int) -> int:
    return struct.unpack(">I", g.c.read_mem(a, 4))[0]


def match_special(g: Game) -> dict[str, Any]:
    """What the match was set up with: the set rule's special rows and stage choice byte, per
    player what Brawl's Special setup makes of them, and the game speed."""
    gg = u32(g, GG)
    rule = g.c.read_mem(u32(g, gg + SET_RULE), 0x20)
    mm = g.c.read_mem(u32(g, gg + MODE_MELEE), 0x200)
    players = []
    for i in range(2):
        p = mm[MM_PLAYERS + i * PLAYER_SIZE:MM_PLAYERS + (i + 1) * PLAYER_SIZE]
        players.append({"flags1b": p[0x1B] & ~0x40, "flags1c": p[0x1C],
                        "start_damage": struct.unpack_from(">H", p, 0x22)[0],
                        "hp": struct.unpack_from(">H", p, 0x24)[0], "size": p[0x3C],
                        "scale": struct.unpack_from(">f", p, 0x40)[0],
                        "gravity": struct.unpack_from(">f", p, 0x48)[0]})
    return {"rows": list(rule[0x18:0x20]), "stage_choice": rule[7], "players": players,
            "speed": struct.unpack_from(">f", mm, 0x50)[0], "mm0d": mm[0x0D], "mm62": mm[0x62]}


def special_room(backend: OnlineBackend, dolphin: Callable[..., DolphinInstance], gpu_backend: str,
                 test: str, ruleset: dict[str, Any]) -> tuple[Game, Game, str]:
    """A and B in a room playing `ruleset` (published by A, the room created from the launcher's
    box): A, B and the room code."""
    ua, ub = backend.create_user(f"{test[:4]}ah", "SPAH"), backend.create_user(f"{test[:4]}bj", "SPBJ")
    a = _boot(dolphin, "sp-a", backend, ua, gpu_backend, test, "gameplay", artifacts=SPECIAL_ART)
    b = _boot(dolphin, "sp-b", backend, ub, gpu_backend, test, "gameplay", artifacts=SPECIAL_ART)
    _all(a.to_main_menu, b.to_main_menu)
    _all(a.to_online_page, b.to_online_page)
    view = backend._http("POST", "/v1/rulesets", {"ruleset": ruleset}, token=ua.session_token)
    rs_code = view["code"]
    rid = write_create(a, ruleset=rs_code)
    assert launcher_answer(a, rid)["result"] == "accepted"
    _wait(lambda: a.scene() == CSS, 60, "A on the room's CSS")
    v = until_view(a, lambda v: v["flags"] & ppom.RF_IN, "A in its room", timeout=60)
    code = v["code"]
    to_join_room(b)
    pick(a)
    pick(b, right=2)
    type_room_code(b, code)
    until_view(b, lambda v: v["flags"] & ppom.RF_IN, "B in the room")
    want = ruleset["rules"].get("special", {})
    for g in (a, b):
        _wait(lambda g=g: rooms(g).get("ruleset", {}).get("code") == rs_code, 30, f"{g.name}: the room's ruleset")
        _wait(lambda g=g: ppom.read_session(g.c, ppom.find_block(g.c))["special"]["special"] == want, 20,
              f"{g.name}: the ruleset's special rows in SESSION")
    return a, b, code


def play_one(a: Game, b: Game) -> dict[str, Any]:
    players = [a, b]
    a.steps("tap START 8", "wait 40")
    _wait(lambda: ppom.read_room(b.c, ppom.find_block(b.c))["slots"][0]["bits"] & ppom.SLOT_READY, 20, "B sees A ready")
    b.steps("tap START 8", "wait 10")
    online_set.wait(lambda: all(online_set.gstatus(g.c)["phase"] == "running" for g in players), 240,
                    "both in the match")
    a.steps("wait 200")
    setups = [match_special(g) for g in players]
    for g in players:
        g.shot("match")
    rep = online_set.play_game(players, 1)
    rep["special_setup"] = setups
    return rep


def ruleset_with(name: str, **rules: Any) -> dict[str, Any]:
    return {"format": 1, "name": name, "rules": {"stocks": 1, "timeMinutes": 2, **rules},
            "stageChoice": "random"}


# Every value of every row: (row, value) with the set rule's byte (1 the box's left value, 2 its right).
CASES = [(row, val, i + 1) for row, vals in ppom.SPECIAL_ROWS for i, val in enumerate(vals)]


@pytest.mark.parametrize("row,value,byte", CASES, ids=[f"{r}-{v}" for r, v, _ in CASES])
def test_special_mode_in_a_room_game(backend: OnlineBackend, dolphin: Callable[..., DolphinInstance],
                                     gpu_backend: str, row: str, value: str, byte: int) -> None:
    """One special row on: both machines' match set up with it (the same setup, the row's byte in the
    set rule), a whole game played with no mismatch."""
    test = f"sp-{row}-{value}"
    extra = {"staminaType": 2} if (row, value) == ("stamina", "stamina") else {}
    a, b, _code = special_room(backend, dolphin, gpu_backend, test,
                               ruleset_with(f"Special {row} {value}", special={row: value}, **extra))
    rep = play_one(a, b)
    index = [r for r, _ in ppom.SPECIAL_ROWS].index(row)
    for s in rep["special_setup"]:
        assert s["rows"][index] == byte and sum(1 for x in s["rows"] if x) == 1, s
    assert rep["special_setup"][0] == rep["special_setup"][1], rep["special_setup"]
    assert rep["checksums"]["mismatches"] == 0, rep
    print(rep["special_setup"][0])


def test_bomb_rain(backend: OnlineBackend, dolphin: Callable[..., DolphinInstance], gpu_backend: str) -> None:
    """P+'s BOMB RAIN item frequency (6): the menu record's frequency on both, a game with no mismatch."""
    a, b, _code = special_room(backend, dolphin, gpu_backend, "sp-bombs",
                               ruleset_with("Bomb Rain", itemFrequency=6))
    for g in (a, b):
        _wait(lambda g=g: (session_rules(g) or {}).get("itemFrequency") == 6, 20, f"{g.name}: bomb rain")
    rep = play_one(a, b)
    for g in (a, b):
        gg = u32(g, GG)
        assert g.c.read_mem(u32(g, gg + 0x24) + 0x810, 1)[0] == 6
    assert rep["checksums"]["mismatches"] == 0, rep


def css_area(g: Game) -> int:
    """The local player area on the room's CSS (muSelCharTask +0x44)."""
    task = u32(g, u32(g, u32(g, 0x805A0060) + 4) + 0x400)
    return u32(g, task + 0x44)


ALL_STAR_TABLE, ALL_STAR_TWIN = 0x80588000, 0x80588300


def all_star_roster(g: Game, entries: list[tuple[int, int, int, int]]) -> None:
    """The local panel's roster as P+'s CSS leaves it after placing the coin on each of `entries`
    (character, P+'s colour, costume, CSS id) in turn (AllStarVs.asm)."""
    a = bytearray(0x9C)
    b = bytearray(0x68)
    for k, (kind, colour, costume, css) in enumerate(entries):
        a[3 * k:3 * k + 3] = bytes((kind, colour, costume))
        b[2 * k:2 * k + 2] = bytes((css, costume))
    a[3 * len(entries)] = 0xCC
    g.c.write_mem(ALL_STAR_TABLE, bytes(a))
    g.c.write_mem(ALL_STAR_TABLE + 0x9C, bytes((3 * len(entries),)))
    g.c.write_mem(ALL_STAR_TWIN, bytes(b))
    g.c.write_mem(ALL_STAR_TWIN + 0x68, bytes((2 * len(entries), 0xAC)))


def test_all_star(backend: OnlineBackend, dolphin: Callable[..., DolphinInstance], gpu_backend: str) -> None:
    """All-Star: each player's roster (placed on their CSS, P+'s table) goes with their lock-in; both
    machines start the match with each player's first character and the same tables, a lost stock
    brings the roster's next one (P+'s tables in the rollback set), and the game ends with no
    mismatch."""
    a, b, _code = special_room(backend, dolphin, gpu_backend, "sp-allstar",
                               ruleset_with("All-Star", stocks=3, special={"other": "all-star"}))
    # A: Fox then Marth; B: Marth then Mewtwo (CSS 0x24, character 0x28) in costume 1.
    rosters = {a.name: [(0x07, 0, 0, 0x07), (0x15, 0, 0, 0x13)], b.name: [(0x15, 0, 0, 0x13), (0x28, 1, 1, 0x24)]}
    for g in (a, b):
        all_star_roster(g, rosters[g.name])
    players = [a, b]
    a.steps("tap START 8", "wait 40")
    _wait(lambda: ppom.read_room(b.c, ppom.find_block(b.c))["slots"][0]["bits"] & ppom.SLOT_READY, 20, "B sees A ready")
    b.steps("tap START 8", "wait 10")
    online_set.wait(lambda: all(online_set.gstatus(g.c)["phase"] == "running" for g in players), 240,
                    "both in the match")
    a.steps("wait 200")
    setups = [match_special(g) for g in players]
    tables = [(g.c.read_mem(ALL_STAR_TABLE, 0x280), g.c.read_mem(ALL_STAR_TWIN, 0x1C0)) for g in players]
    sessions = [ppom.read_session(g.c, ppom.find_block(g.c)) for g in players]
    assert setups[0] == setups[1] and setups[0]["rows"][7] == 2, setups
    assert tables[0] == tables[1], "the All-Star tables differ"
    for se in sessions:   # each player's roster in SESSION, as their lock-in carried it
        assert [x["roster"] for x in se["extra"][:2]] == [rosters[a.name], rosters[b.name]], se["extra"][:2]
    ta = tables[0][0]
    assert ta[0:7] == bytes((0x07, 0, 0, 0x15, 0, 0, 0xCC)), ta[0:8].hex()
    assert ta[0xA0:0xA7] == bytes((0x15, 0, 0, 0x28, 1, 1, 0xCC)), ta[0xA0:0xA8].hex()
    # Each port's character through the match (gmGlobalModeMelee's players, which P+'s respawn
    # rewrites with the roster's next), sampled on both machines while the game runs.
    seen: dict[str, list[set[int]]] = {g.name: [set(), set()] for g in players}
    done = threading.Event()

    def sample() -> None:
        while not done.is_set():
            for g in players:
                try:
                    if online_set.gstatus(g.c)["phase"] != "running":
                        continue
                    mm = g.c.read_mem(u32(g, u32(g, GG) + MODE_MELEE) + MM_PLAYERS, 2 * PLAYER_SIZE)
                except Exception:   # noqa: BLE001 (the match ending under the read)
                    continue
                for i in range(2):
                    seen[g.name][i].add(mm[i * PLAYER_SIZE])
            time.sleep(0.25)
    t = threading.Thread(target=sample, daemon=True)
    t.start()
    try:
        rep = online_set.play_game(players, 1)
    finally:
        done.set()
        t.join(5)
    print("characters seen", {k: [sorted(hex(c) for c in v) for v in vs] for k, vs in seen.items()})
    assert rep["checksums"]["mismatches"] == 0, rep
    # Someone lost a stock: their port played their roster's second character too.
    assert any(seen[a.name][i] >= {r[0][0], r[1][0]} for i, r in enumerate((rosters[a.name], rosters[b.name]))), seen



COSTUME_LIST = 0x80585B08   # P+'s CSS slot table's costume lists (+0x10 a slot): {colour, file} each


def costume_files(g: Game, css: int) -> list[int]:
    """The character's costume files by costume number (P+'s list, up to its colour 0x0C)."""
    raw = g.c.read_mem(u32(g, COSTUME_LIST + 0x10 * css), 0x40)
    out = []
    for k in range(0x20):
        if raw[2 * k] == 0x0C:
            break
        out.append(raw[2 * k + 1])
    return out


def costume_on_css(g: Game, css: int, costume: int) -> None:
    """The local panel's coin on `css`, then X up to `costume`."""
    B.css_pick_character(g.c, 0, css)
    for _ in range(costume):
        g.c.pad_script(0, [{"buttons": ["X"], "hold": 3}, {"buttons": [], "hold": 6}])
        B.step(g.c, 12)
    assert u32(g, css_area(g) + 0x1BC) == costume, u32(g, css_area(g) + 0x1BC)


def test_all_star_costume_files(backend: OnlineBackend, dolphin: Callable[..., DolphinInstance],
                                gpu_backend: str) -> None:
    """All-Star loads each player's costume (bug report #184, Falcon's costume 17: "I tried to play
    all star battle and it ... broke"). P+'s roster entry holds the costume's file, not its number
    (Falcon's 17 is FitCaptain44.pac), and it reaches the match whatever the file (Dolphin dropped
    any past 31, and the match then asked for FitCaptain17.pac, which does not exist, and never
    started); a player with no entry of their own (the coin placed before All-Star came on) gets
    their costume's file too (Samus's 9 is FitSamus27.pac)."""
    a, b, _code = special_room(backend, dolphin, gpu_backend, "sp-allstar-files",
                               ruleset_with("All-Star files", special={"other": "all-star"}))
    samus, falcon = B.CSS_ID["samus"], B.CSS_ID["captain_falcon"]
    files = {css: costume_files(a, css) for css in (samus, falcon)}
    assert len(files[samus]) > 9 and len(files[falcon]) > 17, files
    want_a, want_b = files[falcon][17], files[samus][9]
    assert (want_a, want_b) == (44, 27), (want_a, want_b)   # P+ v3.2.1
    falcon_kind, samus_kind = B.CSS_TO_CHAR_KIND[falcon], B.CSS_TO_CHAR_KIND[samus]
    all_star_roster(a, [])
    costume_on_css(a, falcon, 17)
    # A's roster as P+'s CSS made it: Falcon, his costume's file and its number.
    assert a.c.read_mem(ALL_STAR_TABLE, 4) == bytes((falcon_kind, want_a, 17, 0xCC)), a.c.read_mem(ALL_STAR_TABLE, 4).hex()
    costume_on_css(b, samus, 9)
    all_star_roster(b, [])   # none placed since All-Star came on
    players = [a, b]
    a.steps("tap START 8", "wait 40")
    _wait(lambda: ppom.read_room(b.c, ppom.find_block(b.c))["slots"][0]["bits"] & ppom.SLOT_READY, 20, "B sees A ready")
    b.steps("tap START 8", "wait 10")
    try:
        online_set.wait(lambda: all(online_set.gstatus(g.c)["phase"] == "running" for g in players), 240,
                        "both in the match")
    except TimeoutError:
        raise AssertionError("the match never ran: a fighter's costume file did not load (bug #184)") from None
    a.steps("wait 200")
    for g in players:
        se = ppom.read_session(g.c, ppom.find_block(g.c))
        assert [x["roster"] for x in se["extra"][:2]] == [[(falcon_kind, want_a, 17, falcon)], [(samus_kind, want_b, 9, samus)]],             se["extra"][:2]
        mm = g.c.read_mem(u32(g, u32(g, GG) + MODE_MELEE) + MM_PLAYERS, 2 * PLAYER_SIZE)
        assert [mm[i * PLAYER_SIZE + 5] for i in range(2)] == [want_a, want_b], (g.name, mm[5], mm[PLAYER_SIZE + 5])
        g.shot("match")
    rep = online_set.play_game(players, 1)
    assert rep["checksums"]["mismatches"] == 0, rep


def css_fps(g: Game, secs: float = 4.0) -> float:
    """The game loops a second (gfApplication's counter over `secs` of wall time)."""
    f0 = B.read_frame_counters(g.c.read_mem)["app"]
    t0 = time.monotonic()
    time.sleep(secs)
    return (B.read_frame_counters(g.c.read_mem)["app"] - f0) / (time.monotonic() - t0)


def test_all_star_room_css_and_next_game(backend: OnlineBackend, dolphin: Callable[..., DolphinInstance],
                                         gpu_backend: str) -> None:
    """The All-Star room CSS runs at full speed, and the next game starts from each player's own
    roster (Sarah's board, 2026-10-29: "why am i 45 fps after one game in all-star mode", "we also
    ended up as the same character next game, like even though he never picked kirby"). P+'s
    All-Star code in the CSS's setChar leaves 0xAC in r15, where RoomCss kept the other panel's
    view: every frame that panel was set up again, and P+'s AsyncRSP copied its portrait (0.9 MB,
    byte by byte); the CSS ran at about 47 frames a second before and after a game. The match
    setup writes every player's roster into P+'s tables in session order, so back on the CSS the
    local table held P1's roster, and P2's next lock-in carried it."""
    a, b, _code = special_room(backend, dolphin, gpu_backend, "sp-allstar-next",
                               ruleset_with("All-Star next", stocks=2, special={"other": "all-star"}))
    # As players build them, the last entry the coin placed: A Marth then Fox (its coin on Fox
    # from special_room), B Fox then Mewtwo (a coin placed with All-Star on, P+'s hook appends it).
    marth, fox, mewtwo = (0x15, 0, 0, 0x13), (0x07, 0, 0, 0x07), (0x28, 0, 0, 0x24)
    all_star_roster(a, [marth, fox])
    all_star_roster(b, [fox])
    B.css_pick_character(b.c, 0, 0x24)
    for g in (a, b):
        g.steps("wait 30")
    own = {g.name: (g.c.read_mem(ALL_STAR_TABLE, 0xA0), g.c.read_mem(ALL_STAR_TWIN, 0x70)) for g in (a, b)}
    assert own[b.name][0][:7] == bytes((*fox[:3], *mewtwo[:3], 0xCC)), own[b.name][0][:8].hex()
    rosters = {a.name: [marth, fox], b.name: [fox, mewtwo]}
    for g in (a, b):
        fps = css_fps(g)
        assert fps > 57, (g.name, "the All-Star CSS before the game", fps)
    players = [a, b]
    a.steps("tap START 8", "wait 40")
    _wait(lambda: ppom.read_room(b.c, ppom.find_block(b.c))["slots"][0]["bits"] & ppom.SLOT_READY, 20, "B sees A ready")
    b.steps("tap START 8", "wait 10")
    rep = online_set.play_game(players, 1)
    assert rep["checksums"]["mismatches"] == 0, rep
    _wait(lambda: a.scene() == CSS and b.scene() == CSS, 180, "both back on the room's CSS")
    for g in (a, b):
        g.steps("wait 120")
        fps = css_fps(g)
        assert fps > 57, (g.name, "the All-Star CSS after the game", fps)
        # The local panel's tables as this player left them, not P1's from the match.
        assert g.c.read_mem(ALL_STAR_TABLE, 0xA0) == own[g.name][0], g.c.read_mem(ALL_STAR_TABLE, 8).hex()
        assert g.c.read_mem(ALL_STAR_TWIN, 0x70) == own[g.name][1]
        g.shot("back-on-css")
    # Game 2 with the coins kept (no new coin): each player's own roster, its first character first.
    a.steps("tap START 8", "wait 40")
    _wait(lambda: ppom.read_room(b.c, ppom.find_block(b.c))["slots"][0]["bits"] & ppom.SLOT_READY, 30, "B sees A ready")
    b.steps("tap START 8", "wait 10")
    online_set.wait(lambda: all(online_set.gstatus(g.c)["phase"] == "running" for g in players), 240, "game 2")
    a.steps("wait 200")
    for g in players:
        se = ppom.read_session(g.c, ppom.find_block(g.c))
        assert [x["roster"] for x in se["extra"][:2]] == [rosters[a.name], rosters[b.name]], se["extra"][:2]
        mm = g.c.read_mem(u32(g, u32(g, GG) + MODE_MELEE) + MM_PLAYERS, 2 * PLAYER_SIZE)
        assert [mm[i * PLAYER_SIZE] for i in range(2)] == [marth[0], fox[0]], (g.name, mm[0], mm[PLAYER_SIZE])
        g.shot("game-2")


def test_stamina_hp(backend: OnlineBackend, dolphin: Callable[..., DolphinInstance], gpu_backend: str) -> None:
    """Stamina: each player's HP picked on their panel's box (Brawl's HP list, area+0x1D0 = HP / 10)
    goes with their lock-in, the others' panels show it once ready, and the match gives each player
    theirs on both machines."""
    a, b, _code = special_room(backend, dolphin, gpu_backend, "sp-hp",
                               ruleset_with("Stamina HP", special={"stamina": "stamina"}))
    for g, pick_ in ((a, 20), (b, 12)):   # 200 HP, 120 HP
        g.c.write_mem(css_area(g) + 0x1D0, struct.pack(">I", pick_))
        g.steps("wait 10")
    a.steps("tap START 8", "wait 40")
    _wait(lambda: ppom.read_session(b.c, ppom.find_block(b.c))["special"]["room_hp"][0] == 200, 20,
          "B sees A's 200 HP")
    b.shot("a-hp-on-b")
    players = [a, b]
    b.steps("tap START 8", "wait 10")
    online_set.wait(lambda: all(online_set.gstatus(g.c)["phase"] == "running" for g in players), 240,
                    "both in the match")
    a.steps("wait 200")
    setups = [match_special(g) for g in players]
    for g in players:
        g.shot("match")
    rep = online_set.play_game(players, 1)
    assert setups[0] == setups[1], setups
    assert [p["hp"] for p in setups[0]["players"]] == [200, 120], setups[0]
    assert rep["checksums"]["mismatches"] == 0, rep


def plugin_sym(g: Game, name: str) -> int:
    """A plugin symbol's address in `g` (test_rooms_ui.ppom_sym, for this module's Game)."""
    import test_rooms_ui
    shim = type("Shim", (), {"c": g.c, "u32": lambda self, a: u32(g, a)})()
    return test_rooms_ui.ppom_sym(shim, name)


def test_special_box_loads_in_a_room(backend: OnlineBackend, dolphin: Callable[..., DolphinInstance],
                                     gpu_backend: str) -> None:
    """The SPECIAL page's box from Dolphin's own SD card reading (Online/SpecialAssets, while the
    game has the card open: staging 2026-10-27, "needs Project+'s menu files" because the card could
    not be opened a second time): the host opens RULES and turns to SPECIAL, the box is made."""
    a, b, _code = special_room(backend, dolphin, gpu_backend, "sp-box", ruleset_with("Box", special={"body": "metal"}))
    ppom.set_cfg_bits(a.c, ppom.CFG_TEST_OPEN_RULES)
    task = lambda: u32(a, u32(a, u32(a, 0x805A0060) + 4) + 0x400)
    _wait(lambda: u32(a, task() + 0x630) != 0, 20, "A's rules menu open")
    a.steps("wait 60")
    for _ in range(3):
        a.steps("tap R 4", "wait 40")
    box = plugin_sym(a, "g_specialBox")
    _wait(lambda: a.c.read_mem(box, 4) == b"\0\0\0\2" and u32(a, box + 8) >= 1, 30, "the Special Versus box made")
    d = a.c.read_mem(box, 0x28)
    assert list(d[0x14:0x1C]) == [0, 0, 0, 1, 0, 0, 0, 0], d.hex()
    assert struct.unpack_from(">I", d, 0x20)[0] == 0, d.hex()   # no model failed
    a.shot("special-box")
    hints = plugin_sym(a, "g_pageHints")
    assert u32(a, hints) > 0, "the L / R hints were not drawn"
