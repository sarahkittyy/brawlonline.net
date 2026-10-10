"""Rooms, Dolphin's side (docs/rooms-game-interface.md, docs/rooms-protocol.md), end to end.

Each Dolphin boots P+ with our plugin (PPOM v5) and goes to P+'s Versus character select, where
the plugin stays quiet. This script then plays the game's part of the room CSS through the PPOM
block in game memory, as the plugin's room CSS will: `CMD_ROOM` requests in the mailbox, the
lock-in and the screen byte in LOCAL; it reads back what Dolphin publishes (SESSION's room bytes,
LOCAL's room phase and join counter, the RoomStatus answers). Dolphin talks to our real local mm
and accounts servers (ppharness/backend.py); `mmclient room` plays other members.

The room's match is played on the Versus CSS as gprb_online.py does (the plugin's online CSS is
the game side's work): both games pick the same characters and stage, and the gameplay session
that the room's start connected (ports = slots, the room host deciding) takes the match over.

Run with this worktree's Dolphin (PPHARNESS_DOLPHIN_DIR) and plugin (PPHARNESS_PLUGIN), e.g.
    PPHARNESS_INSTANCE_PREFIX=roomsd- python -m pytest harness/tests/test_rooms.py -m "dolphin and server"
"""

from __future__ import annotations

import argparse
import concurrent.futures
import json
import os
import struct
import subprocess
import sys
import threading
import time
import urllib.request
import uuid
from datetime import datetime, timedelta, timezone
from pathlib import Path
from typing import Any, Callable

import pytest

from ppharness import paths
from ppharness.backend import OnlineBackend, OnlineUser, server_binary
from ppharness.instance import DolphinInstance, InstanceConfig

from test_online import _logged_in, _make_backend, _wait

ROOT = paths.workspace_root()
sys.path.insert(0, str(ROOT / "tools" / "gamecode"))
sys.path.insert(0, str(ROOT / "tools" / "sdcard"))
sys.path.insert(0, str(ROOT / "harness" / "tools"))

import patch_sd  # noqa: E402
import ppom  # noqa: E402
from ppharness import brawl as B  # noqa: E402

pytestmark = [pytest.mark.dolphin, pytest.mark.server]

PLUGIN = Path(os.environ.get("PPHARNESS_PLUGIN") or ROOT / "game-code" / "PPOnline" / "PPOnline.rel")

S_STATE, S_GAME = 4, 6
L_LOCK = 0x28


@pytest.fixture(scope="module")
def backend() -> Any:
    be = _make_backend()
    try:
        yield be
    finally:
        be.stop()


