"""The waiting room: Unranked and Ranked search from Brawl's online waiting room (the Online
Training stage, a Sandbag in the middle, the search line in the room's scrolling banner), as
Brawl's own "With Anyone" and Brawlback's Quickplay do (docs/game-code.md, "The waiting room").

Instances are named ``wr-*`` (``ppharness clean --prefix wr-``). Screenshots go to
``run/artifacts/game-bridge/<test>/``. Needs what ``test_online_unranked.py`` needs.
"""

from __future__ import annotations

import json
from typing import Callable, Iterator

import pytest

from ppharness import brawl as B
from ppharness import paths
from ppharness.backend import OnlineBackend
from ppharness.client import PadInput
from ppharness.instance import DolphinInstance

from test_online import _make_backend, _wait
from test_online_game import Game, _boot, _both, _find_logged, _peer_shown, online_set, ppom
from test_online_ranked import STARTERS, _ranked, _strike_to, _wait_view
from test_online_alt_costumes import ALT_R, ALT_Z, COSTUME_ALT_R, COSTUME_ALT_Z, _alt_words, _check_match, _mm_costumes, _stale_words

pytestmark = [pytest.mark.dolphin, pytest.mark.server, pytest.mark.gpu]

ROOT = paths.workspace_root()
STAGE_ONLINE_TRAINING = 0x37
ST_MANSION = "st_mansion.rel"   # P+'s module for the Online Training stage (Training.param)
UNRANKED_STAGES = [0x21, 0x2D]   # Smashville, Dream Land (a two-stage test list)
WIFIPR_HUD = 0x805A0300          # ifWifiprTask's HUD: the banner and the player panels
SCREEN_ONLINE_BUSY = 4           # PPOM: an online search (the launcher shows no "In game")
# training_room.cpp's debug word (scratch[5]): 0xF the step (3 in the room, 7 after it), 0xF0
# the room's exit code, 0xF00 how it ended (1 the match, 2 the CSS), 0x10000 the banner seen, 0x20000 a Sandbag made.
SCR_ROOM = 5
ROOM_RUNNING, ROOM_ENDED = 3, 7
ROOM_BANNER, ROOM_SANDBAG = 0x10000, 0x20000


def _fade(g: Game) -> int:
    """The alpha of the black drawn over the screen last frame (training_room.cpp fadeAlpha)."""
    return _room(g) >> 24


@pytest.fixture(scope="module")
def backend(tmp_path_factory: pytest.TempPathFactory) -> Iterator[OnlineBackend]:
    rules = json.loads((ROOT / "server" / "config" / "rulesets.json").read_text(encoding="utf-8"))
    rules["unranked"]["stages"] = UNRANKED_STAGES
    path = tmp_path_factory.mktemp("rulesets") / "rulesets.json"
    path.write_text(json.dumps(rules), encoding="utf-8")
    be = _make_backend(rulesets_file=path, ticket_ttl_secs=120)
    try:
        yield be
    finally:
        be.stop()


def _room(g: Game) -> int:
    return g.debug_scratch()[SCR_ROOM]


def _stage_kind(g: Game) -> int:
    mm = g.c.read_u32(g.c.read_u32(0x805A00E0) + 8)   # GameGlobal -> gmGlobalModeMelee
    return int.from_bytes(g.c.read_mem(mm + 0x1A, 2), "big")


def _fighter_x(g: Game) -> float | None:
    ps = B.read_players(g.c.read_mem, max_entries=1)
    return ps[0].x if ps else None


def _in_room(g: Game) -> bool:
    return g.scene() == "scMelee" and (_room(g) & 0xF) == ROOM_RUNNING


def _lock_in_held(g: Game, held: tuple[str, ...]) -> None:
    """START with `held` down on the connected CSS (game 1's character step): locks in."""
    n = online_set.local(g.c)["lock"]["seq"]
    r = 255 if "R" in held else 0
    for _ in range(4):
        g.c.pad_script(g.pad, [PadInput(buttons=list(held), r=r, hold=12),
                               PadInput(buttons=list(held) + ["START"], r=r, hold=8), PadInput(hold=6)])
        try:
            _wait(lambda: online_set.local(g.c)["lock"]["seq"] > n, 3, "the lock-in")
            return
        except AssertionError:
            continue
    raise AssertionError(f"{'+'.join(held)}+START x4: no lock-in")


