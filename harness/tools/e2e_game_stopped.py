#!/usr/bin/env python3
"""The launcher's game buttons with Dolphin open but its game stopped, end to end (the user,
2026-10-18: "many times i close the game but not dolphin ... do an action in the client, it says
'sending you there', and then i have to close dolphin and click again"; and "they also need to
distinguish between the replay / playback dolphin and the actual game dolphin").

    launcher (its own profile) -> log in -> Play -> the game's menus
      1. Stop (the harness's stop_game: Dolphin's own Stop, as its toolbar button or closing the game
         window does): Dolphin's main window stays, the game is gone. Home's "Create room": the launcher must close that Dolphin and start the game,
         which creates the room (before: "Your game is creating a room." and nothing happened).
      2. The game in its room: "Create room" again goes to the running game (no restart), and the
         launcher hears at once that it is in a game.
      3. The playback Dolphin open (Settings > Replays' Configure, the replay/spectate Dolphin):
         it is not the game. Stop the game again, "Create room": the game Dolphin restarts into a
         room, the playback Dolphin stays open and is never asked to close.

    python harness/tools/e2e_game_stopped.py [--keep] [--dolphin path/to/Dolphin.exe]

Needs what harness/tools/e2e_launcher.py needs (the launcher built, Dolphin.exe, the plugin, the
server binaries, the portable Postgres, a Brawl disc image, run/template-user). Windows only. Run it
from PowerShell with the repository's .venv
(docs: harness tests from Bash see no GPU).
"""

from __future__ import annotations

import argparse
import json
import sys
import time
import traceback
from pathlib import Path
from typing import Any

sys.path.insert(0, str(Path(__file__).resolve().parent))

import e2e_launcher as E  # noqa: E402
from ppharness import _platform, paths  # noqa: E402
from ppharness.backend import OnlineBackend  # noqa: E402
from ppharness.instance import find_free_port  # noqa: E402

log = E.log
wait_until = E.wait_until

STARTING = "Starting the game. It will create a room."
HANDED = "Your game is creating a room."

def dolphins_of(launcher: E.Launcher) -> list[int]:
    assert launcher.proc
    return sorted(E.descendants(launcher.proc.pid, "Dolphin.exe"))


def game_status(profile: Path) -> dict[str, Any] | None:
    """The game Dolphin's game-status.json (the netplay User folder's Online folder)."""
    for f in profile.rglob("game-status.json"):
        if "playback" in str(f).lower():
            continue
        try:
            return json.loads(f.read_text(encoding="utf-8"))
        except (OSError, ValueError):
            return None
    return None


def stop_game(p: E.Player) -> None:
    assert p.c
    p.c.stop_game()
    log(f"Stop on Dolphin {p.dolphin_pid}")


def stopped(p: E.Player) -> bool:
    """The game is gone and Dolphin is still open (its harness server lives as long as Dolphin)."""
    assert p.c
    return p.c.status().state == "uninitialized"


def launcher_log(profile: Path) -> str:
    try:
        return (profile / "logs" / "main.log").read_text(encoding="utf-8", errors="replace")
    except OSError:
        return ""


def toast_seen(lch: E.Launcher, text: str, timeout: float) -> bool:
    try:
        lch.wait_text(text, timeout)
        return True
    except TimeoutError:
        return False