class Player:
    """One booted P+ with the plugin; this script is its room CSS."""

    def __init__(self, inst: DolphinInstance, user: OnlineUser):
        self.inst = inst
        self.user = user
        self.c = inst.client
        self._b: ppom.Block | None = None
        self.lock_seq = 0

    @property
    def b(self) -> ppom.Block:
        if self._b is None:
            _wait(lambda: self.c.call("game_bridge_status")["found"], 60, "the PPOM block")
            self._b = ppom.find_block(self.c)
            assert self._b.version == 5, self._b
        return self._b

    # -- the game's side
    def room(self, op: str, arg: int = 0, arg2: int = 0, code: str = "") -> dict[str, Any]:
        """Post a CMD_ROOM and return Dolphin's RoomStatus answer."""
        seq = ppom.post(self.c, self.b, ppom.CMD_ROOM, ppom.room_request(op, arg, arg2, code))

        def answered() -> dict[str, Any] | None:
            r = ppom.read_response(self.c, self.b)
            return r if r["seq"] == seq else None

        r = _wait(answered, 10, f"the answer to ROOM {op}", interval=0.02)
        assert r["cmd"] == ppom.CMD_ROOM and r["status"] == 0, r
        # Taken (the plugin's own poll may have taken it already).
        mb = self.c.read_mem(self.b.mailbox + ppom.MB_RESP_COUNT, 4)
        self.c.write_mem(self.b.mailbox + ppom.MB_RESP_SEEN, mb)
        return ppom.decode_room_status(r["payload"])

    def poll(self) -> dict[str, Any]:
        return self.room("poll")

    def screen(self, name: str) -> None:
        """LOCAL.screen as the room CSS reports it. The plugin writes its own screen every frame
        (here: P+'s Versus CSS, offline), so the harness's value stands in for it in Dolphin."""
        self.c.call("rooms_request", op="screen", screen=ppom.SCREENS[name])

    def lock(self, ready: bool, kind: int = 0xFF, costume: int = 0, game: int | None = None,
             stage: int = 0xFFFF) -> None:
        """The lock-in (LOCAL LockIn), as writeLockIn: the body, then its seq."""
        if game is None:
            game = self.view()["game"]
        self.lock_seq += 1
        body = struct.pack(">BBBBHBB", 1 if ready else 0, kind, kind, costume, stage, 0, game)
        self.c.write_mem(self.b.local + L_LOCK + 4, body)
        self.c.write_mem(self.b.local + L_LOCK, struct.pack(">I", 0x1000 + self.lock_seq))

    # -- what Dolphin published
    def view(self) -> dict[str, Any]:
        return ppom.read_room(self.c, self.b)

    def rooms(self) -> dict[str, Any]:
        return self.c.call("rooms_status")

    def gprb(self) -> dict[str, Any]:
        return self.c.call("gprb_status")

    def until_text(self, want: str, timeout: float = 15, error: bool | None = None) -> dict[str, Any]:
        def ok() -> dict[str, Any] | None:
            st = self.poll()
            if st["text"] == want and (error is None or bool(st["error"]) == error):
                return st
            return None
        try:
            return _wait(ok, timeout, f"{self.user.display_name}'s line {want!r}", interval=0.1)
        except AssertionError:
            raise AssertionError(f"line is {self.poll()!r}, wanted {want!r}; rooms {self.rooms()}")

    def until_view(self, pred: Callable[[dict[str, Any]], Any], what: str, timeout: float = 15) -> dict[str, Any]:
        try:
            return _wait(lambda: (lambda v: v if pred(v) else None)(self.view()), timeout, what, interval=0.1)
        except AssertionError:
            raise AssertionError(f"{what}: view {self.view()}, rooms {self.rooms()}")

    def online_dir(self) -> Path:
        return Path(self.inst.user_dir) / "Online"

    def game_status(self) -> dict[str, Any]:
        return json.loads((self.online_dir() / "game-status.json").read_text())


def _boot(dolphin: Callable[..., DolphinInstance], name: str, be: OnlineBackend, user: OnlineUser,
          before_launch: Callable[[Path], None] | None = None,
          after_connect: Callable[[Any], None] | None = None) -> Player:
    if not PLUGIN.exists():
        pytest.skip(f"{PLUGIN} not built (game-code/build.sh)")
    ini = {"Online": {"UseDevServer": True, "MatchmakingPort": be.mm_port,
                      "DevAccountsUrl": be.accounts_url}}
    cfg = InstanceConfig(cpu_thread=False, video_backend="Null", dolphin_ini=ini,
                         standard_controllers=(0, 1))
    inst = dolphin(name, config=cfg, client_timeout=60.0)
    d = inst.create()
    patch_sd.patch_image(d / "Wii" / "sd.raw",
                         [(PLUGIN.read_bytes(), f"{patch_sd.PLUGIN_DIR}/{PLUGIN.name}")])
    user.write_user_json(d)
    if before_launch:
        before_launch(d)
    inst.launch()
    inst.connect()
    if after_connect:
        after_connect(inst.client)
    p = Player(inst, user)
    p.c.online_session_backend("gameplay")
    _logged_in(inst, user)
    return p


def _to_versus_css(p: Player) -> None:
    """P+ boots to the plugin's ONLINE page: B to the main menu, then Versus (gprb_session.py)."""
    c = p.c
    for port in (0, 1):
        c.pad_set(port)
    B.wait_scene(c, [B.Scene.MAIN_MENU, B.Scene.CSS], 60 * 120)
    if B.read_scene(c.read_mem).scene is B.Scene.MAIN_MENU:
        B.step(c, 60)
        B.tap(c, 0, ["B"], hold=3, release=60)
        B.tap(c, 0, ["DUP"], hold=4, release=40)
        B.boot_to_css(c, 0)
    B.wait_scene(c, [B.Scene.CSS], 60 * 120)
    B.step(c, 30)   # the plugin's last menu requests answered


