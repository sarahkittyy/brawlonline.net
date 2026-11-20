# Brawl Online core: one online implementation for Dolphin and EmissaryRecomp (design)

Status: being built on branch `emissary-recomp` (steps 0, 1 and 2a done, §8). Decisions 2026-11-13 (§7). The user's request (Sarah's board, 2026-11-13): "we need to get rid of the companion app. i know dolphin and the recomp are both c++(?) so maybe it would make sense to have one submodule that both depend on??? it would also require a lot of tests to make sure nothing at all changed when we modularize it out."

## 1. Today

- **Dolphin** does everything around the game in C++: `dolphin/Source/Core/Core/Online/` (18,000 lines: GameBridge, Rooms, Matchmaking, GameSetup, Chat, RoomMessages, Ranked, Spectate, RoomWatch, SpeedCheck, User, ...) and `Rollback/GameplaySession.cpp` (11,700 lines: the session's state machine, setup sync, GekkoNet, replays, desync recovery), on top of Dolphin's emulator (memory, CPU, CoreTiming, video, audio).
- **EmissaryRecomp** (PR #2) has only the rollback engine in the game (GekkoNet, snapshots); everything else is kalomaze's Python companion (`tools/online`, ~1,500 lines, Direct and Unranked 1v1 only) plus our `mmclient` binary, driving the game over a loopback control socket (memory read/write, gates, arm).
- Both are C++ (the recomp's runtime is C++17/20, built with clang; Dolphin C++20 with MSVC/clang/gcc). The plugin (PPOnline) is the same in both: it talks through the PPOM block in game memory.

## 2. The idea

Move the online code out of Dolphin into a library with no Dolphin types, **brawl-online-core**, behind two small interfaces each engine implements. Dolphin keeps its behaviour exactly (it calls the same code through the interfaces); the recomp links the same library in-process, and the companion and the control socket go away.

```
                 brawl-online-core (C++20, no Dolphin, no recomp)
   ┌─────────────────────────────────────────────────────────────────────┐
   │ protocol/   PeerData (wire), RoomMessages, MatchRules, SetFormat,    │  pure
   │             ChatText, ChatCrypto, ReplayFile (.brep), constants       │
   │ net/        Matchmaking (ENet mm client), Ipv6, User, Ranked, Chat,  │  needs Services
   │             Spectate, RecentCodes, OnlineClient, OnlineSession       │
   │ game/       GameBridge (PPOM), GameSetup, Rooms, RoomWatch,          │  needs Services +
   │             SpeedCheck, ReplayRecorder                               │  Engine
   │ session/    the session state machine of GameplaySession: arm, setup │
   │             sync, barriers, GekkoNet events, checksums, desync       │
   │             recovery, quits, game-status.json / join-room.json       │
   │ third_party GekkoNet (Brawl Online's copy), ENet                     │
   └─────────────────────────────────────────────────────────────────────┘
          ▲ Services (host services)            ▲ Engine (the game under it)
          │  log, files, paths, config,         │  read/write guest memory
          │  threads/timers, HTTP, UDP/ENet,    │  save/load a frame's state
          │  on-screen messages                 │  advance one frame (run the game loop)
          │                                     │  loop-top / stage-create / barrier hooks
   ┌──────┴──────────┐                   ┌──────┴────────────────┐
   │ Dolphin         │                   │ EmissaryRecomp runtime │
   │ (RollbackManager│                   │ (brawl_rollback.cpp's  │
   │  CoreTiming,    │                   │  snapshot store, its   │
   │  JIT, VI, DSP)  │                   │  interpreter hooks)    │
   └─────────────────┘                   └────────────────────────┘
```

`Guest` (GameplayRollback.h) already wraps guest memory reads behind one class; most game-facing code goes through it, which is where `Engine` starts.

### 2.1 What stays in Dolphin

Whole-machine rollback (`MachineSession`, `MachineSDCard`: Other Games), replay playback and its Dolphin-only parts (ClipRecorder, keyframes), RollbackManager and every JIT/CoreTiming/VI/audio detail of saving, loading and hiding re-runs, harness hooks. The core never sees them: Dolphin's `Engine` implementation calls them.

### 2.2 What the recomp provides

Its `Engine`: guest memory (`Memory::`), save/load (its snapshot store, now driven by the core's session instead of its own copy of the loop), "run one frame" (its loop hook at `0x800171B4`), the stage-create and loop-top hooks it already has. Its `Services`: SDL/std threads and timers, sockets (it already has portable ones from kalomaze's transport work), curl or WinHTTP for HTTP, its log. Then the runtime reads `User/Online` like Dolphin does: no companion, no control socket, no Python, no `mmclient`.

## 3. Where the code lives

Every protocol change today is one commit in this monorepo (server + Dolphin + plugin + launcher). That has to stay true, so:

- **The core lives in the monorepo** (`core/` beside `dolphin/`, `server/`, `game-code/`); Dolphin builds it from there with no submodule.
- **EmissaryRecomp fetches only `core/`** at a pinned commit of this repo, in its `build.py` as it fetches its other dependencies: `git clone --filter=blob:none --no-checkout`, `git sparse-checkout set core`, checkout of the pin (a few MB, not the 162 MB repository; a git submodule can only take a whole repository). Bumping the pin is the "recomp protocol" update of brawl-recompiled.md §7.4.

(Alternative: the core as its own repo, a submodule of both. Simpler mechanics, but every protocol change becomes two commits in two repos in the right order, which is where Dolphin and the plugin would start drifting.)

## 4. Proving nothing changed

**Machine code, for the moves.** A step that only moves files (and repoints includes) is checked by the instructions themselves: each moved file's object in `brawl_online_core` against the object it had in Dolphin's `core` target, disassembled (`dumpbin /disasm`) and compared function by function (`E:\emissary\scripts\fn_compare.py` for now; anonymous-namespace path hashes and string-constant names are normalised, the latter differing only by the log macros' `__FILE__`). For that the core is compiled exactly as Dolphin's own code: added from `dolphin/Source/CMakeLists.txt` (Dolphin's Source-level definitions) with Dolphin's precompiled header. Steps 1 and 2a: 592 of 592 functions identical.

**The same header names, not a new interface, for Dolphin's helpers.** The moved code keeps including Dolphin's `Common/...` headers (files, timers, ENet, HTTP, UPnP, random, logging, settings). In Dolphin's tree those are Dolphin's headers, so the moves change no instruction; EmissaryRecomp provides headers of the same names with its own implementations (kalomaze's `gekkonet_compat/Common/Logging/Log.h` already does this for GekkoNet). A `Services` interface (§2) would have changed every call site, and so every function, before the recomp needs any of it. `Engine` (guest memory, save/load, the frame hooks) is still a real interface: Dolphin's emulator headers can't be stood in for.

The other checks:

The extraction is a refactor: Dolphin must behave byte for byte as before. Before the first step, a baseline is recorded with today's main; after every step it is recorded again and compared.

| Check | How | Pass |
|---|---|---|
| Unit tests | Dolphin `tests` target (`Online/`, `Rollback/` suites), mm `cargo test`, new core unit tests (they move with the code) | all pass |
| Replay corpus | ~200 prod `.brep` files (every mode, stage, 4-player rooms) through Dolphin's replay playback **and** the rollback benchmark (playback as a 3-frame sync test): every frame's checksum against the recording | 0 mismatches, same as the baseline |
| Golden sessions | Harness scenarios against local staging (`tools/staging`): Direct, Unranked, Ranked set with ping check, 2v2 with pair room, 3- and 4-player rooms with sit-out/watch, spectating, a forced desync and its recovery, a quit/runback, a launcher invite. Seeds and clocks fixed; recorded: every PeerData message both ways, every mm message, the PPOM Local/Session blocks each frame, the `.brep`, `game-status.json` | identical to the baseline after normalising ids and timestamps |
| Harness suite | the 93 `harness/tests` (42 online/rooms/ranked/replay) | same results as the baseline |
| Mixed versions | the refactored Dolphin against the released one (prod build) in staging, every mode | plays, no desync |
| Performance | `machine_pace` / speed numbers for a Direct match | no regression |

Each step is a commit that only moves code or puts an interface in front of a call, never both with a behaviour change; the golden diff of every step must be empty before the next.

## 5. Steps

| # | Step | Size | Risk |
|---|---|---|---|
| 0 | The baseline tooling: golden-session recorder and differ (harness), the replay corpus runner, the baseline itself | 2-3 days | none (tests only) |
| 1 | `core/` with CMake; move the pure files (PeerData, RoomMessages, MatchRules, SetFormat, ChatText, ChatCrypto, GameAlive, ReplayFile, constants) and their unit tests; Dolphin links it | 1-2 days | low |
| 2 | `Services` (log, files, paths, config, threads, timers, HTTP, ENet/UDP, OSD) with Dolphin's implementation; move Matchmaking, Ipv6, User, Chat, Ranked, Spectate, RecentCodes, OnlineClient, OnlineSession | 3-4 days | low-medium |
| 3 | `Engine` (from `Guest` + the writes and hooks GameBridge uses); move GameBridge, GameSetup, Rooms (its Other Games paths call back into Dolphin), RoomWatch, SpeedCheck, ReplayRecorder | 1 week | medium |
| 4 | Split GameplaySession: the session state machine into `core/session/`, the save/load/advance/re-run hiding into Dolphin's `Engine` | 1-2 weeks | high (the hot path) |
| 5 | The recomp's `Engine` + `Services`; its rollback loop driven by the core's session; delete the control socket and the companion; crossplay tests (recomp vs Dolphin in staging, every mode) | 1-2 weeks, with kalomaze | medium |

Steps 0-4 change only Dolphin and leave the recomp alone; step 5 is the recomp's. (Step 2 became: move the files with their `Common/...` includes as they are, §4; the recomp's same-named headers come with step 5.)

## 6. Where the work lands

Everything is on the `emissary-recomp` branch (based on main, which stays quiet meanwhile, 2026-11-13), and on our own branch on top of kalomaze's PR #2 (`rollback-support`) for the recomp side.

## 7. Decisions (2026-11-13)

- Q1: the core stays in the monorepo (`core/`); EmissaryRecomp fetches only that folder at a pinned commit (a sparse, blob-less clone in its `build.py`), no mirror repo and no full submodule.
- Q2: the work lands on `emissary-recomp`, not main (main stays quiet meanwhile).
- Q3: the recomp runs the shared session (our input delay, time sync, held buttons, re-run hiding, desync recovery, quit rules, replays, spectating); it keeps its own snapshot store, which is fast. The Python companion, `mmclient` and the control socket go.

## 8. Done

| Step | Commit | Check |
|---|---|---|
| 0 | `harness/tools/core_replay_corpus.py` (52b927cc) | a corpus of 50 of Sarah's replays (rooms, 2v2, Direct, Unranked, Ranked) through the baseline build: E:\emissary\core\baseline.jsonl; unit tests 1,612/1,615 (3 failures that predate this work) |
| 1 | 85085375: PeerData, RoomMessages, MatchRules, SetFormat, ChatText, GameAlive, Timeouts | 172/172 functions identical, same unit test results |
| 2a | 3d20718e: Ipv6, User, RecentCodes, ChatCrypto, Chat, Matchmaking, Spectate, ReplayFile | 592/592 functions identical (steps 1 + 2a), same unit test results |
| 2b | befdfe8c: OnlineClient, OnlineSession, Ranked, Config/OnlineSettings | 821/821 functions identical (steps 1-2b), same unit test results |
| 2c | 4637c92c: GameSetup, Rooms | 1,024/1,024 functions identical (steps 1-2c), same unit test results; Linux build |
| 3a | a82e083b: GameBridge, GameplaySession, GameplayRollback, ReplayRecorder | every function of the 24 moved objects identical (8,242, with the library code emitted beside ours), same unit test results |
| 4a | the `Engine` (core/include/BrawlOnline/Engine.h), slice A: the session's CPU and loop control (go on into the frame, skip it, back to the loop top, the FRAME_END store, the guest calls from the loop top, the saved registers, CPU running) go through it; Dolphin's is Rollback/DolphinEngine, set in Core::Init | code changed, so checked by running it: the 50-replay corpus plays the same (every replay passes with the same frames checked and end, 556,351 frames checksummed); unit tests as before; 52 of the harness's online tests (Direct, Ranked, rooms, 2v2, leaves, runbacks, sudden death, rollback): 41 pass, 3 more pass when run again alone on both builds (they had failed with three groups at once), 8 fail the same way on the baseline build |
| 4b | slices B and C: the session's emulated time and pacing (ticks, re-run flag, load wait, audio waits, speed adjustment, frame advance, present pacing, sleep lead, throttle re-anchor, rollback burst, schedule) and its snapshots (region set, save, load, ring, the mailbox hole, stats) go through the `Engine`, same names and types as CoreTiming's and RollbackManager's; core/ no longer includes CoreTiming.h, SystemTimers.h, VideoInterface.h or RollbackManager.h | code changed, so checked by running it: the corpus plays exactly as on the baseline (50/50, the same frames checked and ends, 556,351 frames); unit tests as before; harness: 25 of 26 online tests run (Direct, Ranked, rooms, Dolphin's netplay rollback) pass, alone where three groups or the corpus beside them had made them time out (the baseline fails one of the same alone). test_ranked_desync_restarts_the_game's own set-up (its scripted fight must land a hit, its walk-off must stop at one stock) fails at random on every build, the baseline's included (2 of 17 runs; 7 of 22 here): the match's RNG is seeded at random and the fight is driven by the wall clock. The session's resets and the bridge run before Core::Init, so Dolphin's engine is set when the program loads. After the commit (a4f57446): test_replay 12/12 (seeking, highlights, clips and renders; one render passed when run again alone, its recording's peer had timed out), test_spectate 3/3, test_sudden_death, test_salty_runback 3/3; the Linux build (GCC) and its unit tests (only PageTableHostMappingTest.MemChecks fails, as before) |

The old questions, as asked:

- **Q1, where the core lives:** in the monorepo with a mirror repo for the recomp's submodule (recommended, §3), or its own repo as a submodule of both?
- **Q2, where the extraction lands:** step by step on main, each step proven unchanged (recommended, §6), or on the `emissary-recomp` branch?
- **Q3, the recomp's rollback loop:** replace the recomp's own GekkoNet loop (`brawl_rollback.cpp`) with the core's session (recommended: one implementation of setup, checksums, desync recovery and quits), or keep it and share only the online/menu side? This one is kalomaze's as much as ours.

## 9. What core/ still takes from Dolphin (after 4b)

The 24 moved files still include 33 Dolphin headers. Each goes one of three ways: **same-named header** (the recomp ships a header of the same path whose types do the same thing over its runtime; Dolphin's code and machine code stay as they are), **Engine** (the engines behave differently: a call through `BrawlOnline::Engine`, Dolphin's implementation doing exactly what the code did), or **out of the core** (Dolphin-only code the core calls today: behind an Engine call or a hook Dolphin sets).

| Header(s) | Used for | Way |
|---|---|---|
| Core/HW/CPU.h, the PowerPC registers in the loop hooks | going on / skipping the frame, guest calls, saved registers | Engine: done (4a) |
| Core/CoreTiming.h, HW/SystemTimers.h, HW/VideoInterface.h, Core/Rollback/RollbackManager.h | the session's pacing and its snapshots | Engine: done (4b) |
| Core/System.h, Core/HW/Memmap.h, Core/PowerPC/MMU.h | guest memory (`Guest`, CopyToEmu, Read_U8, HostRead/HostWrite/HostIsRAMAddress) | same-named headers: guest memory is the same flat big-endian MEM1/MEM2 in both, and the checksum path stays a pointer read |
| Core/Core.h | CPUThreadGuard, Core::State, run-on-CPU-thread | same-named header (the recomp's hooks are already on its game thread) |
| VideoCommon/OnScreenDisplay.h | on-screen messages | shared: core/ui (§11), Dolphin's header is the shared one |
| Core/Config/MainSettings.h, ConfigManager.h, Config/GraphicsSettings.h | the online settings and a few of Dolphin's | same-named headers (the recomp's settings file) |
| Core/Brawlback/.../gekkonet.h, BrawlbackConstants.h, InputCommon/GCPadStatus.h, Core/Rollback/UdpSocket6.h | GekkoNet, constants, the pad struct, the IPv6 socket | into core/ itself (shared code with no Dolphin types) |
| VideoCommon/Fifo.h, AsyncRequests.h, Present.h, Rollback/PresentStats.h, FrameProfile.h, ClipRecorder.h | GPU determinism and sync, redraw while paused, Dolphin's frame diagnostics, clips | Engine (no-ops or the recomp's own where it has them) |
| AudioCommon/*.h, HW/GCPad.h, InputCommon/GCAdapter.h | audio during re-runs, local pad reads | Engine |
| Core/PowerPC/PowerPC.h, Debugger/OSThread.h, Harness/Harness.h | the loop-stall diagnostic, guest backtraces, harness hooks | Engine (diagnostics), harness out of the core |
| Core/Rollback/MachineSession.h, ReplayKeyframes.h, NetPlayClient.h, Online/RoomWatch.h, SpeedCheck.h, ModsRoom.h, SpecialAssets.h | Other Games (whole-machine sessions, mods rooms), replay keyframes, room watching, the speed check | out of the core: a hook Dolphin sets, none in the recomp (P+ only) until those move too |

## 10. EmissaryRecomp in-process (2026-11-15)

Sarah's goal: the core end to end in EmissaryRecomp, tested and not desyncing (recomp against recomp, then against Dolphin), so the rest is UI, QA and speed.

- **Standalone build (core/standalone/, 00afd805).** Built outside Dolphin's tree from what the recomp fetches with a sparse clone: `core/`, `dolphin/Source/Core/Common` (Dolphin's own Common, compiled as is), `dolphin/Source/Core/Core/Brawlback/include` (GekkoNet, constants), a few Dolphin files the core reuses as they are (FrameProfile, UdpSocket6, ReplayPlayback, GameplayOnlineBackend; their headers are copied into the build's own include root, so Dolphin's real `Core/` headers are never seen) and the Externals they use. Toolchain: the recomp's llvm-mingw clang, C++23. Dolphin's Common has a few MinGW guards (share modes, stat, SEH in SetThreadName, cpuid), each false under MSVC. GekkoNet: the recomp's vendored copy (identical to Dolphin's) when the parent build has the target (`BRAWL_ONLINE_GEKKONET_TARGET`), else its own.
- **Compat headers (core/compat/include).** Same-named stand-ins for the Dolphin `Core/`, `VideoCommon/`, `AudioCommon/` and `InputCommon/` headers the core includes, over `BrawlOnline::Host` (guest memory, on-screen messages, local pads, stop, host jobs, `HarnessActive`); no-ops for Dolphin-only parts (Other Games' MachineSession and mods rooms, clips, frame profiles, present stats, GPU FIFO).
- **The recomp side** (EmissaryRecomp, our branch `brawlonline/windows-build` on PR #2): `runtime/include/brawl_online_bridge.h` joins the two halves in plain types (both have a `Memory`); `runtime/brawl_online/` is the core's side (the Engine and Host over the bridge, the hooks), `runtime/src/brawl_online_core.cpp` the runtime's. The loop: at `LOOP_TOP` (0x800171B4) the core says go on, skip (the loop end, then one `VIWaitForRetrace`, as Dolphin's CPU keeps running while its throttle paces), or call (a guest function, then the loop top again); `FRAME_END` (0x80017504) and `LOOP_END` (0x80017508) are interpreter pc hooks; the pad thread's read and update run where updateLow returns (0x8002BA84); the stage hooks at `__ct__7stMelee` and 0x80949BC0. Snapshots: the recomp's SnapshotStore and WriteWatch over the region set the core resolves (merged by physical address), a ring of 8 like RollbackManager's. Time: DetClock (60.75 MHz time base, paced to real time). The harness protocol (JSON lines over TCP, `brawl_online_harness.cpp`) answers what the online tests use, so ppharness drives a recomp as it drives Dolphin (`harness/ppharness/recomp.py`, `PPHARNESS_RECOMP` / `PPHARNESS_RECOMP_NAMES`), and a test can pair one with a Dolphin.
- **MEM1's mirrors.** Leaving the stage select where anyone picks, P+ online's automatic A finds no port that pressed it and Brawl reads a pad status of port -1 (gfPadSystem - 0x40), whose word 0x805BA480 indexes a table at 0x806B91C8 to 0x8348B5C8. Dolphin masks that with its 32 MiB RAM mask (MEM1 0x8148B5C8, a real value that decides a branch); the recomp had no mapping there and crashed. The recomp now maps MEM1 again every 32 MiB of 0x80000000-0x87FFFFFF and 0xC0000000-0xC7FFFFFF (views of the same backing).
- **Pads.** A harness recomp has a controller on port 1 only, as Dolphin's harness instances do (the other ports read error 0xFF), so the game's pad memory is the same on both.

Checked (2026-11-15, recomp build of `brawlonline/windows-build` + this branch at a762eff4's core, Dolphin from this branch):

| Check | Result |
|---|---|
| The 50-replay corpus played by the recomp | 50/50 pass, 556,690 frames checksummed, the same ends as Dolphin (again on the final build: every replay the same frames) |
| test_direct_set_under_the_gameplay_session, recomp vs Dolphin | pass twice: 3 games (random, FD with Z, Battlefield with L), 8,726 and 8,478 confirmed checksums, 0 mismatches, frame traces identical; the recomp is the slow side, so only Dolphin rolled back (1,079 and 1,062) |
| the same, recomp vs recomp | pass: 6,134 confirmed checksums, 0 mismatches (24 rollbacks: a local link) |
| test_direct_set_through_netsim[bad_wifi] (new: the same set through netsim, 60 ms, 25 ms jitter, 2% burst loss), recomp vs recomp | pass: 340 rollbacks (depth up to 7), 2,777 confirmed checksums, 0 mismatches, traces identical |
| the same, recomp vs Dolphin | 10,867 confirmed checksums, 0 mismatches, traces identical in all 3 games; Dolphin rolled back 1,234 times, the recomp (the slower side, so it rarely predicts) 8 times (the test asked for both until then; now for either) |

Not yet: the other modes (Unranked, Ranked, rooms, 2v2, spectating, replays recorded live by the recomp), the srand/srandi seed hooks (Dolphin's char-select random), the sound allocation hooks, time sync's speed adjustment and pacing schedule (the recomp's Engine has no-ops for `SetRollbackSpeedAdjustment`, `AdvanceThrottle` and `ScheduledHostTime`), Linux and macOS builds of the recomp side.

Speed (2026-11-19, after merging kalomaze's experimental-horizon-optimizations, recomp 771c27b): the recomp holds 60 fps in a match against Dolphin, and a rollback costs 6-9 ms on average (about 2 frames deep, max 27 ms) through netsim's bad_wifi. Before, it ran 40-51 fps with rollbacks of 30-75 ms: 45% of its game thread was in `GuestCodePatch::Sync()` (RunPatched syncs after every call of a patched function, and each sync compared every open code-and-data page word by word against its pristine copy); his branch compares each page with its last-synced copy (`g_seen`) and translates P+'s fixed patches ahead of time (code variants). One replay alone: 198.3 s before, 116.5 s now (real time 109 s); Sync() 58.5% of the game thread before, 1.5% now. His other switches: lean memory stays off on PC (its masked window has no MEM1 mirrors, above); MKW_FAST_FLOAT, MKW_EABI_REGS and MKW_LEAN_DIRTY_MARKS=OFF are not for online play (not bit-identical, P+ breaks it, no rollback). Open: once in 5 crossplay sets, game 1 stopped right after the start barrier (neither side heard the other; not seen again with a stall catcher); once in 50 replays started six at a time, SDL_GPU's Vulkan device was lost at start-up and aurora's thread crashed on it. The Windows build gets no Direct3D 12 driver from SDL_GPU (its shaders are SPIR-V only) and runs Vulkan.

## 11. The in-game UI, shared (2026-11-19)

Sarah's board: "we need chat, probs using the same imgui backend, so we can reuse the code, along with the spectator chat and replay seek / clip features. we need the clip hotkey".

**One implementation: `core/ui`.** Dolphin's on-screen messages (`OSD::`, VideoCommon/OnScreenDisplay), the chat window with spectator chat (`OnlineChatUI`) and the replay overlay with its seek bar, highlights and clip mode (`ReplayOverlayUI`) moved there as they were (`BrawlOnline/UI/*.h`, namespaces unchanged), in their own library `brawl_online_ui` over an `imgui` target: Dolphin's Externals/imgui, or the recomp's (aurora's, brought to Dolphin's 1.92.2b: fonts of any size, glyphs made when first drawn, the same rasterizer). What only an engine knows comes through `BrawlOnline::UI::Hooks` (`BrawlOnline/UI/Hooks.h`): the game's rectangle in the window (Dolphin: the presenter's target rectangle; the chat's default place), whether OSD messages show (Dolphin's [Interface] setting), blocking the game's keyboard and mouse while the box has the keyboard (Dolphin: ciface; the recomp polls `IsTyping`), a texture for a message's icon (Dolphin's achievement badges), and every message added (the recomp logs them, as before). The fonts and style are shared too (`BrawlOnline/UI/Style.h`: Dolphin's OSD font, Noto Sans with Noto Sans JP merged in for the chat, ImGui's dark style with rounded windows), always stb_truetype. Dolphin's VideoCommon/OnScreenDisplay.h is now the shared header (`../core/ui/include` is on Dolphin's include path); OnScreenUI loads the fonts and sets the hooks through `core/ui` and draws exactly as before. The standalone build has `BRAWL_ONLINE_UI` (on in the recomp): with it the compat OnScreenDisplay.h is the shared one; without, messages go to `Host::OnScreenMessage` as before.

**In the recomp** (EmissaryRecomp `brawlonline/ui`): `runtime/brawl_online/brawl_online_ui.cpp` draws the same three in the runtime's ImGui frame (settings_overlay::Draw, once per presented frame), in Dolphin's order (chat, overlay, messages), with Dolphin's style and font pushed and the runtime's put back after. Dolphin's ImGui coordinates are backbuffer pixels scaled by the DPI scale; the runtime's are window points with the framebuffer scale applied when drawn, so the UI draws at scale 1 with no framebuffer scale. Messages go under the runtime's F10 bar when it shows (its FPS box is top-right, so the top-left corner is free). The chat key is Dolphin's ([Online] ChatKey, a Dolphin input expression parsed by the runtime's InputExpr: Windows key names, Dolphin's controller button names on SDL gamepads; T when unset); while the box has the keyboard the keyboard and mouse bindings read as released (aurora `PADBlockKeyboard`) and controllers play on, as in Dolphin, and typed keys are not the runtime's hotkeys. [Online] DisableChat and the chat's text size and window geometry are read from Dolphin.ini by the same code. The recomp's harness answers `chat_status`, `chat_send`/`chat_hide`/`chat_report`/`chat_leave`, `chat_ui` and `osd_messages` (Dolphin's commands, the same core calls).

Checked: Dolphin unit tests as before (1,641 pass, OnlineChatUITest included; the 3 old failures); harness test_chat 2/2 on Dolphin; `harness/tests/test_ui_crossplay.py`, Dolphin against the recomp (PPHARNESS_RECOMP_NAMES=game-b, with PPHARNESS_UI_TYPE=1): connected on the Direct CSS, both chat ways on the CSS and in game 1 (kana and kanji), the window drawn open on both at the same size and font (264x144, 12 px in a 900x506 window), the core's messages up ("Ping: 16"), and the recomp's player types a line with the keyboard (T, the text, Enter) that Dolphin receives. Screenshots: run/artifacts/ui-crossplay/.

Not the same yet: an OSD message window made after the chat window is drawn over it (ImGui's order of first appearance: in Dolphin a boot message comes first); the recomp has no "Show / Hide Seek Bar" hotkey; the recomp's chat key on a controller and DisableChat were not tried.

**Clips.** The clip hotkey is the launcher's (Highlights scans each replay's inputs with the hotkey of its time, launcher lrz_clips.ts); the recomp records the same .brep files with the core's ReplayRecorder (same folder and names: `Replays/<yyyy-mm>/<date> Direct G1 - ... .brep`, seen in crossplay runs since 2026-11-15; the press after GAME! is the same recorder code, its AFTR chunk, not tried in the recomp), so its matches' clips should come up in Highlights as Dolphin's do (not tried with a launcher). Rendering a clip to MP4 (Rollback/ClipRecorder: frames from the frame dumper, samples from the mixer, Media Foundation / AVFoundation / FFmpeg encoders) stays in Dolphin: the launcher renders highlights with its playback Dolphin, from any engine's replay. For the recomp to render, it needs: presented frames read back at a tick (BRAWL_CAPTURE_FRAME reads a frame back, not each presented frame in order), the DSP's samples with their ticks, and ClipRecorder plus its encoders moved out of Dolphin (they need nothing of Dolphin's but those two inputs).

**Room watch.** Online/RoomWatch.cpp is still Dolphin's (the hook the core calls): it arms a live playback in the netplay Dolphin from mm's watch channel and pads Dolphin's emulated controllers for START/B. The recomp plays replays with the same playback code (ReplayPlayback.cpp in the standalone build) and now draws the overlay (corpus replay 001 through the comm file: title, players, play/pause, seek bar, Clip and time over the match; its title card, which covered the whole playback, is lifted while a playback runs), but RoomWatch needs, before it can move: its controller reads through the Engine/Host (GCPad, Wiimote, GCAdapter today), the playback armed in the recomp's running game (ArmPlayback room_watch: the recomp's playback today starts at launch from BRAWL_ONLINE_REPLAY_COMM), and seeking, which in Dolphin loads savestate keyframes (ReplayKeyframes, out of the core; the recomp has its snapshot store but no keyframes).
