"""The real path a player takes, end to end: two launchers -> Dolphin.exe (Qt) -> the game's online
menus -> matchmaking -> the rollback session. A thin wrapper around ``tools/e2e_launcher.py``
(its docstring lists what it needs and checks); run it on its own with

    ..\\.venv\\Scripts\\python -m pytest tests/test_e2e_launcher.py -m launcher

It opens real windows (two launchers and two Dolphins, all muted) and takes 6-10 minutes
(a two-game set).
"""

from __future__ import annotations

import sys
from pathlib import Path

import pytest

from ppharness import paths

ROOT = paths.workspace_root()
sys.path.insert(0, str(ROOT / "harness" / "tools"))

pytestmark = [pytest.mark.dolphin, pytest.mark.server, pytest.mark.gpu, pytest.mark.slow,
              pytest.mark.launcher]


def _missing() -> str | None:
    if sys.platform != "win32":
        return "the launcher end-to-end run is written for Windows so far"
    need = {
        "the built launcher (npm run build)": ROOT / "launcher" / "release" / "app" / "dist" / "main" / "main.js",
        "Electron (npm install)": ROOT / "launcher" / "node_modules" / "electron" / "dist" / "electron.exe",
        "Dolphin.exe": paths.dolphin_gui(),
        "the plugin (game-code/build.sh)": ROOT / "game-code" / "PPOnline" / "PPOnline.rel",
        "the server binaries": ROOT / "server" / "target" / "debug" / "mm.exe",
    }
    for what, p in need.items():
        if not Path(p).exists():
            return f"{what} not found: {p}"
    return None


@pytest.mark.parametrize("direct", [False, True], ids=["launcher", "direct-dolphin"])
def test_launcher_to_rollback_session(direct: bool) -> None:
    reason = _missing()
    if reason:
        pytest.skip(reason)
    import e2e_launcher

    args = ["--prefix", "pytest-e2el" if not direct else "pytest-e2eq"]
    if direct:
        args.append("--direct-dolphin")
    summary = e2e_launcher.run(e2e_launcher.build_parser().parse_args(args))
    assert summary["ok"], summary.get("error", "") + "\n" + summary.get("traceback", "")
    # The gameplay-only session: two games from the online CSS, no reboot, equal confirmed frames.
    games = summary["set"]["games"]
    assert len(games) == 2 and all(g["checksums"]["mismatches"] == 0 for g in games), games
    for name in ("alice", "bob"):
        facts = summary["players"][name]["facts"]
        assert facts["session"]["backend"] == "gameplay", facts["session"]
        assert facts["rollback"]["session_started"] and facts["rollback"]["desyncs_detected"] == 0
        assert facts["audio_muted"] is True
        assert not any("netplay" in t.lower() for t in facts["windows"])