def _both(*fns: Callable[[], Any]) -> list[Any]:
    with concurrent.futures.ThreadPoolExecutor(len(fns)) as ex:
        return [f.result() for f in [ex.submit(fn) for fn in fns]]


def _online(p: Player) -> dict[str, Any]:
    return _wait(lambda: (lambda s: s if s["connection"] == "online" else None)(p.rooms()), 20,
                 f"{p.user.display_name}'s online connection")


def _mm_status(be: OnlineBackend) -> dict[str, Any]:
    with urllib.request.urlopen(f"http://127.0.0.1:{be.mm_status_port}/status", timeout=5) as r:
        return json.loads(r.read())


class MmClient:
    """`mmclient room ...` as another member; every message it prints is kept."""

    def __init__(self, be: OnlineBackend, user: OnlineUser, tmp: Path, *args: str):
        uj = tmp / f"{user.display_name}-{uuid.uuid4().hex[:6]}.json"
        uj.write_text(json.dumps(user.user_json))
        cmd = [str(server_binary("mmclient")), *args[:1], "--server", f"127.0.0.1:{be.mm_port}",
               "--user-json", str(uj), *args[1:]]
        self.lines: list[dict[str, Any]] = []
        self.proc = subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
        self._t = threading.Thread(target=self._read, daemon=True)
        self._t.start()

    def _read(self) -> None:
        assert self.proc.stdout
        for line in self.proc.stdout:
            try:
                self.lines.append(json.loads(line))
            except ValueError:
                self.lines.append({"raw": line.rstrip()})

    def until(self, pred: Callable[[dict[str, Any]], bool], what: str, timeout: float = 15) -> dict[str, Any]:
        def found() -> dict[str, Any] | None:
            for m in list(self.lines):
                if pred(m):
                    return m
            assert self.proc.poll() is None or any(pred(m) for m in self.lines), \
                f"mmclient exited ({self.proc.returncode}): {self.lines[-5:]}"
            return None
        return _wait(found, timeout, f"mmclient: {what}", interval=0.1)

    def code(self) -> str:
        return self.until(lambda m: m.get("type") == "room-state", "its room")["code"]

    def stop(self) -> None:
        if self.proc.poll() is None:
            self.proc.kill()
        self.proc.wait(10)


def _write_join(p: Player, code: str, *, age_s: float = 0, version: int = 1) -> str:
    rid = str(uuid.uuid4())
    created = (datetime.now(timezone.utc) - timedelta(seconds=age_s)).isoformat(timespec="milliseconds")
    body = {"version": version, "id": rid, "code": code, "createdAt": created.replace("+00:00", "Z")}
    d = p.online_dir()
    tmp = d / "join-room.json.tmp"
    tmp.write_text(json.dumps(body))
    os.replace(tmp, d / "join-room.json")
    return rid


def _answer(p: Player, rid: str, timeout: float = 5) -> dict[str, Any]:
    def got() -> dict[str, Any] | None:
        try:
            lr = p.game_status().get("lastRequest")
        except (OSError, ValueError):
            return None
        return lr if lr and lr.get("id") == rid else None
    return _wait(got, timeout, f"the answer to launcher request {rid}", interval=0.1)


# ---------------------------------------------------------------------------------------------


