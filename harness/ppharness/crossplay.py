"""Crossplay variants of the online mode tests: EmissaryRecomp players (ppharness/recomp.py) against
Dolphin players and against each other (docs/design/brawl-online-core.md §10.2).

A mode's crossplay test runs that mode's own test with PPHARNESS_RECOMP_NAMES set for the variant:

    @crossplay.VARIANTS
    def test_unranked_crossplay(variant, backend, dolphin, gpu_backend, monkeypatch):
        crossplay.run(variant, "b$", "[ab]$", monkeypatch, backend, dolphin,
                      lambda d: test_unranked_pairs_strangers_on_a_server_stage(backend, d, gpu_backend))

``vs_dolphin``: the players the first pattern matches are recomps, the rest Dolphins; ``vs_recomp``:
every player the second pattern matches is (a test's viewer Dolphin stays one). Skipped unless PPHARNESS_RECOMP names a recomp build. Afterwards, on
top of the test's own checks: some instance really was a recomp, no machine logged a desync (the
core's checksum exchange, every confirmed frame), mm logged the recomp's engine for its matches,
and accounts' game reports name engine=emissary (when the mode reports games).
"""

from __future__ import annotations

import json
import os
import re
from typing import Any, Callable

import pytest

from .instance import DolphinInstance

VARIANTS = pytest.mark.parametrize("variant", [
    pytest.param("vs_dolphin", marks=pytest.mark.skipif(not os.environ.get("PPHARNESS_RECOMP"),
                                                        reason="PPHARNESS_RECOMP is not set")),
    pytest.param("vs_recomp", marks=pytest.mark.skipif(not os.environ.get("PPHARNESS_RECOMP"),
                                                       reason="PPHARNESS_RECOMP is not set")),
])

_ANSI = re.compile(r"\x1b\[[0-9;]*m")
_DESYNC = re.compile(r"gprb: desync at frame (\d+)")
_RUNNING = re.compile(r"gprb: session running")
_ENDED = re.compile(r"gprb: session ended at game frame (\d+) \(([^)]*)\), (\d+) rollbacks, (\d+) desyncs")


def names_for(variant: str, one: str, every: str = "") -> str:
    """PPHARNESS_RECOMP_NAMES for a variant: `one` (the players who are recomps against Dolphins)
    or `every` (all the players: "" is every instance, a test's viewer Dolphin too)."""
    if variant == "vs_dolphin":
        return one
    if variant == "vs_recomp":
        return every
    raise ValueError(variant)


def _log(inst: DolphinInstance) -> str:
    try:
        return inst.log_path.read_text(encoding="utf-8", errors="replace")
    except OSError:
        return ""


def _stored_engines(backend: Any) -> dict[str, int]:
    """game_reports rows by engine (the reporter's)."""
    return {e: int(n) for e, n in backend.query("SELECT engine, count(*) FROM game_reports GROUP BY engine")}


def summary(made: list[DolphinInstance]) -> dict[str, Any]:
    """Per instance: engine, sessions started, sessions ended (frame, why, rollbacks, desyncs), desync
    lines."""
    from .recomp import RecompInstance
    out: dict[str, Any] = {}
    for inst in made:
        text = _log(inst)
        ended = [(int(m.group(1)), m.group(2), int(m.group(3)), int(m.group(4))) for m in _ENDED.finditer(text)]
        out[inst.name] = {
            "engine": "emissary" if isinstance(inst, RecompInstance) else "dolphin",
            "started": len(_RUNNING.findall(text)),
            "sessions": ended,
            "desync_lines": [int(m.group(1)) for m in _DESYNC.finditer(text)],
        }
    return out


def run(variant: str, one: str, every: str, monkeypatch: pytest.MonkeyPatch, backend: Any,
        dolphin: Callable[..., DolphinInstance], body: Callable[[Callable[..., DolphinInstance]], Any], *,
        desyncs_expected: bool = False, game_reports: bool = True, mm_engines: bool = True,
        sessions: bool = True) -> dict[str, Any]:
    """Runs `body` (the mode's test, given an instance factory) with the variant's recomp players,
    then checks what every crossplay run must show (module docstring). Returns the summary."""
    monkeypatch.setenv("PPHARNESS_RECOMP_NAMES", names_for(variant, one, every))
    made: list[DolphinInstance] = []
    # A module's backend serves several tests: only what this run logged counts.
    mm_from, acc_from = len(backend.log_text("mm")), len(backend.log_text("accounts"))
    stored_from = _stored_engines(backend) if game_reports else {}

    def factory(*a: Any, **kw: Any) -> DolphinInstance:
        inst = dolphin(*a, **kw)
        made.append(inst)
        return inst

    body(factory)
    s = summary(made)
    print("crossplay summary", variant, json.dumps(s), flush=True)
    engines = [v["engine"] for v in s.values()]
    assert "emissary" in engines, ("no instance was a recomp", s)
    if variant == "vs_dolphin":
        assert "dolphin" in engines, ("no Dolphin played the recomp", s)
    else:
        assert engines.count("emissary") >= 2, ("recomp against recomp needs two recomps", s)
    if not desyncs_expected:
        bad = {n: v for n, v in s.items() if v["desync_lines"] or any(e[3] for e in v["sessions"])}
        assert not bad, ("desync", bad)
    # A recomp ran at least one session (a session that never ran proves nothing; a test may end
    # with its match still running, so started ones count); not checked where no game is played.
    assert not sessions or any(v["started"] for v in s.values() if v["engine"] == "emissary"), s
    if mm_engines:
        mm = _ANSI.sub("", backend.log_text("mm")[mm_from:])
        lines = [l for l in mm.splitlines() if ("matched" in l or "room match engines" in l) and "engines" in l]
        assert any("emissary" in l for l in lines), ("mm logged no match with the recomp's engine", lines[-5:])
    if game_reports:
        acc = _ANSI.sub("", backend.log_text("accounts")[acc_from:])
        reps = [l for l in acc.splitlines() if " game reported" in l]
        assert any("engine=emissary" in l for l in reps), ("no game report named engine=emissary", reps[-5:])
        assert not any("engine=unknown" in l for l in reps), reps
        # ... and stored with it (game_reports.engine, migration 0044).
        stored = _stored_engines(backend)
        assert stored.get("emissary", 0) > stored_from.get("emissary", 0), ("no game_reports row with engine emissary",
                                                                           stored_from, stored)
        s["stored_engines"] = stored
    return s


def solo(monkeypatch: pytest.MonkeyPatch, pattern: str, dolphin: Callable[..., DolphinInstance],
         body: Callable[[Callable[..., DolphinInstance]], Any]) -> dict[str, Any]:
    """One player's test (the launcher's hand-offs to a running game) with that player a recomp."""
    monkeypatch.setenv("PPHARNESS_RECOMP_NAMES", pattern)
    made: list[DolphinInstance] = []

    def factory(*a: Any, **kw: Any) -> DolphinInstance:
        inst = dolphin(*a, **kw)
        made.append(inst)
        return inst

    body(factory)
    s = summary(made)
    assert any(v["engine"] == "emissary" for v in s.values()), ("no instance was a recomp", s)
    return s


SOLO = pytest.mark.skipif(not os.environ.get("PPHARNESS_RECOMP"), reason="PPHARNESS_RECOMP is not set")
