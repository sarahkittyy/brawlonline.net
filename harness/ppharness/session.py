"""Compose instances, netsim and the client into multi-instance scenarios.

Comparing state "at the same frame" across instances
====================================================

Two Dolphins never run in lockstep: they boot at different times, run on different
threads, and with rollback each one may be *ahead* of what its peer has confirmed.
So "same frame" needs a definition and a way to stop both there.

1. **What counts as the same frame** (:class:`FrameKey`):

   * ``input_polls`` - game frames since boot (for Brawl the server counts the game's
     own per-frame pad reads, skipping rollback resimulation). Exact per game frame for
     instances that booted the same way (offline determinism tests). Lines up between
     netplay peers too, since both count from the netplay boot, but it is local
     progress, not the rollback session's frame.
   * ``frame`` - VI fields since boot. Same caveats, and it also ticks during loads
     and lag frames, so it is the weakest key.
   * ``memory(addr)`` - a u32 game-frame counter read from game memory (for example
     the match frame counter). Both netplay sides agree on it by construction. The
     caller provides the address.
   * ``netplay()`` - the rollback session's own frame number, sampled from
     ``netplay_status().rollback.current_frame`` (a field the Dolphin server adds on
     top of the v1 table). The natural key for rollback netplay, no address needed.

2. **How to stop there** (``method``):

   * ``"pause"`` (exact, default): for each instance, concurrently,
     ``wait_frame`` until ``target - margin`` (or poll the memory key), ``pause``,
     then ``frame_advance`` one VI frame at a time until the key equals ``target``.
     ``frame_advance`` on a paused core is deterministic, so every instance ends up
     *exactly* at ``target`` unless it overshot before ``pause`` took effect (then we
     resume everything and retry with a later target). Then ``hash_mem`` on all
     instances and ``resume``.
   * ``"sample"`` (non-intrusive): never pauses. Each instance is sampled repeatedly
     as ``key, hash_mem, key``; a sample counts only if the key did not change
     around the hash. Once a key value has a sample on every instance, the hashes
     at that key are compared. It needs the hashed ranges to be stable for the
     whole frame (true for state written once per frame, not for scratch memory
     mid-update), and it relies on ``hash_mem`` being atomic with respect to the
     emulated CPU.

3. **Limits** (read before trusting a mismatch under netplay):

   * With rollback, an instance that is ahead holds *speculative* state built from
     predicted inputs. Pausing both at game frame N compares speculative state on
     the side that had not yet received its peer's input for N. A mismatch there is
     not a desync if a rollback would fix it. Mitigations: compare a frame at least
     ``max_rollback`` frames in the past (not possible with pause-at-frame unless
     you compare later frames after confirmation), or hold inputs constant for a few
     frames before the comparison point so prediction is exact ("quiet window").
     The proper fix is a server-side confirmed-frame hash history; see
     "Proposed changes" in docs/harness-protocol.md.
   * Pausing one netplay peer stalls the other once it runs out of inputs; that is
     fine for the comparison, but long pauses can trip netplay timeouts.
   * Whether Dolphin allows ``pause``/``frame_advance`` during netplay is up to the
     server implementation (stock Dolphin forbids pausing in netplay).
   * The ``frame`` and ``input_polls`` keys only line up between instances whose
     boot paths were identical (same launcher, same inputs from boot).
"""

from __future__ import annotations

import concurrent.futures as cf
import contextlib
import dataclasses
import logging
import os
import socket
import threading
import time
from dataclasses import dataclass, field
from typing import Any, Callable, Iterable, Mapping, Sequence

from .client import (HarnessClient, HarnessCommandError, HarnessError, HarnessTimeoutError,
                     NetplayStatus)
from .instance import DolphinInstance, InstanceConfig, find_free_port
from .netsim import NetProfile, NetSim, get_profile

log = logging.getLogger("ppharness.session")


class SessionError(HarnessError):
    pass


