"""HarnessClient against the in-process mock server."""

from __future__ import annotations

import json
import socket
import threading
import time
from pathlib import Path

import pytest

from ppharness import protocol
from ppharness.client import (
    HarnessBusyError,
    HarnessClient,
    HarnessCommandError,
    HarnessConnectionError,
    HarnessProtocolError,
    HarnessTimeoutError,
    PadInput,
    VersionMismatchError,
    WaitTimeoutError,
)
from ppharness.mock_server import (
    GAME_FRAME_ADDR,
    GAME_PADS_ADDR,
    GAME_RANGES,
    MockHarnessServer,
)


@pytest.fixture
def server():
    srv = MockHarnessServer(speed=4.0)  # ~240 fps keeps the tests quick
    srv.start()
    yield srv
    srv.stop()


@pytest.fixture
def client(server):
    c = HarnessClient.connect(server.port, timeout=5.0)
    yield c
    c.close()


# --------------------------------------------------------------------------- protocol helpers


def test_parse_addr_and_ranges():
    assert protocol.parse_addr("0x80001000") == 0x80001000
    assert protocol.parse_addr("2147483648") == 0x80000000
    assert protocol.parse_addr(0x90000000) == 0x90000000
    with pytest.raises(ValueError):
        protocol.parse_addr("0x1_0000_0000")
    with pytest.raises(TypeError):
        protocol.parse_addr(True)  # type: ignore[arg-type]
    protocol.check_range(0x817FFFFC, 4)
    with pytest.raises(ValueError):
        protocol.check_range(0x817FFFFE, 4)       # crosses the end of MEM1
    with pytest.raises(ValueError):
        protocol.check_range(0x80000000, 16 * 1024 * 1024 + 1)
    assert protocol.normalize_ranges([("0x80000000", 4)]) == [[0x80000000, 4]]


def test_pad_input_validation():
    assert PadInput(buttons=["a", "start"]).to_json() == {
        "buttons": ["A", "START"], "main": [128, 128], "c": [128, 128], "l": 0, "r": 0}
    with pytest.raises(ValueError):
        PadInput(buttons=["JUMP"]).to_json()
    with pytest.raises(ValueError):
        PadInput(main=(0, 256)).to_json()
    with pytest.raises(ValueError):
        PadInput(hold=0).to_json(include_hold=True)


# --------------------------------------------------------------------------- basics


def test_ping_and_status(client, server):
    assert client.ping() == 1
    st = client.status()
    assert st.state == "running"
    assert st.game_id == "RSBE01"
    assert st.video_backend == "Null"
    assert st.netplay is None
    assert st.frame >= 0 and st.input_polls >= 0


def test_version_mismatch(server, monkeypatch):
    monkeypatch.setattr(server, "cmd_ping", lambda: {"pong": True, "version": 99})
    with pytest.raises(VersionMismatchError):
        HarnessClient.connect(server.port)


def test_memory_roundtrip(client):
    client.write_mem(0x80400000, b"\x12\x34\x56\x78\x9a")
    assert client.read_mem("0x80400000", 5) == b"\x12\x34\x56\x78\x9a"
    assert client.read_u32(0x80400000) == 0x12345678
    assert client.read_u16(0x80400002) == 0x5678
    client.write_mem(0x90000010, "3f800000")
    assert client.read_f32(0x90000010) == 1.0


def test_hash_mem(client):
    client.pause()
    h1 = client.hash_mem([[0x80400000, 64], ["0x90000000", 16]])
    assert len(h1) == 16 and int(h1, 16) >= 0
    assert client.hash_mem([[0x80400000, 64], [0x90000000, 16]]) == h1
    client.write_mem(0x80400000, b"\x01")
    assert client.hash_mem([[0x80400000, 64], [0x90000000, 16]]) != h1


def test_client_side_address_check(client, server):
    n = len(server.commands)
    with pytest.raises(ValueError):
        client.read_mem(0x70000000, 4)
    assert len(server.commands) == n  # never sent
    client.strict_addresses = False
    with pytest.raises(HarnessCommandError, match="MEM1"):
        client.read_mem(0x70000000, 4)


def test_command_error_is_clear(client):
    with pytest.raises(HarnessCommandError) as ei:
        client.call("no_such_command")
    assert ei.value.cmd == "no_such_command"
    assert "unknown command" in ei.value.message
    # the connection is still usable afterwards
    assert client.ping() == 1


# --------------------------------------------------------------------------- frames


def test_wait_frame(client):
    st = client.status()
    r = client.wait_frame(st.frame + 10)
    assert r.frame >= st.frame + 10
    r2 = client.wait_frame(input_polls=r.input_polls + 5)
    assert r2.input_polls >= r.input_polls + 5
    with pytest.raises(ValueError):
        client.wait_frame()
    with pytest.raises(ValueError):
        client.wait_frame(1, input_polls=1)


