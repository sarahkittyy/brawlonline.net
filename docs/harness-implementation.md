# Harness implementation notes

How the control server described in `harness-protocol.md` is built into our Dolphin fork (branch `harness` of `dolphin/`). Read the "Deviations" section of the protocol doc first.

## Files

New, in `Source/Core/Core/Harness/`:

| File | Contents |
|---|---|
| `Harness.h` | Public API: `ResolvePort`, `Start`/`Shutdown`, `HostCallbacks` (quit/boot hooks from the frontend), and the inline hot-path hooks `ApplyPadOverride`, `OnSIPoll`, `OnGamePadRead`, `OnVIField`, `IsPadOverridden`. When the harness is disabled, each hook is a single relaxed atomic load. |
| `Harness.cpp` | State: frame and poll counters, poll-source selection, pad overrides and scripts, frame-advance break, waiter condition variable, session defaults (audio mute, `HARNESS` log enable). |
| `HarnessServer.cpp` | Socket server (Winsock/POSIX, 127.0.0.1 only, one client) and all command handlers, using picojson for JSON and xxHash's XXH3 for `hash_mem`. |
| `HarnessNetPlay.cpp` | Headless `NetPlay::NetPlayUI` plus host/join/start/leave/status. It mirrors what DolphinQt's `MainWindow::NetPlayHost/Join` and `NetPlayDialog` do. |
| `HarnessInternal.h` | Interfaces shared between the three .cpp files. |

Hooks in existing code (small diffs):

- `HW/GCPad.cpp` (`Pad::GetStatus`), `HW/SI/SI_DeviceGCAdapter.cpp`, and the two `GCAdapter::Input` call sites in `NetPlayClient.cpp`: pad override. Overridden adapter ports also count as connected.
- `HW/SI/SI.cpp` (`UpdateDevices`): SI poll counter.
- `HLE/HLE_Misc.cpp` (`BrawlbackCapturePadThreadReadHook`, after `updateLowGC` returns, non-resim passes only): Brawl per-frame pad-read counter.
- `HW/VideoInterface.cpp` (field boundary): frame counter and frame-advance break.
- `NetPlayClient.{h,cpp}`: rollback counters (`GetRollbackStats()`), updated from `HandleGekkoFrame`/`ProcessGekkoEvents`.
- `VideoCommon/FrameDumper.{h,cpp}`: completed-screenshot counter, so `screenshot` can wait for the PNG.
- `State.{h,cpp}`: `State::WaitForPendingSaves()`.
- `Common/Logging`: new `HARNESS` log type.
- `UICommon/CommandLineParse.cpp`: `--harness-port`.
- `DolphinNoGUI/MainNoGUI.cpp`: starts the harness. Allows no game, keeps running when emulation stops, and calls `Harness::Shutdown()` before `Core::Stop`.
- `DolphinQt/Main.cpp`, `MainWindow.cpp`: start the harness before the main window, and skip the P+ update-check dialog under the harness.
- `Core/CMakeLists.txt`: new sources; links `xxhash::xxhash`.

## How to run

```powershell
$bin = "D:\code\pm_rollback\dolphin\build\release\x64\Binaries"
$u   = "D:\code\pm_rollback\run\<copy of template-user>"
# headless, boot P+ directly
& "$bin\DolphinNoGUI.exe" -u $u -p headless -v Null --harness-port 9101 -e "$u\Launcher\Project+ Offline Launcher.dol"
# with a real renderer, for screenshots
& "$bin\DolphinNoGUI.exe" -u $u -p win32 -v D3D --harness-port 9101 -e "..."
& "$bin\Dolphin.exe" -b -u $u -v D3D --harness-port 9101 -e "..."
# netplay: start with no game, then netplay_host / netplay_join / netplay_start over the socket
& "$bin\DolphinNoGUI.exe" -u $u -p headless -v Null --harness-port 9201
```

Every message is one JSON object per line, sent to 127.0.0.1:port. There's a minimal stdlib Python client in `run\scratch\hclient.py`; `python hclient.py 9101 status` sends a single command. `run\scratch\test1.py` through `test4.py` and `test_exact.py` are the verification scripts.

Test user dirs: copy `run\template-user` (robocopy /E). In the copy, set `SIDevice0`/`SIDevice1` to `6` (Standard Controller) so the game sees harness input immediately. The template uses `12` (Wii U adapter) for ports 0, 1 and 3.

Environment variables: `PPR_HARNESS_PORT` (port if no flag), `PPR_HARNESS_AUDIO=1` (don't force mute), `PPR_HARNESS_POLL_SOURCE=si` (count SI polls instead of Brawl pad reads).

### Useful Brawl addresses (P+ / RSBE01)

- Raw pad state written by the game each frame: `0x805BAD00 + 0x40 * in_game_port`. The first u32 is the current buttons (A=0x100, B=0x200, X=0x400, Y=0x800, START=0x1000). The rollback code uses the same `BRAWL_PAD_RAW_BASE`.
- Scene changes are logged as OSReport lines like `scSelctCharacter -> scSelStage` in `dolphin.log`.
- P+ boots straight to the VS character select. Holding B there goes back to the main menu. Getting into a match: each player holds the stick up for 25 polls, then A; then START, then A on the stage select.

## Known limitations

- **Brawlback rollback hangs on menus (not the harness).** In two runs, harness-driven rollback netplay booted on both instances, reached the CSS, started a GekkoNet session and performed a few rollbacks (2-4 rollbacks, max 4-5 frames, no desyncs detected). Then the host's game loop stalled while the joiner kept running on its own (joiner GekkoNet frame in the thousands, host stuck at 137 / 281). One stall came during cursor movement, when the CSS loads character portraits from SD; the other came at the CSS -> stage select transition. Without moving the cursor, a 30 s CSS session stayed in sync. Rollback mode also ran at about 36 VI fields/s on this PC with two instances, versus 60 in fixed-delay mode. Fixed-delay netplay works, and harness input flows through it as well.
- `screenshot` needs running emulation and a real video backend.
- Memory commands briefly pause the CPU (`CPUThreadGuard`). They block for as long as the CPU thread is itself blocked (e.g. waiting for remote input in fixed-delay netplay).
- `input_polls` only has per-frame accuracy for Brawl/P+ (see the protocol doc's Deviations). Other games fall back to SI polls (usually 2 per frame).
- In fixed-delay netplay, local input reaches the game after the pad buffer delay, so `input_polls`-scheduled scripts land `buffer` frames later than in offline play. That's the same delay a real controller gets.
- Harness netplay is direct-connection only (no traversal or UPnP), and the GekkoNet input delay is fixed at 2 frames by the fork.
- Linux/macOS: the code is written for POSIX sockets as well but has only been built on Windows.