class OvershotError(SessionError):
    def __init__(self, name: str, target: int, reached: int):
        self.name, self.target, self.reached = name, target, reached
        super().__init__(f"{name} was already at {reached}, past target {target}")


class DesyncError(SessionError):
    pass


# --------------------------------------------------------------------------- frame keys


@dataclass(frozen=True)
class FrameKey:
    kind: str                 # "frame" | "input_polls" | "memory"
    addr: int | None = None

    @staticmethod
    def frame() -> "FrameKey":
        return FrameKey("frame")

    @staticmethod
    def input_polls() -> "FrameKey":
        return FrameKey("input_polls")

    @staticmethod
    def memory(addr: int) -> "FrameKey":
        return FrameKey("memory", int(addr))

    @staticmethod
    def netplay() -> "FrameKey":
        """The rollback session's game frame (``netplay_status().rollback.current_frame``)."""
        return FrameKey("netplay")

    def read(self, client: HarnessClient) -> int:
        if self.kind == "memory":
            assert self.addr is not None
            return client.read_u32(self.addr)
        if self.kind == "netplay":
            rb = client.netplay_status().rollback
            if "current_frame" not in rb:
                raise SessionError("netplay_status has no rollback.current_frame "
                                   "(not in a rollback session?)")
            return int(rb["current_frame"])
        st = client.status()
        return st.frame if self.kind == "frame" else st.input_polls

    def __str__(self) -> str:
        if self.kind == "memory":
            return f"memory[0x{self.addr:08x}]"
        return "netplay.current_frame" if self.kind == "netplay" else self.kind


@dataclass
class StateComparison:
    key: FrameKey
    at: int
    hashes: dict[str, str]
    reached: dict[str, int]
    method: str
    attempts: int = 1
    notes: list[str] = field(default_factory=list)

    @property
    def match(self) -> bool:
        return len(set(self.hashes.values())) == 1 and len(set(self.reached.values())) == 1

    def __bool__(self) -> bool:
        return self.match

    def describe(self) -> str:
        verdict = "MATCH" if self.match else "MISMATCH"
        parts = [f"{verdict} at {self.key}={self.at} ({self.method}, {self.attempts} attempt(s))"]
        for name, h in self.hashes.items():
            parts.append(f"  {name}: {h} (key {self.reached.get(name)})")
        parts.extend(f"  note: {n}" for n in self.notes)
        return "\n".join(parts)


Target = DolphinInstance | HarnessClient


def _client(x: Target) -> HarnessClient:
    if isinstance(x, HarnessClient):
        return x
    if x.client is None:
        raise SessionError(f"{x.name} is not connected")
    return x.client


def _name(x: Target, i: int) -> str:
    return x.name if isinstance(x, DolphinInstance) else f"client{i}:{x.port}"


# --------------------------------------------------------------------------- pause_at


def pause_at(client: HarnessClient, target: int, key: FrameKey = FrameKey.input_polls(), *,
             margin: int = 3, timeout: float = 60.0, name: str = "instance",
             max_advance: int = 600) -> int:
    """Pause ``client`` with ``key == target`` exactly. Returns the key value reached.

    Raises OvershotError if the instance is (or ends up) past ``target``.
    """
    cur = key.read(client)
    if cur > target:
        raise OvershotError(name, target, cur)
    deadline = time.monotonic() + timeout
    if target - cur > margin:
        stop_at = target - margin
        if key.kind in ("memory", "netplay"):
            while key.read(client) < stop_at:
                if time.monotonic() > deadline:
                    raise HarnessTimeoutError(f"{name}: {key} did not reach {stop_at}")
                time.sleep(0.002)
        else:
            remaining_ms = int(max(1.0, deadline - time.monotonic()) * 1000)
            client.wait_frame(**{key.kind: stop_at}, timeout_ms=remaining_ms)  # type: ignore[arg-type]
    client.pause()
    cur = key.read(client)
    advanced = 0
    while cur < target:
        if advanced >= max_advance or time.monotonic() > deadline:
            raise HarnessTimeoutError(
                f"{name}: {key} stuck at {cur} < {target} after {advanced} frame advances")
        client.frame_advance(1)
        advanced += 1
        cur = key.read(client)
    if cur != target:
        raise OvershotError(name, target, cur)
    return cur