def test_room_requests_errors_and_the_launcher(backend: OnlineBackend,
                                              dolphin: Callable[..., DolphinInstance],
                                              tmp_path: Path) -> None:
    """One Dolphin and mmclient members: the online connection, create, join by code, the room
    view in SESSION, public/private, Teams, slots, kick, room full / not found / own slot, leave,
    and the launcher hand-off (accepted, refused when busy, stale and malformed requests)."""
    ua = backend.create_user("anna", "ANNA")
    ub, uc, ud, ue = (backend.create_user(n, n[:4].upper()) for n in ("bert", "cleo", "dina", "emil"))
    a = _boot(dolphin, "rooms-a", backend, ua)
    clients: list[MmClient] = []
    try:
        _to_versus_css(a)
        st = _online(a)
        assert st["hellos"] >= 1
        assert _mm_status(backend)["online"] >= 1
        # The launcher's status file is there and fresh; the game has said nothing yet (busy).
        gs = _wait(lambda: (a.online_dir() / "game-status.json").exists() and a.game_status(), 10,
                   "game-status.json")
        assert gs["version"] == 1 and gs["pid"] == a.inst.process.pid and gs["room"] is None
        a.screen("room")
        _wait(lambda: a.game_status()["state"] == "idle" and a.game_status()["screen"] == "room", 10,
              "game-status idle on the room CSS")

        # Create.
        st = a.room("create", 1)
        assert st["phase"] in (1, 2)
        v = a.until_view(lambda v: v["flags"] & ppom.RF_IN, "the room")
        code = v["code"]
        assert len(code) == 4 and all(ch in "BCDFGHJKLMNPQRSTVWXZ" for ch in code), code
        assert v["flags"] == ppom.RF_IN | ppom.RF_PUBLIC and v["host"] == 0 and v["phase"] == 2
        assert v["local_port"] == 0 and v["status"] == 0 and v["mode"] == 0
        s0, s1, s2, s3 = v["slots"]
        assert s0["bits"] == ppom.SLOT_OPEN | ppom.SLOT_TAKEN | ppom.SLOT_HOST and s0["name"] == "anna"
        assert s1["bits"] == ppom.SLOT_OPEN and s2["bits"] == 0 and s3["bits"] == 0
        assert s0["team"] == 0 and s0["char"] == 0xFF
        a.until_text("Waiting for players", error=False)
        _wait(lambda: a.game_status()["room"] == code, 10, "game-status's room")

        # Join (mmclient as bert): the panel fills.
        bert = MmClient(backend, ub, tmp_path, "room", "--join", code, "--hold-secs", "120")
        clients.append(bert)
        v = a.until_view(lambda v: v["slots"][1]["bits"] & ppom.SLOT_TAKEN, "bert in slot 2")
        assert v["slots"][1]["name"] == "bert" and v["slots"][1]["team"] == 1
        a.until_text("Waiting on: anna, bert")

        # Ready: the lock-in becomes room-ready; the others see the character.
        a.lock(True, B.CHAR_KIND["fox"], 2)
        v = a.until_view(lambda v: v["slots"][0]["bits"] & ppom.SLOT_READY, "anna ready")
        assert v["slots"][0]["char"] == B.CHAR_KIND["fox"] and v["slots"][0]["costume"] == 2
        bert.until(lambda m: m.get("type") == "room-state" and m["slots"][0]["player"]["ready"] and
                   m["slots"][0]["player"]["character"] == B.CHAR_KIND["fox"], "anna's character")
        a.lock(False)
        a.until_view(lambda v: not v["slots"][0]["bits"] & ppom.SLOT_READY, "anna unready")

        # Private / public; Teams; a third slot; the colour.
        a.room("public", 0)
        a.until_view(lambda v: v["flags"] == ppom.RF_IN, "private")
        a.room("public", 1)
        a.room("teams", 1)
        v = a.until_view(lambda v: v["flags"] == ppom.RF_IN | ppom.RF_PUBLIC | ppom.RF_TEAMS, "Teams on")
        assert v["mode"] == 0   # two open slots: 1v1 whatever the switch says
        a.room("slot", 2, 1)
        v = a.until_view(lambda v: v["slots"][2]["bits"] == ppom.SLOT_OPEN, "slot 3 open")
        assert v["mode"] == 2
        a.until_text("Waiting for players (2/3)")
        a.room("team", 2)
        a.until_view(lambda v: v["slots"][0]["team"] == 2, "anna green")
        cleo = MmClient(backend, uc, tmp_path, "room", "--join", code, "--hold-secs", "120")
        clients.append(cleo)
        v = a.until_view(lambda v: v["slots"][2]["bits"] & ppom.SLOT_TAKEN, "cleo in slot 3")
        assert v["slots"][2]["name"] == "cleo"

        # The server's refusals, red, the room unchanged.
        a.room("slot", 0, 0)
        a.until_text("You can't close your own slot.", error=True)
        a.room("join", code="ZZZZ")
        a.until_text("Room not found.", error=True)
        a.room("join", code="KFQA")   # a vowel: Dolphin answers without asking mm
        a.until_text("Room not found.", error=True)
        assert a.view()["code"] == code
        # Kick: the host closes an occupied slot.
        a.room("slot", 2, 0)
        v = a.until_view(lambda v: v["slots"][2]["bits"] == 0, "slot 3 closed")
        cleo.until(lambda m: m.get("type") == "room-left" and m.get("reason") == "Removed from the room.",
                   "cleo removed")
        a.room("slot", 1, 0)
        a.until_text("At least 2 slots stay open.", error=True)
        # The error goes after 5 s in a room; the room's line comes back.
        a.until_text("Waiting on: anna, bert", timeout=10, error=False)

        # Room full: another room with both slots taken.
        dina = MmClient(backend, ud, tmp_path, "room", "--create", "--hold-secs", "120")
        clients.append(dina)
        full = dina.code()
        emil = MmClient(backend, ue, tmp_path, "room", "--join", full, "--hold-secs", "120")
        clients.append(emil)
        dina.until(lambda m: m.get("type") == "room-state" and m["slots"][1]["player"], "emil in")
        a.room("join", code=full.lower())
        a.until_text("This room is full.", error=True)
        assert a.view()["code"] == code

        # Leave.
        a.room("leave")
        a.until_view(lambda v: v["flags"] == 0 and v["phase"] == 0, "out of the room")
        bert.until(lambda m: m.get("type") == "room-state" and m["host"] == 2, "bert host")

        # The launcher: a click while idle on the room CSS joins (bert's room).
        join_before = a.view()["join"]
        rid = _write_join(a, code)
        assert _answer(a, rid) == {"id": rid, "result": "accepted"}
        v = a.until_view(lambda v: v["flags"] & ppom.RF_IN and v["code"] == code, "joined from the launcher")
        assert v["join"] == (join_before + 1) & 0xFF and v["local_port"] == 0   # bert moved to host
        assert v["host"] == 1
        _wait(lambda: a.game_status()["room"] == code, 10, "game-status's room")
        assert not (a.online_dir() / "join-room.json").exists()
        # Busy (in a match): refused, nothing changes.
        a.screen("match")
        _wait(lambda: a.game_status()["state"] == "busy", 10, "busy")
        rid = _write_join(a, full)
        assert _answer(a, rid) == {"id": rid, "result": "refused", "message": "Finish your current game first."}
        assert a.view()["code"] == code and a.view()["join"] == v["join"]
        a.screen("room")
        # Stale, malformed: deleted and ignored (lastRequest stays the busy answer).
        for kw in ({"age_s": 300}, {"version": 2}):
            _write_join(a, full, **kw)
            _wait(lambda: not (a.online_dir() / "join-room.json").exists(), 5, "the request taken")
        _write_join(a, "AAAA")
        _wait(lambda: not (a.online_dir() / "join-room.json").exists(), 5, "the request taken")
        time.sleep(1)
        assert a.game_status()["lastRequest"]["id"] == rid and a.view()["code"] == code
        # The status file is rewritten at least every 5 s.
        t0 = a.game_status()["updatedAt"]
        _wait(lambda: a.game_status()["updatedAt"] != t0, 8, "game-status refreshed")

        # Another game signs in with the same account: this one is told and stops reconnecting
        # until the player asks again.
        other = MmClient(backend, ua, tmp_path, "online", "--hold-secs", "60")
        clients.append(other)
        _wait(lambda: a.rooms()["connection_hold"], 15, "the connection held")
        a.until_text("Signed in from another game.", error=True)
        assert a.view()["flags"] == 0
        a.room("create", 1)
        _online(a)
        a.until_view(lambda v: v["flags"] & ppom.RF_IN, "a new room after signing in again")
        print("rooms:", json.dumps({k: a.rooms()[k] for k in ("connects", "hellos", "drops", "sent", "received")}))
    finally:
        for m in clients:
            m.stop()


