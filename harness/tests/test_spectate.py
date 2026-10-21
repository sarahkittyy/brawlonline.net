"""Spectating (docs/design/spectating.md, docs/spectate-protocol.md): a Direct game between two
Dolphins under a local backend with spectating on (MM_SPECTATE=on). Only the first player lets
others stream their games; the second has [Online] StreamGames off and sends nothing. The first
game uploads its replay's chunks over its online connection; a third account lists the game and
watches it from accounts (/v1/live, server-sent events). What a spectator gets must be exactly the
streaming player's own replay file, chunk for chunk, without the fighter memory.

Watching (§6.2): a fresh playback Dolphin follows the stream as the launcher's does, through a
`mirror` comm file and the growing files StreamWriter writes, joining a set mid-game.
"""

from __future__ import annotations

import base64
import json
import os
import struct
import sys
import threading
import time
import types
import urllib.error
import urllib.request
import zlib
from pathlib import Path
from typing import Any, Callable, Iterator

import pytest

from ppharness.backend import OnlineBackend, OnlineUser
from ppharness.instance import DolphinInstance, InstanceConfig

sys.path.insert(0, str(Path(__file__).resolve().parent))
from test_online import _make_backend  # noqa: E402
from test_online_game import PLUGIN, ROOT, Game, _connected_direct  # noqa: E402
import online_set  # noqa: E402
import patch_sd  # noqa: E402
from ppharness import replay as R  # noqa: E402

ARTIFACTS = ROOT / "run" / "artifacts" / "spectate"

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


def watch_events(be: OnlineBackend, stream: str, token: str) -> Iterator[dict[str, Any]]:
    """The watch stream's events as they come."""
    req = urllib.request.Request(be.accounts_url + f"/v1/live/{stream}/watch",
                                 headers={"Authorization": f"Bearer {token}"})
    with urllib.request.urlopen(req, timeout=60) as r:
        data: list[str] = []
        for raw in r:
            line = raw.decode("utf-8").rstrip("\r\n")
            if line.startswith("data:"):
                data.append(line[5:].lstrip())
            elif line == "" and data:
                ev = json.loads("".join(data))
                data = []
                yield ev


def watch(be: OnlineBackend, stream: str, token: str, until: Callable[[dict[str, Any]], bool],
          timeout: float = 60) -> list[dict[str, Any]]:
    """The watch stream's events until `until` accepts one (that one included)."""
    events: list[dict[str, Any]] = []
    deadline = time.monotonic() + timeout
    for ev in watch_events(be, stream, token):
        events.append(ev)
        if until(ev):
            return events
        assert time.monotonic() < deadline, f"watch timed out after {len(events)} events"
    raise AssertionError(f"the watch stream ended early: {events[-3:]}")


BREP_MAGIC = b"\x89BRP\r\n\x1a\n"


