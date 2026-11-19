"""Input suggestions in Other Games' whole-machine sessions (Gprb::Suggest with Machine::Session's
input): the local GameCube controller's changes between two reads go to the other players at once,
and their predictions use them.

Two Dolphins play a whole-machine session (Brawl's disc) through netsim at a 40 ms round trip, with
random pads whose scripts step with the host clock from a random point between two reads
(pad_script_timing "time"), as a person's presses fall anywhere between the game's reads.

- Both on: suggestions go both ways, some predictions are right only by one, no player is judged
  untrustworthy, no desync, and both machines' confirmed frames (inputs and checksums) agree.
- One off (PPR_GPRB_SUGGEST=0, as a build without suggestions): its hello does not say it takes them,
  so the other sends it none, and neither uses any.

Instances are named ``msg-*``.
"""

from __future__ import annotations

import json
import random
import socket
import time
from pathlib import Path
from typing import Any, Callable

import pytest

from ppharness import paths
from ppharness.instance import DolphinInstance, InstanceConfig
from ppharness.netsim import LinkProfile, NetProfile, NetSim

pytestmark = [pytest.mark.slow, pytest.mark.dolphin]


def _free_port(kind: int) -> int:
    for _ in range(200):
        port = random.randint(31000, 38999)
        try:
            with socket.socket(socket.AF_INET, kind) as s:
                s.bind(("127.0.0.1", port))
            return port
        except OSError:
            continue
    raise RuntimeError("no free port")


def _random_script(rng: random.Random, frames: int) -> list:
    # The D-pad and the stick only: A, B or Start can skip Brawl's opening movie, after which a
    # whole-machine session's game can stop for good (its frames stop at the same frame on both
    # machines, the build before suggestions too).
    s, t = [], 0
    while t < frames:
        hold = rng.randint(2, 8)
        k = rng.random()
        if k < 0.5:
            s.append({"main": [rng.randint(0, 255), rng.randint(0, 255)], "hold": hold})
        elif k < 0.85:
            s.append({"buttons": [rng.choice(["DUP", "DDOWN", "DLEFT", "DRIGHT"])], "hold": hold})
        else:
            s.append({"hold": hold})
        t += hold
    return s


def _wait(what: str, fn: Callable[[], Any], timeout: float, interval: float = 1.0) -> Any:
    end = time.monotonic() + timeout
    while True:
        v = fn()
        if v:
            return v
        if time.monotonic() > end:
            raise AssertionError(f"timed out waiting for {what}")
        time.sleep(interval)


def _play(dolphin: Callable[..., DolphinInstance], name: str, env_m1: dict[str, str]) -> list[dict[str, Any]]:
    iso: Path = paths.game_iso() or paths.fallback_iso()
    p0, p1 = _free_port(socket.SOCK_DGRAM), _free_port(socket.SOCK_DGRAM)
    link = LinkProfile(latency_ms=20, jitter_ms=1.0)
    sims = [NetSim(("127.0.0.1", p1), ("127.0.0.1", 0), NetProfile(up=link, down=link)).start(),
            NetSim(("127.0.0.1", p0), ("127.0.0.1", 0), NetProfile(up=link, down=link)).start()]
    t1, t0 = sims[0].listen_addr[1], sims[1].listen_addr[1]
    sessions = {f"msg-{name}-0": ({"slot": 0, "port": p0, "peers": [{"slot": 1, "host": "127.0.0.1", "port": t1}],
                                   "delay": 1}, {}),
                f"msg-{name}-1": ({"slot": 1, "port": p1, "peers": [{"slot": 0, "host": "127.0.0.1", "port": t0}],
                                   "delay": 1}, env_m1)}
    try:
        insts: list[DolphinInstance] = []
        for inst_name, (session, env) in sessions.items():
            cfg = InstanceConfig(cpu_thread=True, video_backend="Null", standard_controllers=(0,), other_ports="none",
                                 config_args=["Dolphin.Core.WiiSDCard=False", f"Dolphin.Core.DefaultISO={iso}",
                                              "Logger.Options.WriteToFile=True", "Logger.Options.Verbosity=4",
                                              "Logger.Logs.Brawlback=True"])
            inst = dolphin(inst_name, config=cfg, boot=str(iso), connect_timeout=120,
                           env={"PPR_MACHINE_SESSION": json.dumps(session), **env})
            inst.create()
            inst.launch()
            inst.connect()
            insts.append(inst)

        def ms(inst: DolphinInstance) -> dict[str, Any]:
            return inst.client.call("machine_status")

        _wait("the session running", lambda: all(ms(i).get("phase") == "running" for i in insts), 240)
        # Past Brawl's boot (buttons at its boot message can hold the game there for good).
        _wait("frame 2400", lambda: all((ms(i).get("frame") or 0) >= 2400 for i in insts), 240)
        for k, i in enumerate(insts):
            i.client.call("pad_script_timing", mode="time", seed=35 + k)
        rng = random.Random(name)
        history: list[dict[str, Any]] = [{} for _ in insts]
        end = time.monotonic() + 30
        while time.monotonic() < end:
            for k, i in enumerate(insts):
                # Short scripts, each from its own random point between two reads.
                i.client.pad_script(0, _random_script(rng, 150))
                history[k].update(i.client.call("machine_history", since=0)["frames"])
            time.sleep(2)
        for k, i in enumerate(insts):
            history[k].update(i.client.call("machine_history", since=0)["frames"])
        st = [ms(i) for i in insts]
        common = sorted(set(history[0]) & set(history[1]), key=int)
        bad = [f for f in common if history[0][f] != history[1][f]]
        print([s.get("suggest") for s in st], f"rollbacks {[s.get('rollbacks') for s in st]}, "
              f"compared {len(common)} confirmed frames, {len(bad)} differ", flush=True)
        assert len(common) > 1000, len(common)
        assert not bad, (bad[:5], [h[bad[0]] for h in history])
        for s in st:
            assert s.get("desyncs") == 0 and s.get("phase") == "running", s
            # The game kept running and took the pads (a stopped game takes no new inputs).
            assert s["suggest"]["local_changes"] > 150, s["suggest"]
        return [s["suggest"] for s in st]
    finally:
        for sim in sims:
            sim.stop()


def test_machine_session_suggestions_go_both_ways_and_stay_in_sync(dolphin: Callable[..., DolphinInstance]) -> None:
    sug = _play(dolphin, "on", {})
    for s in sug:
        assert s["enabled"]
        assert s["sent"] > 20 and s["received"] > 20, sug
        assert s["ignored"] == 0, sug
    # Right predictions that only a suggestion made right, on at least one side.
    assert sum(s["helped"] for s in sug) > 0, sug


def test_machine_session_player_without_suggestions_gets_none(dolphin: Callable[..., DolphinInstance]) -> None:
    sug = _play(dolphin, "mixed", {"PPR_GPRB_SUGGEST": "0"})
    assert sug[0]["enabled"] and not sug[1]["enabled"], sug
    assert sug[0]["sent"] == 0 and sug[1]["sent"] == 0, sug
    assert sug[0]["received"] == 0 and sug[1]["received"] == 0, sug
