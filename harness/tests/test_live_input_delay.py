"""The launcher's Input Delay, changed while the game runs (the user's, 2026-10-20: "just being able
to use the launcher option while dolphin is open", everywhere: Play's modes and Other Games rooms).

The launcher writes ``input-delay.json`` (``{version, id, inputDelay, createdAt}``) into Dolphin's
online folder; the rooms thread takes it within half a second, keeps the setting and hands it to the
running session, which changes this player's delay before its next frame's local input
(Gprb::Session::SetInputDelay, Machine::SetInputDelay). GekkoNet repeats the player's last input for
the frames a higher delay adds and drops its next inputs for the frames a lower one takes away;
every machine gets the same inputs, so the games stay in step. ``game-status.json`` says
``inputDelay: {setting, current, request}``.

- A Direct match (the P+ gameplay session): one player goes 1 -> 4 -> 2 -> auto -> 3 while both
  fight; the other's delay stays; every confirmed frame's checksum agrees on both machines.
- A two-player whole-machine session (Other Games' session, Brawl's disc, random pads, a 60 ms
  round trip through netsim, so that rollbacks run across the changes): the same changes on one
  machine; no desync, and both machines' confirmed frames (inputs and checksums) agree.

Instances are named ``lid-*``. The Direct test needs what ``test_online_game.py`` needs.
"""

from __future__ import annotations

import json
import os
import random
import socket
import threading
import time
import uuid
from pathlib import Path
from typing import Any, Callable

import pytest

from ppharness import flows as F
from ppharness import paths
from ppharness.instance import DolphinInstance, InstanceConfig
from ppharness.netsim import LinkProfile, NetProfile, NetSim

from test_online_game import Game, _confirmed_checksums, _connected_direct, backend, online_set  # noqa: F401

# Each change, and the delay it gives at loopback round trips (0: automatic, 1 frame there).
CHANGES = [(4, 4), (2, 2), (0, 1), (3, 3)]


def request_delay(user_dir: Path, delay: int) -> str:
    """What the launcher writes (launcher/src/dolphin/input_delay_handoff.ts), atomically."""
    online = Path(user_dir) / "Online"
    online.mkdir(parents=True, exist_ok=True)
    rid = str(uuid.uuid4())
    req = {"version": 1, "id": rid, "inputDelay": delay,
           "createdAt": time.strftime("%Y-%m-%dT%H:%M:%S.000Z", time.gmtime())}
    tmp = online / f"input-delay.json.{rid}.tmp"
    tmp.write_text(json.dumps(req))
    os.replace(tmp, online / "input-delay.json")
    return rid


def status_delay(user_dir: Path) -> dict[str, Any] | None:
    try:
        return json.loads((Path(user_dir) / "Online" / "game-status.json").read_text()).get("inputDelay")
    except (OSError, ValueError):
        return None


def wait(what: str, fn: Callable[[], Any], timeout: float, interval: float = 0.1) -> Any:
    end = time.monotonic() + timeout
    while True:
        v = fn()
        if v:
            return v
        if time.monotonic() > end:
            raise AssertionError(f"timed out waiting for {what}")
        time.sleep(interval)


def change_and_check(user_dir: Path, current: Callable[[], int], delay: int, expect: int) -> None:
    """One change: the running game plays with it, and game-status.json says so."""
    rid = request_delay(user_dir, delay)
    wait(f"delay {expect} in the game", lambda: current() == expect, 5)
    st = wait(f"game-status.json for request {rid}",
              lambda: (lambda d: d if d and d.get("request") == rid and d.get("current") == expect else None)(
                  status_delay(user_dir)), 6)
    assert st["setting"] == delay, st
    assert not (Path(user_dir) / "Online" / "input-delay.json").exists()
    print(f"delay {delay} -> playing at {expect}", flush=True)


@pytest.mark.slow
@pytest.mark.dolphin
@pytest.mark.server
@pytest.mark.gpu
def test_direct_match_takes_a_new_delay_mid_game(backend, dolphin: Callable[..., DolphinInstance],  # noqa: F811
                                                 gpu_backend: str) -> None:
    a, b, _, _ = _connected_direct(backend, dolphin, gpu_backend, "live-delay", ("lida", "lidb"), stocks=4,
                                   inst_names=("lid-a", "lid-b"))
    players = [a, b]
    online_set.wait(lambda: all(online_set.gstatus(g.c)["phase"] == "running" for g in players), 240,
                    "the match running on both")
    start_a = online_set.gstatus(a.c)["input_delay"]
    start_b = online_set.gstatus(b.c)["input_delay"]
    assert start_a == 1 and start_b == 1, (start_a, start_b)   # automatic at loopback round trips

    stop = threading.Event()

    def fight(g: Game) -> None:
        port = online_set.local(g.c)["local_port"]
        F.fight([F.Seat(g.c, port, 0, g.name)], 6000, mode="random", seed=f"live-delay-{g.name}",
                stop=stop.is_set)

    th = [threading.Thread(target=fight, args=(g,)) for g in players]
    for t in th:
        t.start()
    try:
        b_delays = set()
        for delay, expect in CHANGES:
            time.sleep(4)
            assert online_set.gstatus(a.c)["phase"] == "running"
            change_and_check(a.inst.user_dir, lambda: online_set.gstatus(a.c)["input_delay"], delay, expect)
            b_delays.add(online_set.gstatus(b.c)["input_delay"])
        time.sleep(4)
    finally:
        stop.set()
        for t in th:
            t.join(timeout=120)
    assert b_delays == {start_b}, ("the other player's delay changed", b_delays)
    cmp = _confirmed_checksums(a, b)
    print(f"checksums: {cmp}", flush=True)
    assert cmp["compared"] > 1000 and cmp["mismatches"] == 0, cmp
    log = (a.inst.user_dir / "Logs" / "dolphin.log").read_text(errors="replace")
    assert "gprb: input delay 1 -> 4" in log and "gprb: input delay 1 -> 3" in log
    for g in players:
        assert online_set.gstatus(g.c)["phase"] == "running"
        g.c.mm_cancel()


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
    s, t = [], 0
    while t < frames:
        hold = rng.randint(2, 8)
        k = rng.random()
        if k < 0.4:
            s.append({"main": [rng.randint(0, 255), rng.randint(0, 255)], "hold": hold})
        elif k < 0.8:
            s.append({"buttons": [rng.choice(["A", "A", "B", "DUP", "DDOWN", "DLEFT", "DRIGHT"])], "hold": hold})
        else:
            s.append({"hold": hold})
        t += hold
    return s


