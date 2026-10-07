"""Synchronous client for the Dolphin harness control protocol (JSON lines over TCP).

Usage::

    with HarnessClient.connect_with_retry(port, total_timeout=60) as c:
        print(c.status())
        c.pad_set(0, buttons=["A"])
        c.wait_frame(input_polls=c.status().input_polls + 10)

Every request gets a unique id. Responses with a different id (for example the late
answer to a request that timed out on our side) are discarded, so the connection stays
usable after a client-side timeout.
"""

from __future__ import annotations

import logging
import os
import socket
import threading
import time
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any, Callable, Iterable, Mapping, Sequence

from . import protocol
from .protocol import parse_addr

log = logging.getLogger("ppharness.client")


# --------------------------------------------------------------------------- errors


class HarnessError(Exception):
    """Base class for every harness client error."""


class HarnessConnectionError(HarnessError):
    """Could not connect, or the connection was lost."""


class HarnessBusyError(HarnessConnectionError):
    """The server already has a client and refused this one."""


class HarnessProtocolError(HarnessError):
    """The server sent something that is not valid protocol."""


class HarnessTimeoutError(HarnessError, TimeoutError):
    """No response within the client-side timeout."""


class HarnessCommandError(HarnessError):
    """The server answered ``ok: false``."""

    def __init__(self, cmd: str, message: str, args: Mapping[str, Any] | None = None):
        self.cmd = cmd
        self.message = message
        self.args_sent = dict(args or {})
        super().__init__(f"{cmd}: {message}")


class WaitTimeoutError(HarnessCommandError, HarnessTimeoutError):
    """``wait_frame`` returned the server-side ``"timeout"`` error."""


class VersionMismatchError(HarnessError):
    pass


# --------------------------------------------------------------------------- results


@dataclass(frozen=True)
class FrameInfo:
    frame: int
    input_polls: int


@dataclass(frozen=True)
class NetplayPlayer:
    pid: int
    name: str
    ping_ms: float | None

    @classmethod
    def from_json(cls, d: Mapping[str, Any]) -> "NetplayPlayer":
        return cls(pid=int(d.get("pid", -1)), name=str(d.get("name", "")), ping_ms=d.get("ping_ms"))


@dataclass(frozen=True)
class NetplayStatus:
    role: str | None
    connected: bool
    players: tuple[NetplayPlayer, ...]
    game_running: bool
    rollback: dict[str, Any]
    raw: dict[str, Any] = field(repr=False, compare=False)

    @classmethod
    def from_json(cls, d: Mapping[str, Any]) -> "NetplayStatus":
        return cls(
            role=d.get("role"),
            connected=bool(d.get("connected", False)),
            players=tuple(NetplayPlayer.from_json(p) for p in d.get("players") or []),
            game_running=bool(d.get("game_running", False)),
            rollback=dict(d.get("rollback") or {}),
            raw=dict(d),
        )


@dataclass(frozen=True)
class Status:
    state: str
    frame: int
    input_polls: int
    game_id: str
    cpu_thread: bool | None
    video_backend: str
    netplay: NetplayStatus | None
    raw: dict[str, Any] = field(repr=False, compare=False)

    @property
    def running(self) -> bool:
        return self.state == "running"

    @classmethod
    def from_json(cls, d: Mapping[str, Any]) -> "Status":
        np = d.get("netplay")
        return cls(
            state=str(d.get("state", "")),
            frame=int(d.get("frame", 0)),
            input_polls=int(d.get("input_polls", 0)),
            game_id=str(d.get("game_id") or ""),
            cpu_thread=d.get("cpu_thread"),
            video_backend=str(d.get("video_backend") or ""),
            netplay=NetplayStatus.from_json(np) if isinstance(np, Mapping) else None,
            raw=dict(d),
        )


@dataclass(frozen=True)
class ScriptWindow:
    starts_at: int
    ends_at: int


@dataclass(frozen=True)
class ScriptStatus:
    active: bool
    remaining: int


@dataclass(frozen=True)
class Screenshot:
    path: Path
    width: int
    height: int


