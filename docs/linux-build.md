# Linux build, tests and cross-platform checks

Status on 2026-10-06/07, branch `linux-build` in the worktree `dolphin-linux/` (off `rollback-fixes` at `15de378723`). Not pushed.

**Summary**

- The fork builds on Linux (Ubuntu 24.04, GCC 13; Arch Linux, GCC 16) with `dolphin-emu-nogui` and the Qt frontend `project-plus-dolphin`. It needed 10 compile fixes and 4 portability fixes; none changes behaviour on Windows x64.
- The whole harness suite passes in the Ubuntu container: 160 passed, 2 skipped (the 2 skips are the real-GPU presentation test; containers have no GPU). This includes the rollback sync test, two-instance rollback netplay (`lan` and `typical`, single and dual core) and the offline Fox vs Falco matches. The harness POSIX socket path works.
- The Arch build passes the sync tests (single and dual core) and both offline matches.
- **Cross-platform determinism: Linux and Windows are bit-identical.** The same recorded input, replayed offline on Linux and on Windows (single core, fixed RTC), gave identical hashes at all 1258 checkpoints (every 30 VI fields on the menus, then every frame of the 1200-frame match), every labelled gameplay region, per-player state, both RNG seeds, the rollback checksum fields, and even whole MEM1 and MEM2.
- **Linux ↔ Windows rollback netplay works**: a Windows host and a Linux joiner (in the container, through netsim) played two matches, single core over `typical` and dual core over `bad_wifi`, with 0 pad or checksum mismatches on about 2000 confirmed frames each and no desync.
- ARM64 (Apple Silicon) needs one more piece of work before rollback can use the JIT. Until then rollback sessions there use the cached interpreter. See [macOS](#macos).

## Building

Everything runs in Docker; nothing is installed on the host or in the user's WSL distro.

Files (all in `dolphin-linux/`):

| file | what |
|---|---|
| `Tools/docker/linux-build.Dockerfile` | Ubuntu 24.04 + Dolphin's documented Linux build dependencies (Qt 6, FFmpeg, X11, evdev, ...) + Python 3.12 and pytest. 1.04 GB. |
| `Tools/docker/arch-build.Dockerfile` | The same on `archlinux:base` (closest to SteamOS 3). |
| `Tools/docker/build.sh` | CMake (Ninja, Release) + build of `dolphin-nogui` and `project-plus-dolphin`; copies `Binaries/` to `$OUT`. `KEEP_GOING=1` reports every failing file. |
| `Tools/docker/test.sh` | Runs pytest in `harness/` with the right `PPHARNESS_*` variables; `test.sh ppharness ...` and `test.sh tool <script> ...` run the CLI and tools. |
| `Tools/docker/run.ps1` | Windows wrapper: mounts the source (read-only), the harness (read-only), the disc and `run/template-user` (read-only), a 12 GB tmpfs for instance dirs, and the main repo's `.git` so the build gets the right revision. |

```powershell
cd D:\code\pm_rollback\dolphin-linux
docker build -t pplus-dolphin-linux:ubuntu24.04 -f Tools\docker\linux-build.Dockerfile Tools\docker
Tools\docker\run.ps1 build                    # -> D:\code\pm_rollback\run\linux-build\ubuntu\Binaries
Tools\docker\run.ps1 test                     # quick suite, no game needed
Tools\docker\run.ps1 -- test -m dolphin tests/test_rollback.py
Tools\docker\run.ps1 -- test -m dolphin tests/test_e2e_match.py -k offline
Tools\docker\run.ps1 test ppharness probe     # {"harness": true, ...}
Tools\docker\run.ps1 -Distro arch build       # Arch image: pplus-dolphin-linux:arch
```

Put `--` before a container command that has dash options; otherwise PowerShell matches them against the script's own parameters (`-m` would become `-MainGitDir`).

On a Linux machine (or the Steam Deck in desktop mode with a distrobox), skip the wrapper:

```sh
docker build -t pplus-dolphin-linux:ubuntu24.04 -f Tools/docker/linux-build.Dockerfile Tools/docker
docker run --rm -v "$PWD:/src:ro" -v "$PWD/build:/build" pplus-dolphin-linux:ubuntu24.04 build
```

or natively: install the packages listed in the Dockerfile, then `cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release && ninja -C build dolphin-nogui project-plus-dolphin`. The binaries are `build/Binaries/dolphin-emu-nogui` and `build/Binaries/project-plus-dolphin`.

Notes:

- **The build tree lives in a Docker volume** (`pplus-build-ubuntu`, `pplus-build-arch`) and only `Binaries/` is copied to `run\linux-build\<distro>\` on D:. Building directly on the D: bind mount works (`run.ps1 -BindBuild`), but every file goes through Docker Desktop's Windows file sharing: CMake's configure step alone took about 20 minutes. In a volume a full build takes about 15 minutes on this PC (16 threads). The volumes are deleted at the end (see [Disk](#disk-usage)).
- **Revision.** Dolphin's netplay refuses peers whose build revision (`git rev-parse HEAD`) differs. A worktree's `.git` file points at a Windows path, so `run.ps1` mounts `dolphin/.git` read-only and sets `GIT_DIR`/`GIT_WORK_TREE`; the Linux build then reports the same revision as a Windows build of the same commit.
- **Submodules.** The worktree's submodules were cloned from the main tree's module repos (`git -c submodule.<name>.url=<dolphin/.git/modules/...>`), because `--reference` refuses the shallow module repos there. `Externals/Qt` and `Externals/FFmpeg-bin` (Windows-only binaries) are not checked out. Nested submodules (`cubeb`'s `sanitizers-cmake` and `googletest`, `libadrenotools`' `linkernsbypass`) are needed: cubeb's CMake fails without them.

Versions: Ubuntu 24.04 has GCC 13.3, CMake 3.28, Qt 6.4.2, Python 3.12.3. Arch has GCC 16.2.1, CMake 4.4.4 (`build.sh` passes `-DCMAKE_POLICY_VERSION_MINIMUM=3.5` for CMake 4), Qt 6.11.2, Python 3.14.7 (Arch only ships the current Python; the harness runs on it unchanged).

## What was fixed

Dolphin side (`linux-build`, 3 commits):

| commit | fix |
|---|---|
| `f3e902488b` Linux: fix the GCC build | GCC builds with `-fno-exceptions`, but `HarnessServer.cpp` and GekkoNet's `replay.cpp`/`replay_session.cpp` use exceptions: enable them for those 3 files. `State.cpp`: `State::` qualification inside `namespace State`. `RollbackManager.cpp`: un-captured `constexpr` sentinel in a nested lambda, `%u` with a `size_t`. `MMU.cpp`: `RollbackManager.h` was included only on `_WIN32` but used everywhere. Missing includes in `CPUCoreBase.h`, `MemoryUtil.cpp`, Tracy. A stray `<winnt.h>` in `SDIOSlot0.cpp`. DolphinQt's Linux `install()` still named the old `dolphin-emu` target (CMake configure failed). |
| `107af7a6f3` Rollback: same snapshot code everywhere | The RAM skip in `Memmap.cpp` was `_WIN32`-only, so elsewhere every rollback save/load also serialized all of MEM1/MEM2. The AVX2 non-temporal copy in `DeltaSaveSlot.cpp` (MSVC-only syntax, needs an AVX2 CPU, no runtime check) never ran: every caller copies one 64-byte granule and the loop only handled 256-byte blocks; replaced by `memcpy`. `job.h`: `pause` doesn't exist on ARM64 (now `yield`), and `aligned_alloc` sizes are rounded up to the alignment (required by C11 and macOS). |
| `f166669140` Rollback: cached interpreter off x86-64 | The dirty-tracking JIT barrier exists only in Jit64. On ARM64, JitArm64's fastmem and constant-address stores would be missing from snapshots. Rollback sessions on non-x86-64 hosts now run the cached interpreter, whose stores go through the MMU (which marks them dirty). See [macOS](#macos). |

The harness POSIX socket code needed no change: it compiled and worked on the first build (`probe` → `protocol v1`, and every harness test passes).

Harness side (top-level repo):

- `_platform.py`/`paths.py`: the headless binary is `dolphin-emu-nogui` on Linux and macOS (was `DolphinNoGUI`), the GUI is `project-plus-dolphin` on Linux and `DolphinQt.app/...` on macOS.
- `instance.py`/`paths.py`: the template's `[Core] DefaultISO` is a Windows path. `PPHARNESS_ISO` overrides it, and if the template's path doesn't exist here, `<root>/game/SSBB_NTSC.iso` is used.
- `test_e2e_match.py`: the video backend for screenshots is D3D11 on Windows and `Null` elsewhere (`PPHARNESS_E2E_VIDEO` overrides). Screenshots are best effort, so the Null runs skip them.
- `test_rollback.py`: the bounded-memory check reads `RssAnon + VmSwap` from `/proc` on Linux (it was Windows-only). This hunk went into the other session's commit `0cad7a5`, which was made from the same working tree.
- `fake_dolphin.py`: `ignore_quit` also ignores SIGTERM, so `test_kill_when_quit_is_ignored` tests the kill path on POSIX too (POSIX shutdown sends SIGTERM before SIGKILL).
- Containers need `--init` (a PID 1 that reaps orphans) for `test_child_dies_with_parent`; `run.ps1` passes it.
- New tools: `tools/xplat_trace.py` (per-frame state traces and their diff) and `tools/xplat_netplay.py` (rollback netplay across machines).

## Test results on Linux

Ubuntu 24.04 container, `f166669140`-equivalent build (the commits on top of `15de378723`), Null video, muted:

| suite | result |
|---|---|
| everything (`test.sh -q`, real Dolphin) | **160 passed, 2 skipped** in 20 min. Skipped: `test_rollback_presents_one_frame_per_displayed_frame[sc/dc]` ("video backend Vulkan not usable here": no GPU in the container). |
| `test_rollback.py` sync test (`PPR_SYNCTEST` 2 and 4, single and dual core) | 4 passed |
| `test_rollback.py` netplay (`lan`/`typical` × sc/dc, press-lands-on-same-frame × sc/dc) | 6 passed: no freeze, no pad or checksum mismatch on confirmed frames, no GekkoNet desync, bounded memory |
| `test_e2e_match.py` | offline Battlefield and FD, fixed-delay netplay, rollback netplay: 4 passed |

Arch Linux container (`f166669140`, GCC 16): sync test 4 passed (2/4 × sc/dc), offline match 2 passed.

## Cross-platform determinism

Method (`harness/tools/xplat_trace.py`): replay the recorded timeline `run/qa/timeline.json` (528 pad submissions: boot → CSS → Fox vs Falco → Battlefield → 1200 frames of chase/random play) on one instance per OS, exactly like `determinism.py compare` in events mode: single core, fixed custom RTC, every submission made while paused at its recorded VI field. Checkpoints every 30 VI fields on the menus and **every frame** from GO to the end. At each checkpoint it stores the xxh3 hash of every labelled region (`brawl.gameplay_ranges` in the match, `brawl.menu_ranges` on menus; both leave out `STATE_EXCLUDED_RANGES`, i.e. Brawlback's audio/framebuffer exclusions and the Rev 1/Rev 2 disc ID), whole MEM1 and MEM2, the per-player state (character, damage, stocks, x/y, action, animation frame), both RNG seeds, the frame counters and the corrected Brawlback checksum. `xplat_trace.py diff` compares two traces.

| | Windows | Linux |
|---|---|---|
| binary | `rollback-fixes` `15de378723`, MSVC (the frozen copy `run/bin/rf-15de378723/`, since deleted by the other session) | Ubuntu container build, same revision + the Linux fixes (GCC 13) |
| `__OSStartTime` | `ffffffff00076e31` | `ffffffff00076e31` |
| final (P1 Fox / P2 Falco) | 9 % / 3 stocks, 0 % / 3 stocks | identical (and identical to the recording) |
| checkpoints | 1258 | 1258 |

**Result: 1258 of 1258 checkpoints identical, including whole MEM1 and MEM2. No divergence at all.** Both are x86-64 Jit64, so this was expected, but it also confirms that MSVC vs GCC builds of the C++ parts (HLE, DSP HLE, the rollback code) don't change game state. Traces: `run/qa/xplat-trace-windows.json`, `run/qa/xplat-trace-linux.json`, diff in `run/qa/xplat-trace-diff.json`.

Only single core was compared: dual core is not bit-reproducible even on one machine (`docs/determinism-findings.md`), so a cross-OS dual-core comparison would not mean anything beyond that. The rollback netplay tests above cover dual core through the confirmed-frame checksums.

The offline RTC caveat (findings, cause 1) still applied to this build: `xplat_trace.py run --osstart <hex>` retries until the OS start time equals the reference run's. Here the first Linux attempt already matched. `0e49c345e2` on `rollback-fixes` removes the caveat.

## Linux ↔ Windows rollback netplay

It works through Docker Desktop's networking, so it was run (`harness/tools/xplat_netplay.py`).

Setup: the **Windows host** is `DolphinNoGUI.exe` from `rollback-fixes` `996a9dea34` (frozen copy in `run/bin/xplat-win-996a9dea34/`). The **Linux joiner** is the Ubuntu container build of the same commit with the 3 Linux commits applied on top (built with `HEAD` at `996a9dea34`, so both report the same revision and Dolphin's netplay version check passes). The joiner connects to `host.docker.internal`, which Docker Desktop forwards to the Windows loopback; the Windows side puts `netsim` between that port and the host, so the joiner's traffic gets the preset's latency, jitter and loss. The Windows driver controls the joiner's Dolphin through a small TCP relay in the container (published on `127.0.0.1:47011`), because Dolphin's harness server only listens on the container's loopback. Each side drives only its own port: CSS → Fox (host) vs Falco (joiner) → Battlefield → 25 s of seeded random play. Then both peers' `rollback_pad_history` is compared on every frame both have confirmed (same check as `test_rollback.py`).

| run | confirmed frames compared (in match) | pad mismatches | checksum mismatches | rollbacks host / joiner | max depth | GekkoNet desyncs | final state |
|---|---|---|---|---|---|---|---|
| `typical` (40 ± 8 ms RTT, 0.5 % loss), single core | 2034 (1874) | 0 | 0 | 117 / 140 | 2 | 0 / 0 | identical on both: Fox 2 stocks 15.9 %, Falco 4 stocks 35.0 % |
| `bad_wifi` (60 ± 25 ms, 2 % burst loss), **dual core** | 2035 (1874) | 0 | 0 | 119 / 189 | 6 | 0 / 0 | identical on both: Fox 4 stocks 36.47 %, Falco 4 stocks 73.36 % |

**Result: Linux ↔ Windows rollback netplay works, with no desync, in single and dual core.** Netsim measured 0.36-0.48 % loss and 21.7 ms one-way median for `typical`, 1.5-2.7 % loss and 38 ms for `bad_wifi`. Reports: `run/qa/xplat-netplay-typical.json`, `run/qa/xplat-netplay-badwifi-dc.json`.

Limits: Docker Desktop's port forwarding adds its own (small) latency on top of netsim, and both ends run on one PC, so this is not a test of a real WAN path. The Steam Deck on the same LAN would be the next step.

To repeat:

```powershell
# Linux joiner (prints the launcher path to use below)
Tools\docker\run.ps1 -DockerArgs @("-p","127.0.0.1:47011:47011","-e","PPHARNESS_DOLPHIN_DIR=/out/Binaries-rev996a9dea34") -- `
    bash /src/Tools/docker/test.sh tool xplat_netplay.py serve --fwd 47011 [--cpu-thread on]
# Windows host
$env:PPHARNESS_DOLPHIN_DIR = "D:\code\pm_rollback\run\bin\xplat-win-996a9dea34"
.venv\Scripts\python harness\tools\xplat_netplay.py drive --remote-port 47011 `
    --remote-launcher "/instances/xplat-joiner-0/Launcher/Project+ Netplay Launcher.dol" `
    --join-host host.docker.internal --preset bad_wifi --cpu-thread on --out run\qa\xplat-netplay.json
```

The 3 Linux commits also apply cleanly on `996a9dea34` and that tree builds on Linux without further changes.

## Steam Deck / Arch

SteamOS 3 is Arch-based, so the Arch image is the closest container. It has newer toolchains than SteamOS itself (SteamOS pins an Arch snapshot), so it mostly shows that the code keeps compiling and running as distros move on. Result: the build needed no changes beyond the Ubuntu ones, and the sync tests and offline matches pass.

What a container cannot check, and needs a real Deck: the GPU backends (Vulkan on the Deck's RDNA2 APU), audio, controllers (Steam Input / evdev), Gaming Mode (gamescope) and performance on its 4-core Zen 2 CPU. The Deck is x86-64, so Jit64 and the dirty-tracking barrier apply: rollback runs with the JIT, and states should stay bit-identical with Windows and Linux PCs as above. For distribution, Flatpak or AppImage is the usual route on the Deck (`ci.yml` already builds both for upstream P+).

## macOS

What the user needs to do on the Mac (not done here: no Mac available):

1. Install Xcode command line tools, Homebrew, then `brew install cmake ninja qt@6 libspng`.
2. Check out `linux-build` with submodules (`git submodule update --init --recursive`).
3. Configure and build:
   ```sh
   cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH="$(brew --prefix qt@6)" \
         -DMACOS_CODE_SIGNING=OFF -DENABLE_AUTOUPDATE=OFF -DCMAKE_POLICY_VERSION_MINIMUM=3.5
   ninja -C build dolphin-nogui project-plus-dolphin
   ```
   Expect a few compile fixes like the Linux ones (Clang is stricter about some things than GCC); the CI workflow below builds both architectures and will show them.
4. Run the harness with Python 3.12: `PPHARNESS_DOLPHIN_DIR=build/Binaries PPHARNESS_ISO=<disc> python -m pytest -m dolphin tests/test_rollback.py` from `harness/`. `_platform.py` has the macOS binary names; process cleanup on macOS relies on the `atexit` hook (no parent-death signal).
5. Determinism against Windows: `xplat_trace.py run` on the Mac and `diff` against `run/qa/xplat-trace-windows.json`.

Known gaps:

- **Apple Silicon (ARM64): no dirty-tracking barrier in JitArm64.** Jit64 marks every guest store in the rollback dirty bitmap (`EmuCodeBlock::EmitJITDirtyBitmapUpdate` and the constant-address variant from `7afaab433a`). JitArm64 has nothing equivalent, so its fastmem and constant-address stores would never reach snapshots. `f166669140` makes rollback sessions on ARM64 use the cached interpreter instead (correct, but slower; it may not hold 60 fps). The real fix is to port the barrier: emit the bitmap update in `JitArm64::EmitBackpatchRoutine` for stores and in the constant-address store paths, plus `dcbz` and the paired-store routines in `JitAsm.cpp`.
- **ARM64 vs x86-64 determinism is untested.** The research notes that Clang contracts FMA on ARM64 by default (`-ffp-contract`), and JitArm64 vs Jit64 float results can differ. Cross-architecture play needs a check like the one above before it is allowed; matchmaking should keep architectures apart until then.
- An Intel Mac uses Jit64, like Windows and Linux.

## CI

`dolphin-linux/.github/workflows/rollback-ci.yml` (not run: nothing is pushed):

- **Hosted:** Ubuntu 24.04 (builds in the same Docker image as above), Windows x64 (MSVC + the `ninja-release-x64` preset), macOS arm64 (`macos-15`) and x86-64 (`macos-15-intel`). Each builds `dolphin-nogui`, `project-plus-dolphin` and Dolphin's gtest `tests`, runs them with `ctest`, and uploads the binaries.
- **Self-hosted (`pplus-game` label):** the disc-dependent tests (`test_rollback.py` and the offline match) on Linux, Windows and macOS runners whose machine has a workspace like `D:\code\pm_rollback` at the repository variable `PPLUS_WORKSPACE`. They run on manual dispatch and on pushes to the owner's branches, never on pull requests.

The existing `ci.yml` (upstream P+'s builder, which checks out `Project-Plus-Development-Team/Project-Plus-Dolphin@master`, not this fork) is left as is.

## Disk usage

Before: C: 12.1 GB free (Docker Desktop was hung; after it was restarted and its VM disk trimmed, C: showed 37.5 GB free), D: 24.5 GB free. Docker before: 6 images (2.2 GB), build cache 9.12 GB (53 entries), none of it from this work.

After (2026-10-07):

| what | where | size |
|---|---|---|
| image `pplus-dolphin-linux:ubuntu24.04` (kept, to rerun the Linux tests) | Docker (C:) | 1.04 GB |
| Arch image, both build volumes, dangling images, this work's build cache | Docker (C:) | removed |
| worktree `dolphin-linux/` (sources + submodules, no Qt/FFmpeg binaries) | D: | 0.45 GB, plus 0.09 GB of git data in `dolphin/.git/worktrees/dolphin-linux` |
| `run/linux-build/` (logs, `ubuntu/Binaries-rev996a9dea34`, `arch/Binaries`) | D: | 0.17 GB |
| `run/bin/xplat-win-996a9dea34/` | D: | 0.04 GB |

Final free space: **C: 39.4 GB, D: 20.9 GB.** Docker: build cache back to 9.12 GB (the 5 entries left from this work are the kept image's own layers). D: dropped by 3.6 GB overall during the session, mostly from other work (the other session's instance dirs and builds); this work's files on D: total about 0.75 GB.

Peak use: the build volumes (a few GB each in Docker's VM disk), the instance dirs on a RAM tmpfs (two 2 GB `sd.raw` copies per netplay test), nothing large on D:.

Rebuilding later: `docker build` the image again if it was removed (about 3 minutes), then `run.ps1 build` (about 15 minutes from scratch). To remove everything: `docker rmi pplus-dolphin-linux:ubuntu24.04`, `docker volume rm pplus-build-ubuntu pplus-build-arch` (if recreated), then `docker buildx prune` for the remaining layers.

**One mistake to fix by hand:** an early `docker image prune -f` (meant for this work's dangling image) also deleted `postgres:16-alpine` (image 81bd698b4594). Docker's own log had reported that image as broken (`layer ... is missing`) after its restart, which is probably why prune treated it as unused. The `ppserver_pgdata` volume is untouched. If the image is still needed: `docker pull postgres:16-alpine`.

## Housekeeping

- Docker Desktop's engine had stopped responding (its VM was paused by Resource Saver and never resumed; then the backend crashed on a stale `userAnalyticsOtlpHttp.sock` in `%LOCALAPPDATA%\Docker\run`). It was fixed by restarting Docker Desktop and renaming that folder to `run.stale-20261006`; the folder can be deleted.
- `run/bin/xplat-win-996a9dea34/` is a frozen copy of the Windows `DolphinNoGUI.exe` used for the netplay check (the other session deleted `run/bin/rf-15de378723/` mid-run). It can be deleted.
