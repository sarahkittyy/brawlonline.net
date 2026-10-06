"""A mock harness server: implements protocol v1 with fake memory and fake frames.

It is used by the unit tests and by ``fake_dolphin`` (a stand-in for the Dolphin
process). It is *not* an emulator, but it behaves like one in the ways the harness
cares about:

* VI frames tick at ``fps * speed`` in a background thread; ``input_polls`` advance
  ``polls_per_frame`` times per frame.
* Pad state is resolved per poll (script > pad_set > neutral) exactly like the
  contract says, and every poll is recorded in ``pad_history``.
* A tiny deterministic "game" lives in fake MEM1 (see the ``GAME_*`` constants):
  a game-frame counter, the last pad state of each port and a 64-byte state blob that
  is a rolling hash of all inputs. Two mocks fed the same inputs therefore have the
  same state at the same game frame, and a desync (``inject_desync`` or ``write_mem``)
  shows up in ``hash_mem``.
* Netplay is faked over real UDP (host <-> joiner datagrams with HELLO/WELCOME/PING/
  START), so the session helpers can be tested end-to-end through ``netsim``.

``hash_mem`` uses BLAKE2b-64 because xxh3 is not in the standard library. Hashes are
only comparable between servers of the same implementation.
"""

from __future__ import annotations

import argparse
import collections
import hashlib
import json
import logging
import socket
import struct
import threading
import time
import zlib
from pathlib import Path
from typing import Any, Callable

from . import protocol

log = logging.getLogger("ppharness.mock")

GAME_FRAME_ADDR = 0x80001000      # u32, increments once per poll while a game runs
GAME_PADS_ADDR = 0x80001100       # 4 x 8 bytes: buttons(u16) main.x main.y c.x c.y l r
GAME_STATE_ADDR = 0x80001200      # 64-byte rolling hash of every input since game start
GAME_STATE_LEN = 64
GAME_RANGES = [[GAME_FRAME_ADDR, 4], [GAME_PADS_ADDR, 32], [GAME_STATE_ADDR, GAME_STATE_LEN]]

PAGE = 1 << 16
NEUTRAL_PAD = ((), (128, 128), (128, 128), 0, 0)


class MockError(Exception):
    pass


# --------------------------------------------------------------------------- memory


class FakeMemory:
    """Sparse big-endian memory covering the MEM1 and MEM2 windows."""

    def __init__(self) -> None:
        self.pages: dict[int, bytearray] = {}

    def _check(self, addr: int, length: int) -> None:
        try:
            protocol.check_range(addr, length, max_len=None)
        except ValueError as e:
            raise MockError(str(e)) from None

    def read(self, addr: int, length: int) -> bytes:
        self._check(addr, length)
        addr = self._canonical(addr)
        out = bytearray()
        while length > 0:
            page, off = divmod(addr, PAGE)
            n = min(length, PAGE - off)
            p = self.pages.get(page)
            out += p[off:off + n] if p is not None else bytes(n)
            addr += n
            length -= n
        return bytes(out)

    @staticmethod
    def _canonical(addr: int) -> int:
        """Uncached mirrors (0xC/0xD) alias the cached windows (0x8/0x9)."""
        return addr - 0x40000000 if addr >= 0xC0000000 else addr

    def write(self, addr: int, data: bytes) -> None:
        self._check(addr, len(data))
        addr = self._canonical(addr)
        i = 0
        while i < len(data):
            page, off = divmod(addr + i, PAGE)
            n = min(len(data) - i, PAGE - off)
            p = self.pages.get(page)
            if p is None:
                p = self.pages[page] = bytearray(PAGE)
            p[off:off + n] = data[i:i + n]
            i += n

    def clear(self) -> None:
        self.pages.clear()


# --------------------------------------------------------------------------- PNG


def write_png(path: Path, width: int, height: int, rgb: tuple[int, int, int]) -> None:
    raw = b"".join(b"\x00" + bytes(rgb) * width for _ in range(height))

    def chunk(tag: bytes, data: bytes) -> bytes:
        return (struct.pack(">I", len(data)) + tag + data
                + struct.pack(">I", zlib.crc32(tag + data) & 0xFFFFFFFF))

    png = (b"\x89PNG\r\n\x1a\n"
           + chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 2, 0, 0, 0))
           + chunk(b"IDAT", zlib.compress(raw))
           + chunk(b"IEND", b""))
    path.write_bytes(png)


