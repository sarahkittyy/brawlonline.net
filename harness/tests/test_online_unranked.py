"""Unranked from the game's own menus, end to end, under the gameplay-only session, and the
server's stage lists in Direct.

Two instances, each with its own ``user.json`` and its own copy of the SD card with our plugin
(``game-code`` ``pponline`` ``7ca3a7e`` or later, PPOM v3), are driven with controller input only:
main menu -> PLAY ONLINE -> WITH ANYONE, which opens Brawl's Wi-Fi OPTIONS page with the two
buttons "Unranked" / "Ranked" (docs/game-code.md section 6) -> Unranked -> a character -> START.
The plugin posts ``FIND_OPPONENT mode=1`` through the PPOM mailbox, Dolphin's matchmaking client
sends Slippi's Unranked ``create-ticket`` (``connectCode: []``) to our ``mm``, which pairs the two
strangers from its Unranked queue and answers with the mode's stage list. The stage lists here
are a test ruleset with two stages, so a stage from one cannot come from Dolphin's built-in P+
legal list. Unranked: both games of the set are drawn from it, without repeating a stage (Slippi's
stage pool). Direct: game 1 is drawn from it, and the loser's pick for game 2 is played even
though it is not in the list (Slippi does not restrict Direct's stage select).

Instances are named ``unr-*`` (``ppharness clean --prefix unr-``). Screenshots go to
``run/artifacts/game-bridge/<test>/``. Needs what ``test_online_game.py`` needs: the real Dolphin
build, a working video backend, the built plugin and the server binaries plus Postgres.
"""

from __future__ import annotations

import json
import re
from typing import Any, Callable, Iterator

import pytest

from ppharness import paths
from ppharness.backend import OnlineBackend
from ppharness.instance import DolphinInstance

from test_online import _make_backend, _wait
from test_online_game import (CMD_GET_MATCH_STATE, Game, _boot, _both, _connected_direct,
                              _find_logged, _peer_shown, online_set, ppom)
from ppharness import brawl as B

pytestmark = [pytest.mark.dolphin, pytest.mark.server, pytest.mark.gpu]

ROOT = paths.workspace_root()
# Smashville, Dream Land (srStageKind). The repository's ruleset has P+'s 15 legal stages; this
# test's has two, so where the stage came from is visible.
UNRANKED_STAGES = [0x21, 0x2D]
# Direct's list for this test: two stages again, without Final Destination, which the loser
# picks for game 2 (online_set.STAGE_PICK).
DIRECT_STAGES = [0x21, 0x2D]
# Long enough for the pairing test (the second player searches seconds after the first), short
# enough for the expiry test.
TICKET_TTL_SECS = 20
TIMEOUT_ERROR = "Search timed out: no opponent found within 20 seconds."


def _backend(tmp_path_factory: pytest.TempPathFactory, **kw: Any) -> Iterator[OnlineBackend]:
    """The backend with this module's test ruleset (two-stage lists for Unranked and Direct)."""
    rules = json.loads((ROOT / "server" / "config" / "rulesets.json").read_text(encoding="utf-8"))
    rules["unranked"]["stages"] = UNRANKED_STAGES
    rules["direct"]["stages"] = DIRECT_STAGES
    path = tmp_path_factory.mktemp("rulesets") / "rulesets.json"
    path.write_text(json.dumps(rules), encoding="utf-8")
    be = _make_backend(rulesets_file=path, **kw)
    try:
        yield be
    finally:
        be.stop()


@pytest.fixture(scope="module")
def backend(tmp_path_factory: pytest.TempPathFactory) -> Iterator[OnlineBackend]:
    yield from _backend(tmp_path_factory, ticket_ttl_secs=TICKET_TTL_SECS)


@pytest.fixture(scope="module")
def direct_backend(tmp_path_factory: pytest.TempPathFactory) -> Iterator[OnlineBackend]:
    """The server's default ticket TTL: typing the second code on the keypad takes longer than
    the short TTL of `backend`."""
    yield from _backend(tmp_path_factory)