def _search(g: Game, mode: int, held: tuple[str, ...] = ()) -> None:
    """START on the CSS locks in and searches (FIND_OPPONENT); the CSS leaves for the room.
    `held`: buttons down with that START (P+'s alt costume: Z AltZ, R AltR)."""
    n = g.finds()
    if held:
        r = 255 if "R" in held else 0
        for _ in range(4):
            g.c.pad_script(g.pad, [PadInput(buttons=list(held), r=r, hold=12),
                                   PadInput(buttons=list(held) + ["START"], r=r, hold=8), PadInput(hold=6)])
            try:
                _wait(lambda: g.finds() > n, 3, "FIND_OPPONENT")
                break
            except AssertionError:
                continue
        else:
            raise AssertionError(f"{'+'.join(held)}+START x4: no FIND_OPPONENT")
    else:
        g.press_until("START", lambda: g.finds() > n, "FIND_OPPONENT")
    _find_logged(g, mode, "")
    # (A room whose match is already set up ends as soon as it runs.)
    _wait(lambda: _in_room(g) or (_room(g) & 0xF) == ROOM_ENDED, 30, f"{g.inst.name} in the waiting room")


def _check_room(g: Game, shot: str) -> None:
    """The room is Brawl's Online Training stage with the player's fighter, the Sandbag and the
    search line in the banner, and Dolphin runs no match in it."""
    assert _stage_kind(g) == STAGE_ONLINE_TRAINING, hex(_stage_kind(g))
    _wait(lambda: _fade(g) == 0, 5, "the room out of the fade")
    _wait(lambda: _room(g) & ROOM_BANNER, 10, "the banner line")
    _wait(lambda: _room(g) & ROOM_SANDBAG, 10, "the Sandbag")
    x0 = _fighter_x(g)
    g.steps("stick left 30", "wait 10")
    x1 = _fighter_x(g)
    assert x0 is not None and x1 is not None and x1 < x0 - 5, ("the player's controller moves the fighter", x0, x1)
    g.steps("tap A 4", "wait 20")
    # Brawl's "Seeking..." player panels are hidden (the user's, 2026-10-16): every panel of the
    # room's HUD (0x805A0300, +0x1C..) is in state 0, none.
    hud = g.c.read_u32(WIFIPR_HUD)
    assert [g.c.read_u32(g.c.read_u32(hud + 0x1C + 4 * k) + 8) for k in range(7)] == [0] * 7
    g.shot(shot)
    st = g.c.mm_status()
    assert st["state"] == "matchmaking", st
    assert ppom.read_room(g.c, ppom.find_block(g.c))["screen"] == SCREEN_ONLINE_BUSY


def _logs_clean(g: Game) -> None:
    """Dolphin never took the room for the match (GameplaySession OnStageCreate, IsWaitingRoom)."""
    text = (g.inst.user_dir / "Logs" / "dolphin.log").read_text(errors="replace")
    assert "differs beyond the stage variant" not in text


def _back_on_css(g: Game, what: str) -> None:
    _wait(lambda: g.scene() == "scSelctCharacter" and (_room(g) & 0xF) == ROOM_ENDED, 30, what)


@pytest.mark.slow
def test_unranked_set_from_the_waiting_room(backend: OnlineBackend, dolphin: Callable[..., DolphinInstance],
                                            gpu_backend: str) -> None:
    """A searches alone and waits in the room; B searches, the server pairs them, both rooms end
    and game 1 starts by itself; the set plays as it does from the CSS (no desync)."""
    test = "waiting-room-unranked"
    ua, ub = backend.create_user("nora", "NORA"), backend.create_user("otto", "OTTO")
    a = _boot(dolphin, "wr-a", backend, ua, gpu_backend, test, "gameplay")
    b = _boot(dolphin, "wr-b", backend, ub, gpu_backend, test, "gameplay")
    _both(a.to_main_menu, b.to_main_menu)
    _both(a.to_online_page, b.to_online_page)
    _both(lambda: a.to_css("unranked"), lambda: b.to_css("unranked"))
    for g in (a, b):
        B.write_rules(g.c, stocks=2, minutes=2, items_off=True)
        ppom.allow_test_rules(g.c)

    _search(a, 1)
    assert _in_room(a)
    _check_room(a, "01-room-searching")
    _search(b, 1)
    _peer_shown(a, ub)
    _peer_shown(b, ua)
    rep = online_set.play_set([a, b], games=2, mode="unranked")
    stages = [g["stage"] for g in rep["games"]]
    assert sorted(stages) == sorted(UNRANKED_STAGES), [hex(s) for s in stages]
    for g in (a, b):
        assert _room(g) & 0xF0F == 0x100 | ROOM_ENDED, hex(_room(g))   # ended into the match
        assert g.bridge()["lost"] == 0
        _logs_clean(g)
        g.c.mm_cancel()