# --------------------------------------------------------------------------- pads


def _pad_tuple(d: dict[str, Any]) -> tuple:
    buttons = tuple(protocol.normalize_buttons(d.get("buttons")))
    main = tuple(int(v) for v in d.get("main", (128, 128)))
    c = tuple(int(v) for v in d.get("c", (128, 128)))
    l_ = int(d.get("l", 0))
    r_ = int(d.get("r", 0))
    for v in (*main, *c, l_, r_):
        if not 0 <= v <= 255:
            raise MockError(f"pad value out of range: {v}")
    if len(main) != 2 or len(c) != 2:
        raise MockError("sticks must be [x, y]")
    return (buttons, main, c, l_, r_)


def _pad_bytes(p: tuple) -> bytes:
    mask = 0
    for b in p[0]:
        mask |= 1 << protocol.BUTTONS.index(b)
    return struct.pack(">HBBBBBB", mask, p[1][0], p[1][1], p[2][0], p[2][1], p[3], p[4])


def pad_dict(p: tuple) -> dict[str, Any]:
    return {"buttons": list(p[0]), "main": list(p[1]), "c": list(p[2]), "l": p[3], "r": p[4]}


MAX_SCRIPT_POLLS = 1_000_000


class _Script:
    """A pad script expanded to one entry per poll index.

    Same semantics as the Dolphin server: entry ``i`` is used for poll index
    ``starts_at + i`` (poll index = the value of ``input_polls`` before that poll), and
    ``ends_at`` is exclusive, so ``input_polls == ends_at`` means the script finished.
    """

    def __init__(self, timeline: list[tuple], starts_at: int):
        self.timeline = timeline
        self.starts_at = starts_at
        self.ends_at = starts_at + len(timeline)

    def at(self, poll_index: int) -> tuple | None:
        if self.starts_at <= poll_index < self.ends_at:
            return self.timeline[poll_index - self.starts_at]
        return None


# --------------------------------------------------------------------------- server