def test_wait_frame_server_timeout(client):
    client.pause()
    st = client.status()
    with pytest.raises(WaitTimeoutError):
        client.wait_frame(st.frame + 5, timeout_ms=100)
    # WaitTimeoutError is both a command error and a timeout
    assert issubclass(WaitTimeoutError, HarnessTimeoutError)
    assert issubclass(WaitTimeoutError, HarnessCommandError)


def test_pause_frame_advance_resume(client):
    client.pause()
    assert client.status().state == "paused"
    f0 = client.status().frame
    time.sleep(0.1)
    assert client.status().frame == f0           # really paused
    assert client.frame_advance(3) == f0 + 3
    assert client.frame_advance() == f0 + 4
    client.resume()
    client.wait_frame(f0 + 10)
    with pytest.raises(HarnessCommandError, match="paused"):
        client.frame_advance(1)


# --------------------------------------------------------------------------- pads


def test_pad_set_and_clear(client, server):
    client.pad_set(0, buttons=["A", "B"], main=(255, 128), l=200)
    st = client.wait_frame(input_polls=client.status().input_polls + 2)
    idx, pads = server.pad_history[-1]
    assert set(pads[0][0]) == {"A", "B"} and pads[0][1] == (255, 128) and pads[0][3] == 200
    # the fake game mirrors port 0's pad into memory: buttons mask A|B = 0b11
    assert client.read_mem(GAME_PADS_ADDR, 2) == b"\x00\x03"
    client.pad_clear(0)
    client.wait_frame(input_polls=st.input_polls + 3)
    assert server.pad_history[-1][1][0][0] == ()


def test_pad_script_exact_polls(client, server):
    client.pause()
    p0 = client.status().input_polls
    frames = [PadInput(buttons=["A"], hold=2), {"buttons": ["B"]}, PadInput(main=(0, 0), hold=3)]
    win = client.pad_script(1, frames, start=p0 + 5)
    assert (win.starts_at, win.ends_at) == (p0 + 5, p0 + 5 + 6)
    st = client.pad_script_status(1)
    assert st.active and st.remaining == 11
    client.resume()
    client.wait_frame(input_polls=win.ends_at + 2)
    played = {idx: pads[1] for idx, pads in server.pad_history if p0 <= idx < win.ends_at + 2}
    seq = [played[i] for i in range(win.starts_at - 1, win.ends_at + 1)]
    assert seq[0][0] == ()                           # before the script: neutral
    assert [s[0] for s in seq[1:3]] == [("A",), ("A",)]
    assert seq[3][0] == ("B",)
    assert all(s[1] == (0, 0) for s in seq[4:7])
    assert seq[7][0] == () and seq[7][1] == (128, 128)  # after: back to neutral
    assert client.pad_script_status(1).active is False


def test_pad_script_errors(client, server):
    with pytest.raises(HarnessCommandError, match="too early"):
        client.wait_frame(input_polls=2)
        client.pad_script(0, [PadInput()], start=0)
    server.device_ports.discard(3)
    with pytest.raises(HarnessCommandError, match="no SI device"):
        client.pad_set(3, buttons=["A"])
    with pytest.raises(ValueError):
        client.pad_set(4)
    with pytest.raises(ValueError):
        client.pad_script(0, [])


# --------------------------------------------------------------------------- misc commands


def test_screenshot_null_backend(client, tmp_path):
    with pytest.raises(HarnessCommandError, match="Null"):
        client.screenshot(tmp_path / "x.png")
    with pytest.raises(ValueError):
        client.screenshot(tmp_path / "x.jpg")


def test_screenshot_writes_png(tmp_path):
    with MockHarnessServer(video_backend="D3D11") as srv:
        with HarnessClient.connect(srv.port) as c:
            shot = c.screenshot(tmp_path / "sub" / "shot.png")
    assert shot.path.read_bytes().startswith(b"\x89PNG")
    assert (shot.width, shot.height) == (64, 48)


def test_save_and_load_state(client, tmp_path):
    client.pause()
    client.write_mem(0x80500000, b"\xaa\xbb")
    client.save_state(tmp_path / "s.state")
    client.write_mem(0x80500000, b"\x00\x00")
    client.load_state(tmp_path / "s.state")
    assert client.read_mem(0x80500000, 2) == b"\xaa\xbb"


def test_log_mark(server):
    lines: list[str] = []
    server.log_fn = lines.append
    with HarnessClient.connect(server.port) as c:
        c.log_mark("step 1")
    assert "[HARNESS] step 1" in lines


def test_quit_closes(server):
    c = HarnessClient.connect(server.port)
    c.quit()
    assert not c.connected
    assert server.quit_requested.wait(2)


# --------------------------------------------------------------------------- connection handling