class StreamWriter:
    """What the launcher does with a watch stream (docs/design/spectating.md §3.3): each game of
    the stream into its own growing .brep (the header, then every chunk as it comes, the trailer
    at the game's end), and the `mirror` comm file the playback Dolphin follows: the games so far,
    those whose rest won't come (`cut`: the stream said `bad`, its uploader stopped), and whether
    the stream is over."""

    def __init__(self, be: OnlineBackend, token: str, out: Path, comm: Path, close_on_end: bool = True):
        self.be, self.token, self.out, self.comm = be, token, out, comm
        self.close_on_end = close_on_end
        self.paths: list[Path] = [out / "1.brep"]   # the first game's, before it exists
        self.cut: set[Path] = set()
        self.over = False
        self.error: BaseException | None = None
        self.stream: str | None = None
        self.write_comm()
        self._thread = threading.Thread(target=self._run, daemon=True)
        self._thread.start()

    def write_comm(self) -> None:
        tmp = self.comm.with_suffix(".tmp")
        tmp.write_text(json.dumps({
            "mode": "mirror", "commandId": "watch-1",
            "queue": [{"path": str(p), "cut": p in self.cut} for p in self.paths],
            "streamOver": self.over, "closeOnEnd": self.close_on_end}))
        os.replace(tmp, self.comm)

    def _run(self) -> None:
        try:
            while self.stream is None:
                games = get_json(self.be, "/v1/live", self.token)["games"]
                if games:
                    self.stream = games[0]["id"]
                else:
                    time.sleep(1)
            files: dict[int, Any] = {}
            meta: dict[int, dict[str, int]] = {}
            for ev in watch_events(self.be, self.stream, self.token):
                t = ev["type"]
                if t == "game":
                    n = ev["game"]
                    for m, f in files.items():
                        if m < n and not f.closed:
                            f.close()
                            self.cut.add(self.out / f"{m}.brep")
                    path = self.out / f"{n}.brep"
                    f = path.open("wb")
                    f.write(BREP_MAGIC + struct.pack("<HHI", 1, 0, 32) + bytes.fromhex(ev["uid"]))
                    f.flush()
                    files[n], meta[n] = f, {"at": 32, "last": -1, "endg": -1}
                    if path not in self.paths:
                        self.paths.append(path)
                        self.write_comm()
                elif t == "chunk":
                    n, b = ev["game"], base64.b64decode(ev["b"])
                    m = meta[n]
                    if b[:4] == b"FRMS":
                        raw = zlib.decompress(b[20:])
                        first, count = struct.unpack_from("<IH", raw, 0)
                        m["last"] = first + count - 1
                    elif b[:4] == b"ENDG":
                        m["endg"] = m["at"]
                    files[n].write(b)
                    files[n].flush()
                    m["at"] += len(b)
                elif t == "bad":
                    print("stream: game", ev["game"], "cut:", ev.get("error"), flush=True)
                    if not files[ev["game"]].closed:
                        files[ev["game"]].close()
                    self.cut.add(self.out / f"{ev['game']}.brep")
                    self.write_comm()
                elif t == "end":
                    n, m = ev["game"], meta[ev["game"]]
                    body = b"BRPT" + struct.pack("<II", m["endg"], m["last"])
                    files[n].write(body + struct.pack("<I", zlib.crc32(body)))
                    files[n].close()
                elif t == "over":
                    break
            for m, f in files.items():
                if not f.closed:
                    f.close()
                    self.cut.add(self.out / f"{m}.brep")
            self.over = True
            self.write_comm()
        except BaseException as e:  # noqa: BLE001 - reported by the test
            self.error = e


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


def _viewer(be: OnlineBackend, dolphin: Callable[..., DolphinInstance], gpu_backend: str,
            comm: Path, out: Path) -> Game:
    """The launcher's playback Dolphin: `-i <comm>`, no account, booted from the menus."""
    cfg = InstanceConfig(cpu_thread=True, video_backend=gpu_backend, extra_args=("-i", str(comm)),
                         dolphin_ini={"Online": {"UseDevServer": True, "MatchmakingPort": be.mm_port,
                                                 "DevAccountsUrl": be.accounts_url}})
    inst = dolphin("viewer", config=cfg, client_timeout=60.0)
    d = inst.create()
    patch_sd.patch_image(d / "Wii" / "sd.raw", [(PLUGIN.read_bytes(), patch_sd.plugin_sd_path(PLUGIN))])
    inst.launch()
    inst.connect()
    return Game(inst, types.SimpleNamespace(display_name="viewer"), out / "viewer")


