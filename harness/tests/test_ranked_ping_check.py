"""Ranked's ping check (bug report #226; Dolphin ``Online/GameSetup.cpp``, plugin ``online_menu.cpp``
and ``match_hud.cpp``).

Ranked's match quality aborts a game at a round trip of 180 ms or more, and pairs mm made under its
150 ms cap often play at 200-250 ms (its GeoIP estimate is far off for South America): RICA#237's
sets were voided about 20 s into game 1, 21 of 28 in a week, often against the same opponent again
and again. Now both machines measure the round trip before game 1's characters and, at one the
match quality would abort for, both players are asked in a popup over the CSS: "High ping: N ms /
Play anyway?" (Sarah's board, 2026-11-05: "a popup would be nice ... just 'Play anyway?'").

- Both PLAY (``test_both_play_at_a_high_ping``): the set goes on, and its match quality no longer
  counts the round trip. The two games' P2P traffic crosses netsim at 100 ms each way (a game round
  trip of about 217 ms): game 1 keeps running well past the 20 s at which it used to be aborted.
- One CANCELs (``test_a_cancel_voids_the_set_and_both_search_again``): the set is void (the server's
  ``not played: high ping``), both games search again by themselves, and mm keeps the two apart
  (mm's rematch hold is off in tests, so only that keeps them from being paired again). Only one
  game measures the high round trip here (``PPR_PING_CHECK_TEST_MS``): the higher of the two is the
  set's, on both.

Instances are named ``pc-*``. Screenshots go to ``run/artifacts/game-bridge/ranked-ping-check/``.
"""

from __future__ import annotations

import re
import struct
import time
from typing import Any, Callable

import pytest

from ppharness.backend import OnlineBackend
from ppharness.instance import DolphinInstance
from ppharness.netsim import LinkProfile

from test_online_game import (PLUGIN, Game, _boot, _both, _p2p_through_netsim, _peer_shown, online_set,
                              ppom)
from test_online_ranked import (SSS, STARTERS, _both_on, _search_ranked, _strike, _view, _wait,
                                _wait_view, backend)  # noqa: F401

pytestmark = [pytest.mark.dolphin, pytest.mark.server, pytest.mark.gpu]

TEST = "ranked-ping-check"
STEP_PING, STEP_CHAR = 6, 3
PLAY, CANCEL = 1, 2
GUARD_S = 0.8   # the popup takes no answer for its first 30 frames


def _sym(g: Game, name: str) -> int:
    """A plugin symbol's address: its offset in PPOnlineMain.map plus its section's address."""
    secidx = {".text": 1, ".ctors": 2, ".dtors": 3, ".rodata": 4, ".data": 5, ".bss": 6}
    u32 = lambda a: struct.unpack(">I", g.c.read_mem(a, 4))[0]
    m = u32(0x800030C8)
    addrs = None
    while m:
        i, nxt, _p, nsec, so = struct.unpack(">5I", g.c.read_mem(m, 0x14))
        if i == 20560:
            secs = g.c.read_mem(so, 8 * nsec)
            addrs = [struct.unpack_from(">I", secs, 8 * k)[0] & ~1 for k in range(nsec)]
            break
        m = nxt
    assert addrs, "the plugin is not loaded"
    cur = None
    for line in (PLUGIN.parent / "PPOnlineMain.map").read_text().splitlines():
        parts = line.split()
        if len(parts) == 5 and parts[4].startswith("."):
            cur = parts[4]
        elif len(parts) == 5 and parts[4] == name:
            return addrs[secidx[cur]] + int(parts[0], 16)
    raise KeyError(name)


def _popup(g: Game) -> dict[str, int]:
    """online_menu.cpp's g_pingPopup."""
    v = struct.unpack(">IIBBBB", g.c.read_mem(_sym(g, "g_pingPopup"), 12))
    return {"shown": v[0], "answers": v[1], "cursor": v[2], "choice": v[3], "requeues": v[4], "drawn": v[5]}


