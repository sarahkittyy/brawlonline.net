"""Unranked from the game's own menus, end to end, under the gameplay-only session.

Two instances, each with its own ``user.json`` and its own copy of the SD card with our plugin,
are driven with controller input only: main menu -> PLAY ONLINE -> WITH ANYONE -> BASIC VERSUS
(Unranked) -> a character -> START. The plugin posts ``FIND_OPPONENT mode=1`` through the PPOM
mailbox, Dolphin's matchmaking client sends Slippi's Unranked ``create-ticket`` (``connectCode:
[]``) to our ``mm``, which pairs the two strangers from its Unranked queue and answers with the
mode's stage list. The stage list here is a test ruleset with two stages, so a stage from it
cannot come from Dolphin's built-in P+ legal list; both games of the set must be played on it,
without repeating a stage (Slippi's stage pool).

Instances are named ``unr-*`` (``ppharness clean --prefix unr-``). Screenshots go to
``run/artifacts/game-bridge/<test>/``. Needs what ``test_online_game.py`` needs: the real Dolphin
build, a working video backend, the built plugin and the server binaries plus Postgres.
"""

from __future__ import annotations

import json
import re
from typing import Callable, Iterator

import pytest

from ppharness import paths
from ppharness.backend import OnlineBackend
from ppharness.instance import DolphinInstance

from test_online import _make_backend, _wait
from test_online_game import (CMD_GET_MATCH_STATE, Game, _boot, _both, _find_logged, _peer_shown,
                              online_set, ppom)
from ppharness import brawl as B

pytestmark = [pytest.mark.dolphin, pytest.mark.server, pytest.mark.gpu]

ROOT = paths.workspace_root()
# Smashville, Dream Land (srStageKind). The repository's ruleset has P+'s 15 legal stages; this
# test's has two, so where the stage came from is visible.
UNRANKED_STAGES = [0x21, 0x2D]
# Long enough for the pairing test (the second player searches seconds after the first), short
# enough for the expiry test.
TICKET_TTL_SECS = 20
TIMEOUT_ERROR = "Search timed out: no opponent found within 20 seconds."


@pytest.fixture(scope="module")
def backend(tmp_path_factory: pytest.TempPathFactory) -> Iterator[OnlineBackend]:
    rules = json.loads((ROOT / "server" / "config" / "rulesets.json").read_text(encoding="utf-8"))
    rules["unranked"]["stages"] = UNRANKED_STAGES
    path = tmp_path_factory.mktemp("rulesets") / "rulesets.json"
    path.write_text(json.dumps(rules), encoding="utf-8")
    be = _make_backend(rulesets_file=path, ticket_ttl_secs=TICKET_TTL_SECS)
    try:
        yield be
    finally:
        be.stop()


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
    log = backend.log_text("mm")
    line = next((l for l in log.splitlines() if "matched" in l and match_id in l), None)
    assert line and ua.connect_code in line and ub.connect_code in line, log[-2000:]
    assert "unranked ticket waiting" in log

    # The set: game 1 starts by itself on a server stage; game 2 after both press START.
    rep = online_set.play_set([a, b], games=2, mode="unranked")
    stages = [g["stage"] for g in rep["games"]]
    assert all(s in UNRANKED_STAGES for s in stages), [hex(s) for s in stages]
    assert sorted(stages) == sorted(UNRANKED_STAGES), ("Slippi's pool repeats no stage", stages)
    host = a if online_set.gstatus(a.c)["role"] == "host" else b
    text = (host.inst.user_dir / "Logs" / "dolphin.log").read_text(errors="replace")
    for game, stage in enumerate(stages, 1):
        assert re.search(rf"gprb lobby: game {game} setup: stage {stage:#x} \(random from 2 stages, "
                         r"server list\)", text), f"game {game}"
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