# --------------------------------------------------------------------------- compare


def compare_state(
    instances: Sequence[Target],
    ranges: Iterable[Sequence[int | str]],
    *,
    at: int | None = None,
    key: FrameKey = FrameKey.input_polls(),
    method: str = "pause",
    lead: int = 30,
    margin: int = 3,
    timeout: float = 60.0,
    attempts: int = 3,
    resume: bool = True,
    names: Sequence[str] | None = None,
) -> StateComparison:
    """Hash the same memory ranges on every instance at the same frame (see module doc).

    at:      key value to compare at; default = the furthest instance's key + ``lead``.
    method:  "pause" (exact) or "sample" (non-intrusive).
    """
    if len(instances) < 2:
        raise ValueError("need at least two instances")
    clients = [_client(x) for x in instances]
    labels = list(names) if names else [_name(x, i) for i, x in enumerate(instances)]
    rs = [list(r) for r in ranges]
    if method == "pause":
        return _compare_pause(clients, labels, rs, at, key, lead, margin, timeout, attempts, resume)
    if method == "sample":
        return _compare_sample(clients, labels, rs, at, key, timeout)
    raise ValueError(f"unknown method {method!r}")


def _resume_all(clients: Sequence[HarnessClient]) -> None:
    for c in clients:
        with contextlib.suppress(HarnessError):
            st = c.status()
            if st.state == "paused":
                c.resume()


def _compare_pause(clients: list[HarnessClient], labels: list[str], ranges: list[list[Any]],
                   at: int | None, key: FrameKey, lead: int, margin: int, timeout: float,
                   attempts: int, resume: bool) -> StateComparison:
    notes: list[str] = []
    last_err: Exception | None = None
    target = at
    with cf.ThreadPoolExecutor(max_workers=len(clients)) as pool:
        for attempt in range(1, attempts + 1):
            if target is None:
                target = max(key.read(c) for c in clients) + lead
            futures = [pool.submit(pause_at, c, target, key, margin=margin, timeout=timeout,
                                   name=n) for c, n in zip(clients, labels)]
            reached: dict[str, int] = {}
            overshot: OvershotError | None = None
            for n, f in zip(labels, futures):
                try:
                    reached[n] = f.result()
                except OvershotError as e:
                    overshot = e
                except BaseException as e:  # noqa: BLE001 - always resume before raising
                    last_err = e
            if overshot is None and last_err is None:
                try:
                    hashes = {n: c.hash_mem(ranges) for n, c in zip(labels, clients)}
                finally:
                    if resume:
                        _resume_all(clients)
                return StateComparison(key, target, hashes, reached, "pause", attempt, notes)
            _resume_all(clients)
            if last_err is not None:
                raise last_err
            assert overshot is not None
            notes.append(str(overshot))
            if at is not None:
                raise overshot
            # Retry further ahead; double the lead each time.
            lead *= 2
            target = None
    raise SessionError(f"could not pause all instances at the same {key} after {attempts} "
                       f"attempts: {'; '.join(notes)}")