def _match_args() -> argparse.Namespace:
    return argparse.Namespace(p1="fox", p2="falco", stage="battlefield", delay=2, region_set="gp-v21",
                              start_frame=240, frames=30000, minutes=1, timeout=400, cpu="sc",
                              save_countdown=None, sample_every=0, save_traces=None)


def test_room_game_two_players(backend: OnlineBackend, dolphin: Callable[..., DolphinInstance]) -> None:
    """Two Dolphins: create, join by code, ready -> room-start -> the mode-3 tickets from the P2P
    port while the online connections stay open -> the gameplay session with ports = slots and
    the room host deciding -> a real match -> room-back, the next game's number and the loser's
    pick carried. Then a second start, and the host leaving it: the other player's room game ends,
    the host passes on."""
    import gprb_session as S

    ua, ub = backend.create_user("hana", "HANA"), backend.create_user("ivan", "IVAN")
    a, b = _both(lambda: _boot(dolphin, "rooms-g-a", backend, ua),
                 lambda: _boot(dolphin, "rooms-g-b", backend, ub))
    _both(lambda: _to_versus_css(a), lambda: _to_versus_css(b))
    _both(lambda: _online(a), lambda: _online(b))
    for p in (a, b):
        p.screen("room")
    a.room("create", 1)
    code = a.until_view(lambda v: v["flags"] & ppom.RF_IN, "the room")["code"]
    st = b.room("join", code=code)
    assert st["phase"] == 1 and st["text"] in (f"Joining room {code}", f"Connecting to room {code}"), st
    vb = b.until_view(lambda v: v["flags"] & ppom.RF_IN, "ivan in the room")
    assert vb["code"] == code and vb["local_port"] == 1 and vb["host"] == 0
    assert vb["slots"][0]["name"] == "hana" and vb["slots"][1]["name"] == "ivan"
    a.until_view(lambda v: v["slots"][1]["bits"] & ppom.SLOT_TAKEN, "ivan seen by hana")
    assert a.view()["game"] == 1 and b.view()["game"] == 1

    # Ready on both: the room starts by itself.
    a.lock(True, B.CHAR_KIND["fox"], 0)
    b.lock(True, B.CHAR_KIND["falco"], 0)
    _both(lambda: _wait(lambda: a.gprb()["phase"] == "connected", 60, "hana's session"),
          lambda: _wait(lambda: b.gprb()["phase"] == "connected", 60, "ivan's session"))
    ga, gb = a.gprb(), b.gprb()
    assert ga["local_slot"] == 0 and gb["local_slot"] == 1, (ga, gb)
    assert ga["role"] == "host" and gb["role"] == "joiner" and gb["host_slot"] == 0, (ga, gb)
    for p in (a, b):
        r = p.rooms()
        assert r["room_game"]["active"] and r["connection"] == "online", r
        mm = p.c.call("mm_status")
        assert mm["mode"] == "teams" and mm["opponent_code"] == code, mm
    # The host decides the setup from the lock-ins (SESSION MATCH_READY, the room's game 1).
    _wait(lambda: a.view()["session_state"] == 2 and b.view()["session_state"] == 2, 20, "the setup")
    assert a.view()["game"] == 1 and b.view()["game"] == 1
    assert a.view()["local_state"] == 2

    # The match, played on the Versus CSS as gprb_online.py does.
    for p in (a, b):
        p.screen("match")
    m: dict[str, Any] = {}
    S.play_match(a.c, b.c, argparse.Namespace(reset_epoch=lambda: None), _match_args(), "online", "sc", 0, 0,
                 ("fox", "falco", "battlefield"), m)
    assert not m.get("error"), m.get("error")
    assert m["checksums"]["mismatches"] == 0, m["checksums"]
    _both(lambda: B.results_to_css(a.c, [0, 1]), lambda: B.results_to_css(b.c, [0, 1]))
    for p in (a, b):
        p.screen("room")
    # room-back: the room waits again, game 2, the session closed, the loser's pick carried.
    for p in (a, b):
        _wait(lambda p=p: p.rooms()["room_game"]["done"] == 1, 30, f"{p.user.display_name}'s game over")
        p.until_view(lambda v: v["status"] == 0 and v["game"] == 2 and v["local_state"] == 0 and
                     v["session_state"] == 0, "the room waiting for game 2", timeout=20)
    pa = [s["picks_stage"] for s in a.view()["slots"]]
    pb = [s["picks_stage"] for s in b.view()["slots"]]
    assert pa == pb and any(pa), (pa, pb)
    assert not any(s["bits"] & (ppom.SLOT_READY | ppom.SLOT_IN_GAME) for s in a.view()["slots"])
    log = (Path(a.inst.user_dir) / "Logs" / "dolphin.log").read_text(errors="replace")
    assert "report-game" not in log

    # Game 2: the stale lock-in (game 1) does not count; a new one starts the room again, and the
    # session starts with the carried pickers.
    time.sleep(1.5)
    assert not a.view()["slots"][0]["bits"] & ppom.SLOT_READY
    a.lock(True, B.CHAR_KIND["fox"], 0)
    b.lock(True, B.CHAR_KIND["falco"], 0)
    _both(lambda: _wait(lambda: a.gprb()["phase"] == "connected", 60, "hana's 2nd session"),
          lambda: _wait(lambda: b.gprb()["phase"] == "connected", 60, "ivan's 2nd session"))
    pickers = sum(1 << i for i, x in enumerate(pa) if x)
    assert a.gprb()["lobby"]["stage_pickers"] == pickers, a.gprb()["lobby"]
    # The host leaves during the start: both room games end; ivan becomes host.
    a.room("leave")
    _wait(lambda: not b.rooms()["room_game"]["active"], 30, "ivan's room game to end")
    # (mm says "waiting" once ivan's room-back is in, which can come after the host change.)
    v = b.until_view(lambda v: v["host"] == 1 and v["slots"][1]["bits"] & ppom.SLOT_HOST and
                     not v["slots"][0]["bits"] & ppom.SLOT_TAKEN and v["status"] == 0,
                     "ivan host, alone, waiting")
    assert v["game"] == 3, v
    assert b.rooms()["connection"] == "online" and not a.rooms()["room_game"]["active"]


