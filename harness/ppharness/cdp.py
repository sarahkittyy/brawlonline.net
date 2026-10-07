"""A minimal Chrome DevTools Protocol client (standard library only).

Used to drive the Electron launcher's renderer in end-to-end runs: evaluate JavaScript, set the
files of an ``<input type=file>``, and take screenshots of the app window only (``Page.
captureScreenshot`` renders the page itself, never the desktop).

    targets = cdp.list_targets(port)              # http://127.0.0.1:<port>/json
    with cdp.CdpSession.connect(port) as s:
        s.evaluate("document.title")
        s.screenshot(Path("launcher.png"))

The launcher exposes the protocol with ``--remote-debugging-port`` (``PPO_REMOTE_DEBUGGING_PORT``
in its test mode, launcher/PPLUS_PORTING.md).
"""

from __future__ import annotations

import base64
import json
import os
import socket
import struct
import threading
import time
import urllib.parse
import urllib.request
from pathlib import Path
from typing import Any


class CdpError(RuntimeError):
    pass


def list_targets(port: int, host: str = "127.0.0.1", timeout: float = 2.0) -> list[dict[str, Any]]:
    with urllib.request.urlopen(f"http://{host}:{port}/json", timeout=timeout) as resp:
        return json.loads(resp.read().decode())


def wait_for_page(port: int, timeout: float = 60.0, url_prefix: str = "") -> dict[str, Any]:
    """The first ``page`` target that is not DevTools itself (and starts with url_prefix)."""
    deadline = time.monotonic() + timeout
    last: Any = None
    while time.monotonic() < deadline:
        try:
            for t in list_targets(port):
                url = t.get("url", "")
                if t.get("type") == "page" and not url.startswith("devtools://") and \
                        url.startswith(url_prefix) and t.get("webSocketDebuggerUrl"):
                    return t
            last = "no page target yet"
        except OSError as e:
            last = e
        time.sleep(0.25)
    raise CdpError(f"no page target on port {port} after {timeout:.0f}s ({last})")


class _WebSocket:
    """Just enough of RFC 6455 for CDP: text frames, client masking, fragmentation, ping/pong."""

    def __init__(self, url: str, timeout: float = 30.0):
        u = urllib.parse.urlparse(url)
        self.sock = socket.create_connection((u.hostname, u.port or 80), timeout=timeout)
        key = base64.b64encode(os.urandom(16)).decode()
        path = u.path + (f"?{u.query}" if u.query else "")
        req = (f"GET {path} HTTP/1.1\r\nHost: {u.hostname}:{u.port}\r\nUpgrade: websocket\r\n"
               f"Connection: Upgrade\r\nSec-WebSocket-Key: {key}\r\nSec-WebSocket-Version: 13\r\n\r\n")
        self.sock.sendall(req.encode())
        head = b""
        while b"\r\n\r\n" not in head:
            chunk = self.sock.recv(4096)
            if not chunk:
                raise CdpError("websocket handshake: connection closed")
            head += chunk
        status = head.split(b"\r\n", 1)[0]
        if b" 101 " not in status:
            raise CdpError(f"websocket handshake failed: {status!r}")
        self._buf = head.split(b"\r\n\r\n", 1)[1]
        self._send_lock = threading.Lock()

    def _recv_exact(self, n: int) -> bytes:
        while len(self._buf) < n:
            chunk = self.sock.recv(max(65536, n - len(self._buf)))
            if not chunk:
                raise CdpError("websocket closed")
            self._buf += chunk
        out, self._buf = self._buf[:n], self._buf[n:]
        return out

    def send_text(self, text: str) -> None:
        self._send(0x1, text.encode())

    def _send(self, opcode: int, data: bytes) -> None:
        header = bytearray([0x80 | opcode])
        n = len(data)
        if n < 126:
            header.append(0x80 | n)
        elif n < 65536:
            header.append(0x80 | 126)
            header += struct.pack(">H", n)
        else:
            header.append(0x80 | 127)
            header += struct.pack(">Q", n)
        mask = os.urandom(4)
        header += mask
        masked = bytes(b ^ mask[i & 3] for i, b in enumerate(data))
        with self._send_lock:
            self.sock.sendall(bytes(header) + masked)

    def recv_text(self) -> str:
        parts: list[bytes] = []
        while True:
            b0, b1 = self._recv_exact(2)
            opcode = b0 & 0x0F
            n = b1 & 0x7F
            if n == 126:
                n = struct.unpack(">H", self._recv_exact(2))[0]
            elif n == 127:
                n = struct.unpack(">Q", self._recv_exact(8))[0]
            mask = self._recv_exact(4) if b1 & 0x80 else None
            data = self._recv_exact(n)
            if mask:
                data = bytes(b ^ mask[i & 3] for i, b in enumerate(data))
            if opcode == 0x9:  # ping
                self._send(0xA, data)
                continue
            if opcode == 0x8:
                raise CdpError("websocket closed by the peer")
            if opcode in (0x0, 0x1, 0x2):
                parts.append(data)
                if b0 & 0x80:
                    return b"".join(parts).decode()

    def close(self) -> None:
        try:
            self._send(0x8, b"")
        except OSError:
            pass
        self.sock.close()