def _pair(backend: OnlineBackend, dolphin: Callable[..., DolphinInstance], gpu_backend: str, names: tuple[str, str],
          inis: tuple[dict[str, Any], dict[str, Any]] = ({}, {}),
          envs: tuple[dict[str, str] | None, dict[str, str] | None] = (None, None)) -> tuple[Game, Game]:
    """Two players searching Ranked, paired, connected (the hand-off started)."""
    ua, ub = backend.create_user(names[0], names[0].upper()[:4]), backend.create_user(names[1], names[1].upper()[:4])
    a = _boot(dolphin, f"pc-{names[0]}", backend, ua, gpu_backend, TEST, "gameplay", dolphin_ini=inis[0], env=envs[0])
    b = _boot(dolphin, f"pc-{names[1]}", backend, ub, gpu_backend, TEST, "gameplay", dolphin_ini=inis[1], env=envs[1])
    _both(a.to_main_menu, b.to_main_menu)
    _both(a.to_online_page, b.to_online_page)
    _both(lambda: a.to_css("ranked"), lambda: b.to_css("ranked"))
    for g in (a, b):
        ppom.allow_test_rules(g.c)
    _search_ranked(a)
    _search_ranked(b)
    _peer_shown(a, ub)
    _peer_shown(b, ua)
    for g in (a, b):
        _wait(lambda g=g: g.c.mm_status()["handoff"] == "started", 30, "the hand-off")
    return a, b


def _asked(g: Game, ms: tuple[int, int]) -> dict[str, Any]:
    """The popup is up on `g` (drawn by match_hud.cpp) at a round trip in `ms`."""
    v = _wait_view(g, lambda v: v["active"] and v["type"] == STEP_PING and ms[0] <= v["ping_ms"] <= ms[1],
                   f"asked at {ms[0]}-{ms[1]} ms", timeout=40)
    _wait(lambda: (lambda p: p if p["shown"] and p["drawn"] else None)(_popup(g)), 10, f"{g.name}: the popup drawn")
    assert v["my_turn"] and not v["may_lock"] and v["my_answer"] == 0, v
    assert v["text"] == f"High ping: {v['ping_ms']} ms - Play anyway? ({v['seconds']})", v
    assert _popup(g)["cursor"] == 1, "the cursor starts on CANCEL"
    return v


def _play(g: Game) -> None:
    g.steps("tap DLEFT 6")
    _wait(lambda: _popup(g)["cursor"] == 0, 5, f"{g.name}: the cursor on PLAY")
    g.steps("tap A 6")
    _wait_view(g, lambda v: v["my_answer"] == PLAY or v["type"] != STEP_PING, "our PLAY taken")


