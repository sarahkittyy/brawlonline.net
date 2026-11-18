"""The build's profile and the plugin's contract (docs/design/pplus-mods.md section 0; the game
code's build.h): the plugin says what it found of P+'s codes in the running game, once, in PPOM's
BUILD; Dolphin logs it; and a build that meets the contract has its online modes open. Runs on
P+ and on every build of its lineage (harness/tools/mod_suite.py): each must meet the contract.
"""

from __future__ import annotations

from typing import Callable

import pytest

from ppharness import paths as ppharness_paths
from ppharness.backend import OnlineBackend
from ppharness.instance import DolphinInstance

from test_online import _wait
from test_online_game import _boot, backend  # noqa: F401
import mod_adapt  # noqa: E402
import ppom  # noqa: E402

pytestmark = [pytest.mark.dolphin, pytest.mark.server, pytest.mark.gpu]


def test_the_build_meets_the_plugins_contract(backend: OnlineBackend, dolphin: Callable[..., DolphinInstance],
                                              gpu_backend: str) -> None:
    user = backend.create_user("prof", "PROF")
    g = _boot(dolphin, "profile", backend, user, gpu_backend, "profile", "gameplay")
    g.to_main_menu()
    block = ppom.find_block(g.c)
    prof = _wait(lambda: (ppom.read_build(g.c, block) or {}).get("profile"), 30, "the build's profile")
    assert prof["missing"] == [], prof
    # The plugin on the card is the one built for this kind of build (the games table's `plugin`).
    assert prof["plugin"] == ppharness_paths.plugin_variant(), prof
    assert prof["slot_table"] and prof["card_folder"] and prof["stage_design"] in ("pplus", "pplus-old"), prof
    if not mod_adapt.active():
        # Project+ v3.2 itself: its own stage select, alt costumes, Code Menu and alternate stages.
        assert prof == {"stage_design": "pplus", "alt_costumes": "pplus", "code_menu": True,
                        "stage_alternates": True, "card_folder": True, "slot_table": True,
                        "plugin": "pplus", "missing": []}, prof
    g.inst.wait_for_log(r"GameBridge: the game's build: stage select P\+.*; it meets the plugin's contract",
                        timeout=15)
    # Its online modes are open: the ONLINE page's WITH FRIENDS opens its page (a build that does
    # not meet the contract only gets the buzzer there: OnlineMenu::modesLocked).
    refused = g.debug_scratch()[7] >> 8
    g.to_online_page()
    g.to_css("direct")
    assert g.scene() == "scSelctCharacter"
    assert g.debug_scratch()[7] >> 8 == refused, "an A on the ONLINE page was refused"


def test_another_kinds_plugin_locks_the_online_modes(backend: OnlineBackend, dolphin: Callable[..., DolphinInstance],
                                                     gpu_backend: str, monkeypatch: pytest.MonkeyPatch) -> None:
    """A mod's build with Project+'s own plugin on its card (the plugin is built once per kind of
    build: game-code/build.sh): the plugin says the build needs another, Dolphin's log names the
    files, and the ONLINE page's modes only buzz. On Project+ itself every variant has what it
    needs (each is a superset of P+'s own), so this runs on the mods."""
    if ppharness_paths.plugin_variant() == "pplus":
        pytest.skip("Project+: every variant of the plugin plays it")
    import test_online_game
    monkeypatch.setattr(test_online_game, "PLUGIN", ppharness_paths.plugin_path("pplus"))
    user = backend.create_user("wrong", "WRNG")
    g = _boot(dolphin, "wrongplugin", backend, user, gpu_backend, "wrongplugin", "gameplay")
    g.c.wait_state("running", timeout=120)
    _wait(lambda: g.scene() in ("muMenuMain", "scSelctCharacter", "scTitle"), 180, "the first screen")
    block = ppom.find_block(g.c)
    prof = _wait(lambda: (ppom.read_build(g.c, block) or {}).get("profile"), 30, "the build's profile")
    assert prof["plugin"] == "pplus" and "plugin" in prof["missing"], prof
    g.inst.wait_for_log(r"GameBridge: the game's build: .*online modes locked, missing: .*the plugin built for",
                        timeout=15)
    if ppharness_paths.plugin_variant() != "pex":
        return   # (REX boots its own way to its title without its plugin's boot path)
    # A build of P+ v3.2's kind still boots to the ONLINE page: WITH FRIENDS only buzzes there.
    g.to_main_menu()
    refused = g.debug_scratch()[7] >> 8
    g.to_online_page()
    g.steps("tap A", "wait 90")
    assert g.scene() == "muMenuMain"
    assert g.debug_scratch()[7] >> 8 > refused, "the A on WITH FRIENDS was not refused"
