"""Rumble in Other Games' whole-machine sessions follows the game's motor command for the player's
port as the game's state has it, through rollbacks (bug report #331).

The SI sends a standard controller's command (0x40, the analog mode, the motor) with every poll, so
on a console the motor is whatever the channel's command register says. The game writes it only
when its rumble changes. A rumble that only a mispredicted frame started is undone by the rollback's
load (the register comes back as "stop"), and nothing sends the stop: the player's controller kept
rumbling until the game's next rumble ended (a PhobGCC on a Wii U adapter dropped off the adapter
every second or few through Other Games matches).

A machine session's sync test (no peers; every frame is loaded and run again) plays Brawl's disc.
The test starts the motor of the local port as the game does (``mmio_write``: the channel's command
with the motor on, then SISR's transfer), which the sync test's next load undoes, as a mispredicted
frame's rumble: the motor stops within a few frames (before the fix it kept running).

Instances are named ``mrb-*``.
"""

from __future__ import annotations

import json
import time
from pathlib import Path
from typing import Any, Callable

import pytest

from ppharness import paths
from ppharness.instance import DolphinInstance, InstanceConfig

pytestmark = [pytest.mark.slow, pytest.mark.dolphin]

SI_CHANNEL0_OUT = 0xCC006400
SISR = 0xCC006438
SISR_WR = 0x80000000


def _wait(what: str, fn: Callable[[], Any], timeout: float, interval: float = 0.05) -> Any:
    end = time.monotonic() + timeout
    while True:
        v = fn()
        if v:
            return v
        if time.monotonic() > end:
            raise AssertionError(f"timed out waiting for {what}")
        time.sleep(interval)


def test_machine_session_motor_follows_the_games_state_through_rollbacks(
        dolphin: Callable[..., DolphinInstance]) -> None:
    iso: Path = paths.game_iso() or paths.fallback_iso()
    session = {"slot": 0, "synctest": 2}
    cfg = InstanceConfig(cpu_thread=True, video_backend="Null", standard_controllers=(0,), other_ports="none",
                         config_args=["Dolphin.Core.WiiSDCard=False", f"Dolphin.Core.DefaultISO={iso}",
                                      "Logger.Options.WriteToFile=True", "Logger.Options.Verbosity=4",
                                      "Logger.Logs.Brawlback=True"])
    inst = dolphin("mrb-0", config=cfg, boot=str(iso), connect_timeout=120,
                   env={"PPR_MACHINE_SESSION": json.dumps(session)})
    inst.create()
    inst.launch()
    inst.connect()
    c = inst.client

    def ms() -> dict[str, Any]:
        return c.call("machine_status")

    def motor() -> dict[str, Any]:
        return c.rumble_status()[0]

    _wait("the session running", lambda: ms().get("phase") == "running", 240, 1.0)
    _wait("frame 600", lambda: (ms().get("frame") or 0) >= 600, 240, 1.0)
    # The game's own command for port 1 (a standard controller's poll command, motor stopped).
    assert not motor()["on"], motor()

    results = []
    for attempt in range(5):
        before = motor()["starts"]

        def start() -> bool:
            # The sync test re-runs two frames for each one shown, and a re-run's command never
            # reaches the controller: again until the store falls in a shown frame.
            c.call("mmio_write", addr=SI_CHANNEL0_OUT, value=0x00400301)
            c.call("mmio_write", addr=SISR, value=SISR_WR)
            time.sleep(0.05)
            return motor()["starts"] > before

        # The motor started (as the game's command did, outside a re-run) ...
        _wait("the motor started", start, 10, 0.0)
        # ... and the next load put the game's state back (the register's "stop"): it stops.
        stopped_in = time.monotonic()
        _wait("the motor stopped after the load", lambda: not motor()["on"], 5)
        results.append(time.monotonic() - stopped_in)
    st = ms()
    print(f"motor stopped {['%.3f' % r for r in results]} s after starting; rollbacks {st.get('rollbacks')}, "
          f"frame {st.get('frame')}", flush=True)
    assert st.get("phase") == "running" and (st.get("rollbacks") or 0) > 100, st
