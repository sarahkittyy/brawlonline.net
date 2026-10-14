"""Replays (docs/design/replays.md): every player records the online games they play, and a
fresh Dolphin plays a recording back as the game that was played.

The playback Dolphin boots P+ with the plugin, no account and its own menus; `replay_play` arms
the file, Dolphin asks the game for the playback (PPOM v8, LOCAL's Direct-invite counter with the
number 3), the plugin opens the recorded mode's CSS and starts the match from the recorded
SESSION, and the session rebuilds the match as a joiner rebuilds its host's: the setup the live
peers compared must be the same, then the recorded host's sync block and task order, the
countdown, and the recorded inputs, every frame once. Each frame's checksum is compared with the
file's.

Screenshots and replays: run/artifacts/replays/.
"""

from __future__ import annotations

import concurrent.futures
import json
import shutil
import sys
import time
import types
from pathlib import Path
from typing import Any, Callable

import pytest

from ppharness.backend import OnlineBackend
from ppharness.instance import DolphinInstance, InstanceConfig

sys.path.insert(0, str(Path(__file__).resolve().parent))
from test_online_game import (  # noqa: E402
    PLUGIN, ROOT, Game, _boot, _connected_direct, _wait, backend)  # noqa: F401
import online_set  # noqa: E402
import patch_sd  # noqa: E402
import ppom  # noqa: E402
from ppharness import brawl as B  # noqa: E402
from ppharness import replay as R  # noqa: E402

ARTIFACTS = ROOT / "run" / "artifacts" / "replays"

pytestmark = [pytest.mark.dolphin, pytest.mark.server, pytest.mark.gpu]


def _viewer(dolphin: Callable[..., DolphinInstance], be: OnlineBackend, gpu_backend: str,
            test: str, name: str = "viewer") -> Game:
    """A playback Dolphin: P+ with the plugin and the test servers (never prod), on the Direct
    CSS with the live games' test rules.

    The live games played the harness's short rules (CFG_TEST_RULES keeps the set rule's stocks
    and times, written on their CSS), which are part of the match setup: the viewer must have
    them too, or playback refuses the match (the setup differs). So it goes to the Direct CSS
    (logged in, as the CSS is locked otherwise) and gets them there; playback then starts from
    that idle CSS (the plugin takes a replay on an idle CSS of the replay's mode). The launcher's
    playback Dolphin has no account and starts from the menus, with real rules from SESSION."""
    user = be.create_user(name, name[:4].upper())
    v = _boot(dolphin, name, be, user, gpu_backend, test, "gameplay", artifacts=ARTIFACTS)
    v.to_main_menu()
    v.to_online_page()
    v.to_css("direct", 0)
    B.write_rules(v.c, stocks=1, minutes=2, items_off=True)
    ppom.allow_test_rules(v.c)
    return v


def _recording(g: Game) -> R.Replay:
    st = g.c.call("replay_recording", flush=True)
    path = Path(st["path"])
    assert path.exists(), st
    return R.read(path)


def _play(v: Game, path: Path, timeout: float = 900) -> dict[str, Any]:
    """Plays `path` on the viewer to its end; returns playback's status."""
    v.c.call("replay_play", path=str(path))
    _wait(lambda: v.c.call("replay_status")["started"], 240, "the replay's match to start")
    # (started is the barrier, in the match's loading; the shot waits for frames on screen.)
    _wait(lambda: (lambda s: s["finished"] or s.get("shown", -1) >= s.get("first_frame", 0) + 60)(v.c.call("replay_status")), 120,
          "the replay's first second")
    v.shot(f"{path.stem[:40]}-started")
    st = _wait(lambda: (lambda s: s if s["finished"] or s["phase"] == "error" else None)(
        v.c.call("replay_status")), timeout, "the replay's last frame", )
    v.shot(f"{path.stem[:40]}-ended")
    return st


def _status(v: Game) -> dict[str, Any]:
    return v.c.call("replay_status")


def _shown(v: Game, frame: int, what: str, timeout: float = 120) -> dict[str, Any]:
    """Paused, not seeking, with `frame` on screen."""
    return _wait(lambda: (lambda s: s if s["paused"] and not s["seeking"] and s["shown"] == frame else None)(
        _status(v)), timeout, what)