@pytest.mark.slow
@pytest.mark.dolphin
def test_machine_session_takes_a_new_delay_mid_game(dolphin: Callable[..., DolphinInstance]) -> None:
    iso = paths.game_iso() or paths.fallback_iso()
    p0, p1 = _free_port(socket.SOCK_DGRAM), _free_port(socket.SOCK_DGRAM)
    link = LinkProfile(latency_ms=30, jitter_ms=1.0)
    sims = [NetSim(("127.0.0.1", p1), ("127.0.0.1", 0), NetProfile(up=link, down=link)).start(),
            NetSim(("127.0.0.1", p0), ("127.0.0.1", 0), NetProfile(up=link, down=link)).start()]
    t1, t0 = sims[0].listen_addr[1], sims[1].listen_addr[1]
    sessions = {"lid-m0": {"slot": 0, "port": p0, "peers": [{"slot": 1, "host": "127.0.0.1", "port": t1}]},
                "lid-m1": {"slot": 1, "port": p1, "peers": [{"slot": 0, "host": "127.0.0.1", "port": t0}]}}
    try:
        _machine_session(dolphin, iso, sessions)
    finally:
        for sim in sims:
            sim.stop()


def _machine_session(dolphin: Callable[..., DolphinInstance], iso: Path, sessions: dict[str, Any]) -> None:
    insts: dict[str, DolphinInstance] = {}
    for name, session in sessions.items():
        cfg = InstanceConfig(cpu_thread=True, video_backend="Null", standard_controllers=(0,), other_ports="none",
                             config_args=["Dolphin.Core.WiiSDCard=False", f"Dolphin.Core.DefaultISO={iso}",
                                          "Logger.Options.WriteToFile=True", "Logger.Options.Verbosity=4",
                                          "Logger.Logs.Brawlback=True"])
        inst = dolphin(name, config=cfg, boot=str(iso), connect_timeout=120,
                       env={"PPR_MACHINE_SESSION": json.dumps(session)})
        inst.create()
        inst.launch()
        inst.connect()
        insts[name] = inst
    m0, m1 = insts["lid-m0"], insts["lid-m1"]

    def ms(inst: DolphinInstance) -> dict[str, Any]:
        return inst.client.call("machine_status")

    wait("the session running", lambda: all(ms(i).get("phase") == "running" for i in insts.values()), 240, 1)
    # Past Brawl's boot (buttons at its boot message can hold the game there for good).
    wait("frame 2400", lambda: all((ms(i).get("frame") or 0) >= 2400 for i in insts.values()), 240, 1)
    start = {n: ms(i)["input_delay"] for n, i in insts.items()}
    assert start == {"lid-m0": 2, "lid-m1": 2}, start   # the session's default without "delay"
    # Automatic at this round trip (60 ms, 77 ms in game) is 1 frame, as at loopback.

    history: dict[str, dict[str, list]] = {n: {} for n in insts}
    rng = random.Random(7)

    def play(seconds: float) -> None:
        end = time.monotonic() + seconds
        while time.monotonic() < end:
            for n, i in insts.items():
                i.client.pad_script(0, _random_script(rng, 90))
                history[n].update(i.client.call("machine_history", since=0)["frames"])
            time.sleep(1)

    play(4)
    for delay, expect in CHANGES:
        change_and_check(m0.user_dir, lambda: ms(m0)["input_delay"], delay, expect)
        assert ms(m1)["input_delay"] == start["lid-m1"]
        play(4)
    for n, i in insts.items():
        history[n].update(i.client.call("machine_history", since=0)["frames"])
    common = sorted(set(history["lid-m0"]) & set(history["lid-m1"]), key=int)
    bad = [f for f in common if history["lid-m0"][f] != history["lid-m1"][f]]
    st0, st1 = ms(m0), ms(m1)
    print(f"compared {len(common)} confirmed frames, {len(bad)} differ; desyncs {st0.get('desyncs')} / "
          f"{st1.get('desyncs')}; rollbacks {st0.get('rollbacks')} / {st1.get('rollbacks')}", flush=True)
    assert len(common) > 1000, len(common)
    assert st0.get("rollbacks", 0) > 20, "no rollbacks to cross the changes"
    assert not bad, (bad[:5], [history[n][bad[0]] for n in history])
    assert st0.get("desyncs") == 0 and st1.get("desyncs") == 0, (st0.get("desyncs"), st1.get("desyncs"))
    assert st0.get("phase") == "running" and st1.get("phase") == "running"
    log = (m0.user_dir / "Logs" / "dolphin.log").read_text(errors="replace")
    assert "input delay 2 -> 4 (set)" in log and "input delay 2 -> 1 (auto" in log, "the changes not logged"