def run(args: argparse.Namespace) -> dict[str, Any]:
    stamp = time.strftime("%Y%m%d-%H%M%S")
    run_name = f"{args.prefix}-{stamp}"
    run_dir = E.RUNS / run_name
    out = E.ARTIFACTS / run_name
    run_dir.mkdir(parents=True, exist_ok=True)
    out.mkdir(parents=True, exist_ok=True)
    dolphin = Path(args.dolphin) if args.dolphin else paths.dolphin_gui()
    iso = E.find_iso(args.iso)
    E.launcher_app_dir(args.allow_stale_launcher)
    summary: dict[str, Any] = {"run": run_name, "dolphin": str(dolphin), "ok": False, "checks": {}}
    checks = summary["checks"]
    be = OnlineBackend(run_dir / "backend")
    p = E.Player("alice", "ALIC", out / "alice")
    try:
        be.start()
        p.user = be.create_user(p.name, p.code_start)
        p.harness_port = find_free_port()
        profile = run_dir / p.name / "profile"
        env = E.launcher_env(p, be, profile, dolphin, find_free_port())
        extra = json.loads(env["PPO_DOLPHIN_EXTRA_ARGS"])
        if args.video:
            extra[extra.index("-v") + 1] = args.video
        # Stop without "Do you want to stop the current emulation?"; no GameCube adapter.
        extra += ["-C", "Dolphin.Interface.ConfirmStop=False", "-C", "Dolphin.Core.SIDevice1=0",
                  "-C", "Dolphin.Core.SIDevice2=0", "-C", "Dolphin.Core.SIDevice3=0"]
        env["PPO_DOLPHIN_EXTRA_ARGS"] = json.dumps(extra)
        lch = E.Launcher(p.name, profile, env, p.out / "launcher")
        p.launcher = lch
        lch.start()
        lch.login(p.user)
        lch.finish_quick_start(iso)
        lch.play()
        p.connect()
        p.boot_to_main_menu()
        first = wait_until(lambda: dolphins_of(lch), 30, "the game Dolphin")
        p.dolphin_pid = first[0]
        log(f"game Dolphin {p.dolphin_pid}")

        # 1. Stop the game, keep Dolphin open, click Create room.
        stop_game(p)
        wait_until(lambda: stopped(p), 30, "the game to stop")
        time.sleep(2)
        checks["stopped_dolphin_still_open"] = _platform.pid_alive(p.dolphin_pid)
        st = game_status(profile)
        checks["status_after_stop"] = st
        log(f"game-status.json after Stop: {st}")
        lch.shot("10-game-stopped")
        p.c = None  # the harness connection belonged to the stopped game's Dolphin
        t0 = time.monotonic()
        lch.click("Create room")
        restarted = wait_until(lambda: [d for d in dolphins_of(lch) if d != p.dolphin_pid], 60,
                               "a new game Dolphin")
        checks["stopped_old_dolphin_closed"] = not _platform.pid_alive(p.dolphin_pid)
        checks["stopped_new_dolphin_after_s"] = round(time.monotonic() - t0, 1)
        checks["stopped_toast_starting"] = toast_seen(lch, STARTING, 10)
        lch.shot("11-create-room-after-stop")
        p.dolphin_pid = restarted[0]
        p.connect()
        p.boot_to_main_menu()
        room = wait_until(lambda: (game_status(profile) or {}).get("room"), 120, "the game's room")
        checks["stopped_room_created"] = room
        log(f"the new game created room {room}")

        # 2. In a game: Create room goes to the game, no restart, answered at once.
        before = launcher_log(profile).count("The open Dolphin is in a game")
        t0 = time.monotonic()
        lch.click("Create room")
        wait_until(lambda: launcher_log(profile).count("The open Dolphin is in a game") > before, 15,
                   "the in-game answer")
        checks["ingame_answer_s"] = round(time.monotonic() - t0, 1)
        time.sleep(3)
        checks["ingame_same_dolphin"] = dolphins_of(lch) == [p.dolphin_pid]
        lch.shot("12-create-room-in-game")

        # 3. The playback Dolphin open: not the game.
        lch.eval("window.electron.dolphin.configureDolphin('playback')")
        playback = wait_until(lambda: [d for d in dolphins_of(lch) if d != p.dolphin_pid], 60,
                              "the playback Dolphin")[0]
        log(f"playback Dolphin {playback}")
        stop_game(p)
        wait_until(lambda: stopped(p), 30, "the game to stop")
        time.sleep(2)
        p.c = None
        old = p.dolphin_pid
        lch.click("Create room")
        new = wait_until(lambda: [d for d in dolphins_of(lch) if d not in (old, playback)], 60,
                         "a new game Dolphin beside the playback one")
        checks["playback_old_game_closed"] = not _platform.pid_alive(old)
        checks["playback_still_open"] = _platform.pid_alive(playback)
        checks["playback_toast_starting"] = toast_seen(lch, STARTING, 10)
        p.dolphin_pid = new[0]
        p.connect()
        p.boot_to_main_menu()
        checks["playback_room_created"] = wait_until(lambda: (game_status(profile) or {}).get("room"),
                                                     120, "the game's room")
        checks["playback_still_open_after"] = _platform.pid_alive(playback)
        lch.shot("13-create-room-beside-playback")
        log_text = launcher_log(profile)
        checks["launcher_log_lines"] = [ln for ln in log_text.splitlines() if "The open Dolphin" in ln]

        summary["ok"] = bool(
            checks["stopped_old_dolphin_closed"] and checks["stopped_toast_starting"]
            and checks["stopped_room_created"] and checks["ingame_same_dolphin"]
            and checks["ingame_answer_s"] < 3 and checks["playback_old_game_closed"]
            and checks["playback_still_open"] and checks["playback_room_created"]
            and checks["playback_still_open_after"])
    except Exception as e:  # noqa: BLE001
        summary["error"] = f"{type(e).__name__}: {e}"
        log(traceback.format_exc())
        try:
            if p.launcher and p.launcher.s:
                p.launcher.shot("99-failure")
        except Exception:  # noqa: BLE001
            pass
    finally:
        E.teardown([p], be)
        (out / "summary.json").write_text(json.dumps(summary, indent=2), encoding="utf-8")
        E.cleanup_run_dir(run_dir, args.keep)
    return summary


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--prefix", default="stopped")
    ap.add_argument("--dolphin", help="Dolphin.exe (default: the Qt build in dolphin/build)")
    ap.add_argument("--iso")
    ap.add_argument("--video", help="video backend instead of D3D11 (OGL, Vulkan)")
    ap.add_argument("--keep", action="store_true")
    ap.add_argument("--allow-stale-launcher", action="store_true")
    summary = run(ap.parse_args(argv))
    print(json.dumps(summary, indent=2, default=str))
    return 0 if summary["ok"] else 1


if __name__ == "__main__":
    sys.exit(main())