def _seek_around(v: Game, path: Path) -> dict[str, Any]:
    """Plays `path` holding at its end, and on the way: pause, steps forward and back, jumps of
    5 s back and 10 s forward, a seek to the start, then on to the end. Every frame run again
    after a keyframe was put back is checked against the file's checksum like the others."""
    def ctl(action: str, arg: float | None = None) -> None:
        v.c.call("replay_control", action=action, **({"arg": arg} if arg is not None else {}))

    v.c.call("replay_play", path=str(path), hold_at_end=True)
    _wait(lambda: _status(v)["started"], 240, "the replay's match to start")
    st = _wait(lambda: (lambda s: s if s["shown"] >= 600 else None)(_status(v)), 240, "frame 600")
    last = st["last_frame"]
    ctl("pause")
    st = _wait(lambda: (lambda s: s if s["paused"] and not s["seeking"] else None)(_status(v)), 30, "the pause")
    f = st["shown"]
    time.sleep(1)
    assert _status(v)["shown"] == f, "paused, the frame must not move"
    v.shot(f"{path.stem[:30]}-paused")
    ctl("step")
    _shown(v, f + 1, "one step forward")
    for k in range(3):
        ctl("back")
        _shown(v, f - k, f"step back {k + 1}")
    v.shot(f"{path.stem[:30]}-stepped-back")
    ctl("jump", -5)
    _shown(v, f - 2 - 300, "a jump back of 5 s")
    ctl("jump", 10)
    _shown(v, min(f - 2 + 300, last), "a jump forward of 10 s")
    ctl("seek", 30)
    _shown(v, 30, "a seek near the start")
    st = _status(v)
    assert st["keyframes"] >= 3, st
    ctl("play")
    st = _wait(lambda: (lambda s: s if s["at_end"] else None)(_status(v)), 600, "the end, held")
    v.shot(f"{path.stem[:30]}-end")
    ctl("back")
    _shown(v, st["last_frame"] - 1, "a step back from the end")
    ctl("play")
    _wait(lambda: (lambda s: s if s["at_end"] and s["shown"] == st["last_frame"] else None)(_status(v)),
          60, "the end again")
    ctl("play")   # at the end: the match ends as it did for its player
    return _wait(lambda: (lambda s: s if s["finished"] else None)(_status(v)), 120, "the replay to end")


def _check_pair(fa: R.Replay, fb: R.Replay) -> None:
    """Both players' files of one game: the same game id, inputs and (confirmed) checksums."""
    assert fa.uid == fb.uid
    assert fa.trailer and fb.trailer, (fa.truncated, fb.truncated)
    assert fa.endg and fb.endg and fa.endg["endMethod"] == fb.endg["endMethod"] == "game", (fa.endg, fb.endg)
    assert fa.first_frame == fb.first_frame and fa.ports == fb.ports == 0x3
    final = min(fa.endg["finalFrame"], fb.endg["finalFrame"])
    n = final - fa.first_frame + 1
    assert n > 600, n
    assert fa.slots[:n] == fb.slots[:n]
    assert fa.checksums[:n] == fb.checksums[:n]
    assert {p["displayName"] for p in fa.info["players"]} == {p["displayName"] for p in fb.info["players"]}
    assert fa.info["mode"] == "direct" and fa.info["matchId"] == fb.info["matchId"] != ""
    # Raw fighter memory: every frame of both ports (FTSP says what the blocks are).
    assert fa.fighter_block == fa.fighter_spec["bytes"] and len(fa.fighters) == 2
    assert all(len(d) == len(fa.slots) * fa.fighter_block for d in fa.fighters)
    # The posture block's position (x at +0xC, a big-endian float) moves during the game.
    xs = {R.fighter_block(fa, 0, f, "posture")[0xC:0x10] for f in range(fa.first_frame, fa.last_frame, 60)}
    assert len(xs) > 3, xs