class Sampler:
    """The viewer's playback status twice a second, kept (what it went through). Once the set's
    second game has played 5 s, it presses Go Live once (the harness client is this thread's)."""

    def __init__(self, v: Game, status_file: Path):
        self.v, self.status_file = v, status_file
        self.samples: list[dict[str, Any]] = []
        self.went_live_at: int | None = None   # the sample index when Go Live was pressed
        self.clicked: dict[str, Any] = {}      # click_during_catch_up's statuses
        self._stop = threading.Event()
        self._thread = threading.Thread(target=self._run, daemon=True)
        self._thread.start()

    def _run(self) -> None:
        while not self._stop.wait(0.5):
            try:
                st = self.v.c.call("replay_status")
                try:
                    st["file"] = json.loads(self.status_file.read_text())
                except (OSError, ValueError):
                    st["file"] = {}
                self.samples.append(st)
                if (self.went_live_at is None and st["file"].get("index") == 1 and st.get("started")
                        and not st.get("complete", True) and st.get("shown", -1) >= 300):
                    self.v.c.call("replay_control", action="live")
                    self.went_live_at = len(self.samples)
                    self.click_during_catch_up()
            except Exception:  # noqa: BLE001 - a missed sample
                pass

    def click_during_catch_up(self) -> None:
        """A click on the game while it catches up ("Catching up to live (N%). Click to watch",
        the user 2026-10-21) is Play: the catch-up stops where it is and the game plays on at 1x
        from there, not paused. Then Go Live again, for the rest of the test."""
        deadline = time.monotonic() + 5
        st: dict[str, Any] = {}
        while time.monotonic() < deadline:
            st = self.v.c.call("replay_status")
            if st.get("catching_up") and st.get("shown", -1) >= 0:
                break
            time.sleep(0.02)
        else:
            self.clicked = {"error": f"never seen catching up: {st}"}
            return
        try:
            self.v.shot("catching-up")
        except Exception:  # noqa: BLE001 - the picture is only for looking at
            pass
        self.v.c.call("replay_control", action="play")
        at = self.v.c.call("replay_status")
        time.sleep(2)
        later = self.v.c.call("replay_status")
        self.clicked = {"before": st, "at": at, "later": later}
        # (Kept as samples too: this thread took no others meanwhile.)
        for s in (st, at, later):
            try:
                s["file"] = json.loads(self.status_file.read_text())
            except (OSError, ValueError):
                s["file"] = {}
            self.samples.append(s)
        self.v.c.call("replay_control", action="live")

    def stop(self) -> None:
        self._stop.set()
        self._thread.join(5)

    def error(self) -> str | None:
        """The viewer's error, once it has one (it never recovers from it: fail at once)."""
        for s in self.samples:
            f = s.get("file", {})
            if f.get("state") == "error":
                return f"{f.get('error')} (needsPlayAnyway {f.get('needsPlayAnyway')})"
        return None


