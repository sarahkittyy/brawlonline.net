"""Input suggestions (Gprb::Suggest, GitHub issue #35): a player's controller changes that can start
an action go to the other players between frames, and their predictions use them.

Two Dolphins play a Direct game through netsim at a 40 ms round trip (delay 1: their inputs come a
little after the frame needs them, where suggestions help most). Both fight with random macros
whose pad scripts step with the host clock from a random point between two reads
(pad_script_timing "time"), as a person's presses fall anywhere between the game's reads.

- Both on: suggestions go both ways, some predictions are right only by one, no player is judged
  untrustworthy, and every confirmed frame's checksum agrees.
- One off (PPR_GPRB_SUGGEST=0, as a build without suggestions): it never says it takes them, so the
  other sends it none, and neither uses any.
"""

from __future__ import annotations

import threading
import time
from typing import Any, Callable

import pytest

from ppharness import flows as F
from ppharness.instance import DolphinInstance
from ppharness.netsim import LinkProfile

from test_online_game import (Game, _confirmed_checksums, _connected_direct,  # noqa: F401
                              _p2p_through_netsim, backend, online_set)

pytestmark = [pytest.mark.dolphin, pytest.mark.server, pytest.mark.gpu]


def _play(backend, dolphin: Callable[..., DolphinInstance], gpu_backend: str, name: str,
          env_b: dict[str, str]) -> tuple[list[dict[str, Any]], dict[str, Any]]:
    sims, inis = _p2p_through_netsim(LinkProfile(latency_ms=20, jitter_ms=1.5))
    try:
        a, b, _, _ = _connected_direct(backend, dolphin, gpu_backend, name, ("sugga", "suggb"), stocks=99,
                                       inst_names=(f"{name}-a", f"{name}-b"), dolphin_ini=inis,
                                       env=(None, env_b))
        players = [a, b]
        online_set.wait(lambda: all(online_set.gstatus(g.c)["phase"] == "running" for g in players), 240,
                        "the match running on both")
        for i, g in enumerate(players):
            g.c.call("pad_script_timing", mode="time", seed=35 + i)
        stop = threading.Event()

        def fight(g: Game) -> None:
            port = online_set.local(g.c)["local_port"]
            F.fight([F.Seat(g.c, port, 0, g.name)], 10 ** 7, mode="random", seed=f"{name}-{g.name}",
                    stop=stop.is_set)

        th = [threading.Thread(target=fight, args=(g,), daemon=True) for g in players]
        for t in th:
            t.start()
        try:
            time.sleep(30)
        finally:
            stop.set()
            for t in th:
                t.join(timeout=60)
        st = [online_set.gstatus(g.c) for g in players]
        cmp = _confirmed_checksums(a, b)
        for g in players:
            g.c.mm_cancel()
        print([s.get("suggest") for s in st], [s.get("pops") for s in st], cmp, flush=True)
        return [s["suggest"] for s in st], cmp
    finally:
        for sim in sims:
            sim.stop()


def test_suggestions_go_both_ways_and_keep_the_game_in_sync(backend, dolphin: Callable[..., DolphinInstance],  # noqa: F811
                                                           gpu_backend: str) -> None:
    sug, cmp = _play(backend, dolphin, gpu_backend, "sugg-on", {})
    assert cmp["compared"] > 1000 and cmp["mismatches"] == 0, cmp
    for s in sug:
        assert s["enabled"]
        assert s["sent"] > 20 and s["received"] > 20, sug
        assert s["ignored"] == 0, sug
    # Right predictions that only a suggestion made right, on at least one side.
    assert sum(s["helped"] for s in sug) > 0, sug


def test_a_player_without_suggestions_gets_none_and_sends_none(backend, dolphin: Callable[..., DolphinInstance],  # noqa: F811
                                                               gpu_backend: str) -> None:
    sug, cmp = _play(backend, dolphin, gpu_backend, "sugg-mixed", {"PPR_GPRB_SUGGEST": "0"})
    assert cmp["compared"] > 1000 and cmp["mismatches"] == 0, cmp
    assert sug[0]["enabled"] and not sug[1]["enabled"], sug
    assert sug[0]["sent"] == 0 and sug[1]["sent"] == 0, sug
    assert sug[0]["received"] == 0 and sug[1]["received"] == 0, sug