def _compare_sample(clients: list[HarnessClient], labels: list[str], ranges: list[list[Any]],
                    at: int | None, key: FrameKey, timeout: float) -> StateComparison:
    samples: dict[str, dict[int, str]] = {n: {} for n in labels}
    unstable: list[str] = []
    lock = threading.Lock()
    done = threading.Event()
    deadline = time.monotonic() + timeout

    def common() -> int | None:
        keys = set.intersection(*(set(s) for s in samples.values()))
        if at is not None:
            return at if at in keys else None
        return max(keys) if keys else None

    def worker(c: HarnessClient, n: str) -> None:
        while not done.is_set() and time.monotonic() < deadline:
            k1 = key.read(c)
            h = c.hash_mem(ranges)
            k2 = key.read(c)
            if k1 != k2:
                continue
            with lock:
                prev = samples[n].get(k1)
                if prev is not None and prev != h:
                    unstable.append(f"{n}: two different hashes at {key}={k1}")
                samples[n].setdefault(k1, h)
                if common() is not None:
                    done.set()
            if at is not None and k1 > at:
                return  # it will never come back to `at`

    with cf.ThreadPoolExecutor(max_workers=len(clients)) as pool:
        futs = [pool.submit(worker, c, n) for c, n in zip(clients, labels)]
        for f in futs:
            f.result()
    k = common()
    if k is None:
        raise SessionError(f"sampling found no common {key} value within {timeout:.1f}s "
                           f"(sampled: {', '.join(f'{n}: {len(s)}' for n, s in samples.items())})")
    hashes = {n: samples[n][k] for n in labels}
    notes = ["sampled while running: only valid for ranges stable within a frame"] + unstable
    return StateComparison(key, k, hashes, {n: k for n in labels}, "sample", 1, notes)


def assert_same_state(instances: Sequence[Target], ranges: Iterable[Sequence[int | str]],
                      **kw: Any) -> StateComparison:
    cmp = compare_state(instances, ranges, **kw)
    if not cmp.match:
        raise DesyncError(cmp.describe())
    return cmp


# --------------------------------------------------------------------------- netplay


def _wait(pred: Callable[[], bool], timeout: float, what: str, interval: float = 0.1) -> None:
    deadline = time.monotonic() + timeout
    while not pred():
        if time.monotonic() > deadline:
            raise HarnessTimeoutError(f"timed out after {timeout:.1f}s waiting for {what}")
        time.sleep(interval)


def _start_when_ready(host: HarnessClient, timeout: float) -> None:
    """``netplay_start``, retried while the server reports that a peer lacks the game.

    Right after joining, the joiner still has to report that it has the game; Dolphin's
    server refuses to start until then ("not all players have the game (yet)").
    """
    deadline = time.monotonic() + timeout
    while True:
        try:
            host.netplay_start()
            return
        except HarnessCommandError as e:
            if "have the game" not in e.message or time.monotonic() > deadline:
                raise
            time.sleep(0.2)


class NetplaySession:
    """Host + joiner (through netsim). Use as a context manager or call ``close()``."""

    def __init__(self, host: DolphinInstance, joiner: DolphinInstance, netsim: NetSim,
                 netplay_port: int):
        self.host = host
        self.joiner = joiner
        self.netsim = netsim
        self.netplay_port = netplay_port
        self.failed = False
        self._closed = False

    @property
    def instances(self) -> list[DolphinInstance]:
        return [self.host, self.joiner]

    @property
    def clients(self) -> list[HarnessClient]:
        return [_client(self.host), _client(self.joiner)]

    def netplay_status(self) -> dict[str, NetplayStatus]:
        return {i.name: _client(i).netplay_status() for i in self.instances}

    def report(self) -> dict[str, Any]:
        out: dict[str, Any] = {"netsim": self.netsim.stats()}
        for i in self.instances:
            with contextlib.suppress(HarnessError):
                out[i.name] = _client(i).status().raw
        return out

    def compare_state(self, ranges: Iterable[Sequence[int | str]], **kw: Any) -> StateComparison:
        return compare_state(self.instances, ranges, **kw)

    def mark_failed(self) -> None:
        self.failed = True
        for i in self.instances:
            i.mark_failed()

    def close(self, success: bool | None = None) -> None:
        if self._closed:
            return
        self._closed = True
        if success is False:
            self.mark_failed()
        errors = []
        # Joiner first, so the host does not wait on a peer that is going away.
        for inst in (self.joiner, self.host):
            try:
                inst.cleanup(None if success is None else success)
            except Exception as e:  # noqa: BLE001 - always try to clean the rest
                errors.append(e)
        self.netsim.stop()
        if errors:
            raise errors[0]

    def __enter__(self) -> "NetplaySession":
        return self

    def __exit__(self, exc_type: Any, exc: Any, tb: Any) -> None:
        self.close(success=exc_type is None)