def test_room_start_three_players_teams(backend: OnlineBackend,
                                        dolphin: Callable[..., DolphinInstance]) -> None:
    """Three Dolphins, Teams on: the host opens slot 3, two players join by code, everyone readies
    with the default colours (red, blue, green), the room starts and every Dolphin's gameplay
    session connects on its slot with the room host deciding, and the host's setup is a team
    battle of the three. (The 3-player match itself is not played here.) Then everyone leaves."""
    users = [backend.create_user(n, n[:4].upper()) for n in ("kira", "lars", "mona")]
    ps = _both(*[lambda u=u, i=i: _boot(dolphin, f"rooms-3-{i}", backend, u) for i, u in enumerate(users)])
    _both(*[lambda p=p: _to_versus_css(p) for p in ps])
    _both(*[lambda p=p: _online(p) for p in ps])
    a, b, c = ps
    for p in ps:
        p.screen("room")
    a.room("create", 1)
    code = a.until_view(lambda v: v["flags"] & ppom.RF_IN, "the room")["code"]
    a.room("slot", 2, 1)
    a.room("teams", 1)
    a.until_view(lambda v: v["flags"] & ppom.RF_TEAMS and v["slots"][2]["bits"] & ppom.SLOT_OPEN,
                 "slot 3 open, Teams on")
    b.room("join", code=code)
    b.until_view(lambda v: v["flags"] & ppom.RF_IN, "lars in")
    c.room("join", code=code)
    vc = c.until_view(lambda v: v["flags"] & ppom.RF_IN, "mona in")
    assert vc["local_port"] == 2 and vc["mode"] == 2 and [s["team"] for s in vc["slots"][:3]] == [0, 1, 2]
    for p, ch in zip(ps, ("mario", "fox", "marth")):
        p.lock(True, B.CHAR_KIND[ch], 0)
    _both(*[lambda p=p: _wait(lambda: p.gprb()["phase"] == "connected", 60,
                              f"{p.user.display_name}'s session") for p in ps])
    st = [p.gprb() for p in ps]
    assert [s["local_slot"] for s in st] == [0, 1, 2], st
    assert [s["role"] for s in st] == ["host", "joiner", "joiner"], st
    assert all(s["host_slot"] == 0 for s in st[1:]), st
    _wait(lambda: all(p.view()["session_state"] == 2 for p in ps), 20, "the setup")
    for p in ps:
        se = ppom.read_session(p.c, p.b)
        assert se["teams"] == 1 and se["game"] == 1, se
        assert [pl["present"] for pl in se["players"][:3]] == [1, 1, 1], se
        assert [pl["team"] for pl in se["players"][:3]] == [0, 1, 2], se
        assert [pl["char_kind"] for pl in se["players"][:3]] == [B.CHAR_KIND[x] for x in ("mario", "fox", "marth")]
    for p in ps:
        p.room("leave")
    for p in ps:
        _wait(lambda p=p: not p.rooms()["room_game"]["active"] and p.gprb()["phase"] in ("idle", "ended"),
              30, f"{p.user.display_name}'s room game to end")