@pytest.mark.slow
def test_both_play_at_a_high_ping(backend: OnlineBackend, dolphin: Callable[..., DolphinInstance],
                                  gpu_backend: str) -> None:
    sims, inis = _p2p_through_netsim(LinkProfile(latency_ms=100.0))
    try:
        a, b = _pair(backend, dolphin, gpu_backend, ("pia", "pob"), inis=inis)
        players = [a, b]
        va, vb = _asked(a, (190, 260)), _asked(b, (190, 260))
        assert va["ping_ms"] == vb["ping_ms"], (va, vb)   # the higher of the two, on both
        time.sleep(GUARD_S)
        a.shot("01-asked")
        _play(a)
        v = _wait_view(a, lambda v: v["type"] == STEP_PING and v["my_answer"] == PLAY, "waiting for pob")
        assert re.fullmatch(r"Waiting for pob \(\d+\)", v["text"]), v
        a.shot("02-waiting")
        _play(b)
        for g in players:
            v = _wait_view(g, lambda v: v["type"] == STEP_CHAR, "the character step")
            assert v["text"].startswith("Opponent: "), v
            g.inst.wait_for_log(r"GameSetup: ping check: both play at \d+ ms", timeout=5)
            g.inst.wait_for_log(r"gprb: the match quality ignores the round trip", timeout=5)
        a.shot("03-character-step")

        # Game 1 at about 217 ms: it used to be aborted about 20 s in (debt +4 a 2.5 s interval).
        _both_on(players, SSS, "game 1: both on the stage select")
        for g in players:
            _wait_view(g, lambda v: v["type"] == 1 and sorted(v["selectable"]) == STARTERS, "the starters")
        host = a if _view(a)["my_turn"] else b
        join = b if host is a else a
        _strike(host, 0x04, join)
        _wait_view(join, lambda v: v["my_turn"], "the joiner's turn")
        _strike(join, 0x21, host)
        _strike(join, 0x2E, host)
        _strike(host, 0x09, join)
        online_set.wait(lambda: all(online_set.gstatus(g.c)["phase"] == "running" for g in players), 180,
                        "game 1 running on both")
        started = time.monotonic()
        while time.monotonic() - started < 40:
            for g in players:
                st = online_set.gstatus(g.c)
                assert st["phase"] == "running", (g.name, st.get("phase"), st.get("error"))
            time.sleep(2)
        a.shot("04-game-running-40s")
        for g in players:
            log = g.inst.log.read()
            assert "match quality low" not in log, g.name
            # The game's own round trip (GekkoNet's) was one the match quality counts: it would have
            # aborted the game after 8 intervals (+4 each, 30 to abort), about 20 s.
            pings = [int(p) for p in re.findall(r"gprb net start: frame \d+: .*? ping (\d+) ms", log)]
            assert pings and min(pings) >= 180, (g.name, pings)
    finally:
        for s in sims:
            s.stop()


@pytest.mark.slow
def test_a_cancel_voids_the_set_and_both_search_again(backend: OnlineBackend,
                                                      dolphin: Callable[..., DolphinInstance],
                                                      gpu_backend: str) -> None:
    matched_before = backend.log_text("mm").count("matched match_id=mode.ranked")
    a, b = _pair(backend, dolphin, gpu_backend, ("cia", "cob"), envs=({"PPR_PING_CHECK_TEST_MS": "230"}, None))
    players = [a, b]
    for g in players:
        _asked(g, (230, 230))   # b measured about 17 ms; a's 230 is the set's
    finds = [g.finds() for g in players]
    time.sleep(GUARD_S)
    a.steps("tap B 6")   # B cancels, wherever the cursor is
    va = _wait_view(a, lambda v: v["my_answer"] == CANCEL, "our CANCEL taken")
    assert va["text"] == "You cancelled (230 ms ping)", va
    vb = _wait_view(b, lambda v: v["peer_answer"] == CANCEL, "cia's CANCEL heard")
    assert vb["text"] == "cia chose not to play at 230 ms", vb
    b.shot("01-cancelled-by-cia")
    for g, f in zip(players, finds):
        g.inst.wait_for_log(r"GameBridge: the ranked set ended in its ping check: .*; searching again", timeout=10)
        # Searching again by itself, from the waiting room, with the opponent to keep away.
        _wait(lambda g=g, f=f: g.finds() > f, 15, f"{g.name}: searching again")
        _wait(lambda g=g: _popup(g)["requeues"] >= 1, 5, f"{g.name}: the requeue")
    for uid in (a.user.uid, b.user.uid):
        _wait(lambda uid=uid: re.search(rf"ranked pair kept apart.*uid={uid}", backend.log_text("mm")), 20,
              f"mm keeps {uid} away")
    _wait(lambda: "not played: high ping" in backend.log_text("accounts"), 20, "the set voided")
    assert re.search(r'ranked set settled.*status="TERMINATED".*not played: high ping',
                     backend.log_text("accounts")), backend.log_text("accounts")[-2000:]
    # Both search, nobody else does, and mm's rematch hold is off: they are still not paired.
    time.sleep(20)
    assert backend.log_text("mm").count("matched match_id=mode.ranked") == matched_before + 1
    for g in players:
        assert g.c.mm_status()["state"] in ("matchmaking", "initializing"), g.c.mm_status()
    b.shot("02-searching-again")
