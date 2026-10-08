"""A hostile peer against the gameplay session (Gprb::Session) of a real Dolphin.

One Dolphin joins a session whose "host" is this test: a plain UDP socket that speaks the
session's packet format ('C' + JSON control messages, 'P'/'Q' pings, 'G' GekkoNet packets) and
sends what a malicious matchmaking opponent could. Dolphin must stay up and must not take any of
it into the lobby (and from there into SESSION and the game's memory):

* control messages that used to crash Dolphin: 60,000 nested arrays (picojson recursed until the
  network thread's stack overflowed), hex that std::stoul threw on, array elements of the wrong
  type (picojson's get<T> aborts), numbers out of range;
* lock-ins and match setups with characters, costumes and stages the game cannot produce (it
  indexes its tables with them);
* a "leave" from another port (anyone who can guess the port);
* a pong from the future (the host waits RTT/2 before it starts) and a flood of GekkoNet packets
  on the character select (queued until a match polls them).

A well-formed setup is taken afterwards (the positive control).

Run: ``pytest -m dolphin tests/test_peer_hostile.py`` (about a minute, one Dolphin, Null video).
"""

from __future__ import annotations

import json
import os
import socket
import struct
import time
from typing import Any, Callable

import pytest

from ppharness.instance import DolphinInstance, InstanceConfig

pytestmark = pytest.mark.dolphin

PV_HEX = "00" * 0x3C  # port values: no name tag (the game's default controls)


def _wait(pred: Callable[[], Any], timeout: float, what: str, interval: float = 0.1) -> Any:
    deadline = time.monotonic() + timeout
    while True:
        v = pred()
        if v:
            return v
        if time.monotonic() > deadline:
            raise AssertionError(f"timed out after {timeout:.0f} s waiting for {what}")
        time.sleep(interval)


class FakeHost:
    """The test's side of the session: a UDP socket Dolphin (the joiner) talks to."""

    def __init__(self) -> None:
        self.sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.sock.bind(("127.0.0.1", 0))
        self.sock.settimeout(0.2)
        self.port = self.sock.getsockname()[1]
        self.peer: tuple[str, int] | None = None

    def learn_peer(self, timeout: float = 20.0) -> tuple[str, int]:
        deadline = time.monotonic() + timeout
        while self.peer is None:
            if time.monotonic() > deadline:
                raise AssertionError("Dolphin never sent the fake host a packet")
            try:
                _, addr = self.sock.recvfrom(65536)
                self.peer = addr
            except socket.timeout:
                pass
        return self.peer

    def drain(self) -> None:
        try:
            while True:
                self.sock.recvfrom(65536)
        except (socket.timeout, BlockingIOError, ConnectionResetError):
            pass

    def raw(self, data: bytes) -> None:
        assert self.peer is not None
        self.sock.sendto(data, self.peer)

    def control(self, msg: dict[str, Any] | str) -> None:
        text = msg if isinstance(msg, str) else json.dumps(msg)
        self.raw(b"C" + text.encode())

    def close(self) -> None:
        self.sock.close()


def _state(**kw: Any) -> dict[str, Any]:
    msg = {"t": "st", "v": 1, "host": True, "name": "evil", "game": "RSBE01", "sel": {},
           "match": 0, "seed": 1234, "at_s": False, "applied": False, "at_start": False,
           "go": False, "setup": "0000000000000000", "winner": 255}
    msg.update(kw)
    return msg


def _lock(**kw: Any) -> dict[str, Any]:
    lock = {"ready": True, "css": 7, "kind": 7, "costume": 0, "stage": 0xFFFF, "asl": 0,
            "game": 1, "pv": PV_HEX}
    lock.update(kw)
    return lock


@pytest.fixture
def joiner(dolphin: Callable[..., DolphinInstance]) -> DolphinInstance:
    inst = dolphin("peer-hostile", config=InstanceConfig(video_backend="Null"), connect_timeout=60)
    inst.start()
    inst.client.wait_state("running", timeout=60)
    return inst