def test_launcher_join_waiting_at_start_up(backend: OnlineBackend,
                                           dolphin: Callable[..., DolphinInstance],
                                           tmp_path: Path) -> None:
    """The launcher wrote join-room.json before Dolphin started (a room clicked with the game
    closed): Dolphin takes the request at once, answers `accepted` and keeps it until the game says
    it is on its menus (the boot is not "busy"), then joins and bumps LOCAL roomJoin for the game to
    go to the room CSS."""
    uh, uo = backend.create_user("host", "HOST"), backend.create_user("olga", "OLGA")
    owner = MmClient(backend, uh, tmp_path, "room", "--create", "--hold-secs", "300")
    try:
        code = owner.code()
        rid = str(uuid.uuid4())

        def put_request(user_dir: Path) -> None:
            created = datetime.now(timezone.utc).isoformat(timespec="milliseconds").replace("+00:00", "Z")
            (user_dir / "Online" / "join-room.json").write_text(
                json.dumps({"version": 1, "id": rid, "code": code, "createdAt": created}))

        # The game has not said where it is yet (a game at its boot): the harness holds the
        # screen at "unknown" for the plugin, which reports its menus as soon as it is there.
        p = _boot(dolphin, "rooms-boot", backend, uo, before_launch=put_request,
                  after_connect=lambda c: c.call("rooms_request", op="screen", screen=0))
        _wait(lambda: p.rooms()["launch_pending"] == code, 30, "the request taken and kept")
        assert not (p.online_dir() / "join-room.json").exists()
        _to_versus_css(p)
        _online(p)
        # Still kept: the game has not shown its menus; accepted already, idle for the launcher.
        assert p.rooms()["launch_pending"] == code and p.view()["flags"] == 0
        assert _answer(p, rid) == {"id": rid, "result": "accepted"}
        assert p.game_status()["state"] == "idle"
        p.screen("offline")   # the boot's scenes: not a reason to refuse
        time.sleep(1.5)
        assert p.rooms()["launch_pending"] == code and p.view()["flags"] == 0
        join = p.view()["join"]
        p.screen("menus")
        v = p.until_view(lambda v: v["flags"] & ppom.RF_IN and v["code"] == code, "in the room")
        assert v["join"] == (join + 1) & 0xFF and v["local_port"] == 1 and v["slots"][0]["name"] == "host"
        owner.until(lambda m: m.get("type") == "room-state" and m["slots"][1]["player"] and
                    m["slots"][1]["player"]["displayName"] == "olga", "olga seen by the host")
        # The game away from the room CSS for 5 s (it never went there): Dolphin leaves for it.
        p.until_view(lambda v: v["flags"] == 0, "left after 5 s on the menus", timeout=15)
    finally:
        owner.stop()


