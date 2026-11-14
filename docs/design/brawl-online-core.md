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

The old questions, as asked:

- **Q1, where the core lives:** in the monorepo with a mirror repo for the recomp's submodule (recommended, §3), or its own repo as a submodule of both?
- **Q2, where the extraction lands:** step by step on main, each step proven unchanged (recommended, §6), or on the `emissary-recomp` branch?
- **Q3, the recomp's rollback loop:** replace the recomp's own GekkoNet loop (`brawl_rollback.cpp`) with the core's session (recommended: one implementation of setup, checksums, desync recovery and quits), or keep it and share only the online/menu side? This one is kalomaze's as much as ours.