def test_hostile_peer_cannot_crash_or_write_the_lobby(joiner: DolphinInstance) -> None:
    c = joiner.client
    host = FakeHost()
    spoofer = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    spoofer.bind(("127.0.0.1", 0))
    try:
        c.call("gprb_connect", role="join", host="127.0.0.1", remote_port=host.port, name="victim")
        host.learn_peer()

        def status() -> dict[str, Any]:
            # "busy": the session's lock was held for over a second (the CPU thread's loop-top
            # work); ask again.
            return _wait(lambda: (lambda s: None if s.get("busy") else s)(
                c.call("gprb_status", timeout=10)), 30, "gprb_status without busy")

        def alive(what: str) -> dict[str, Any]:
            st = status()
            assert st["phase"] not in ("ended", "error", "idle"), f"{what}: session {st['phase']}"
            return st

        # The peer is heard (a well-formed hello first).
        host.control(_state())
        _wait(lambda: status()["peer"]["seen"], 10, "the joiner to hear the fake host")

        # 1. Messages that crashed Dolphin before.
        host.control("[" * 60000)
        host.control("[" * 15000)  # under the size limit: the depth check refuses it
        host.control('{"a":' * 5000 + "1" + "}" * 5000)
        host.control(_state(sync={"game_frame": "zz" * 0x18, "app": 1, "serial": 2,
                                  "rng": [1, 2, 3]}))
        host.control(_state(sync={"game_frame": "00" * 0x18, "app": 1, "serial": 2,
                                  "rng": ["x", {}, None], "start_pos": [[], "a", 1, 2]}))
        host.control(_state(sync={"game_frame": "00" * 0x18, "app": 1e300, "serial": -5,
                                  "rng": [1, 2, 3]}))
        host.control(_state(init="zz" * 0x20, init_match=0))
        host.control(_state(init="00" * 0x200, init_match=-1))
        host.control(_state(setup="not hex at all", match=1e300, winner=-1, seed="x"))
        host.control(_state(name="N" * 20000))
        host.control(_state(tasks=[["t"] * 2000] * 3))
        host.control(_state(tasks=[[{"a": 1}, 7, None]]))
        host.control(_state(lock={"ready": True, "kind": "fox", "costume": [], "pv": "zz" * 0x3C}))
        time.sleep(0.5)
        st = alive("after the crash cases")
        assert len(st["peer"]["name"]) <= 64

        # 2. Lock-ins with things the game cannot produce are no lock-ins.
        for bad in (_lock(kind=0x99), _lock(kind=0x3E), _lock(costume=0x40), _lock(kind=1.5),
                    _lock(kind=-1)):
            host.control(_state(lock=bad))
            time.sleep(0.15)
            pl = alive("bad lock")["lobby"]["peer_lock"]
            assert not pl["ready"] and pl["kind"] == 0xFF, f"lock {bad} was taken: {pl}"
        host.control(_state(lock=_lock(stage=0x26)))
        time.sleep(0.15)
        assert alive("bad stage pick")["lobby"]["peer_lock"]["stage"] == 0xFFFF

        # 3. Match setups the joiner must not take (it would write them into SESSION).
        def setup(stage: int, players: list[list[Any]]) -> dict[str, Any]:
            return {"game": 1, "stage": stage, "asl": 0, "players": players}

        good_players = [[0x07, 0, PV_HEX], [0x15, 1, PV_HEX]]
        for bad in (setup(0x26, good_players), setup(0xFFFF, good_players), setup(0x38, good_players),
                    setup(0x80, good_players),
                    setup(0x01, [[0x99, 0, PV_HEX], [0x15, 1, PV_HEX]]),
                    setup(0x01, [[0x07, 0x80, PV_HEX], [0x15, 1, PV_HEX]]),
                    setup(0x01, [[0x07, 0, PV_HEX]]),
                    setup(0x01, [[0x07, 0, PV_HEX]] * 5),
                    setup(0x01, [["x", 0, PV_HEX], [0x15, 1, PV_HEX]])):
            host.control(_state(match_setup=bad))
            time.sleep(0.15)
            lobby = alive("bad setup")["lobby"]
            assert lobby["setup"]["game"] == 0, f"setup {bad} was taken: {lobby['setup']}"

        # 4. Anyone else on the network: a "leave" from another port is ignored.
        spoofer.sendto(b'C{"t":"leave"}', host.peer)
        spoofer.sendto(b"C" + json.dumps(_state(match_setup=setup(0x01, good_players))).encode(),
                       host.peer)
        time.sleep(0.5)
        st = alive("after the spoofed leave")
        assert not st["disconnected"]
        assert st["lobby"]["setup"]["game"] == 0, "a setup from a spoofed address was taken"
        assert st["foreign_packets"] >= 2

        # 5. A pong from the future, and GekkoNet packets nobody polls on the CSS.
        future_us = int(time.time() * 1e6) + 10**15
        host.raw(b"Q" + struct.pack("<Q", future_us & (2**64 - 1)))
        for _ in range(3000):
            host.raw(b"G" + os.urandom(64))
        host.raw(b"P" + b"x" * 100)
        time.sleep(0.5)
        st = alive("after the flood")
        assert st["peer"]["rtt_ms"] < 5000

        # 6. The positive control: a well-formed setup is taken.
        host.control(_state(match_setup=setup(0x01, good_players)))
        lobby = _wait(lambda: (lambda l: l if l["setup"]["game"] == 1 else None)(status()["lobby"]),
                      10, "the joiner to take the well-formed setup")
        assert lobby["setup"]["stage"] == 0x01
        assert [p[0] for p in lobby["setup"]["players"]] == [0x07, 0x15]

        # Dolphin itself still answers.
        assert c.status().state in ("running", "paused")
    finally:
        try:
            c.call("gprb_stop", timeout=10)
        except Exception:  # noqa: BLE001 - best effort; the fixture kills the instance anyway
            pass
        spoofer.close()
        host.close()