def test_online_connection_comes_back_after_mm_restarts(dolphin: Callable[..., DolphinInstance]) -> None:
    """mm goes away while the player is in a room: the line says the connection is lost and the
    room is gone; once mm is back (same port) Dolphin reconnects by itself with its backoff, says
    hello again (the player counts as online again) and tries the old room once ("Room not found.":
    a restarted mm has no rooms)."""
    be = _make_backend()
    try:
        u = be.create_user("pia", "PIA")
        p = _boot(dolphin, "rooms-mm", be, u)
        _to_versus_css(p)
        _online(p)
        p.screen("room")
        p.room("create", 1)
        code = p.until_view(lambda v: v["flags"] & ppom.RF_IN, "the room")["code"]
        mm = be._procs["mm"]
        mm.kill()
        mm.wait(10)
        p.until_text("Lost the connection to the server.", timeout=30, error=True)
        assert p.view()["flags"] == 0 and p.rooms()["connection"] != "online"
        time.sleep(3)   # a few failed attempts (backoff)
        env = be._env()
        env["MM_LISTEN"] = f"127.0.0.1:{be.mm_port}"
        env["MM_STATUS_LISTEN"] = f"127.0.0.1:{be.mm_status_port}"
        be._spawn("mm", env)
        st = _wait(lambda: (lambda s: s if s["connection"] == "online" else None)(p.rooms()), 60,
                   "the connection back")
        assert st["hellos"] >= 2 and st["drops"] >= 1, st
        assert any(f"joining {code} again" in line for line in st["log"]), st["log"]
        p.until_text("Room not found.", timeout=15, error=True)
        assert _wait(lambda: _mm_status(be)["online"] == 1, 10, "online again on mm")
    finally:
        be.stop()