def _unranked_css_reached(g: Game) -> None:
    """The CSS was opened from the Unranked button (the plugin's menuState is mode + 1)."""
    assert ppom.read_debug(g.c, ppom.find_block(g.c))["menuState"] - 1 == 1


def _setup_logged(host: Game, game: int, stage: int, how: str) -> None:
    text = (host.inst.user_dir / "Logs" / "dolphin.log").read_text(errors="replace")
    assert re.search(rf"gprb lobby: game {game} setup: stage {stage:#x} " + how, text), \
        f"game {game}: stage {stage:#x} {how}"


def _search_unranked(g: Game) -> None:
    """START on the Unranked CSS locks in and searches (Slippi's FN_LOCK_IN_AND_SEARCH)."""
    n = g.finds()
    g.press_until("START", lambda: g.finds() > n, "FIND_OPPONENT")
    _find_logged(g, 1, "")


@pytest.mark.slow
def test_unranked_pairs_strangers_on_a_server_stage(backend: OnlineBackend,
                                                    dolphin: Callable[..., DolphinInstance],
                                                    gpu_backend: str) -> None:
    """Both players search Unranked from the in-game menu, the server pairs them, and the set
    starts by itself on a random stage from the server's Unranked list; game 2 is random from it
    too (no stage select in Unranked) and is the other stage."""
    test = "unranked-set"
    ua, ub = backend.create_user("nora", "NORA"), backend.create_user("otto", "OTTO")
    a = _boot(dolphin, "unr-a", backend, ua, gpu_backend, test, "gameplay")
    b = _boot(dolphin, "unr-b", backend, ub, gpu_backend, test, "gameplay")
    _both(a.to_main_menu, b.to_main_menu)
    _both(a.to_online_page, b.to_online_page)
    _both(lambda: a.to_css("unranked"), lambda: b.to_css("unranked"))
    for g in (a, b):
        _unranked_css_reached(g)
        B.write_rules(g.c, stocks=2, minutes=2, items_off=True)

    # A searches alone first: queued, the game shows "Searching for opponent".
    _search_unranked(a)
    _wait(lambda: a.c.mm_status()["state"] == "matchmaking", 20, "A in the Unranked queue")
    a.steps("wait 30")
    a.shot("04-searching")
    assert a.response()["mm_state"] in (1, 2)

    # B searches: the server pairs the two, both connect, both games are told who it is.
    _search_unranked(b)
    ra = _peer_shown(a, ub)
    rb = _peer_shown(b, ua)
    assert {ra["role"], rb["role"]} == {1, 2}, (ra, rb)
    for g, other, r in ((a, ub, ra), (b, ua, rb)):
        assert r["peer_code"] == other.connect_code and r["error"] == "", r
        g.steps("wait 10")
        g.shot("05-opponent")

    # Dolphin's view: one Unranked match with the server's stage list, handed to the session.
    sa, sb = a.c.mm_status(), b.c.mm_status()
    match_id = sa["match"]["match_id"]
    assert match_id == sb["match"]["match_id"] and match_id.startswith("mode.unranked-"), (sa, sb)
    for s in (sa, sb):
        assert s["mode"] == "unranked" and s["state"] == "connection_success", s
        assert s["match"]["stages"] == UNRANKED_STAGES, s["match"]
        assert s["match"]["items"] == 0
    for g in (a, b):
        st = _wait(lambda g=g: (lambda s: s if s["handoff"] == "started" else None)(g.c.mm_status()),
                   30, "the hand-off")
        assert st["session"]["backend"] == "gameplay", st["session"]
        lobby = online_set.gstatus(g.c)["lobby"]
        assert lobby["stages"] == UNRANKED_STAGES and lobby["stages_from_server"], lobby
        se = ppom.read_session(g.c, ppom.find_block(g.c))
        assert se["mode"] == 1 and se["num_players"] == 2, se
        # PPOM v3: every player's port values travel in SESSION.
        assert all("pv" in p for p in se["players"]), se
    log = backend.log_text("mm")
    line = next((l for l in log.splitlines() if "matched" in l and match_id in l), None)
    assert line and ua.connect_code in line and ub.connect_code in line, log[-2000:]
    assert "unranked ticket waiting" in log

    # The set: game 1 starts by itself on a server stage; game 2 after both press START.
    rep = online_set.play_set([a, b], games=2, mode="unranked")
    stages = [g["stage"] for g in rep["games"]]
    assert all(s in UNRANKED_STAGES for s in stages), [hex(s) for s in stages]
    assert sorted(stages) == sorted(UNRANKED_STAGES), ("Slippi's pool repeats no stage", stages)
    host, _ = online_set.roles([a, b])
    for game, stage in enumerate(stages, 1):
        _setup_logged(host, game, stage, r"\(random from 2 stages, server list\)")
    for g in (a, b):
        assert g.bridge()["lost"] == 0
        g.c.mm_cancel()