@pytest.mark.slow
def test_direct_game_records_on_both_and_plays_back(backend: OnlineBackend,
                                                   dolphin: Callable[..., DolphinInstance],
                                                   gpu_backend: str) -> None:
    """A Direct game under the gameplay session: both players' Dolphins write a replay of it
    (the same game id, inputs and checksums in both), and a fresh Dolphin plays each one back to
    the end with every frame's checksum equal to the recorded one."""
    test = "direct"
    a, b, ua, ub = _connected_direct(backend, dolphin, gpu_backend, "replay-direct", ("mona", "nick"),
                                     stocks=1)
    online_set.play_set([a, b], games=1, mode="direct", panels={g.name: g.panel for g in (a, b)})
    online_set.wait(lambda: not any(g.c.call("replay_recording")["recording"] for g in (a, b)), 60,
                    "both recordings to end")
    fa, fb = _recording(a), _recording(b)
    _check_pair(fa, fb)
    out = ARTIFACTS / test
    out.mkdir(parents=True, exist_ok=True)
    for f in (fa, fb):
        shutil.copy2(f.path, out / f.path.name)
    for g in (a, b):
        g.c.mm_cancel()

    v = _viewer(dolphin, backend, gpu_backend, test)
    st = _play(v, fa.path)
    assert st["phase"] != "error", st
    assert st["desync_frame"] == -1, st
    assert st["frames_checked"] >= fa.endg["finalFrame"] - fa.first_frame, st
    # Back on the CSS for the next one.
    _wait(lambda: v.scene() == "scSelctCharacter", 120, "the CSS after the replay")
    # The other player's file, with the controls.
    st = _seek_around(v, fb.path)
    assert st["phase"] != "error", st
    assert st["desync_frame"] == -1, st
    _wait(lambda: v.scene() == "scSelctCharacter", 120, "the CSS after the replay")


@pytest.mark.slow
def test_three_player_room_game_plays_back(backend: OnlineBackend,
                                           dolphin: Callable[..., DolphinInstance],
                                           gpu_backend: str) -> None:
    """A room's free-for-all of three (P1, P2, P3): every player records it, and a fresh Dolphin
    plays one of the files back from a CSS of another mode (the plugin goes to the room mode's
    CSS for it), with no checksum differing. Rooms' SESSION (the room's view, its rules) and the
    3-4 player match (gone flags, the elimination order) come out of the file as they went in."""
    import contextlib
    import threading

    from test_rooms_game import CSS, create_room, pick, to_join_room, type_room_code, until_view
    from ppharness import flows as F

    test = "room3"
    ports = (0, 1, 2)
    users = [backend.create_user(n, f"RP{'ABC'[k]}X") for k, n in enumerate(["ria", "rib", "ric"])]
    gs = [_boot(dolphin, f"room-{x}", backend, u, gpu_backend, test, "gameplay", artifacts=ARTIFACTS)
          for x, u in zip("abc", users)]
    with concurrent.futures.ThreadPoolExecutor(3) as ex:
        list(ex.map(lambda g: g.to_main_menu(), gs))
        list(ex.map(lambda g: g.to_online_page(), gs))
    a = gs[0]
    code = create_room(a)
    pick(a)
    from test_rooms_game import PANEL_X, hand_to
    hand_to(a, PANEL_X[2], -10.0)
    a.steps("tap A 8", "wait 30")
    until_view(a, lambda v: v["slots"][2]["bits"] & ppom.SLOT_OPEN, "slot 3 open")
    hand_to(a, -23.0, 10.5)
    with concurrent.futures.ThreadPoolExecutor(2) as ex:
        list(ex.map(to_join_room, gs[1:]))
    for k, g in enumerate(gs[1:], 1):
        pick(g, right=2 * k)
    for g in gs:
        B.write_rules(g.c, stocks=1, minutes=2, items_off=True)
        ppom.allow_test_rules(g.c)
    for k, g in enumerate(gs[1:], 1):
        type_room_code(g, code)
        until_view(g, lambda v: v["flags"] & ppom.RF_IN, f"player {k + 1} in the room")
    until_view(a, lambda v: all(v["slots"][p]["bits"] & ppom.SLOT_TAKEN for p in ports), "all players")
    for g in gs:
        g.steps("wait 60")
    for g in gs:
        g.steps("tap START 8", "wait 10")
    online_set.wait(lambda: all(online_set.gstatus(g.c)["phase"] in ("running", "error", "ended") for g in gs),
                    240, "the room's game running on all")
    assert all(online_set.gstatus(g.c)["phase"] == "running" for g in gs)

    def off(g: Game, port: int) -> None:
        with contextlib.suppress(Exception):
            F.fight([F.Seat(g.c, port, 0, g.name)], 60 * 60, mode="selfdestruct", seed=f"r{port}",
                    stop=lambda: online_set.gstatus(g.c)["phase"] != "running")
    th = [threading.Thread(target=off, args=(g, ports[k])) for k, g in enumerate(gs) if k > 0]
    for t in th:
        t.start()
    for t in th:
        t.join(300)
    online_set.wait(lambda: all(online_set.gstatus(g.c)["phase"] != "running" for g in gs), 240, "the game to end")
    _wait(lambda: all(g.scene() == CSS for g in gs), 180, "all back on the room's CSS")
    online_set.wait(lambda: not any(g.c.call("replay_recording")["recording"] for g in gs), 60,
                    "the recordings to end")
    files = [_recording(g) for g in gs]
    assert len({f.uid for f in files}) == 1
    for f in files:
        assert f.trailer and f.endg["endMethod"] == "game", (f.truncated, f.endg)
        assert f.ports == 0x7 and f.info["mode"] == "room" and len(f.info["players"]) == 3
    out = ARTIFACTS / test
    out.mkdir(parents=True, exist_ok=True)
    shutil.copy2(files[1].path, out / files[1].path.name)

    v = _viewer(dolphin, backend, gpu_backend, test)
    st = _play(v, files[1].path)
    assert st["phase"] != "error", st
    assert st["desync_frame"] == -1, st
    assert st["frames_checked"] >= files[1].endg["finalFrame"] - files[1].first_frame, st