def _modules(g: Game) -> list[str]:
    """The modules gfModuleManager has loaded, by name (what its isLoaded and destroy compare):
    the manager at *0x805A0038, its 16 gfModuleInfo (60 bytes) from +8, each with its gfModule
    at +0 and its name at +0x1A."""
    mgr = g.c.read_u32(0x805A0038)
    names = []
    for i in range(16):
        info = mgr + 8 + 60 * i
        if g.c.read_u32(info):
            names.append(g.c.read_mem(info + 0x1A, 33).split(b"\0")[0].decode("latin-1"))
    return names


@pytest.mark.slow
def test_waiting_room_again_after_unranked(backend: OnlineBackend, dolphin: Callable[..., DolphinInstance],
                                           gpu_backend: str) -> None:
    """Bug report #31 (2026-10-16): the room that ends straight into Unranked's game 1 left its
    stage module (P+'s st_mansion.rel) loaded, and the next search's room hung the game as it
    loaded (training_room.cpp, "The stage module's name"). After game 1 the module is gone, and
    the next room loads and plays."""
    test = "waiting-room-again"
    ua, ub = backend.create_user("ida", "IDA"), backend.create_user("jon", "JON")
    a = _boot(dolphin, "wr-aa", backend, ua, gpu_backend, test, "gameplay")
    b = _boot(dolphin, "wr-ab", backend, ub, gpu_backend, test, "gameplay")
    _both(a.to_main_menu, b.to_main_menu)
    _both(a.to_online_page, b.to_online_page)
    _both(lambda: a.to_css("unranked"), lambda: b.to_css("unranked"))
    for g in (a, b):
        B.write_rules(g.c, stocks=1, minutes=2, items_off=True)
        ppom.allow_test_rules(g.c)

    _search(a, 1)
    assert _in_room(a)
    _wait(lambda: ST_MANSION in _modules(a), 20, "the room's stage module loaded")
    _search(b, 1)
    _peer_shown(a, ub)
    _peer_shown(b, ua)
    online_set.play_set([a, b], games=1, mode="unranked")
    for g in (a, b):
        assert _room(g) & 0xF0F == 0x100 | ROOM_ENDED, hex(_room(g))   # ended into the match
        assert ST_MANSION not in _modules(g), (g.inst.name, "the room's stage module stayed loaded", _modules(g))

    # As the player in the report: B out of the CSS (the session ends; back on the ONLINE page,
    # WITH ANYONE highlighted), Unranked again, search.
    a.steps("hold B 90", "until muMenuMain 600", "wait 90")
    a.steps("tap A", "wait 90", "tap A", "wait 400", "until scSelctCharacter 600", "wait 30")
    a.steps("stick up 30", "wait 5", "tap A 8", "wait 30")
    a.shot("02-css-again")
    _search(a, 1)
    assert _in_room(a)
    _check_room(a, "03-room-again")
    _logs_clean(a)


@pytest.mark.slow
def test_waiting_room_cancels(backend: OnlineBackend, dolphin: Callable[..., DolphinInstance],
                              gpu_backend: str) -> None:
    """START held for half a second in the room cancels the search and goes back to the CSS, the
    character still selected; a short START press does nothing (bug report #37, 2026-10-16: a
    press as Training's pause cancelled a ranked search as its match was made). B, short or
    held, is the fighter's and cancels nothing (the user's, 2026-10-16: no hold B to leave the
    room). The player plays from controller port 4, the one that pressed START on the CSS."""
    test = "waiting-room-cancel"
    ua = backend.create_user("pia", "PIA")
    a = _boot(dolphin, "wr-c", backend, ua, gpu_backend, test, "gameplay", standard_controllers=(3,))
    a.pad = 3
    a.to_main_menu()
    a.to_online_page()
    a.to_css("unranked")
    picked = a.css()["char"]

    _search(a, 1)
    assert _in_room(a)
    _check_room(a, "00-room-port-4")
    a.steps("wait 30", "tap START 4", "wait 30", "tap START 20", "wait 30")
    assert _in_room(a), "a START press (or 20 frames) does not leave the room"
    assert a.c.mm_status()["state"] == "matchmaking", "nor cancel the search"
    a.steps("tap START 40")
    # The screen goes black before the room ends, and stays black through the load.
    _wait(lambda: _fade(a) == 255, 5, "the fade to black")
    _back_on_css(a, "back on the CSS after holding START")
    _wait(lambda: a.c.mm_status()["state"] == "idle", 10, "the search cancelled")
    a.steps("wait 40")
    assert _fade(a) == 0, "the CSS out of the fade"
    a.shot("01-css-after-start")
    assert a.css()["char"] == picked, (a.css(), picked)

    _search(a, 1)
    assert _in_room(a)
    a.steps("wait 30", "tap B 6", "wait 30")
    assert _in_room(a), "a short B is an attack, not a cancel"
    a.steps("tap B 120", "wait 30")
    a.shot("02-room-after-hold-b")
    assert _in_room(a), "holding B does not leave the room"
    assert a.c.mm_status()["state"] == "matchmaking", "nor cancel the search"