class MockHarnessServer:
    def __init__(
        self,
        port: int = 0,
        host: str = "127.0.0.1",
        *,
        boot: bool = True,
        fps: float = 59.94,
        speed: float = 1.0,
        polls_per_frame: int = 1,
        video_backend: str = "Null",
        cpu_thread: bool = True,
        game_id: str = "RSBE01",
        device_ports: tuple[int, ...] = (0, 1, 2, 3),
        log_fn: Callable[[str], None] | None = None,
        on_quit: Callable[[], None] | None = None,
        netplay_bind: str = "127.0.0.1",
    ):
        self.host = host
        self.requested_port = port
        self.fps = fps
        self.speed = speed
        self.polls_per_frame = polls_per_frame
        self.video_backend = video_backend
        self.cpu_thread = cpu_thread
        self.game_id = game_id
        self.device_ports = set(device_ports)
        self.log_fn = log_fn or (lambda s: log.info("%s", s))
        self.on_quit = on_quit
        self.netplay_bind = netplay_bind

        self.memory = FakeMemory()
        self.frame = 0
        self.input_polls = 0
        self.state = "uninitialized"
        self.pad_set_state: dict[int, tuple] = {}
        self.scripts: dict[int, _Script | None] = {p: None for p in range(4)}
        self.pad_history: collections.deque[tuple[int, tuple]] = collections.deque(maxlen=200_000)
        self.commands: list[dict[str, Any]] = []
        self.refused_connections = 0
        self.quit_requested = threading.Event()

        self._lock = threading.RLock()
        self._cond = threading.Condition(self._lock)
        self._stop = threading.Event()
        self._listener: socket.socket | None = None
        self._active: socket.socket | None = None
        self._threads: list[threading.Thread] = []
        self._netplay: _FakeNetplay | None = None
        self._boot_on_start = boot
        self.port = 0

    # ---------------------------------------------------------------- lifecycle

    def start(self) -> int:
        s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        s.bind((self.host, self.requested_port))
        s.listen(4)
        self._listener = s
        self.port = s.getsockname()[1]
        if self._boot_on_start:
            self.boot_game()
        for target in (self._accept_loop, self._emu_loop):
            t = threading.Thread(target=target, daemon=True, name=f"mock-{target.__name__}")
            t.start()
            self._threads.append(t)
        return self.port

    def stop(self) -> None:
        self._stop.set()
        with self._cond:
            self._cond.notify_all()
        if self._listener is not None:
            try:
                self._listener.close()
            except OSError:
                pass
        if self._active is not None:
            try:
                self._active.shutdown(socket.SHUT_RDWR)
            except OSError:
                pass
        if self._netplay is not None:
            self._netplay.close()
        for t in self._threads:
            if t is not threading.current_thread():
                t.join(timeout=2)

    def __enter__(self) -> "MockHarnessServer":
        if not self.port:
            self.start()
        return self

    def __exit__(self, *exc: Any) -> None:
        self.stop()

    # ---------------------------------------------------------------- fake game

    def boot_game(self) -> None:
        with self._cond:
            self.memory.clear()
            self.frame = 0
            self.input_polls = 0
            self.state = "running"
            self._next_tick = time.monotonic()
            self._cond.notify_all()
        self.log_fn(f"Booting game {self.game_id}")

    @property
    def game_frame(self) -> int:
        return int.from_bytes(self.memory.read(GAME_FRAME_ADDR, 4), "big")

    def inject_desync(self) -> None:
        """Flip one byte of the game state blob (simulates a desync)."""
        with self._lock:
            b = bytearray(self.memory.read(GAME_STATE_ADDR, 1))
            b[0] ^= 0xFF
            self.memory.write(GAME_STATE_ADDR, bytes(b))

    def _resolve_pad(self, port: int, poll_index: int) -> tuple:
        sc = self.scripts[port]
        if sc is not None:
            pad = sc.at(poll_index)
            if pad is not None:
                return pad
        return self.pad_set_state.get(port, NEUTRAL_PAD)

    def _step(self) -> None:
        """Advance one VI frame (caller holds the lock)."""
        self.frame += 1
        for _ in range(self.polls_per_frame):
            idx = self.input_polls
            pads = tuple(self._resolve_pad(p, idx) if p in self.device_ports else NEUTRAL_PAD
                         for p in range(4))
            self.input_polls += 1
            self.pad_history.append((idx, pads))
            gf = self.game_frame + 1
            pad_blob = b"".join(_pad_bytes(p) for p in pads)
            state = self.memory.read(GAME_STATE_ADDR, GAME_STATE_LEN)
            new_state = hashlib.blake2b(state + gf.to_bytes(4, "big") + pad_blob,
                                        digest_size=GAME_STATE_LEN).digest()
            self.memory.write(GAME_FRAME_ADDR, gf.to_bytes(4, "big"))
            self.memory.write(GAME_PADS_ADDR, pad_blob)
            self.memory.write(GAME_STATE_ADDR, new_state)
        self._cond.notify_all()

    def _emu_loop(self) -> None:
        self._next_tick = time.monotonic()
        while not self._stop.is_set():
            with self._cond:
                if self.state != "running":
                    self._cond.wait(0.05)
                    self._next_tick = time.monotonic()
                    continue
            period = 1.0 / (self.fps * self.speed)
            self._next_tick += period
            delay = self._next_tick - time.monotonic()
            if delay > 0:
                time.sleep(delay)
            elif delay < -0.25:  # fell far behind (debugger, overloaded box): resync
                self._next_tick = time.monotonic()
            with self._cond:
                if self.state == "running":
                    self._step()

    # ---------------------------------------------------------------- networking

    def _accept_loop(self) -> None:
        assert self._listener is not None
        while not self._stop.is_set():
            try:
                conn, _ = self._listener.accept()
            except OSError:
                return
            with self._lock:
                busy = self._active is not None
                if not busy:
                    self._active = conn
            if busy:
                self.refused_connections += 1
                threading.Thread(target=self._refuse, args=(conn,), daemon=True).start()
                continue
            t = threading.Thread(target=self._serve, args=(conn,), daemon=True, name="mock-client")
            t.start()

    @staticmethod
    def _refuse(conn: socket.socket) -> None:
        """Send the refusal line, half-close, drain, close (avoids a TCP reset eating it)."""
        try:
            conn.sendall(protocol.encode(
                {"id": None, "ok": False, "error": "another harness client is already connected"}))
            conn.shutdown(socket.SHUT_WR)
            conn.settimeout(1.0)
            while conn.recv(65536):
                pass
        except OSError:
            pass
        finally:
            conn.close()

    def _serve(self, conn: socket.socket) -> None:
        conn.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        f = conn.makefile("rb")
        quit_after = False
        try:
            while not self._stop.is_set():
                line = f.readline(protocol.MAX_LINE_BYTES)
                if not line:
                    break
                if not line.strip():
                    continue
                resp, quit_after = self._handle_line(line)
                try:
                    conn.sendall(protocol.encode(resp))
                except OSError:
                    break
                if quit_after:
                    break
        except OSError:
            pass
        finally:
            try:
                f.close()
                conn.close()
            except OSError:
                pass
            with self._lock:
                if self._active is conn:
                    self._active = None
        if quit_after:
            self._do_quit()

    def _handle_line(self, line: bytes) -> tuple[dict[str, Any], bool]:
        try:
            msg = protocol.decode(line)
        except ValueError as e:
            return {"id": None, "ok": False, "error": f"bad json: {e}"}, False
        req_id = msg.get("id")
        cmd = msg.get("cmd")
        args = {k: v for k, v in msg.items() if k not in ("id", "cmd")}
        self.commands.append(msg)
        handler = getattr(self, f"cmd_{cmd}", None) if isinstance(cmd, str) else None
        if handler is None:
            return {"id": req_id, "ok": False, "error": f"unknown command: {cmd}"}, False
        try:
            result = handler(**args)
        except MockError as e:
            return {"id": req_id, "ok": False, "error": str(e)}, False
        except TypeError as e:
            return {"id": req_id, "ok": False, "error": f"bad arguments: {e}"}, False
        except (KeyError, ValueError) as e:
            return {"id": req_id, "ok": False, "error": f"bad arguments: {e}"}, False
        return {"id": req_id, "ok": True, "result": result or {}}, cmd == "quit"

    def _do_quit(self) -> None:
        self.quit_requested.set()
        self.log_fn("Harness: quit requested")
        with self._cond:
            self.state = "stopping"
            self._cond.notify_all()
        if self.on_quit is not None:
            self.on_quit()
        self.stop()

    # ---------------------------------------------------------------- helpers

    def _need_game(self) -> None:
        if self.state not in ("running", "paused"):
            raise MockError("emulation is not running")

    def _need_device(self, port: int) -> int:
        port = int(port)
        if not 0 <= port <= 3:
            raise MockError(f"invalid port {port}")
        if port not in self.device_ports:
            raise MockError(f"port {port} has no SI device (configure a Standard Controller)")
        return port

    # ---------------------------------------------------------------- commands

    def cmd_ping(self) -> dict:
        return {"pong": True, "version": protocol.PROTOCOL_VERSION}

    def cmd_status(self) -> dict:
        with self._lock:
            return {
                "state": self.state,
                "frame": self.frame,
                "input_polls": self.input_polls,
                "game_id": self.game_id if self.state in ("running", "paused") else "",
                "cpu_thread": self.cpu_thread,
                "video_backend": self.video_backend,
                "netplay": self._netplay.status() if self._netplay else None,
            }

    def cmd_read_mem(self, addr: int, len: int) -> dict:  # noqa: A002 - protocol name
        if int(len) > protocol.MAX_MEM_LEN:
            raise MockError("len must be <= 16 MiB")
        with self._lock:
            self._need_game()
            return {"hex": self.memory.read(int(addr), int(len)).hex()}

    def cmd_write_mem(self, addr: int, hex: str) -> dict:  # noqa: A002
        with self._lock:
            self._need_game()
            self.memory.write(int(addr), bytes.fromhex(hex))
        return {}

    def cmd_read_u32(self, addr: int) -> dict:
        with self._lock:
            self._need_game()
            return {"value": int.from_bytes(self.memory.read(int(addr), 4), "big")}

    def cmd_hash_mem(self, ranges: list) -> dict:
        h = hashlib.blake2b(digest_size=8)
        with self._lock:
            self._need_game()
            for a, n in ranges:
                h.update(self.memory.read(int(a), int(n)))
        return {"xxh3_64": h.hexdigest()}

    def cmd_pad_set(self, port: int, **pad: Any) -> dict:
        port = self._need_device(port)
        with self._lock:
            self.pad_set_state[port] = _pad_tuple(pad)
        return {}

    def cmd_pad_clear(self, port: int) -> dict:
        port = self._need_device(port)
        with self._lock:
            self.pad_set_state.pop(port, None)
            self.scripts[port] = None
        return {}

    def cmd_pad_script(self, port: int, frames: list, start: Any = "next") -> dict:
        port = self._need_device(port)
        if not isinstance(frames, list):
            raise MockError("'frames' must be a list")
        timeline: list[tuple] = []
        for fr in frames:
            if not isinstance(fr, dict):
                raise MockError("each frame must be an object")
            hold = int(fr.get("hold", 1))
            if hold < 1:
                raise MockError("'hold' must be >= 1")
            if len(timeline) + hold > MAX_SCRIPT_POLLS:
                raise MockError("script too long")
            timeline.extend([_pad_tuple(fr)] * hold)
        with self._lock:
            next_poll = self.input_polls
            if start == "next":
                s = next_poll
            elif isinstance(start, int) and not isinstance(start, bool) and start >= 0:
                s = start
                if s < next_poll:
                    raise MockError(f"start {s} is too early (the earliest poll that can still "
                                    f"be scheduled is {next_poll})")
            else:
                raise MockError("'start' must be \"next\" or an absolute input_polls value")
            # A new script replaces whatever script the port had.
            sc = _Script(timeline, s)
            self.scripts[port] = sc if timeline else None
            return {"starts_at": sc.starts_at, "ends_at": sc.ends_at}

    def cmd_pad_script_status(self, port: int) -> dict:
        port = self._need_device(port)
        with self._lock:
            sc = self.scripts[port]
            if sc is None or self.input_polls >= sc.ends_at:
                return {"active": False, "remaining": 0}
            return {"active": True, "remaining": sc.ends_at - self.input_polls}

    def cmd_wait_frame(self, frame: int | None = None, input_polls: int | None = None,
                       timeout_ms: int = protocol.DEFAULT_WAIT_TIMEOUT_MS) -> dict:
        if (frame is None) == (input_polls is None):
            raise MockError("give exactly one of frame or input_polls")
        deadline = time.monotonic() + int(timeout_ms) / 1000.0
        with self._cond:
            while True:
                cur = self.frame if frame is not None else self.input_polls
                target = frame if frame is not None else input_polls
                if cur >= int(target):  # type: ignore[arg-type]
                    return {"frame": self.frame, "input_polls": self.input_polls}
                if self._stop.is_set() or self.state == "stopping":
                    raise MockError("emulation stopped")
                remaining = deadline - time.monotonic()
                if remaining <= 0:
                    raise MockError("timeout")
                self._cond.wait(min(remaining, 0.1))

    def cmd_pause(self) -> dict:
        with self._cond:
            self._need_game()
            self.state = "paused"
        return {}

    def cmd_resume(self) -> dict:
        with self._cond:
            self._need_game()
            self.state = "running"
            self._cond.notify_all()
        return {}

    def cmd_frame_advance(self, n: int = 1) -> dict:
        with self._cond:
            if self.state != "paused":
                raise MockError("frame_advance requires the emulation to be paused")
            for _ in range(int(n)):
                self._step()
            return {"frame": self.frame}

    def cmd_screenshot(self, path: str) -> dict:
        p = Path(path)
        if not p.is_absolute() or p.suffix.lower() != ".png":
            raise MockError("path must be an absolute .png path")
        if self.video_backend == "Null":
            raise MockError("screenshots are not available with the Null video backend")
        self._need_game()
        if self.state == "paused":
            raise MockError("screenshots require running emulation (resume first)")
        p.parent.mkdir(parents=True, exist_ok=True)
        w, h = 64, 48
        write_png(p, w, h, (self.frame % 256, 80, 160))
        return {"path": str(p), "width": w, "height": h}

    def cmd_save_state(self, path: str) -> dict:
        with self._lock:
            self._need_game()
            data = {"frame": self.frame, "input_polls": self.input_polls,
                    "pages": {str(k): bytes(v).hex() for k, v in self.memory.pages.items()}}
        Path(path).write_text(json.dumps(data))
        return {}

    def cmd_load_state(self, path: str) -> dict:
        try:
            data = json.loads(Path(path).read_text())
        except (OSError, ValueError) as e:
            raise MockError(f"cannot load state: {e}") from None
        with self._cond:
            self._need_game()
            self.frame = data["frame"]
            self.input_polls = data["input_polls"]
            self.memory.pages = {int(k): bytearray.fromhex(v) for k, v in data["pages"].items()}
            self._cond.notify_all()
        return {}

    def cmd_netplay_host(self, port: int, game: str, name: str = "host", rollback: bool = True,
                         delay: int | None = None) -> dict:
        if self._netplay is not None:
            raise MockError("already in a netplay session")
        if self.state in ("running", "paused"):
            raise MockError("cannot host netplay while a game is running")
        self._netplay = _FakeNetplay(self, "host", name, rollback=bool(rollback))
        self._netplay.host(self.netplay_bind, int(port))
        self.log_fn(f"NetPlay: hosting on port {port} game={game} rollback={rollback} delay={delay}")
        return {}

    def cmd_netplay_join(self, host: str, port: int, name: str = "joiner",
                         game: str | None = None) -> dict:
        if self._netplay is not None:
            raise MockError("already in a netplay session")
        self._netplay = _FakeNetplay(self, "client", name)
        self._netplay.join(host, int(port))
        self.log_fn(f"NetPlay: joining {host}:{port}")
        return {}

    def cmd_netplay_start(self) -> dict:
        if self._netplay is None or self._netplay.role != "host":
            raise MockError("netplay_start is only valid on the host")
        self._netplay.start_game()
        return {}

    def cmd_netplay_status(self) -> dict:
        if self._netplay is None:
            return {"role": None, "connected": False, "players": [], "game_running": False,
                    "rollback": {}}
        return self._netplay.status()

    def cmd_netplay_leave(self) -> dict:
        if self._netplay is not None:
            self._netplay.close()
            self._netplay = None
        return {}

    def cmd_log_mark(self, text: str) -> dict:
        self.log_fn(f"[HARNESS] {text}")
        return {}

    def cmd_quit(self) -> dict:
        return {}