def test_second_client_is_refused(server, client):
    with pytest.raises(HarnessBusyError):
        HarnessClient.connect(server.port, timeout=2)
    assert server.refused_connections == 1
    assert client.ping() == 1  # the first client is unaffected
    client.close()
    time.sleep(0.1)
    with HarnessClient.connect(server.port) as c2:   # free again
        assert c2.ping() == 1


def test_connect_with_retry_waits_for_server():
    with socket.socket() as s:
        s.bind(("127.0.0.1", 0))
        port = s.getsockname()[1]
    srv = MockHarnessServer(port)
    t = threading.Timer(0.6, srv.start)
    t.start()
    try:
        t0 = time.monotonic()
        c = HarnessClient.connect_with_retry(port, total_timeout=10, interval=0.05)
        assert time.monotonic() - t0 >= 0.5
        assert c.ping() == 1
        c.close()
    finally:
        t.join()
        srv.stop()


def test_connect_with_retry_gives_up():
    with socket.socket() as s:
        s.bind(("127.0.0.1", 0))
        port = s.getsockname()[1]
    t0 = time.monotonic()
    with pytest.raises(HarnessConnectionError, match="no harness server"):
        HarnessClient.connect_with_retry(port, total_timeout=0.5, interval=0.05)
    assert time.monotonic() - t0 < 3


def test_connect_with_retry_stops_when_process_dies():
    with socket.socket() as s:
        s.bind(("127.0.0.1", 0))
        port = s.getsockname()[1]
    t0 = time.monotonic()
    with pytest.raises(HarnessConnectionError, match="process exited"):
        HarnessClient.connect_with_retry(port, total_timeout=30, is_alive=lambda: False)
    assert time.monotonic() - t0 < 1


class _ScriptedServer:
    """A raw TCP server answering with canned lines, for protocol edge cases."""

    def __init__(self, handler):
        self.sock = socket.socket()
        self.sock.bind(("127.0.0.1", 0))
        self.sock.listen(1)
        self.port = self.sock.getsockname()[1]
        self.thread = threading.Thread(target=self._run, args=(handler,), daemon=True)
        self.thread.start()

    def _run(self, handler):
        conn, _ = self.sock.accept()
        f = conn.makefile("rb")
        try:
            handler(conn, f)
        finally:
            time.sleep(0.2)
            conn.close()
            self.sock.close()


def _reply(conn, req, result=None, **kw):
    msg = {"id": req["id"], "ok": True, "result": result or {}}
    msg.update(kw)
    conn.sendall((json.dumps(msg) + "\n").encode())


def test_late_response_after_timeout_is_discarded():
    def handler(conn, f):
        ping = json.loads(f.readline())
        _reply(conn, ping, {"pong": True, "version": 1})
        slow = json.loads(f.readline())          # status: answer too late
        time.sleep(0.5)
        _reply(conn, slow, {"state": "running", "frame": 1})
        nxt = json.loads(f.readline())
        _reply(conn, nxt, {"state": "paused", "frame": 2})

    srv = _ScriptedServer(handler)
    c = HarnessClient.connect(srv.port, timeout=2)
    with pytest.raises(HarnessTimeoutError):
        c.call("status", timeout=0.2)
    st = c.status()                               # must not get the stale answer
    assert st.state == "paused" and st.frame == 2
    c.close()


def test_garbage_and_split_lines():
    def handler(conn, f):
        ping = json.loads(f.readline())
        line = json.dumps({"id": ping["id"], "ok": True, "result": {"pong": True, "version": 1}})
        conn.sendall(line[:10].encode())
        time.sleep(0.1)
        conn.sendall((line[10:] + "\n").encode())
        st = json.loads(f.readline())
        conn.sendall(b"this is not json\n")

    srv = _ScriptedServer(handler)
    c = HarnessClient.connect(srv.port, timeout=2)
    with pytest.raises(HarnessProtocolError):
        c.status()


def test_connection_lost_mid_request():
    def handler(conn, f):
        ping = json.loads(f.readline())
        _reply(conn, ping, {"pong": True, "version": 1})
        f.readline()
        conn.shutdown(socket.SHUT_RDWR)

    srv = _ScriptedServer(handler)
    c = HarnessClient.connect(srv.port, timeout=2)
    with pytest.raises(HarnessConnectionError, match="connection lost"):
        c.status()
    assert not c.connected
    with pytest.raises(HarnessConnectionError, match="not connected"):
        c.status()


def test_large_read(client):
    data = bytes(range(256)) * 4096  # 1 MiB
    client.write_mem(0x90100000, data)
    assert client.read_mem(0x90100000, len(data)) == data


def test_fake_game_state_advances(client):
    client.pause()
    gf = client.read_u32(GAME_FRAME_ADDR)
    client.frame_advance(2)
    assert client.read_u32(GAME_FRAME_ADDR) == gf + 2
    h = client.hash_mem(GAME_RANGES)
    client.frame_advance(1)
    assert client.hash_mem(GAME_RANGES) != h