@dataclass
class PadInput:
    """One controller state. ``hold`` is only used inside ``pad_script``."""

    buttons: Sequence[str] = ()
    main: tuple[int, int] = (128, 128)
    c: tuple[int, int] = (128, 128)
    l: int = 0  # noqa: E741 - protocol name
    r: int = 0
    hold: int = 1

    def to_json(self, include_hold: bool = False) -> dict[str, Any]:
        d: dict[str, Any] = {
            "buttons": protocol.normalize_buttons(self.buttons),
            "main": _stick(self.main, "main"),
            "c": _stick(self.c, "c"),
            "l": _byte(self.l, "l"),
            "r": _byte(self.r, "r"),
        }
        if include_hold:
            if int(self.hold) < 1:
                raise ValueError("hold must be >= 1")
            d["hold"] = int(self.hold)
        return d


NEUTRAL = PadInput()


def _byte(v: int, name: str) -> int:
    v = int(v)
    if not 0 <= v <= 255:
        raise ValueError(f"{name} must be 0-255, got {v}")
    return v


def _stick(v: Sequence[int], name: str) -> list[int]:
    if len(v) != 2:
        raise ValueError(f"{name} must be [x, y]")
    return [_byte(v[0], name), _byte(v[1], name)]


def _check_port(port: int) -> int:
    port = int(port)
    if not 0 <= port < protocol.NUM_PORTS:
        raise ValueError(f"pad port must be 0-3, got {port}")
    return port


def _pad_to_json(p: PadInput | Mapping[str, Any], include_hold: bool) -> dict[str, Any]:
    if isinstance(p, PadInput):
        return p.to_json(include_hold)
    if isinstance(p, Mapping):
        known = {"buttons", "main", "c", "l", "r", "hold"}
        extra = set(p) - known
        if extra:
            raise ValueError(f"unknown pad fields {sorted(extra)}")
        kw = dict(p)
        if not include_hold:
            kw.pop("hold", None)
        return PadInput(**kw).to_json(include_hold)
    raise TypeError(f"expected PadInput or mapping, got {type(p).__name__}")


# --------------------------------------------------------------------------- client