@pytest.mark.slow
def test_direct_loser_picks_off_the_server_list(direct_backend: OnlineBackend,
                                                dolphin: Callable[..., DolphinInstance],
                                                gpu_backend: str) -> None:
    """Direct with the server's Direct list: game 1 is random from it (Slippi: Direct's first
    stage is random from the allowed stages); game 2 is the loser's pick on P+'s whole stage
    select, Final Destination, played although it is not in the list (Slippi does not restrict
    Direct's pick; the unranked branch's Dolphin replaced such a pick with a random stage)."""
    test = "direct-server-stages"
    a, b, _, _ = _connected_direct(direct_backend, dolphin, gpu_backend, test, ("rosa", "sven"),
                                   inst_names=("unr-da", "unr-db"))
    for g in (a, b):
        lobby = online_set.gstatus(g.c)["lobby"]
        assert lobby["stages"] == DIRECT_STAGES and lobby["stages_from_server"], lobby
        assert g.c.mm_status()["match"]["stages"] == DIRECT_STAGES
    rep = online_set.play_set([a, b], games=2, mode="direct",
                              panels={g.name: g.panel for g in (a, b)})
    stages = [g["stage"] for g in rep["games"]]
    pick = B.STAGE_KIND[online_set.STAGE_PICK]
    assert pick not in DIRECT_STAGES
    assert stages[0] in DIRECT_STAGES and stages[1] == pick, [hex(s) for s in stages]
    host, _ = online_set.roles([a, b])
    _setup_logged(host, 1, stages[0], r"\(random from 2 stages, server list\)")
    _setup_logged(host, 2, pick, r"\(picked\)")
    for g in (a, b):
        assert g.bridge()["lost"] == 0
        g.c.mm_cancel()


def test_unranked_search_ends_with_the_server_error(backend: OnlineBackend,
                                                    dolphin: Callable[..., DolphinInstance],
                                                    gpu_backend: str) -> None:
    """A lone Unranked search: the client waits forever (as Slippi's), so the server ends the
    ticket at its TTL with an explicit get-ticket-resp error, and the game shows the text. While
    the error shows the character stays locked; Z clears it (Slippi: "Press Z to clear error")."""
    test = "unranked-timeout"
    u = backend.create_user("pia", "PIA")
    g = _boot(dolphin, "unr-t", backend, u, gpu_backend, test, "record")
    g.to_main_menu()
    g.to_online_page()
    g.to_css("unranked")
    _unranked_css_reached(g)
    _search_unranked(g)
    _wait(lambda: g.c.mm_status()["state"] == "matchmaking", 20, "in the Unranked queue")
    r = _wait(lambda: (lambda r: r if r["cmd"] == CMD_GET_MATCH_STATE and r["mm_state"] == 5
                       else None)(g.response()), TICKET_TTL_SECS + 30, "the error in the mailbox")
    assert r["error"] == TIMEOUT_ERROR, r
    st = g.c.mm_status()
    assert st["state"] == "error" and st["error"] == TIMEOUT_ERROR
    assert st["error_source"] == "get_ticket", st
    g.steps("wait 30")
    g.shot("05-unranked-timeout")
    assert g.locked()
    before = g.css()
    g.steps("tap B 8", "wait 20")
    assert g.css() == before
    g.press_until("Z", lambda: g.c.mm_status()["state"] == "idle", "the cleanup")
    _wait(lambda: not g.locked(), 5, "the CSS to unlock")
    g.shot("06-after-clear")