# --------------------------------------------------------------------------- fake netplay


class _FakeNetplay:
    """Minimal UDP 'netplay' so session code can be exercised through netsim."""

    def __init__(self, server: MockHarnessServer, role: str, name: str, rollback: bool = True):
        self.server = server
        self.role = role
        self.name = name
        self.rollback = rollback
        self.sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.players: dict[int, dict[str, Any]] = {}
        self.peers: dict[tuple, int] = {}
        self.connected = False
        self.game_running = False
        self.pid = 1 if role == "host" else None
        self._closed = threading.Event()
        self._ping_sent: dict[int, float] = {}
        self._seq = 0
        self._host_addr: tuple | None = None

    def _send(self, msg: dict, addr: tuple) -> None:
        try:
            self.sock.sendto(json.dumps(msg).encode(), addr)
        except OSError:
            pass

    def host(self, bind: str, port: int) -> None:
        self.sock.bind((bind, port))
        self.players[1] = {"pid": 1, "name": self.name, "ping_ms": 0}
        self.connected = True
        self._spawn()

    def join(self, host: str, port: int) -> None:
        self.sock.bind((self.server.netplay_bind, 0))
        self._host_addr = (host, port)
        self._spawn()

    def _spawn(self) -> None:
        for target in (self._recv_loop, self._tick_loop):
            threading.Thread(target=target, daemon=True, name=f"fake-netplay-{self.role}").start()

    def _tick_loop(self) -> None:
        while not self._closed.wait(0.1):
            if self.role == "client" and not self.connected and self._host_addr:
                self._send({"t": "HELLO", "name": self.name}, self._host_addr)
            targets = list(self.peers) if self.role == "host" else (
                [self._host_addr] if self.connected and self._host_addr else [])
            for addr in targets:
                self._seq += 1
                self._ping_sent[self._seq] = time.monotonic()
                self._send({"t": "PING", "seq": self._seq}, addr)
                if self.role == "host" and self.game_running:
                    self._send({"t": "START"}, addr)

    def _recv_loop(self) -> None:
        self.sock.settimeout(0.2)
        while not self._closed.is_set():
            try:
                data, addr = self.sock.recvfrom(65536)
            except (socket.timeout, ConnectionResetError):
                continue
            except OSError:
                return
            try:
                msg = json.loads(data)
            except ValueError:
                continue
            t = msg.get("t")
            if t == "HELLO" and self.role == "host":
                pid = self.peers.get(addr)
                if pid is None:
                    pid = len(self.players) + 1
                    self.peers[addr] = pid
                    self.players[pid] = {"pid": pid, "name": msg.get("name", "?"), "ping_ms": None}
                    self.server.log_fn(f"NetPlay: player {pid} ({msg.get('name')}) joined from {addr}")
                self._send({"t": "WELCOME", "pid": pid,
                            "players": list(self.players.values())}, addr)
            elif t == "WELCOME" and self.role == "client":
                if not self.connected:
                    self.server.log_fn(f"NetPlay: connected as player {msg['pid']}")
                self.connected = True
                self.pid = msg["pid"]
                for p in msg.get("players", []):
                    self.players.setdefault(p["pid"], dict(p))
                self.players.setdefault(self.pid, {"pid": self.pid, "name": self.name, "ping_ms": 0})
            elif t == "PING":
                self._send({"t": "PONG", "seq": msg["seq"]}, addr)
            elif t == "PONG":
                sent = self._ping_sent.pop(msg.get("seq"), None)
                if sent is not None:
                    rtt = round((time.monotonic() - sent) * 1000.0, 2)
                    pid = self.peers.get(addr, 1) if self.role == "host" else 1
                    if pid in self.players:
                        self.players[pid]["ping_ms"] = rtt
            elif t == "START" and self.role == "client" and not self.game_running:
                self._begin()

    def _begin(self) -> None:
        self.game_running = True
        self.server.log_fn("NetPlay: game started")
        self.server.boot_game()

    def start_game(self) -> None:
        if not self.game_running:
            for addr in self.peers:
                self._send({"t": "START"}, addr)
            self._begin()

    def status(self) -> dict:
        return {
            "role": self.role,
            "connected": self.connected,
            "players": [dict(p) for p in sorted(self.players.values(), key=lambda p: p["pid"])],
            "game_running": self.game_running,
            "rollback": {"rollbacks": 0, "max_rollback_frames": 0, "desyncs_detected": 0,
                         "frames_resimulated": 0} if self.rollback else {},
        }

    def close(self) -> None:
        self._closed.set()
        try:
            self.sock.close()
        except OSError:
            pass


# --------------------------------------------------------------------------- CLI


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(prog="python -m ppharness.mock_server",
                                 description="Run a mock harness server (fake memory, fake frames).")
    ap.add_argument("--port", type=int, default=0)
    ap.add_argument("--no-boot", action="store_true", help="start idle (for netplay)")
    ap.add_argument("--video", default="Null")
    ap.add_argument("--speed", type=float, default=1.0)
    args = ap.parse_args(argv)
    logging.basicConfig(level=logging.INFO, format="%(message)s")
    srv = MockHarnessServer(args.port, boot=not args.no_boot, video_backend=args.video,
                            speed=args.speed)
    port = srv.start()
    print(json.dumps({"port": port}), flush=True)
    try:
        while not srv.quit_requested.wait(0.5):
            pass
    except KeyboardInterrupt:
        pass
    srv.stop()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