@pytest.mark.slow
def test_ranked_game_1_from_the_waiting_room(backend: OnlineBackend, dolphin: Callable[..., DolphinInstance],
                                             gpu_backend: str) -> None:
    """Ranked waits in the room too; once connected both rooms end on the CSS, the game setup
    begins (game 1's characters, "Opponent: <name>", then the strikes on the stage select) and
    game 1 plays."""
    test = "waiting-room-ranked"
    ua, ub = backend.create_user("rhea", "RHEA"), backend.create_user("sven", "SVEN")
    a = _boot(dolphin, "wr-ra", backend, ua, gpu_backend, test, "gameplay")
    b = _boot(dolphin, "wr-rb", backend, ub, gpu_backend, test, "gameplay")
    _both(a.to_main_menu, b.to_main_menu)
    _both(a.to_online_page, b.to_online_page)
    _both(lambda: a.to_css("ranked"), lambda: b.to_css("ranked"))
    for g in (a, b):
        B.write_rules(g.c, stocks=1, minutes=2, items_off=True)
        ppom.allow_test_rules(g.c)

    _search(a, 0)
    assert _in_room(a)
    _check_room(a, "01-room-searching")
    _search(b, 0)
    _peer_shown(a, ub)
    _peer_shown(b, ua)
    players = [a, b]
    host = a if _ranked(a)["local_port"] == 0 else b
    join = b if host is a else a
    for g in players:
        other = b if g is a else a
        v = _wait_view(g, lambda v: v["game"] == 1 and v["type"] == 3, "game 1's character step")
        assert v["text"].startswith(f"Opponent: {other.name} - Press START ("), v
        assert _room(g) & 0xF0F == 0x200 | ROOM_ENDED, hex(_room(g))   # ended on the CSS
        # The opponent's name is shown: from here leaving the set counts (Ranked's "left" no
        # longer says beforeStageSelect).
        _wait(lambda: _ranked(g)["stage_select"], 10, f"{g.inst.name}: the game setup noted")
    _strike_to(host, join, 0x01)
    rep = online_set.play_game(players, 1)
    assert rep["stage"] == 0x01, rep
    assert rep["checksums"]["mismatches"] == 0, rep
    for g in players:
        _logs_clean(g)


@pytest.mark.slow
def test_ranked_cancel_before_the_stage_select(backend: OnlineBackend, dolphin: Callable[..., DolphinInstance],
                                               gpu_backend: str) -> None:
    """Bug report #37 (2026-10-16): a ranked search cancelled in the room just as its match was
    made counted as the canceller's abandon, since only the opponent reported anything. Here the
    two players drop each other's P2P packets (PPR_TEST_P2P_BLOCK), so the match stays at
    "connecting"; one holds START. Its Dolphin reports "left" before the stage select, and the
    server ends the set TERMINATED with nobody rated."""
    test = "waiting-room-ranked-cancel"
    ua, ub = backend.create_user("tove", "TOVE"), backend.create_user("ugo", "UGOX")
    a = _boot(dolphin, "wr-xa", backend, ua, gpu_backend, test, "gameplay",
              env={"PPR_TEST_P2P_BLOCK": ub.connect_code})
    b = _boot(dolphin, "wr-xb", backend, ub, gpu_backend, test, "gameplay",
              env={"PPR_TEST_P2P_BLOCK": ua.connect_code})
    _both(a.to_main_menu, b.to_main_menu)
    _both(a.to_online_page, b.to_online_page)
    _both(lambda: a.to_css("ranked"), lambda: b.to_css("ranked"))

    _search(a, 0)
    _search(b, 0)
    _wait(lambda: a.c.mm_status()["state"] == "opponent_connecting", 30, "matched, connecting")
    assert _in_room(a)
    a.steps("tap START 40")
    left = _wait(lambda: _ranked(a)["left_before_session"] or None, 10, "the left before the session")
    _back_on_css(a, "back on the CSS after holding START")
    res = _wait(lambda: (lambda r: r if r["status"] != "ASSIGNED" else None)(
        backend._http("GET", f"/v1/ranked/result?matchId={left}&uid={ua.uid}")), 30, "the set settled")
    assert res["status"] == "TERMINATED" and res["reason"] == "left before the stage select", res
    assert res["rating"] is None, res
    for u in (ua, ub):
        pub = backend._http("GET", f"/user/{u.uid}?additionalFields=rank")
        assert pub["rank"]["ratingUpdateCount"] == 0, pub
    _logs_clean(a)


