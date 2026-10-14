"""Spectating (docs/design/spectating.md, docs/spectate-protocol.md): a Direct game between two
Dolphins under a local backend with spectating on (MM_SPECTATE=on). Only the first player lets
others stream their games; the second has [Online] StreamGames off and sends nothing. The first
game uploads its replay's chunks over its online connection; a third account lists the game and
watches it from accounts (/v1/live, server-sent events). What a spectator gets must be exactly the
streaming player's own replay file, chunk for chunk, without the fighter memory.
"""

from __future__ import annotations

import base64
import json
import struct
import sys
import threading
import time
import urllib.error
import urllib.request
from pathlib import Path
from typing import Any, Callable, Iterator

import pytest

from ppharness.backend import OnlineBackend, OnlineUser
from ppharness.instance import DolphinInstance

sys.path.insert(0, str(Path(__file__).resolve().parent))
from test_online import _make_backend  # noqa: E402
from test_online_game import _connected_direct  # noqa: E402
import online_set  # noqa: E402

pytestmark = [pytest.mark.dolphin, pytest.mark.server, pytest.mark.gpu]

STREAMED = {b"INFO", b"SETP", b"SYNC", b"FRMS", b"EVNT", b"ENDG"}


@pytest.fixture(scope="module")
def spectate_backend() -> Iterator[OnlineBackend]:
    be = _make_backend(spectate="on")
    try:
        yield be
    finally:
        be.stop()


def file_chunks(path: Path) -> list[bytes]:
    """The whole chunks (header and stored bytes) of a .brep, in file order."""
    data = path.read_bytes()
    header = struct.unpack_from("<I", data, 12)[0]
    at, out = header, []
    while at + 20 <= len(data):
        kind = data[at:at + 4]
        if kind == b"BRPT":
            break
        stored = struct.unpack_from("<I", data, at + 8)[0]
        out.append(data[at:at + 20 + stored])
        at += 20 + stored
    return out


def get_json(be: OnlineBackend, path: str, token: str) -> Any:
    req = urllib.request.Request(be.accounts_url + path, headers={"Authorization": f"Bearer {token}"})
    with urllib.request.urlopen(req, timeout=10) as r:
        return json.load(r)


def watch(be: OnlineBackend, stream: str, token: str, until: Callable[[dict[str, Any]], bool],
          timeout: float = 60) -> list[dict[str, Any]]:
    """The watch stream's events until `until` accepts one (that one included)."""
    req = urllib.request.Request(be.accounts_url + f"/v1/live/{stream}/watch",
                                 headers={"Authorization": f"Bearer {token}"})
    events: list[dict[str, Any]] = []
    deadline = time.monotonic() + timeout
    with urllib.request.urlopen(req, timeout=30) as r:
        data: list[str] = []
        for raw in r:
            line = raw.decode("utf-8").rstrip("\r\n")
            if line.startswith("data:"):
                data.append(line[5:].lstrip())
            elif line == "" and data:
                ev = json.loads("".join(data))
                data = []
                events.append(ev)
                if until(ev):
                    return events
            assert time.monotonic() < deadline, f"watch timed out after {len(events)} events"
    raise AssertionError(f"the watch stream ended early: {events[-3:]}")


class ListPoller:
    """Polls /v1/live every second in the background; keeps every entry seen."""

    def __init__(self, be: OnlineBackend, token: str):
        self.be, self.token = be, token
        self.seen: list[dict[str, Any]] = []
        self._stop = threading.Event()
        self._thread = threading.Thread(target=self._run, daemon=True)
        self._thread.start()

    def _run(self) -> None:
        while not self._stop.wait(1.0):
            try:
                self.seen.extend(get_json(self.be, "/v1/live", self.token)["games"])
            except Exception:  # noqa: BLE001 - a missed poll only loses one sample
                pass

    def stop(self) -> None:
        self._stop.set()
        self._thread.join(5)


@pytest.mark.slow
def test_a_direct_game_streams_from_the_player_who_allows_it(
        spectate_backend: OnlineBackend, dolphin: Callable[..., DolphinInstance], gpu_backend: str) -> None:
    be = spectate_backend
    carol: OnlineUser = be.create_user("carol", "CARO")
    a, b, ua, ub = _connected_direct(be, dolphin, gpu_backend, "spectate-direct", ("sora", "tobi"),
                                     stocks=1, dolphin_ini=(None, {"Online": {"StreamGames": False}}))
    poller = ListPoller(be, carol.session_token)
    try:
        online_set.play_set([a, b], games=1, mode="direct", panels={g.name: g.panel for g in (a, b)})
        online_set.wait(lambda: not any(g.c.call("replay_recording")["recording"] for g in (a, b)), 60,
                        "both recordings to end")
        # The upload goes on after the game (2 KB/s): until everything is sent.
        online_set.wait(lambda: (lambda s: s.get("game") is not None and s["sent"] == s["chunks"])(
            a.c.call("spectate_status")), 120, "the upload to finish")
    finally:
        poller.stop()

    sa, sb = a.c.call("spectate_status"), b.c.call("spectate_status")
    # Pauses are the uploader keeping out of the game's way (on a busy test PC they may happen);
    # stopping for the game would lose spectators the rest of it.
    print("uploader:", sa)
    assert sa["role"] == "primary" and not sa["stopped"], sa
    assert sb["game"] is None, f"StreamGames off: the second game offered nothing ({sb})"

    # Listed while it played, with both players and the right mode.
    playing = [g for g in poller.seen if g["game"]["state"] == "playing"]
    assert playing, f"never listed while playing: {poller.seen[-3:]}"
    entry = playing[-1]
    assert entry["mode"] == "direct"
    assert {p["uid"] for p in entry["players"]} == {ua.uid, ub.uid}
    assert {p["displayName"] for p in entry["players"]} == {"sora", "tobi"}

    # A player may not watch their own game.
    try:
        get_json(be, f"/v1/live/{entry['id']}/watch", ua.session_token)
        raise AssertionError("a player could watch their own game")
    except urllib.error.HTTPError as e:
        assert e.code == 403

    # What a spectator gets is the streaming player's replay without fighter memory, byte for byte.
    events = watch(be, entry["id"], carol.session_token, lambda ev: ev["type"] in ("end", "bad"))
    assert events[0]["type"] == "welcome", events[0]
    assert events[-1]["type"] == "end", events[-1]
    streamed = [base64.b64decode(ev["b"]) for ev in events if ev["type"] == "chunk"]
    rec = Path(a.c.call("replay_recording")["path"])
    expected = [c for c in file_chunks(rec) if c[:4] in STREAMED]
    assert len(streamed) == len(expected), (len(streamed), len(expected))
    assert streamed == expected
    assert any(c[:4] == b"FTRM" for c in file_chunks(rec)), "the file itself keeps fighter memory"

    for g in (a, b):
        g.c.mm_cancel()