@pytest.mark.slow
def test_a_spectator_joins_a_set_mid_game_and_watches_it_live(
        spectate_backend: OnlineBackend, dolphin: Callable[..., DolphinInstance], gpu_backend: str) -> None:
    """A two-game Direct set on the server's rules, streamed by the first player. A playback
    Dolphin booted while game 1 runs follows the stream in `mirror` mode: it waits for the game's
    first frames (catching up to live if game 1 was well under way by then), takes game 2 when it
    is queued and plays it from its start at normal speed behind live, catches up when Go Live is
    pressed, and at the stream's end holds the last frame, counts down and asks to be closed. Every frame it runs
    matches the recorded checksum. On a busy PC the uploader may stop for a game (players first,
    docs/design/spectating.md §4.3): then the viewer plays what came and goes on to the next."""
    be = spectate_backend
    dana: OnlineUser = be.create_user("dana", "DANA")
    out = ARTIFACTS / "watch"
    out.mkdir(parents=True, exist_ok=True)
    for old in out.glob("*.brep"):
        old.unlink()
    comm = out / "comm.json"
    status_file = Path(str(comm) + ".status.json")
    status_file.unlink(missing_ok=True)

    a, b, _, _ = _connected_direct(be, dolphin, gpu_backend, "spectate-watch", ("sora", "tobi"),
                                   test_rules=False,
                                   dolphin_ini=(None, {"Online": {"StreamGames": False}}))
    # The upload without its backoff: this test is about the viewer. On this busy PC (three
    # Dolphins, one catching up flat out) the shipping uploader holds for most of a game and sends
    # it at its end, which test_spectate_safety.py covers.
    st = a.c.call("spectate_config", stream=True, forced=True)
    assert st["stream"], st
    writer = StreamWriter(be, dana.session_token, out, comm)
    v = _viewer(be, dolphin, gpu_backend, comm, out)
    sampler = Sampler(v, status_file)
    try:
        online_set.play_set([a, b], games=2, mode="direct", panels={g.name: g.panel for g in (a, b)})
        for g in (a, b):
            g.c.mm_cancel()
        # The viewer rebuilds the match with the online rules (no account, as the launcher's
        # playback Dolphin): a template whose games keep other rules ("setup differs") can't be
        # watched, and nothing below would end.
        assert sampler.error() is None, f"the viewer can't play the stream: {sampler.error()}"
        # The stream is over 120 s after its last game (docs/spectate-protocol.md §2).
        online_set.wait(lambda: writer.over or writer.error or sampler.error(), 400, "the stream to be over")
        assert writer.error is None, writer.error
        online_set.wait(lambda: sampler.error() or (
            sampler.samples and sampler.samples[-1]["file"].get("state") == "close"),
            900, "the viewer to reach the end and ask to be closed")
        assert sampler.error() is None, f"the viewer can't play the stream: {sampler.error()}"
        v.shot("end")
    finally:
        sampler.stop()

    samples = sampler.samples
    (out / "samples.json").write_text(json.dumps(samples, indent=1))
    states = [s["file"].get("state") for s in samples]
    print("viewer states:", sorted(set(s for s in states if s)))
    assert "loading" in states, "it waited for the game's first frames"
    # Game 2, there from its start (the user, 2026-10-14): from its first frame at normal speed,
    # behind the players by the stream's delay, never catching up by itself...
    assert sampler.went_live_at is not None, "game 2 never played 5 s"
    g2 = [s for s in samples[:sampler.went_live_at] if s["file"].get("index") == 1 and s.get("started")]
    assert g2 and not any(s.get("catching_up") for s in g2), "game 2 caught up by itself"
    shown = [s["shown"] for s in g2 if s.get("shown", -1) >= 0]
    assert shown and min(shown) < 120, f"game 2 didn't start at its start: {shown[:5]}"
    # Armed from its setup, its countdown waited for the first frames (2026-10-19): it plays from
    # GO with the live buffer in hand, never stopping for "Buffering" in its first 5 s.
    early = [s for s in g2 if min(shown) < s.get("shown", -1) < min(shown) + 300]
    lead = [(s.get("live_edge", 0) + 180 - s["shown"]) / 60 for s in early]
    print(f"game 2 at its start: {lead[:1] and round(lead[0], 1)} s behind the newest frame received")
    assert not any(s.get("buffering") for s in early), [
        (s["shown"], s.get("live_edge"), s.get("buffering")) for s in early]
    behind = max(s.get("live_edge", 0) - s.get("shown", 0) for s in g2 if s.get("shown", -1) >= 0)
    print(f"game 2 behind the live edge before Go Live: {behind / 60:.1f} s")
    assert behind > 120, "game 2 was never behind live: nothing to test Go Live on"
    # ... until Go Live: then it catches up and plays at the live edge (before the game ended).
    after = samples[sampler.went_live_at:]
    assert any(s.get("catching_up") for s in after), "Go Live didn't catch up"
    # A click while catching up: stopped there and played on at 1x (not paused, not live).
    clicked = sampler.clicked
    assert "error" not in clicked and clicked, clicked
    at, later = clicked["at"], clicked["later"]
    print(f"click while catching up: at frame {at['shown']}, "
          f"{(at['live_edge'] - at['shown']) / 60:.1f} s behind live; 2 s later frame {later['shown']}")
    assert not at["catching_up"] and not at["paused"], at
    assert not later["catching_up"] and not later["paused"], later
    assert 60 <= later["shown"] - at["shown"] <= 200, (at["shown"], later["shown"])
    live = [s for s in after
            if s.get("live") and not s.get("catching_up") and not s.get("complete", True) and
            not s.get("buffering") and s.get("live_edge", 0) - s.get("shown", 0) <= 90]
    assert live, "never played at the live edge after Go Live"
    rates = [s.get("catch_rate", 0) for s in samples if s.get("catch_rate")]
    print(f"catch-up rate: max {max(rates or [0]):.1f}x")
    # Both games played, each with every frame checked and none differing.
    by_index: dict[int, dict[str, Any]] = {}
    for s in samples:
        if s.get("started"):
            by_index[s["file"].get("index", -1)] = s
    assert set(by_index) >= {0, 1}, sorted(by_index)
    for i, path in enumerate(writer.paths[:2]):
        rep = R.read(path)
        s = by_index[i]
        assert s["desync_frame"] == -1, s
        if path in writer.cut:
            print(f"game {i + 1} was cut after {len(rep.slots)} frames")
            assert s["frames_checked"] >= len(rep.slots) - 1, (i, s)
        else:
            assert rep.trailer, f"{path}: the writer's file is a whole replay"
            assert s["frames_checked"] >= rep.endg["finalFrame"] - rep.first_frame - 1, (i, s, rep.endg)