def two_player_netplay(
    preset: str | NetProfile = "typical",
    rollback: bool = True,
    *,
    delay: int | None = None,
    seed: Any = 0,
    name: str = "netplay",
    exe: str | os.PathLike[str] | Sequence[str] | None = None,
    template: str | os.PathLike[str] | None = None,
    instances_root: str | os.PathLike[str] | None = None,
    config: InstanceConfig | None = None,
    joiner_config: InstanceConfig | None = None,
    game: str | os.PathLike[str] | None = None,
    start_game: bool = True,
    keep: bool = False,
    audio: bool | None = None,
    env: Mapping[str, str] | None = None,
    netplay_port: int | None = None,
    connect_timeout: float = 60.0,
    join_timeout: float = 30.0,
    start_timeout: float = 120.0,
    host_name: str = "host",
    joiner_name: str = "joiner",
) -> NetplaySession:
    """Launch a host and a joiner, connect the joiner through netsim, start the game.

    Both instances start without a game (``boot=None``); the host hosts
    ``game`` (default: its own "Project+ Netplay Launcher.dol"), the joiner joins via
    the netsim proxy (so all joiner<->host traffic is impaired), and the host starts
    the game. Spikes in the profile are timed from the moment the game starts.

    Returns a started NetplaySession (use it in a ``with`` block).
    """
    profile = get_profile(preset)

    def make(role: str, cfg: InstanceConfig | None) -> DolphinInstance:
        return DolphinInstance(f"{name}-{role}", exe=exe, template=template,
                               instances_root=instances_root,
                               config=cfg if cfg is not None else InstanceConfig(),
                               boot=None, keep=keep, env=env, audio=audio,
                               connect_timeout=connect_timeout)

    host = make("host", config)
    joiner = make("joiner", joiner_config if joiner_config is not None else
                  (dataclasses.replace(config) if config is not None else None))
    sim: NetSim | None = None
    try:
        with cf.ThreadPoolExecutor(max_workers=2) as pool:
            list(pool.map(lambda i: i.create(), (host, joiner)))
        host.launch()
        joiner.launch()
        hc, jc = host.connect(), joiner.connect()

        port = netplay_port or find_free_port(kind=socket.SOCK_DGRAM)
        hc.netplay_host(port, game or host.netplay_launcher(), name=host_name,
                        rollback=rollback, delay=delay)
        sim = NetSim(("127.0.0.1", port), ("127.0.0.1", 0), profile, seed=seed).start()
        log.info("netplay host on %d, joiner via netsim %d (%s)", port, sim.listen_port,
                 profile.name)
        jc.netplay_join("127.0.0.1", sim.listen_port, name=joiner_name,
                        game=joiner.netplay_launcher() if game is None else game)
        _wait(lambda: len(hc.netplay_status().players) >= 2 and jc.netplay_status().connected,
              join_timeout, "the joiner to connect")
        session = NetplaySession(host, joiner, sim, port)
        if start_game:
            _start_when_ready(hc, start_timeout)
            sim.reset_epoch()
            _wait(lambda: all(c.netplay_status().game_running and c.status().running
                              for c in (hc, jc)), start_timeout, "the game to start on both")
        return session
    except BaseException:
        for inst in (joiner, host):
            inst.mark_failed()
            with contextlib.suppress(Exception):
                inst.cleanup(False)
        if sim is not None:
            sim.stop()
        raise
