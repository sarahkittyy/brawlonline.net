# Brawl Online core: one online implementation for Dolphin and EmissaryRecomp (design)

Status: design, 2026-11-13. Not started. The user's request (Sarah's board, 2026-11-13): "we need to get rid of the companion app. i know dolphin and the recomp are both c++(?) so maybe it would make sense to have one submodule that both depend on??? it would also require a lot of tests to make sure nothing at all changed when we modularize it out."

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
- **EmissaryRecomp gets it as a submodule** of a small read-only mirror repo (`sarahkittyy/brawl-online-core`), which CI on this repo updates from `core/` (`git subtree split`) on every push to main. The recomp pins a commit; bumping it is the "recomp protocol" update of brawl-recompiled.md §7.4.

(Alternative: the core as its own repo, a submodule of both. Simpler mechanics, but every protocol change becomes two commits in two repos in the right order, which is where Dolphin and the plugin would start drifting.)

## 4. Proving nothing changed

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

Steps 0-4 change only Dolphin and leave the recomp alone; step 5 is the recomp's.

## 6. Branch or main

These files change about twice a day on main (31 commits touching `Online/` or `GameplaySession.cpp` in the last two weeks). A branch that moves them all would conflict with nearly every one of those commits, and resolving a conflict in a moved file means re-applying the change by hand. So the extraction steps (0-4) should land on main one at a time, each verified by §4 and kept short, rather than on `emissary-recomp`; only step 5 (and the launcher's EmissaryRecomp work) belongs on the branch and on kalomaze's PR. **Sarah's call (§7, Q2).**

## 7. Questions for Sarah

- **Q1, where the core lives:** in the monorepo with a mirror repo for the recomp's submodule (recommended, §3), or its own repo as a submodule of both?
- **Q2, where the extraction lands:** step by step on main, each step proven unchanged (recommended, §6), or on the `emissary-recomp` branch?
- **Q3, the recomp's rollback loop:** replace the recomp's own GekkoNet loop (`brawl_rollback.cpp`) with the core's session (recommended: one implementation of setup, checksums, desync recovery and quits), or keep it and share only the online/menu side? This one is kalomaze's as much as ours.