@pytest.mark.slow
def test_launcher_comm_file_plays_from_a_fresh_boot(backend: OnlineBackend,
                                                   dolphin: Callable[..., DolphinInstance],
                                                   gpu_backend: str) -> None:
    """The launcher's way (docs/design/replays.md §6.2): a Direct game on the server's rules,
    then a Dolphin started with `-i <comm.json>` (written before it starts, as
    PlaybackDolphinInstance does) and no account. Nothing drives it: the game boots to the ONLINE
    page by itself, Dolphin arms the replay once the plugin runs (not at the boot DOL, whose game
    ID is not the disc's), the plugin opens the CSS and starts the match, and the replay plays to
    its end with no checksum differing. The status file next to the comm file says so."""
    test = "comm"
    a, b, _, _ = _connected_direct(backend, dolphin, gpu_backend, "replay-comm", ("lena", "otto"),
                                   test_rules=False)
    online_set.play_set([a, b], games=1, mode="direct", panels={g.name: g.panel for g in (a, b)})
    online_set.wait(lambda: not any(g.c.call("replay_recording")["recording"] for g in (a, b)), 60,
                    "both recordings to end")
    fa = _recording(a)
    assert fa.trailer and fa.endg["endMethod"] == "game", (fa.truncated, fa.endg)
    out = ARTIFACTS / test
    out.mkdir(parents=True, exist_ok=True)
    shutil.copy2(fa.path, out / fa.path.name)
    for g in (a, b):
        g.c.mm_cancel()

    comm = out / "playback-comms.json"
    status = Path(str(comm) + ".status.json")
    status.unlink(missing_ok=True)
    comm.write_text(json.dumps({"mode": "normal", "replay": str(fa.path), "commandId": "c1"}))
    cfg = InstanceConfig(cpu_thread=True, video_backend=gpu_backend, extra_args=("-i", str(comm)),
                         dolphin_ini={"Online": {"UseDevServer": True, "MatchmakingPort": backend.mm_port,
                                                 "DevAccountsUrl": backend.accounts_url}})
    inst = dolphin("viewer", config=cfg, client_timeout=60.0)
    d = inst.create()
    patch_sd.patch_image(d / "Wii" / "sd.raw", [(PLUGIN.read_bytes(), patch_sd.plugin_sd_path(PLUGIN))])
    inst.launch()
    inst.connect()
    v = Game(inst, types.SimpleNamespace(display_name="viewer"), out / "viewer")  # no account

    def file_status() -> dict[str, Any]:
        try:
            return json.loads(status.read_text())
        except (OSError, ValueError):
            return {}
    seen: set[str] = set()

    def started() -> dict[str, Any] | None:
        fs = file_status()
        seen.add(fs.get("state", ""))
        assert fs.get("state") != "error", fs
        st = v.c.call("replay_status")
        return st if st["started"] else None
    _wait(started, 300, "the replay's match to start by itself")
    assert "loading" in seen or "waiting" in seen, seen
    v.shot("started")
    st = _wait(lambda: (lambda s: s if s["at_end"] or s["finished"] else None)(_status(v)), 900,
               "the replay's last frame")
    v.shot("end")
    assert st["phase"] != "error", st
    assert st["desync_frame"] == -1, st
    assert st["frames_checked"] >= fa.endg["finalFrame"] - fa.first_frame, st
    fs = file_status()
    assert fs.get("commandId") == "c1" and fs.get("state") in ("playing", "ended"), fs