@pytest.mark.slow
def test_another_game_picked_mid_game_plays_it(
        spectate_backend: OnlineBackend, dolphin: Callable[..., DolphinInstance], gpu_backend: str) -> None:
    """Picking another game while one plays (the user, 2026-10-14: the names changed, but the
    first game's match stayed on screen with no seek bar). The launcher writes a new `mirror`
    request (a new commandId) to the playback Dolphin's comm file; the game must leave the match
    it is in and start the new one. Twice: once while the first game plays, then back to it while
    the second one is paused (the CPU thread waits at the loop top then). Each game plays from its
    start with every frame checked. The last one closes at its end ("Close Dolphin after a
    spectated game ends"): the launcher ends the process, so a setting changed while watching
    (the resolution, its Settings window still open) must be saved by then (the user, 2026-10-14:
    the replay Dolphin didn't remember its resolution)."""
    be = spectate_backend
    out = ARTIFACTS / "switch"
    out.mkdir(parents=True, exist_ok=True)
    comm = out / "comm.json"
    status_file = Path(str(comm) + ".status.json")
    status_file.unlink(missing_ok=True)

    a, b, _, _ = _connected_direct(be, dolphin, gpu_backend, "spectate-switch", ("sora", "tobi"),
                                   test_rules=False)
    online_set.play_set([a, b], games=1, mode="direct", panels={g.name: g.panel for g in (a, b)})
    online_set.wait(lambda: not any(g.c.call("replay_recording")["recording"] for g in (a, b)), 60,
                    "both recordings to end")
    paths = []
    for g in (a, b):
        src = Path(g.c.call("replay_recording", flush=True)["path"])
        paths.append(out / f"{g.name}.brep")
        paths[-1].write_bytes(src.read_bytes())
        g.c.mm_cancel()
    reps = [R.read(p) for p in paths]
    assert all(r.trailer for r in reps), "whole replays"

    def request(command: str, path: Path, close_on_end: bool = False) -> None:
        tmp = comm.with_suffix(".tmp")
        tmp.write_text(json.dumps({"mode": "mirror", "commandId": command,
                                   "queue": [{"path": str(path), "cut": False}],
                                   "streamOver": True, "closeOnEnd": close_on_end}))
        os.replace(tmp, comm)

    def file_status() -> dict[str, Any]:
        try:
            return json.loads(status_file.read_text())
        except (OSError, ValueError):
            return {}

    def playing(command: str, after_serial: int, what: str) -> dict[str, Any]:
        """The request's game started (its own serial) and played a second from near its start."""
        def check() -> dict[str, Any] | None:
            fs = file_status()
            assert fs.get("state") != "error", fs
            st = v.c.call("replay_status")
            if (fs.get("commandId") == command and st.get("serial", 0) > after_serial and st["started"]
                    and st.get("shown", -1) >= st.get("first_frame", 0) + 60):
                return st
            return None
        return online_set.wait(check, 240, what)

    request("c1", paths[0])
    v = _viewer(be, dolphin, gpu_backend, comm, out)
    st = playing("c1", 0, "the first game to play")
    serial = st["serial"]
    online_set.wait(lambda: v.c.call("replay_status").get("shown", 0) >= st["first_frame"] + 300, 60,
                    "5 s of the first game")
    v.shot("first")

    # Another game while this one plays.
    request("c2", paths[1])
    st = playing("c2", serial, "the second game to start after the switch")
    serial = st["serial"]
    assert st["shown"] < st["first_frame"] + 600, f"the second game didn't start at its start: {st}"
    v.shot("second")

    # Back to the first while the second is paused.
    v.c.call("replay_control", action="pause")
    online_set.wait(lambda: v.c.call("replay_status")["paused"], 30, "the pause")
    request("c3", paths[0], close_on_end=True)
    st = playing("c3", serial, "the first game again after a switch while paused")
    v.shot("first-again")
    # The resolution as the Settings window sets it while watching: not saved yet. (5x: the
    # harness's User template has 3x.)
    gfx = v.inst.user_dir / "Config" / "GFX.ini"
    v.c.call("config_set", system="Graphics", section="Settings", key="InternalResolution", value="5")
    assert "InternalResolution = 5" not in (gfx.read_text() if gfx.exists() else ""), "saved before the close"
    st = online_set.wait(lambda: (lambda s: s if s["at_end"] or s["finished"] else None)(
        v.c.call("replay_status")), 900, "the first game's end")
    assert st["desync_frame"] == -1, st
    assert st["frames_checked"] >= reps[0].endg["finalFrame"] - reps[0].first_frame - 1, st
    assert not st["paused"] or st["at_end"], st
    online_set.wait(lambda: file_status().get("state") == "close", 60, "the viewer to ask to be closed")
    assert "InternalResolution = 5" in gfx.read_text(), "the resolution wasn't saved before the close"