class CdpSession:
    """One page's DevTools session. Commands are synchronous; events are ignored."""

    def __init__(self, ws_url: str, timeout: float = 30.0):
        self.ws = _WebSocket(ws_url, timeout=timeout)
        self.timeout = timeout
        self._id = 0

    @classmethod
    def connect(cls, port: int, timeout: float = 60.0) -> "CdpSession":
        return cls(wait_for_page(port, timeout)["webSocketDebuggerUrl"])

    def __enter__(self) -> "CdpSession":
        return self

    def __exit__(self, *exc: Any) -> None:
        self.close()

    def close(self) -> None:
        self.ws.close()

    def send(self, method: str, params: dict[str, Any] | None = None,
             timeout: float | None = None) -> dict[str, Any]:
        self._id += 1
        my_id = self._id
        self.ws.send_text(json.dumps({"id": my_id, "method": method, "params": params or {}}))
        self.ws.sock.settimeout(timeout or self.timeout)
        while True:
            msg = json.loads(self.ws.recv_text())
            if msg.get("id") == my_id:
                if "error" in msg:
                    raise CdpError(f"{method}: {msg['error']}")
                return msg.get("result", {})

    def evaluate(self, expression: str, timeout: float | None = None) -> Any:
        r = self.send("Runtime.evaluate", {"expression": expression, "awaitPromise": True,
                                           "returnByValue": True}, timeout=timeout)
        if r.get("exceptionDetails"):
            raise CdpError(f"JS exception: {json.dumps(r['exceptionDetails'])[:800]}")
        return r.get("result", {}).get("value")

    def set_file_input(self, selector: str, files: list[str]) -> None:
        """What choosing files in the native dialog does: sets the input's files (real paths)."""
        doc = self.send("DOM.getDocument", {"depth": 0})
        node = self.send("DOM.querySelector", {"nodeId": doc["root"]["nodeId"], "selector": selector})
        if not node.get("nodeId"):
            raise CdpError(f"no element matches {selector!r}")
        self.send("DOM.setFileInputFiles", {"nodeId": node["nodeId"], "files": files})

    def screenshot(self, path: Path, attempts: int = 6) -> Path:
        """PNG of the page (the app window's content) only."""
        path = Path(path)
        path.parent.mkdir(parents=True, exist_ok=True)
        for _ in range(attempts):
            self.send("Page.bringToFront")
            # An occluded window produces no frames; ask for one first.
            try:
                self.evaluate("new Promise((r) => requestAnimationFrame(() => r(1)))", timeout=5)
            except (CdpError, OSError):
                pass
            try:
                r = self.send("Page.captureScreenshot", {"format": "png"}, timeout=10)
            except (CdpError, OSError, socket.timeout):
                continue
            path.write_bytes(base64.b64decode(r["data"]))
            return path
        raise CdpError(f"screenshot {path} timed out")