class HarnessClient:
    """One TCP connection to a harness server. Thread-safe (calls are serialized)."""

    def __init__(self, port: int, host: str = "127.0.0.1", timeout: float = 10.0,
                 strict_addresses: bool = True):
        self.host = host
        self.port = int(port)
        self.timeout = float(timeout)
        self.strict_addresses = strict_addresses
        self.server_version: int | None = None
        self._sock: socket.socket | None = None
        self._buf = bytearray()
        self._next_id = 1
        self._abandoned: set[int] = set()
        self._lock = threading.RLock()

    # ---------------------------------------------------------------- connection

    @property
    def connected(self) -> bool:
        return self._sock is not None

    def open(self, timeout: float | None = None) -> "HarnessClient":
        """Connect once (no retry) and verify the server with ``ping``."""
        t = self.timeout if timeout is None else timeout
        try:
            sock = socket.create_connection((self.host, self.port), timeout=t)
        except OSError as e:
            raise HarnessConnectionError(f"cannot connect to {self.host}:{self.port}: {e}") from e
        try:
            self_connected = sock.getsockname() == sock.getpeername()
        except OSError:
            self_connected = False
        if self_connected:
            # TCP simultaneous open: connecting to a free ephemeral port on localhost before
            # Dolphin listens can connect the socket to itself, which then echoes our requests.
            sock.close()
            raise HarnessConnectionError(f"cannot connect to {self.host}:{self.port}: connected to itself "
                                         f"(no server listening yet)")
        sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        self._sock = sock
        self._buf.clear()
        # A busy server sends its refusal line right after accept and closes. Look for it
        # before writing anything: once we have sent a request, the server's close can turn
        # into a TCP reset on Windows and the refusal line is lost.
        refusal = self._peek_refusal(0.05)
        if refusal is not None:
            self.close()
            raise HarnessBusyError(f"server refused the connection: {refusal}")
        try:
            self.ping(timeout=t)
        except HarnessConnectionError as e:
            self.close()
            if isinstance(e, HarnessBusyError):
                raise
            raise HarnessConnectionError(
                f"{e} (closed during the handshake: the server may be shutting down, or "
                f"another client may be connected)") from e
        except BaseException:
            self.close()
            raise
        return self

    def _peek_refusal(self, wait: float) -> str | None:
        assert self._sock is not None
        try:
            self._sock.settimeout(wait)
            chunk = self._sock.recv(65536)
        except (socket.timeout, BlockingIOError):
            return None
        except OSError:
            return self._drain_refusal()
        if not chunk:
            return "connection closed by server"
        self._buf += chunk
        return self._drain_refusal()

    @classmethod
    def connect(cls, port: int, host: str = "127.0.0.1", timeout: float = 10.0,
                **kw: Any) -> "HarnessClient":
        return cls(port, host, timeout, **kw).open()

    @classmethod
    def connect_with_retry(
        cls,
        port: int,
        host: str = "127.0.0.1",
        *,
        total_timeout: float = 60.0,
        interval: float = 0.1,
        max_interval: float = 1.0,
        is_alive: Callable[[], bool] | None = None,
        timeout: float = 10.0,
        **kw: Any,
    ) -> "HarnessClient":
        """Keep trying to connect while Dolphin boots.

        ``is_alive`` is polled between attempts; when it returns False (the process
        exited) we stop immediately instead of waiting for the full timeout.
        A busy server (another client connected) is reported at once.
        """
        deadline = time.monotonic() + total_timeout
        last_err: Exception | None = None
        attempt = 0
        while True:
            attempt += 1
            if is_alive is not None and not is_alive():
                raise HarnessConnectionError(
                    f"process exited before the harness server on port {port} accepted a "
                    f"connection (after {attempt - 1} attempts; last error: {last_err})"
                )
            remaining = deadline - time.monotonic()
            try:
                client = cls(port, host, timeout, **kw)
                return client.open(timeout=max(0.2, min(timeout, remaining, 5.0)))
            except HarnessBusyError:
                raise
            except (HarnessConnectionError, HarnessTimeoutError) as e:
                last_err = e
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                raise HarnessConnectionError(
                    f"no harness server on {host}:{port} after {total_timeout:.1f}s "
                    f"({attempt} attempts; last error: {last_err})"
                )
            time.sleep(min(interval, remaining))
            interval = min(interval * 1.5, max_interval)

    def close(self) -> None:
        with self._lock:
            if self._sock is not None:
                try:
                    self._sock.close()
                finally:
                    self._sock = None
                    self._buf.clear()

    def __enter__(self) -> "HarnessClient":
        if not self.connected:
            self.open()
        return self

    def __exit__(self, *exc: Any) -> None:
        self.close()

    def __repr__(self) -> str:
        state = "connected" if self.connected else "closed"
        return f"<HarnessClient {self.host}:{self.port} {state}>"

    # ---------------------------------------------------------------- core I/O

    def _read_line(self, deadline: float) -> bytes:
        assert self._sock is not None
        while True:
            nl = self._buf.find(b"\n")
            if nl >= 0:
                line = bytes(self._buf[:nl])
                del self._buf[: nl + 1]
                return line
            if len(self._buf) > protocol.MAX_LINE_BYTES:
                raise HarnessProtocolError("response line exceeds the maximum size")
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                raise socket.timeout()
            self._sock.settimeout(remaining)
            chunk = self._sock.recv(1 << 20)
            if not chunk:
                raise ConnectionResetError("server closed the connection")
            self._buf += chunk

    def call(self, cmd: str, *, timeout: float | None = None, **args: Any) -> dict[str, Any]:
        """Send a raw command and return its ``result`` object.

        Raises HarnessCommandError when the server answers ``ok: false``.
        """
        t = self.timeout if timeout is None else float(timeout)
        with self._lock:
            if self._sock is None:
                raise HarnessConnectionError(f"not connected (cmd {cmd!r})")
            req_id = self._next_id
            self._next_id += 1
            msg = {"id": req_id, "cmd": cmd, **args}
            data = protocol.encode(msg)
            log.debug("-> %s", data[:500])
            deadline = time.monotonic() + t
            try:
                self._sock.settimeout(t)
                self._sock.sendall(data)
                while True:
                    line = self._read_line(deadline)
                    if not line.strip():
                        continue
                    log.debug("<- %s", line[:500])
                    try:
                        resp = protocol.decode(line)
                    except ValueError as e:
                        raise HarnessProtocolError(f"invalid JSON from server: {line[:200]!r}") from e
                    rid = resp.get("id")
                    if rid == req_id:
                        break
                    if rid is None and resp.get("ok") is False:
                        # The only id-less error in v1 is the "second client" refusal.
                        err = str(resp.get("error", ""))
                        self.close()
                        raise HarnessBusyError(f"server refused the connection: {err}")
                    if rid in self._abandoned:
                        self._abandoned.discard(rid)
                        continue
                    log.warning("discarding unexpected response %r", resp)
            except socket.timeout:
                self._abandoned.add(req_id)
                raise HarnessTimeoutError(f"{cmd}: no response within {t:.1f}s") from None
            except (ConnectionError, OSError) as e:
                refusal = self._drain_refusal()
                self.close()
                if refusal is not None:
                    raise HarnessBusyError(f"server refused the connection: {refusal}") from e
                if cmd == "quit":
                    # The server may exit before its reply reaches us; that is fine.
                    return {}
                raise HarnessConnectionError(f"{cmd}: connection lost: {e}") from e

        if resp.get("ok") is True:
            result = resp.get("result")
            return dict(result) if isinstance(result, Mapping) else {}
        if resp.get("ok") is False:
            err = str(resp.get("error", "unknown error"))
            if cmd == "wait_frame" and err == "timeout":
                raise WaitTimeoutError(cmd, err, args)
            raise HarnessCommandError(cmd, err, args)
        raise HarnessProtocolError(f"response without ok field: {resp!r}")

    def _drain_refusal(self) -> str | None:
        """After a connection error, look for an id-less refusal line already received."""
        data = bytes(self._buf)
        if self._sock is not None:
            try:
                self._sock.settimeout(0.2)
                while len(data) < 65536:
                    chunk = self._sock.recv(65536)
                    if not chunk:
                        break
                    data += chunk
            except OSError:
                pass
        for raw in data.split(b"\n"):
            try:
                msg = protocol.decode(raw)
            except ValueError:
                continue
            if msg.get("id") is None and msg.get("ok") is False:
                return str(msg.get("error", ""))
        return None

    # ---------------------------------------------------------------- helpers

    def _addr(self, addr: int | str, length: int) -> int:
        a = parse_addr(addr)
        if self.strict_addresses:
            protocol.check_range(a, length)
        return a

    # ---------------------------------------------------------------- commands

    def ping(self, timeout: float | None = None, check_version: bool = True) -> int:
        r = self.call("ping", timeout=timeout)
        if not r.get("pong"):
            raise HarnessProtocolError(f"bad ping reply {r!r}")
        version = int(r.get("version", -1))
        self.server_version = version
        if check_version and version != protocol.PROTOCOL_VERSION:
            raise VersionMismatchError(
                f"server speaks protocol v{version}, client v{protocol.PROTOCOL_VERSION}"
            )
        return version

    def status(self) -> Status:
        return Status.from_json(self.call("status"))

    def read_mem(self, addr: int | str, length: int) -> bytes:
        a = self._addr(addr, length)
        r = self.call("read_mem", addr=a, len=int(length))
        data = bytes.fromhex(r["hex"])
        if len(data) != length:
            raise HarnessProtocolError(f"read_mem returned {len(data)} bytes, asked for {length}")
        return data

    def write_mem(self, addr: int | str, data: bytes | bytearray | str) -> None:
        if isinstance(data, str):
            data = bytes.fromhex(data)
        a = self._addr(addr, len(data))
        self.call("write_mem", addr=a, hex=bytes(data).hex())

    def read_u32(self, addr: int | str) -> int:
        a = self._addr(addr, 4)
        return int(self.call("read_u32", addr=a)["value"])

    def read_u8(self, addr: int | str) -> int:
        return self.read_mem(addr, 1)[0]

    def read_u16(self, addr: int | str) -> int:
        return int.from_bytes(self.read_mem(addr, 2), "big")

    def read_f32(self, addr: int | str) -> float:
        import struct
        return struct.unpack(">f", self.read_mem(addr, 4))[0]

    def hash_mem(self, ranges: Iterable[Sequence[int | str]]) -> str:
        rs = protocol.normalize_ranges(ranges)
        if self.strict_addresses:
            for a, n in rs:
                protocol.check_range(a, n, max_len=None)
        return str(self.call("hash_mem", ranges=rs, timeout=max(self.timeout, 30.0))["xxh3_64"])

    def pad_set(self, port: int, pad: PadInput | Mapping[str, Any] | None = None, *,
                buttons: Sequence[str] = (), main: Sequence[int] = (128, 128),
                c: Sequence[int] = (128, 128), l: int = 0, r: int = 0) -> None:  # noqa: E741
        if pad is None:
            pad = PadInput(buttons, tuple(main), tuple(c), l, r)  # type: ignore[arg-type]
        self.call("pad_set", port=_check_port(port), **_pad_to_json(pad, include_hold=False))

    def pad_clear(self, port: int) -> None:
        self.call("pad_clear", port=_check_port(port))

    def pad_script(self, port: int, frames: Sequence[PadInput | Mapping[str, Any]],
                   start: str | int = "next") -> ScriptWindow:
        if not frames:
            raise ValueError("pad_script needs at least one frame")
        if start != "next" and not isinstance(start, int):
            raise ValueError("start must be 'next' or an absolute input_polls value")
        r = self.call("pad_script", port=_check_port(port),
                      frames=[_pad_to_json(f, include_hold=True) for f in frames], start=start)
        return ScriptWindow(int(r["starts_at"]), int(r["ends_at"]))

    def pad_script_status(self, port: int) -> ScriptStatus:
        r = self.call("pad_script_status", port=_check_port(port))
        return ScriptStatus(bool(r["active"]), int(r["remaining"]))

    def wait_frame(self, frame: int | None = None, *, input_polls: int | None = None,
                   timeout_ms: int = protocol.DEFAULT_WAIT_TIMEOUT_MS) -> FrameInfo:
        """Block until ``frame`` or ``input_polls`` is reached (exactly one must be given)."""
        if (frame is None) == (input_polls is None):
            raise ValueError("give exactly one of frame or input_polls")
        args: dict[str, Any] = {"timeout_ms": int(timeout_ms)}
        if frame is not None:
            args["frame"] = int(frame)
        else:
            args["input_polls"] = int(input_polls)  # type: ignore[arg-type]
        r = self.call("wait_frame", timeout=timeout_ms / 1000.0 + self.timeout, **args)
        return FrameInfo(int(r["frame"]), int(r["input_polls"]))

    def pause(self) -> None:
        self.call("pause")

    def resume(self) -> None:
        self.call("resume")

    def frame_advance(self, n: int = 1) -> int:
        if n < 1:
            raise ValueError("n must be >= 1")
        return int(self.call("frame_advance", n=int(n), timeout=self.timeout + n / 30.0)["frame"])

    def screenshot(self, path: str | os.PathLike[str], timeout: float | None = None) -> Screenshot:
        p = Path(path).expanduser().resolve()
        if p.suffix.lower() != ".png":
            raise ValueError("screenshot path must end in .png")
        p.parent.mkdir(parents=True, exist_ok=True)
        r = self.call("screenshot", path=str(p), timeout=timeout)
        return Screenshot(Path(r.get("path", p)), int(r.get("width", 0)), int(r.get("height", 0)))

    def save_state(self, path: str | os.PathLike[str]) -> None:
        p = Path(path).expanduser().resolve()
        p.parent.mkdir(parents=True, exist_ok=True)
        self.call("save_state", path=str(p), timeout=max(self.timeout, 30.0))

    def load_state(self, path: str | os.PathLike[str]) -> None:
        self.call("load_state", path=str(Path(path).expanduser().resolve()),
                  timeout=max(self.timeout, 30.0))

    def netplay_host(self, port: int, game: str | os.PathLike[str], name: str = "host",
                     rollback: bool = True, delay: int | None = None) -> None:
        args: dict[str, Any] = {"port": int(port), "game": str(game), "name": name,
                                "rollback": bool(rollback)}
        if delay is not None:
            args["delay"] = int(delay)
        self.call("netplay_host", **args)

    def netplay_join(self, host: str, port: int, name: str = "joiner",
                     game: str | os.PathLike[str] | None = None) -> None:
        """Join a host. ``game`` is an optional local path hint for the game file (an
        extension of the v1 table that the Dolphin server implements)."""
        args: dict[str, Any] = {"host": host, "port": int(port), "name": name}
        if game is not None:
            args["game"] = str(game)
        self.call("netplay_join", **args)

    def netplay_start(self) -> None:
        self.call("netplay_start")

    def netplay_status(self) -> NetplayStatus:
        return NetplayStatus.from_json(self.call("netplay_status"))

    def netplay_leave(self) -> None:
        self.call("netplay_leave")

    def rollback_pad_history(self, since: int = 0) -> dict[int, tuple[int, ...]]:
        """``{gekko_frame: (buttons_p0, buttons_p1, buttons_p2, buttons_p3, crc32)}`` for the
        pads the game used on each recent rollback frame (see the protocol doc)."""
        rows = self.call("rollback_pad_history", since=int(since)).get("frames", [])
        return {int(r[0]): tuple(int(x) for x in r[1:]) for r in rows}

    # ------------------------------------------------------------------ online play

    def online_status(self) -> dict[str, Any]:
        """Login state from ``<User>/Online/user.json`` (no play key)."""
        return self.call("online_status")

    def mm_search_direct(self, code: str, *, session: str = "auto", **args: Any) -> dict[str, Any]:
        """Start a Direct search for ``code``; returns ``mm_status``. ``session="none"`` keeps
        the P2P link instead of handing it to the netplay session. Extra args: ``game``,
        ``delay``, ``auto_start``, ``selections``."""
        return self.call("mm_search_direct", code=code, session=session, **args)

    def mm_search(self, mode: str, code: str = "", **args: Any) -> dict[str, Any]:
        return self.call("mm_search", mode=mode, code=code, **args)

    def mm_status(self) -> dict[str, Any]:
        return self.call("mm_status")

    def mm_cancel(self) -> dict[str, Any]:
        return self.call("mm_cancel")

    def online_session_backend(self, backend: str) -> dict[str, Any]:
        """Replace the session backend: ``netplay`` (default), ``record`` (records the
        hand-off and holds the P2P link; the game keeps running) or ``none``."""
        return self.call("online_session_backend", backend=backend)

    def game_bridge_status(self) -> dict[str, Any]:
        """Dolphin's side of the game's PPOM mailbox (Online/GameBridge.h)."""
        return self.call("game_bridge_status")

    def game_bridge_config(self, **args: Any) -> dict[str, Any]:
        """``enabled`` (servicing on/off), ``hand_off`` (FIND_OPPONENT hands the match to the
        session backend)."""
        return self.call("game_bridge_config", **args)

    def log_mark(self, text: str) -> None:
        self.call("log_mark", text=str(text))

    def quit(self, timeout: float = 10.0) -> None:
        """Ask Dolphin to shut down cleanly. The connection is closed afterwards."""
        try:
            self.call("quit", timeout=timeout)
        finally:
            self.close()

    # ---------------------------------------------------------------- conveniences

    def wait_until(self, predicate: Callable[[Status], bool], timeout: float = 30.0,
                   interval: float = 0.05, what: str = "condition") -> Status:
        """Poll ``status`` until ``predicate(status)`` is true."""
        deadline = time.monotonic() + timeout
        while True:
            st = self.status()
            if predicate(st):
                return st
            if time.monotonic() >= deadline:
                raise HarnessTimeoutError(f"timed out after {timeout:.1f}s waiting for {what}; "
                                          f"last status {st}")
            time.sleep(interval)

    def wait_state(self, state: str, timeout: float = 60.0) -> Status:
        return self.wait_until(lambda s: s.state == state, timeout, what=f"state {state!r}")

    def wait_frames(self, n: int, timeout_ms: int | None = None) -> FrameInfo:
        """Wait ``n`` VI frames from now."""
        cur = self.status().frame
        t = timeout_ms if timeout_ms is not None else int(max(10000, n * 1000 / 30))
        return self.wait_frame(cur + n, timeout_ms=t)
