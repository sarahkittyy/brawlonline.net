"""Zelda/Sheik transformations under rollback, then the scene change after the game (player
report, prod 2026-10-13, Ranked set game 1, Mewtwo vs Sheik at ~85 ms with rollbacks on 17% of
the frames). At the match's end the game printed "gfHeapManager>> Heap(MenuInstance) has allocated
block (7)!!" with no block listed and the heap all free ("Memory Block Num: 0"), then stayed in
scMemoryChange: no CSS, the opponent left.

sora_melee keeps about 60 blocks in MenuInstance during a match and allocates and frees groups of
7. The heap, with its gfMemoryPool header and block count at its start, was outside the rollback
set (gp-v22) while the blocks' owners are rolled back, so rollbacks left the count off by
multiples of 7, and the scene change waits for it to be 0. gp-v23 adds the heap. With gp-v22 this
test hung on the Zelda side in 3 runs of 3 (counts 65522, 65459 and 7 at the scene change); with
gp-v23 the count stayed at 60/53 and both reached the CSS.

One side does Zelda's down-B every 90 frames while the other walks, under a netsim preset; the
pause screen's quit then ends game 1, and both must reach the CSS without that message. The test
also logs MenuInstance's block count during the match. PPHARNESS_REGION_SET tries another set.
"""
from __future__ import annotations

import os
import time
from typing import Any, Callable

import pytest

from ppharness.backend import OnlineBackend
from ppharness.client import PadInput
from ppharness.instance import DolphinInstance
from ppharness import brawl as B

from test_online_game import _connected_direct, _p2p_through_netsim, backend  # noqa: E402,F401
import online_set  # noqa: E402

pytestmark = [pytest.mark.dolphin, pytest.mark.server, pytest.mark.gpu]

LEAK = "has allocated block"
CHAR_A = os.environ.get("PPHARNESS_TRANSFORM_CHAR", "zelda")


def _log(g) -> str:
    return (g.inst.user_dir / "Logs" / "dolphin.log").read_text(errors="replace")


def _pick(name: str) -> Callable[[Any], None]:
    def pick(g: Any) -> None:
        B.css_pick_character(g.c, 0, B.CSS_ID[name])
        B.step(g.c, 10)
    return pick


def _region_set(g: Any) -> None:
    """PPHARNESS_REGION_SET: the online session's region set (a name or a path), for trying a new
    set on a build whose default is older."""
    name = os.environ.get("PPHARNESS_REGION_SET")
    if name:
        # Only the harness's search commands take the gameplay backend's options (StartSearch);
        # they stay for the game's own search. A search for nobody, cancelled at once.
        g.c.mm_search_direct("NONE#1", backend="gameplay", region_set=name)
        g.c.mm_cancel()
        print(g.name, "region set", name)


def _menu_pool(g) -> tuple[int, int, int] | None:
    """MenuInstance's gfMemoryPool header (at the heap's start): +0x14 the allocated block count
    (u16, +0x16 its peak), +0x1C the bytes in use (+0x18 their peak)."""
    heaps = {h.name: h for h in B.read_heap_table(g.c.read_mem)}
    h = heaps.get("MenuInstance")
    if h is None:
        return None
    hdr = g.c.read_mem(h.start, 0x20)
    return h.start, int.from_bytes(hdr[0x14:0x16], "big"), int.from_bytes(hdr[0x1C:0x20], "big")


def _transform_loop(g, n: int) -> None:
    down_b = [PadInput(main=(128, 20), buttons=["B"], hold=4), PadInput(hold=86)]
    g.c.pad_script(0, down_b * n)


def _walk(g, n: int) -> None:
    g.c.pad_script(0, [PadInput(main=(60, 128), hold=20), PadInput(main=(196, 128), hold=20)] * n)


@pytest.mark.slow
@pytest.mark.parametrize("preset", ["bad_wifi", "lan"])
def test_transformations_then_scene_change(backend: OnlineBackend, dolphin: Callable[..., DolphinInstance],
                                           gpu_backend: str, preset: str) -> None:
    sims, inis = _p2p_through_netsim(preset) if preset != "lan" else ([], (None, None))
    try:
        a, b, _, _ = _connected_direct(backend, dolphin, gpu_backend, f"transform-scene-{preset}",
                                       ("zora", "wade"), stocks=4, dolphin_ini=inis,
                                       on_css=(_pick(CHAR_A), _pick("mario")),
                                       before_online=(_region_set, _region_set))
        online_set.wait(lambda: all(online_set.gstatus(g.c)["phase"] == "running" for g in (a, b)), 240,
                        "game 1 to run on both")
        _transform_loop(a, 90)
        _walk(b, 200)
        seen: dict[str, list[tuple[int, int]]] = {a.name: [], b.name: []}
        changes = 0
        t_end = time.monotonic() + float(os.environ.get("PPHARNESS_TRANSFORM_SECONDS", "90"))
        while time.monotonic() < t_end:
            if any(online_set.gstatus(g.c)["phase"] != "running" for g in (a, b)):
                break
            for g in (a, b):
                p = _menu_pool(g)
                if p and (not seen[g.name] or seen[g.name][-1] != p[1:]):
                    seen[g.name].append(p[1:])
            time.sleep(1)
        for g in (a, b):
            print(g.name, "MenuInstance blocks/bytes", " ".join(f"{n}/{u:#x}" for n, u in seen[g.name]))
        for g in (a, b):
            log = _log(g)
            changes = max(changes, log.count("#fighter change begin"))
            st = online_set.gstatus(g.c)
            print(g.name, st["phase"], st.get("end_reason"), "rollbacks", st.get("rollbacks"),
                  "desyncs", st.get("desyncs_detected"), "fighter changes", log.count("#fighter change begin"))
            assert not st.get("desyncs_detected"), (g.name, st.get("desyncs_detected"))
        assert changes > 0, "no transformation happened"
        # The pause screen's quit ends game 1 (scMelee -> scMemoryChange, where the heap check
        # runs); both must reach the CSS (Direct goes back to it).
        for g in (a, b):
            g.c.pad_script(0, [PadInput(hold=2)])
        b.steps("wait 30")
        a.steps("tap START 8", "wait 60", "hold L+R+A+START 90")
        try:
            online_set.wait(lambda: all(online_set.scene(g.c) == online_set.CSS for g in (a, b)), 120,
                            "both back on the CSS")
        finally:
            for g in (a, b):
                g.shot("after-game-1")
                lines = [ln for ln in _log(g).splitlines() if LEAK in ln or "Alloc Couner" in ln
                         or "Memory Block Num" in ln]
                print(g.name, "scene", online_set.scene(g.c), lines)
        for g in (a, b):
            assert LEAK not in _log(g), g.name
    finally:
        for sim in sims:
            sim.stop()