def _room_alt(g: Game, want: int, shot: str) -> None:
    """In the room, the fighter (slot 0) wears the alt held on the search's START (`want`: 0,
    ALT_Z or ALT_R) and no other slot has one."""
    assert online_set.local(g.c)["lock"]["costume"] & 0xC0 == want, online_set.local(g.c)
    _wait(lambda: _fade(g) == 0, 5, "the room out of the fade")
    words = _alt_words(g)
    assert words == [{ALT_Z: 1, ALT_R: 2}.get(want, 0), 0, 0, 0], words
    c = _mm_costumes(g)[0]
    assert c == {ALT_Z: COSTUME_ALT_Z, ALT_R: COSTUME_ALT_R}.get(want, c if c < 0x20 else -1), hex(c)
    g.shot(shot)


@pytest.mark.slow
def test_waiting_room_alt_costumes(backend: OnlineBackend, dolphin: Callable[..., DolphinInstance],
                                   gpu_backend: str) -> None:
    """Bug report #38 (2026-10-16): after a match in a Z-alt the room's fighter wore the Z-alt on
    every later search, whatever was held, until the player left online play. P+ sets each slot's
    costume from its alt words every frame, and only the match setup wrote them; the room took
    what the last match had left. Here the words start as a match with P1 in AltZ (and P+'s 2 in
    the empty slots) leaves them: a search with nothing held has no alt in the room, one with R
    held AltR, one with Z held AltZ."""
    test = "waiting-room-alt-costumes"
    ua = backend.create_user("alma", "ALMA")
    a = _boot(dolphin, "wr-alt", backend, ua, gpu_backend, test, "gameplay")
    a.to_main_menu()
    _stale_words([1, 2, 2, 2])(a)
    a.to_online_page()
    a.to_css("unranked")

    for i, (held, want) in enumerate([((), 0), (("R",), ALT_R), (("Z",), ALT_Z)]):
        _search(a, 1, held)
        assert _in_room(a)
        _room_alt(a, want, f"0{i}-room-{'+'.join(held) or 'none'}")
        a.steps("tap START 40")
        _back_on_css(a, "back on the CSS after holding START")
        _wait(lambda: a.c.mm_status()["state"] == "idle", 10, "the search cancelled")
        a.steps("wait 40")


@pytest.mark.slow
def test_ranked_game_1_alt_costumes(backend: OnlineBackend, dolphin: Callable[..., DolphinInstance],
                                    gpu_backend: str) -> None:
    """Bug report #38: Ranked's game 1 plays the alts held on its START (A Z, B R), on both
    machines, through the room, game 1's character step (whose START is game 1's, the user's,
    2026-10-19) and the strikes; B's machine starts with AltZ words left from an earlier match."""
    test = "waiting-room-ranked-alts"
    ua, ub = backend.create_user("zola", "ZOLA"), backend.create_user("remy", "REMY")
    a = _boot(dolphin, "wr-za", backend, ua, gpu_backend, test, "gameplay")
    b = _boot(dolphin, "wr-zb", backend, ub, gpu_backend, test, "gameplay")
    _both(a.to_main_menu, b.to_main_menu)
    _stale_words([1, 1, 2, 2])(b)
    _both(a.to_online_page, b.to_online_page)
    _both(lambda: a.to_css("ranked"), lambda: b.to_css("ranked"))
    for g in (a, b):
        B.write_rules(g.c, stocks=1, minutes=2, items_off=True)
        ppom.allow_test_rules(g.c)

    _search(a, 0, ("Z",))
    _room_alt(a, ALT_Z, "01-room-a-altz")
    _search(b, 0, ("R",))
    _peer_shown(a, ub)
    _peer_shown(b, ua)
    host = a if _ranked(a)["local_port"] == 0 else b
    join = b if host is a else a
    for g, held in ((a, ("Z",)), (b, ("R",))):
        _wait_view(g, lambda v: v["game"] == 1 and v["type"] == 3 and v["my_turn"], "game 1's character step")
        _lock_in_held(g, held)
    _strike_to(host, join, 0x01)
    port_a = online_set.local(a.c)["local_port"]
    port_b = online_set.local(b.c)["local_port"]
    _check_match(a, b, {port_a: ALT_Z, port_b: ALT_R})
    for g in (a, b):
        _logs_clean(g)
