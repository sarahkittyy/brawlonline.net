# Gameplay-only rollback: status

Branch `gameplay-rollback` in the worktree `dolphin-gprb/` (off `rollback-fixes`, merged with `rollback-fixes` at `a1f9ec2685`, at `d36794a6e1` (the online client), at `ad474c0358` (GameBridge, recent codes, Qt session backend), and in Phase 8 with `c27636d256`, `4944245954` and `50e800b9d6`). Not pushed. Head: `7788a27738`. **Default region set: gp-v19** (Phase 8); **gp-v21 since branch `nplayer-determinism`** (P+'s per-port records for all four ports, and the Tmp heap: `docs/nplayer/determinism.md`). **Merged into `rollback-fixes`** (`df44299556`, 2026-10-07), where the gameplay session is now the default online backend and starts matches from the game's own online CSS (Phase 7). Phase 9 (open issue 9, a race in the snapshot code) is on `rollback-fixes` at `001dd0b2df`.

**The session model** (user decision):
- Each player boots and uses the menus alone.
- They connect on the character select, exchange selections and start the same match.
- Only gameplay state is saved and restored during the match.

**Measurement caveat.** One 8-core Windows PC, shared with other agents' Dolphin instances.
- Single core = `CPUThread = False`. Dual core = `CPUThread = True` with P+'s `GPUDeterminismMode = fake-completion`.
- Video `Null`, muted.
- FPS and timings are noisy.

## Tools

All in `harness/tools/`. They need numpy.

| Tool | What it does |
|---|---|
| `gprb_ab.py record` | One instance plays a closed-loop match from a sim-start fixture. Every frame it records the `gfPadStatus` slots the frame consumed (`game_pads`, anchored, see Phase 2). |
| `gprb_ab.py pair` | A and B (different menu histories) load their own sim-start fixtures and get the sync writes (RNG, `g_GameFrame`, gfApplication counter). Then both run freely (no per-frame pauses, unthrottled) with the recorded input, given one of two ways: injected frame-anchored (`--input inject`), or replayed through the SI like the spike did (`--input si`). Their per-frame traces (`frame_trace`) are compared frame by frame. |
| `gprb_late_divergence.py`, `gprb_transplant.py`, `gprb_nudge.py`, `gprb_debug.py` | The Phase 2 investigation. |
| `gprb_synctest.py` | Phase 3 single-instance sync test. |
| `gprb_session.py` | Phase 4 two-instance sessions through netsim. |
| `gprb_online.py` | Online hand-off: our accounts/mm servers, two logged-in instances, Direct search with `backend="gameplay"`, one match, then a leave or freeze case. |
| `gprb_resimtrace.py` | Instruction-trace diff of a frame's first run against its resimulation. |
| `gprb_rngcmp.py` | Compares two peers' `PPR_GPRB_RNG_LOG` logs frame by frame (newest pass) and shows the first differing `mtRand` calls with their callers. |
| `gprb_changed_mem.py` | Memory outside the region set that changes from frame to frame in a match. |
| `gprb_mispredict.py` | Phase 6: one instance from a countdown savestate with recorded input: `ref` (sync test), `mp` (misprediction sync test), `replay=<pass log>` (a network session's peer, exactly), `--no-rollback` (ground truth); traces compared with a reference; sound sampling. |
| `gprb_passlog.py` | Phase 6: show and edit pass logs (`fix`, `norb`, `flat`). |
| `gprb_memdiff.py` | Phase 6: whole-memory dump diffs outside/inside the set, the DOL objects behind them, and a scan for global list heads whose nodes are rolled back. |
| `gprb_hang.py` | `cdb` stacks of every thread of a hung instance. |
| `gprb_replay_stress.py` | Phase 9: one pass log replayed N times (J at a time), optionally with CPU-burning processes at normal priority (`--burn`); each replay is classified (identical to the ground truth, drift, hang with `cpu_state` and back chain, crash, harness error). |

New harness commands (`Source/Core/Core/Harness/HarnessServer.cpp`), listed under [Harness commands](#harness-commands): `frame_trace`, `frame_trace_config`, `game_pads`, `cpu_trace`, `mem_chunk_hashes`, `disasm`, `timing_nudge`, `gprb_synctest`, `gprb_connect`, `gprb_set_selections`, `gprb_status`, `gprb_checksums`, `gprb_stop`.

## Phase 1: dual core on the new code

**Setup:**
- Experiment 4 of the feasibility spike: identical inputs; A and B with different menu histories; RNG and frame counters synced at the simulation start.
- Run on the merged build, with the spike's PS2 fixtures: Peach vs Game & Watch on Pokémon Stadium 2, items off.
- Compared every game frame, from the per-frame traces: per-port fighter state, the three RNGs, and game set.
- The instances never pause, so the old harness pause crash cannot interfere. The coordinator reports it fixed by `a1f9ec2685`.

**Inputs.** Two recorded closed-loop matches:
- "calm": runs to the 8-minute time limit; game set at frame 29,014.
- "gentle": ends by stocks; game set at frame 22,884.

**Result: runs identical through game set** (raw: `run/qa/gprb3/m1-*.json`).

| Input given to A and B | Single core | Dual core |
|---|---|---|
| Frame-anchored injection (Phase 2 fix) | **4 / 4** | **6 / 6** |
| Through the SI, as the spike did | 0 / 4. All diverge at frame 214 (the first input after GO). | 2 / 6. The others diverge at 214, 8,775 and 8,783. |

**What this shows:**
- **Dual core is not worse than single core** once the input is anchored to the game frame. 6 of 6 full matches were bit-identical in every compared field through game set, including a full 8-minute match.
- The spike's dual-core divergences were the same input race as its late single-core divergence (Phase 2), plus the harness pause crash. Its 6-of-11 crash rate came from pausing, and nothing pauses here.
- The SI method's divergence depends on scheduling. That is why its dual-core runs were sometimes identical (both peers happened to race the same way) and sometimes not.
- **After game set** every pair (and even A against its own recording) diverges in `g_mtRand`, 6-20 frames after game set. That part of the end-of-match sequence depends on timing. It is Phase 5 material.

## Phase 2: the late divergence (frame ~8,450)

**The leading hypothesis was wrong.** The P+ Gecko code handler does not run from the audio hook during a match:
- The vBrawl/P+ hook-type code moves the handler from `AXNextFrame` **during boot** to VBI **after the strap screen** (the spike and research 05 read it the other way around).
- Live, in a match:
  - `0x801E9A2C` (the `blr` of `fn_801E99C4`, called from the VI retrace interrupt) = `b 0x800018A8`, the handler entry;
  - `0x80200984` (`AXNextFrame`) = `blr`;
  - the strap-screen flag `0x80497ED0` = 1.
- The handler therefore runs at the VI retrace, right before the main thread wakes for the frame.

**The real cause: Brawl's pad thread races the main game loop.**

**1. Reproduced on the new build.** The spike's two savestates at frame 8,420 (A and B, `run/qa/gprb/ps2/lockstep-8420-*.sav`), replayed with the recorded inputs, still diverge at game frame 8,455:
- G&W's x is −71.8405 on A and −71.7205 on B;
- action, animation frame, pads (as sampled at frame boundaries) and RNG are equal.

**2. Not the JIT.** The interpreter (`CPUCore = 0`) reproduces it bit for bit.

**3. Instruction trace.** A new interpreter trace (`cpu_trace`) records PC, effective address and the loaded or stored value for every instruction. It covers the main thread, other threads and interrupt context, over game frame 8,455 on both A and B.
- **The first control-flow difference** is a scheduler list iterating different tasks:
  - On B's menu path, the `StLoaderPlayer` tasks were destroyed and recreated, but `StLoaderPokemonSe` survived, so the order differs.
  - This is present in every frame. Relinking B's lists to A's order did not move the divergence, so it is harmless here. The session copies the host's task order at the barrier anyway.
- **With the order equalized, the first relevant difference is input.**
  - G&W's input processing (fighter at 0x812DDD00) copies its controller from a struct at 0x805BC060.
  - That struct is filled from a stack copy of `gfPadStatus` slot 1 (0x805BAD40).
  - Stick X reads 0 on A and 0x7F on B. P+'s stick code (0x8056ED38) turns that into 0.0 against 1.0, so G&W moves 0.12 further on B.
- **Who writes slot 1.** The slots are written only by the **pad thread**:
  - OSThread 0x805BA108;
  - `gfPadSystem::updateLow` → `updateLowGC` 0x80029680-0x800296E8;
  - then P+'s post-processing, which P+ chains in place of `updateLow`'s `blr` and which returns to 0x8002BA84.
- **The race.** The pad thread writes the slots once per frame, at a time that is not tied to the main loop:
  - on B its update for that frame ran *before* the main loop's top;
  - on A it ran just *after* the main thread had copied the slot (record #183,840 against #2,026).
- So the same input sequence reaches the game, but which sample a game frame consumes depends on thread timing, i.e. on the emulated-time phase. That phase differs between machines with different menu histories.
- This explains why the divergence:
  - is rare;
  - lands at a seemingly random frame;
  - changes with the RTC and the menu history;
  - shows no memory precursor at frame boundaries (the spike sampled the slots at frame boundaries, after the pad thread had caught up on both).

**4. Fix: frame-anchored input (`Gprb::Pads`, `GameplayRollback.cpp`).**
- At the top of every main-loop iteration, the slots of the driven ports get that frame's input.
- The new HLE hook at 0x8002BA84 runs right after every pad-thread update (and after P+'s post-processing). It keeps the fresh sample as the local input and puts the frame's slots back.
- So the main thread sees one constant input for the whole frame.
- The harness exposes this as `game_pads`: record, anchor, inject per game frame. The gameplay session feeds GekkoNet's input through the same path.
- **Proof:**
  - with A's per-frame slots injected into B, B matches A through frame 8,600, where it diverged at 8,455 before;
  - over full matches it holds 10 of 10 (Phase 1 table, including the full 8-minute match in single and dual core).

**Also investigated, for the record:**
- Transplanting all of B's differing memory into A's state hangs the game: OS state and hardware state disagree.
- Transplanting only non-OS memory gives a third outcome (neither A nor B).
- Small emulated-time shifts of A (`timing_nudge`, up to 100k cycles) change nothing over 500 frames. Larger ones also move SI latching, so they are not a clean test.
- Code memory and lowmem are equal on A and B at 8,420, except known words (OS current context, `__OSStartTime`, 0x800B9F84).

## Phase 3: region-restricted snapshots and the sync test

### What was built (`dolphin-gprb`)

**`RollbackManager` region mode** (`BeginRegionMode`):
- Saves and loads cover only the 64-byte granules of a region set, with Brawlback's incremental dirty tracking (`DeltaSaveSlot`).
- Dirty granules outside the set are dropped at save time.
- A load restores the set's granules and the main thread's registers, which are taken at the loop top.
- Device, timing, CPU-slice and L1 state are not restored: emulated time keeps running forward through a rollback, as in Slippi.

**Region sets** are data: `Data/Sys/Rollback/gp-v*.json`. Each file's `notes` say what changed and why.
- Heaps are resolved by name from the live heap table at session start, and `rel_data` adds every loaded REL's data and bss.
- `add` and `remove` take static ranges.
- **gp-v8 is the default.**

**The gameplay session** (`Gprb::Session`, `GameplaySession.cpp`):
- It drives Brawl's game loop through the existing loop-top, frame-end and loop-end HLE hooks.
- GekkoNet is used directly; there is no `NetPlayClient`.
- The same machinery runs both:
  - the **sync test**: a GekkoNet stress session, armed by `gprb_synctest`, that starts at `start_frame`;
  - the network session (Phase 4).

**Sync test checks.** Every frame, the state from `distance` frames back is restored and the frames are simulated again. Each frame's checksum is compared with its first run:
- the checksum covers the frame counter, the three RNGs, and per port the fighter's active instance, damage, stocks, x, y and status kind;
- a mismatch is logged per part (`desync_log`);
- optionally (`hash_regions`), every 4 KiB chunk of the region set is hashed and compared too.

### Fixes the sync test forced

Each one is a commit on `gameplay-rollback`.

| Symptom | Cause | Fix |
|---|---|---|
| Resimulated frame one game frame short; RNG different | Brawl's main loop paces `gameProc` by emulated time (`r20` = VI fields elapsed, accumulator at gfApplication+0xFC, 0-3 logic steps per pass). Region mode does not rewind time. | While a session drives the loop, every pass runs exactly one logic step (HLE `GprbPacingElapsedHook` 0x800172D0, `GprbPacingStepsHook` 0x8001730C). Lag slows the game down instead of skipping. |
| Main thread hung in `__fwrite`/`__memrchr` with a trashed stack | gp-v1 includes 0x80493EC0+0x40, which is the C stdio `FILE` table (`__files`). Another thread's `OSReport` print was rolled back under it. | gp-v2 drops it. |
| Corrupted loads, then crashes, in the countdown | The loader thread allocates and loads RNG-chosen Pokémon and Assist Trophy resources into set heaps during the countdown. | Rollback starts at `start_frame` (240, after GO). The countdown runs with neutral input and no rollback. After GO, no thread other than the main thread writes the set (all-threads trace of frames 225-240). |
| Dolphin crashed in `LoadIndexedXF` | A resimulated frame's render emitted an indexed XF load from a guest address outside RAM. | `VideoCommon`: such a load reads zeroes. |
| Main thread spinning with interrupts off in `fn_8002C49C` | WiiPad list nodes were restored without their list heads (in System FW), forming a cycle. | gp-v3 dropped WiiPad. gp-v6 restores both halves. |
| Main thread never left `gfTaskScheduler::process` | Tasks live in set heaps but the scheduler's list heads live in System FW. Tasks created or removed in rolled-back frames left a cycle. | gp-v5 adds the scheduler (0x805B8A00+0x140) and AreaManager's links. |
| Desync after the first button presses | (a) Input history (`gfPadSystem` debug/game/menu/merged pads) is outside the set. (b) `updateGame` fills the game pads from a pad queue that the pad thread fills asynchronously. (c) More input state: the controller struct 0x805BC060, the Replay heap, the main thread's stack above the loop (a frame counter at 0x805B50D8), and two areas P+ uses for its own input data outside every heap (0x935F0000, 0x805A7400). | (a) and (c): gp-v4 to gp-v8. (b): while input is anchored, bit 0x08 of `gfPadSystem+0x34` is cleared at every loop top, so the game pads come from the slots. |

**How the input-state rows were found.** `harness/tools/gprb_resimtrace.py` saves a countdown savestate with the JIT. Then, under the interpreter, it records the first run of a desyncing frame and its resimulation (`cpu_trace occurrences=2`, armed after the load) and lists the first loads whose values differ. Each row of the table above came out of one such trace.

**Coverage caveat.** P+ v3.2 has no Pokémon Trainer: Charizard, Squirtle and Ivysaur are standalone characters. Zelda and Sheik are separate characters too, with no transformation. Those two requested cases are covered by the standalone Pokémon and Zelda.

The CSS recipe in `ppharness` cannot reach some icons (Peach) on the current P+ CSS, on any build. The sync test therefore picks Fox and Falco and writes the wanted characters into `gmSelCharData` on the stage select, which is also how a session applies the peer's selection.

### Results: gp-v8 sync test, full matches

Single core, `distance` 2 (every frame: restore the state of 2 frames back, run 1 frame again, then the new frame), both ports driven closed-loop with random macros until game set. Commits `2605f5b123` (gp-v8) and `fb111dc1ba` (session end at game set). Raw: `run/qa/gprb5/b2`, `b3`.

| Scenario (P1, P2, stage, items) | Frames | Rollbacks | GekkoNet checksum mismatches | End |
|---|---|---|---|---|
| Fox, Falco, Battlefield | 5,057 | 5,055 | **0** | game set (stocks) |
| Mario, Marth, Final Destination, **items on** | 17,490 | 17,488 | **0** | game set |
| Ice Climbers, Charizard, Smashville, **items on** | 14,810 | 14,808 | **0** | game set |
| Ice Climbers, Olimar, Pokémon Stadium 2 | 28,793 | 28,791 | **0** | game set (8:00 time-out, all PS2 transformations) |
| Squirtle, Zelda, Smashville | 28,793 | 28,791 | **0** | game set (8:00 time-out) |
| Peach, Game & Watch, Pokémon Stadium 2 | 2,621 | 2,619 | **0** | main thread crashed (see open issues) |

Earlier gp-v8 runs (before the session-end change): Fox/Falco 4,704 frames; Ice Climbers/Olimar 7,273; Squirtle/Zelda 7,638; Ice Climbers/Charizard with items 12,455; Peach/Game & Watch 1,997. All 0 mismatches.

Not a single checksum mismatch in about 150,000 resimulated frames. The checksum covers the frame counter, the three RNGs and, per port, the fighter's instance, damage, stocks, position and action state, so a wrong region set shows up within frames (gp-v3 and gp-v4 did, at the first button press).

**Deeper rollbacks.** `distance` 7 (6 frames resimulated every frame, the full prediction window), gp-v8: Fox/Falco 5,355 frames and Ice Climbers/Charizard with items 11,019 frames, both to game set with **0 mismatches** (`run/qa/gprb5/d7`).

**gp-v9** (gp-v8 plus the camera manager, see Phase 4), `distance` 2, raw `run/qa/gprb5/v9`. All 0 mismatches:
- Fox/Falco: 5,539 frames, to game set.
- Mario/Marth with items: 18,350 frames, to game set.
- Squirtle/Zelda: 28,793 frames, to game set.
- Ice Climbers/Olimar: 21,054 frames, to game set.
- Peach/Game & Watch: 4,257 frames, then a crash (open issues).
- Ice Climbers/Charizard with items: 2,056 frames, then a stall (open issues).

**Dual-core sync test**, Mario/Marth on FD: 21,286 frames, 0 mismatches.

**Region hashes are not a criterion.** With `hash_regions` every 4 KiB chunk of the set is hashed after each save and compared with the frame's first run. In region mode these differ on every frame, while the gameplay checksum never does. `PPR_GPRB_DIFF_FRAME` (byte diff) and a `cpu_trace` of a first run against its resimulation show why:
- The main loop reads `OSGetTick` right after the loop top (`r28`, the frame's start time). Region mode does not rewind time, so the resimulation sees a later tick. P+ code (0x808F3xxx) keeps counters derived from it (0x80B8A2BC: 0x3B against 0x3A), and so does a .sbss sum at 0x805A0368.
- Dead stack below the loop's stack pointer and uninitialised locals (`fn_80116F98` reads a byte at 0x805B4755 before writing it).
- The camera's view matrices (copied into many objects) differed: `gfCameraManager` was outside the set. gp-v9 adds it (Phase 4); with it, frame 10 of a Fox/Falco test differs in 6 spans instead of about 150, all of them tick-derived or dead stack.

None of the remaining ones reaches the fighters, the RNGs or the frame counter in a sync test. The sync test therefore runs with `--no-hash`. This is the price of not rewinding time, the same trade Slippi makes.

### Cost: gp-v8 against whole-machine snapshots

Fox/Falco on Battlefield, `distance` 2, the two runs side by side on the same machine (raw: `run/qa/gprb5/cost`). Both use Brawlback's incremental (dirty-granule) saves.

| | Region | Save, average | Load, average | Load, max |
|---|---|---|---|---|
| gp-v8 | 45.4 MB in 98 ranges | **0.55 ms** | **0.96 ms** | 2.5 ms |
| Whole machine | MEM1 + MEM2 + device state | 0.90 ms | 1.57 ms | 4.7 ms |

The region set is about 40 % cheaper per save and per load. The gain is modest because dirty tracking already skips clean memory. The real gain of region mode is correctness: device, timing and OS state are never rewound.

**Save outliers of 5-17 s** (`save_us_max`) were not save work. They were synchronous log writes: `RollbackManager` logged two INFO lines per frame, and a write stalled when the shared disk was saturated (the other agent's runs showed the same 16.8 s maximum). A WARN with a timing breakdown now reports any save over 100 ms; it never fired. The per-frame lines are DEBUG now (`fb111dc1ba`).

### Open issues (Phase 3)

See [Open issues](#open-issues) at the end: Peach crashes (an article outside the set), and the main-thread stalls.

## Phase 4: two independently booted instances in one session

### How a session starts (`Gprb::Session`, network mode)

1. Both instances boot alone and reach the character select by different menu paths. B picks and drops other characters, backs out to the main menu, opens Rules, comes back, picks in the other order and lingers on the stage select.
2. On the CSS: `gprb_connect` (A hosts, B joins, through a `ppharness.netsim` proxy). They exchange state over UDP (JSON control messages, ping/RTT, and GekkoNet packets on the same socket).
3. `gprb_set_selections` exchanges the selections. Each instance drives its own CSS/SSS to the agreed match. The sync test's `gmSelCharData` write is the fallback for icons the CSS recipe cannot reach. At its first pass through `scMelee` the joiner takes the host's `gmGlobalModeMelee` init block (stage, rules, stage variant), waiting for it if needed.
4. **First pass through `scMelee`:** both seed the three RNGs from `MatchSeeds(session seed, match index)` and the object serial counter from 0x10000, before anything RNG-dependent loads.
5. **Barrier at the first simulation frame:**
   - Both compare a hash of the match setup (rules block; per player the character, state, stocks and costume).
   - The joiner copies the host's sync block (`g_GameFrame`, the gfApplication counter, the RNGs, the serial counter).
   - The joiner relinks its gfTaskScheduler lists into the host's order: task order follows menu history.
6. The countdown runs without rollback and with neutral input.
7. **Start barrier at `start_frame` (240, after GO):**
   - Both create the GekkoNet game session (each with its own input delay, see below; prediction window 7) and complete its handshake.
   - The host sends "go" and starts RTT/2 later; the joiner starts on "go".
8. The match runs under rollback with gp-v9 region snapshots (gp-v8 until `fe35002ead`). Each instance plays only its local port.
9. The session ends `MAX_ROLLBACK_FRAMES + 12` frames after game set, on the same frame on both. The connection stays up for the next match.

**Fix needed on the way** (`a3b61add52`): GekkoNet sends its sync requests from `gekko_update_session`, not from its network poll. The start barrier only polled, so the handshake never happened (both timed out after 60 s). The barrier now updates without local input until the session has started. It keeps that update's events (frame 0's save and advance) for the first `RunFrame`.

### Results

Fox (A, port 1) against Falco (B, port 2) on Battlefield. Each side plays random macros closed-loop on its own possibly-speculative state, until game set by stocks. Compared afterwards:
- **confirmed checksums:** `gprb_checksums`, every frame both have confirmed, minus a 16-frame margin;
- **per-frame traces:** `frame_trace` (RNGs, fighters, game set).

Raw: `run/qa/gprb5/s2/matrix.json` (commit `a3b61add52`).

| Preset | CPU | Frames | Rollbacks (A, B) | Deepest | Confirmed checksums | Traces |
|---|---|---|---|---|---|---|
| lan (1 ms) | single | 3,250 | 0, 1 | 2 | 3,235 compared, **0 mismatches** | identical through game set (3,475 frames) |
| lan | dual | 4,901 | 0, 0 | 0 | 4,886, **0** | identical (5,126) |
| typical (40 ms, ±8, 0.5 % loss) | single | 3,419 | 49, 41 | 7 | 3,404, **0** | identical (3,644) |
| typical | dual | 4,799 | 205, 229 | 7 | 4,784, **0** | identical (5,024) |
| bad_wifi (60 ms, ±25, 2 % burst loss) | single | 3,908 | 413, 421 | 5 | 3,893, **0** | identical (4,133) |
| bad_wifi | dual | 4,857 | 485, 485 | 5 | 4,842, **0** | identical (5,082) |

Every run ended on both sides with `game set` on the same session frame. Both instances then showed the results screen with the same RNG state.

**Save and load cost in sessions:** save 0.6-0.8 ms on average, load 0.8-1.4 ms.

**Again on the final build** (`fe35002ead`: gp-v9, the online merge, 3-minute limit; raw `run/qa/gprb5/final`). All six identical through game set:

| Preset | Single core | Dual core |
|---|---|---|
| lan | 4,130 frames, 0 mismatches | 2,409, 0 |
| typical | 5,108, 0 (48, 46 rollbacks) | 6,422, 0 (372, 205) |
| bad_wifi | 1,760, 0 (165, 183) | 1,961, 0 (188, 201) |

**Rollback counts are low.** The random macros hold inputs for many frames, so most predictions ("same as last frame") are right. The deepest rollback reaches the prediction window (7) under `typical`. The sync test covers rollback depth separately, every frame.

### Consecutive matches on one connection

`gprb_session.py --matches N`: after game set, both confirm the results screen (`results_to_css`). On the CSS they drop the old tokens and pick the next match: Mario/Marth on Final Destination, then Falco/Fox on Smashville. `--minutes 2` writes a 2-minute limit on both CSSs so that matches end sooner. Raw: `run/qa/gprb5/s3`, `s4`, `s6`, `s9`.

**Single core: three matches in a row, all identical** (`ee7a1273cb`, gp-v9, `run/qa/gprb5/s9`):

| Match | Frames | Rollbacks | Confirmed checksums | Traces |
|---|---|---|---|---|
| Fox/Falco, Battlefield | 5,612 | 78, 84 | 5,597, **0 mismatches** | identical through game set |
| Mario/Marth, Final Destination | 7,193 | 70, 68 | 7,178, **0** | identical through game set |
| Falco/Fox, Smashville | 5,943 | 68, 68 | 5,928, **0** | identical through game set |

Earlier single-core sessions, all 0 mismatches:
- Mario/Marth as the second match: 28,793 frames (the 8:00 time-out), 9,790 frames, and 20,421 frames.

**Two fixes this needed:**
- **The next match's RNG seed was written too early** (`f8156f2735`). After the session ended at game set the scene was still `scMelee`, so the "first pass through the match scene" seeded the RNGs again inside the old match's last frames. The next match is now armed only after the scene has been left.
- **Smashville's stage variant differs between machines** (`ee7a1273cb`). The third match never started: the barrier's setup check stopped it. The logged setup keys differed in one byte of the `gmGlobalModeMelee` init block (stage 0x21, variant 06 against 03). The stage select fills that byte from the console clock (Smashville's lighting), and the two instances run with different RTCs, as two players' consoles would.
  - The fix: the joiner now takes the host's init block at its first pass through `scMelee`, before the match loads, and waits for it if needed.
  - The joiner logged `took the host's match init block (…0300… -> …0600…)`, and the Smashville match then ran identically.

### Dual core: rollback sessions diverge on some content (resolved in Phase 6)

**Resolved:** the cause was not dual core but the ground-collision list heads outside the set (gp-v11, see [Phase 6](#the-dual-core-divergence-ground-collision-list-heads-gp-v11)). The investigation as it stood:

Dual-core sessions of Fox/Falco on Battlefield are identical (the table above, and 4 more). **Mario/Marth on Final Destination diverged in 6 of 7 dual-core sessions** under `typical`, also as the first match of a session. So it is the content, not the match index. The divergence came anywhere from game frame 797 to 7,245.

**Controls:**

| Run | Result |
|---|---|
| The same matchup, dual core, `lan` with delay 6 (0 rollbacks) | 10,778 frames, identical |
| Dual-core sync test, every frame rolled back with identical input (`fd-mario-marth`, `distance` 2) | 21,286 frames, 0 mismatches |
| Single-core sessions of the same matchup (5 runs, 70-312 rollbacks) | all identical |

So it takes **dual core plus rollbacks with corrected input**. The sync test cannot see this class: it resimulates with the same input.

**What diverges.** `PPR_GPRB_RNG_LOG` patches `mtRand::generate` and logs every call with 8 stack levels. Comparing the newest pass of every frame on both peers (`harness/tools/gprb_rngcmp.py`), the first differing frame always has the same shape:
- One peer makes 6 extra `randf` calls: position and direction jitter in `fn_807A3338` (sora_melee).
- The chain is `interpretCmd__16acCmdInterpreter` → `0x8077B2C0` (the generic command dispatch) → `0x807A6294` → `0x807A3324` → `fn_807A3338`, i.e. a fighter **effect command** spawning a graphic effect with random offsets from the gameplay RNG.
- The fighters' state is still identical at that frame. On the other peer the command did not run at all in that frame (`PPR_GPRB_PROBE=0x807a3338` shows the call on one side only).
- The fighters then diverge 20-30 frames later.

**First suspect, found on the way: the camera** (gp-v9, `ee7a1273cb`).
- `gfCameraManager` (0x805B6D20+0x740) lives in System FW, outside the set, so a resimulated frame started from the newer camera.
- With it in the set, a resimulated frame's region set differs from the first run only in tick-derived values and dead stack. Before, it differed in about 150 32-byte spans (all the view matrices); after, in 6.
- But the dual-core divergence remained (gp-v9: game frame 1,884; then 1,478; one clean run of 10,778 frames).

**What decides whether the effect command runs is still unknown.** Leads:
- **State left behind by a mispredicted run.** A non-set address the command interpreter or effect system reads. `harness/tools/gprb_changed_mem.py` lists memory outside the set that changes per frame. Candidates: the Tmp heap (gameplay code writes 0x8105C32D per frame), P+ code-embedded variables in REL `.text` (0x8076C988/0x8076C9A4, 0x8069B868 toggled by Gecko codes every frame, as 0x807824D4 already is), and .sbss beyond the set's granules (0x805A0800-0x805A0D30).
- **Gecko codes run at VBI, in the middle of resimulated passes.** Their writes into code and data then land at a different point of the frame than in the first run. In dual core that point moves with the GPU timing.
- **The next step:** a "misprediction sync test". It runs each frame first with perturbed input, then rolls back and runs the true input, and compares against a reference without rollback. That would reproduce this in one instance.

### Online hand-off (`Online::Session` → `Gprb::Session`)

`rollback-fixes` (`d36794a6e1`, the online client) is merged into `gameplay-rollback` (`ecbd88b92e`). `Gprb::GameplayOnlineBackend` (`Core/Rollback/GameplayOnlineBackend.cpp`) implements `Online::SessionBackend` as `docs/backend-design.md` 5.5 describes:
- `link.Release()`, then `Gprb::Session::Connect` on the punched port. The host is the decider; the peer is the address the P2P connection came up with (`connected[0]`). Then `SetSelections(selections)`.
- The harness picks it per search: `mm_search_direct … backend="gameplay"`, with optional `delay`, `region_set` and `dedupe_resim_sounds` (on by default).
- **Input delay:** each player sets the delay of their own inputs, as Slippi's delay frames (all modes, ranked too): the launcher's Input Delay setting, Dolphin.ini `[Online] InputDelay` (0 automatic, the default; 1-9 frames; the launcher offers Auto, 2, 3, 4). Automatic picks it when the game's GekkoNet session starts, from the control channel's round trip (pings every 200 ms, smoothed): 2 frames below 70 ms, 3 below 150 ms, else 4 (`Gprb::Session::AutoInputDelay`). It stays for the game. The harness `delay` option overrides the setting. `gprb_status` reports it as `input_delay`; dolphin.log has `gprb: input delay N (auto|set, round trip M ms)`. A modified client can always run its own inputs at less delay; nothing checks the other player's.

The three requirements:
1. **The host also sends first:** with matchmaking the host knows the guest's address and sends control packets to it from the start. The guest's first packet confirms or corrects the address.
2. **Timeout `Online::PeerSilenceTimeoutMs(delay)`** (7.2 s at delay 2), for GekkoNet in a match and for the session's own silence check on the CSS and in the countdown.
3. **Leaving notifies the peer at once:** `Stop()` sends a `leave` control message three times. The peer ends its session; in a match this happens on the CPU thread at the next loop top. `gprb_status` then reports `disconnected: true`, the flag the game will read (5.6).

`Stop()` now also ends the countdown and start-barrier phases; before, it left region mode on.

**Verified end to end** with our accounts/mm servers and a portable Postgres (`harness/tools/gprb_online.py`, raw `run/qa/gprb5/online`). Both instances boot P+ independently, log in from `user.json`, reach the CSS, and run a Direct search for each other with `backend="gameplay"`:

| Case | Result |
|---|---|
| Matchmaking to session connected | 0.6 s |
| Fox/Falco match under gameplay rollback | 3,531 confirmed frames, **0 mismatches**; traces identical through game set |
| Guest leaves on the CSS (`mm_cancel`) | host `ended`, `peer left`, `disconnected`: seen within one 20 ms poll |
| Guest leaves during the next match | host's match ended within **16 ms** (`end_reason: peer left`) |
| Guest frozen for 6 s in a match | stays in (`running`, not disconnected), the match goes on |
| Guest frozen until dropped | host dropped it after **7.22 s** (`peer timed out`) |

**Fix needed on the way** (`fe35002ead`): GekkoNet reports a dropped peer once. When that update ran inside the session's host wait, the event was lost and the host waited forever. It now sets `peer_left`, which ends the session at the next loop top.

**Not done:**
- No `Online::SessionBackend` is registered by default outside the harness: DolphinNoGUI registers the netplay backend, and `backend="gameplay"` switches.
- The game-side reaction (the DISCONNECTED text, the end without "GAME!", back to the CSS) is the in-game UI of 5.6. After a drop, the host's match goes on locally until that exists.
- The frozen guest only notices on its own GekkoNet timeout after it resumes.

## Phase 5: sound and the match end

### Match end and the way back to the CSS

- **The session ends at game set** (`fb111dc1ba`): `MAX_ROLLBACK_FRAMES + 12` frames after the first frame that shows game set, past the deepest rollback. A rollback that undoes the game set resets it.
  - Both peers end on the same session frame: in every session, `game_set_frame` and the end frame were equal on both sides.
  - The post-game-set divergence of Phase 1 (`g_mtRand` 6-20 frames after game set) no longer matters: the result is fixed at game set, and from then on each instance runs alone.
  - Both instances reached `scVsResult`; in the `lan` run, even with equal RNG state.
- **Results to the CSS and the next match:** `results_to_css` on both, then the next match on the same connection. Three consecutive matches are identical in single core (Phase 4). The fixes needed were the next-match guard and the init block.
- The sync test also ends at game set now: every sync test that reached game set ended there (`end_reason: game set`).

### Sound

Brawl's sound system is not rolled back: the Sound heap and the AX state are outside the set on purpose. What a resimulated pass does with a sound start is selectable. `Gprb::Session::OnSoundAlloc` runs at the three allocation call sites of `detail_SetupSound`, where `r8` is the sound id.

| Mode | A resimulated pass | Cost |
|---|---|---|
| `play` (the session default; the online backend's default is `dedupe`) | starts the sound again | every rollback plays the rolled-back frames' sounds twice |
| `suppress_resim_sounds` | gets "no channel" for every sound (Brawlback's approach) | sounds that only the corrected input starts are lost |
| `dedupe_resim_sounds` (Slippi-style bookkeeping) | does not start a sound (by id) that an earlier run of the same frame already started; does start sounds that only the corrected input causes | a sound started by a mispredicted run and not by the corrected one plays on |

**Gameplay is unaffected in all three modes.** Sync tests at `distance` 7 (6 frames resimulated every frame):

| Mode | Scenario | Frames | Mismatches | Sound allocations in resimulated passes |
|---|---|---|---|---|
| play | Fox/Falco | 5,355 | 0 | not suppressed |
| play | Ice Climbers/Charizard, items | 11,019 | 0 | not suppressed |
| suppress | Fox/Falco | 3,023 | 0 | 2,418 of 2,418 suppressed |
| suppress | Ice Climbers/Charizard, items | 12,914 | 0 | all suppressed |
| dedupe | Fox/Falco | 6,172 | 0 | 5,232 of 5,232 deduplicated |
| dedupe | Mario/Marth, items | 25,954 | 0 | 22,057 of 22,061 deduplicated |

In a sync test the input never changes, so dedupe suppresses nearly everything.

Stopping mispredicted sounds and re-attaching the game's handles: done in Phase 6 ([Sound: stop and re-attach](#sound-stop-and-re-attach-slippi-style-bookkeeping)).

## Phase 6: the FIFO tail, the slow manager, the ground-collision lists, sound bookkeeping

Commits `e8c3bc2000` (gp-v10, FIFO tail), `980673530a` (replays, diagnostics, sound bookkeeping), `fade07580e` (merge of `rollback-fixes`), `e7b2076039` (gp-v11 as the default, `no_rollback`), `d31f69fc54` (sound bookkeeping by default in sessions), `b79fe057cf` (gp-v12 as the default). Builds used for the runs: `run/bin/gprbw-p` (`980673530a` plus the full dumps), `gprbw-s` (`e7b2076039`), `gprbw-t` (`d31f69fc54`), `gprbw-v`/`gprbw-w` (`b79fe057cf`). Raw results: `run/qa/gprbw/`.

### New tools

| Tool / command | What it does |
|---|---|
| `PPR_GPRB_PASS_LOG` / `gprb_session.py --pass-log DIR --save-countdown DIR` | Every GekkoNet update of a network session (load depth, initial save, every pass's frame, save flag and input slots), per peer and run, plus each peer's countdown savestate. |
| `gprb_mispredict.py run --state <peer's countdown save> --modes replay=<pass log>` | Runs that peer's whole session again in one instance, exactly: same loads, same passes, same inputs. Also in dual core: the replays' traces equal the real peers' traces frame for frame, and two replays of one log have byte-identical MEM1 and MEM2 at every dumped frame. |
| `gprb_passlog.py` | `show` the updates around a frame; make variants of a log: `fix` (mispredicted passes get the final input, rollbacks stay), `norb` (an update neither loads nor resimulates), `flat` (no rollbacks at all: the session's ground truth). |
| `PPR_GPRB_DUMP_FRAMES` + `PPR_GPRB_DUMP_DIR` (+ `PPR_GPRB_DUMP_FULL=1`) | The region set (and with `FULL` all of MEM1 and MEM2) at every save of the chosen session frames. |
| `gprb_memdiff.py` | `diff`: granules that differ between two runs' dumps, outside (or inside) the set; `dol`: the DOL data objects (symbols.txt) behind them; `scan`: DOL words outside the set that point into the set's heaps and change during a match (global list heads whose nodes are rolled back). |
| `gprb_mispredict.py` `ref` / `mp` modes, `gprb_synctest` `inject_input`, `mispredict_ports/offset/every` | Sync test with recorded input; misprediction sync test (the first run of every frame gets another frame's input). |
| `gprb_samples` (`gprb_session.py --sample-every`) | Chunk hashes of the region set every N confirmed frames, compared between peers. |
| `PPR_GPRB_CENSUS=1` + `gprb_census` | Every granule outside the set written during the match. |
| `gpu_state` | CP FIFO, PI FIFO, PE control, interrupt cause/mask, deterministic-GPU flag. |
| `gprb_sound_state` | The sound archive player's allocated sounds: id, general handle, owned or orphaned, duplicate ids. |
| `gprb_hang.py` | `cdb` stacks of every thread of a hung instance (the tools call it on harness time-outs). |

### The GX FIFO ring tail: the draw-done hangs and the Peach article crash

**Root cause.** Region-mode loads restore whole 64-byte granules, and a range that starts or ends inside a granule takes the rest of it along. The System heap starts at 0x80611F60. The granule 0x80611F40-0x80611F80 also holds the last 32 bytes of the GX FIFO ring: `gpu_state` in a match shows CP FIFO base 0x805D1E60 and end 0x80611F40 (the RenderFifo heap, outside the set). Every time the CPU wrote that 32-byte block (the ring wraps every few frames), the next save captured it with the System heap's first bytes, and a load wrote the stale commands back. When the command processor had not yet read the block, the GPU ran stale or half-overwritten commands:
- a lost draw-done (PE finish) command: the main thread waits forever in `GXWaitDrawDone` (`fn_801F0A30`, called from `fn_80023AE4`, the frame's render start, which waits for the previous frame's `GXSetDrawDone`). That is the hang of open issue 3.
- extra or missing draw-done interrupts. The draw-done callback (0x80023DD4 → `moMeleeDrawDownCallback`) drives `soDisposeInstanceManager::notifyDrawDone`, which frees the instances (fighters' articles among them) disposed in earlier frames. Freed at the wrong time, a Peach article was gone while her fighter state still referred to it: the burst of `存在しないArticleへのchangeMotion命令です` and the crash of open issue 2. (This chain is inferred from the code and from the fix; the crash itself was not traced instruction by instruction.)
- garbage commands on the GPU thread: indexed-XF loads from addresses outside RAM (the Phase 3 `LoadIndexedXF` crash, seen twice more this round in dual-core sessions).

**Evidence.**
- Loads now count how often excluded bytes differed from the snapshot, i.e. how often the old code would have written different bytes: in a Peach sync test 2,288 of 13,453 loads for 0x80611F40 (17 %), and 732 of 1,818 in a dual-core misprediction test. No other granule shared with non-gameplay memory changed during a match.
- Before: Peach crashed in 4 of 5 sync tests (2,600-7,500 frames); 2-3 of about 20 long sync tests hung in `GXWaitDrawDone`.
- After (gp-v10/gp-v11, sync tests to game set, single core, `distance` 2): Peach/Game & Watch on PS2 21,191, 21,347 and 25,939 frames, 0 mismatches, no crash; Peach/Game & Watch on Smashville 15,674. The other article users: Link/Snake (bombs, grenades) 28,729 and 28,299 (gp-v11), Toon Link/Diddy (bombs, bananas) 20,493 and 26,034 (gp-v11), R.O.B./Olimar (gyro, Pikmin) 28,793. Every one reached game set. No hang in the 20 long sync tests and the 16 dual-core sessions run since (below).

**Fix (`e8c3bc2000`).** Region sets have an `exclude` list: bytes a load never writes, even inside a granule the set touches; those granules are restored byte by byte, all others whole. gp-v10 excludes 0x80611F40+0x20. A first version restored every partly covered granule byte by byte; that dropped DOL gameplay globals sharing granules with the `dolw-g1` ranges (Peach: 12 desyncs), so only the FIFO tail is excluded.

### The global slow-motion manager (gp-v10)

The first gp-v10 sync test of Ice Climbers/Charizard with items desynced. A slow-motion request made by a run that was rolled back stayed active: `gfSlowManager` lives in DOL .bss/.sbss/.sdata, outside the set. gp-v10 adds the requests (0x804953B0+0x20), `s_needsUpdate` (.sbss 0x805A0080) and `s_maxSlowRate` (.sdata 0x8059C690). With all three: 14,805 frames, 0 mismatches.

### The dual-core divergence: ground-collision list heads (gp-v11)

**It is not dual-core specific.** A single-core Mario/Marth FD session diverged too (980/927 rollbacks, game frame ~19,100), with the same first difference (one peer's effect command draws 6 `randf`). Dual core only has more rollbacks. Not the input (both peers' confirmed pads are identical), not the sound mode (play, suppress and dedupe all diverge), not GPU timing, not harness polling. Misprediction sync tests on one instance seemed clean, but only because their reference was itself a sync test with the same fault (see the ground truth below).

**Reproduction.** The pass log and countdown savestate of each peer of a diverged dual-core session (Mario/Marth FD, `typical`, run `s7` 1: the traces diverge at game frame 6,661 = session frame 6,421) replay that peer's session exactly: replay host = real host, replay join = real join, and the two replays diverge at the same frame. Variants of the logs (`gprb_passlog.py`) then show which rollback matters:

| Replay | vs the other peer | vs ground truth (`flat`) |
|---|---|---|
| join as recorded (misprediction at 6,399, 1-frame rollback) | diverges at 6,661 | identical |
| join without that misprediction / without that rollback | diverges at 6,661 | identical |
| host as recorded (mispredictions at 6,413-6,414, 2-frame rollback at 6,415) | diverges at 6,661 | diverges at 6,661 |
| host, rollback kept but **input corrected** (`fix`) | diverges | diverges |
| host **without that rollback** (`norb`) | identical | identical |

So the host's rollback at 6,415 changed the outcome even when it resimulated with the input the first run already had. Whole-memory dumps of the `fix` and `norb` replays (`gprb_memdiff.py`) are identical up to 6,412. At 6,415 the set differs only in Mario's instance heap, in the links of two list nodes; outside the set in the list head `lbl_8049E594` (.bss, `{count, sentinel next, sentinel prev}`), count 4 against 3.

**Mechanism.** `lbl_8049E594` and its neighbours (`lbl_8049E570`, `g_grCollisionList` 0x8049E57C, `lbl_8049E588`) are intrusive lists of ground-collision objects (`fn_80133C1C` links, `fn_80133C78`/`fn_80133B98` unlink; `fn_80112AD4`/`fn_80112B7C` iterate and update each object). The objects live in the fighters' instance heaps (in the set); the list heads did not. When a frame that creates (or destroys) such an object is rolled back, the load restores the objects and their neighbours' links but leaves the head's count and sentinel links as the discarded run left them. The resimulation creates the object again at the same address and links it after `sentinel.prev`, which is the object itself: it ends up linked to itself, and iteration from the sentinel skips it. That peer then stops updating that collision object, and gameplay differs some frames later (here the first visible difference was again an effect command's `randf`). The sync test's own check cannot see it: both runs of every frame start from the same corrupted list.

**Fix (gp-v11, `e7b2076039`).** gp-v10 plus 0x8049E570+0x30 (the four ground-collision list heads). `gprb_memdiff.py scan` over a whole match (7 dumps from game frame 100 to 8,900) finds one more DOL object outside the set that points into the set's heaps and changes: the last 0x14-byte record of the array at 0x8049EDE0 (`lbl_8049EDD8`, entries pointing into the Effect heap), half covered by `dolw-g1`; gp-v11 adds 0x8049EE40+0x20. (The other hits are thread stacks and contexts, AX/DSP buffers, GX display lists and `gfSceneRoot`/`gfKeepFrameBuffer`.) gp-v11 is the default region set of sync tests, sessions and the online backend.

**The sync test against the ground truth.** The new sync-test option `no_rollback` runs the same session machinery but simulates every frame exactly once (`gprb_mispredict.py --no-rollback`): the game as it would run without rollback. Every rolled-back run with the same recorded input must end with the same per-frame trace.

| Recorded input | Run | gp-v10 | gp-v11 |
|---|---|---|---|
| Mario/Marth FD, dual core, `distance` 4 | sync test (`ref`) | **differs from frame 312** (RNG; fighters from 368) | identical, 7,926 frames |
| same | misprediction test, `play` / `suppress` / `dedupe` sounds | (the old reference had the same fault) | identical, 7,932-7,934 frames, all three |
| Ice Climbers/Olimar PS2, single core | sync test | differs from frame 2,687 (`distance` 2) | identical, 21,766 frames, to game set |

So with gp-v10 even a plain sync test drifted from the real game within seconds, and its own first-run-against-resimulation check stayed silent. The other variants pin it to the list heads: restoring only `lbl_8049E594` (12 bytes) is enough to match the ground truth; restoring only the effect record is not.

**Evidence in sessions.**
- Replays of the three diverged `s7` sessions with gp-v11: host and join replays identical to the end (they had diverged at game frames 6,661, 17,877 and 5,690). The host replay of run 1 equals the no-rollback ground truth.
- Mario/Marth on Final Destination, **dual core**, `typical` (40 ms, ±8 ms, 0.5 % loss), gp-v11, sound bookkeeping on (`s8`, build `gprbw-p`; `s10`, build `gprbw-t`): **10 complete matches, all to game set with 0 confirmed-checksum mismatches and identical per-frame traces**: 24,870, 23,283, 19,473, 21,602, 27,664 (`s8`) and 21,082, 23,513, 14,661, 23,252, 26,875 frames (`s10`), 647-1,039 rollbacks per peer, deepest 7. Before the fix the same matchup diverged in 6 of 7 (Phase 4), and in 3 of 4 with gp-v10 (`s7`). Five more `s8` sessions ended early, not by a desync: four with `peer timed out` (237-13,457 frames, 0 mismatches up to there) while the machine ran 10+ Dolphin instances, one failed to start (harness time-out); three `s10` runs failed to start for the same reason (harness port collision or time-out under load).
- Fox/Falco on Battlefield, dual core, `typical`, gp-v11 (`s9`): 3,835 and 7,581 frames, both to game set, 0 mismatches, traces identical.
- Sync tests with gp-v11, single core, `distance` 2, all to game set with 0 mismatches: Ice Climbers/Olimar PS2 28,793; Ice Climbers/Charizard with items 11,408; Mario/Marth FD 10,356; Mario/Marth FD with items 19,205; Link/Snake 28,299; Toon Link/Diddy 26,034; Peach/Game & Watch PS2 22,581 and Smashville 10,165; Fox/Falco 3,981; Squirtle/Zelda 28,793; R.O.B./Olimar 28,793, 28,122, 22,248 and 28,793; Ice Climbers/Olimar 28,793 and 25,027; Ice Climbers/Charizard with items 16,615 and 18,926. Two of these runs had one 11-frame burst each (R.O.B./Olimar at game frame 8,445, Ice Climbers/Olimar at 25,178): the camera quake, fixed in gp-v12 (below).

**Not restored, on purpose:** the task-id counter `gUnk8059c66c` (0x8059C66C, `gfTask::updateId`). A resimulated frame gives new objects ids one higher than the other peer's (seen in the dumps: 0x8000000F against 0x8000000E). Ids are compared for equality only as far as seen, and restoring the counter would hand out ids still held by tasks that survive a rollback outside the set. No divergence traced to it.

### 11-frame desync bursts in sync tests: the camera quake (gp-v12)

Rare sync-test bursts of exactly 11 consecutive frames whose fighters' checksum differed between first run and resimulation, then agreed again: gp-v10 Ice Climbers/Olimar at frames 11,748 and 11,807, Ice Climbers/Charizard with items at 11,284; gp-v11 R.O.B./Olimar at game frame 8,445 and Ice Climbers/Olimar at 25,178 (about one per 100,000 frames).

**Reproduction.** Sync tests now write pass logs as well (`gprb_synctest.py --pass-log DIR` saves a countdown state and `<name>-<n>.synctest.m0`). The replay of the Ice Climbers/Olimar run reproduced the burst at the same frames with the same checksums. `PPR_GPRB_INTERP_FROM=25172` ran the replay with the JIT up to 6 frames before the burst and then under the interpreter, and `cpu_trace` recorded game frame 25,178's first run and its resimulation (`gprb_mispredict.py --trace-frame 25178`): 1.9 M instructions each. `gprb_resimtrace.diff` shows the first value difference that is not a tick: in `cmReqQuake__FiP5Vec3f` → `fn_8009D564`, a load of `+0x270` of the camera quake controller (0x805B69E0, pointer at .sbss 0x805A0280) returns 0 in the first run and 0x10 in the resimulation; control flow splits right after.

**Cause.** The quake controller is a System FW block (0x805B69C0, 0x2A0 bytes with its header) next to `gfCameraManager`, outside the set. A quake request sets the flag at +0x270 and the amplitude at +0x268. The first run of the frame set them; the load did not clear them; the resimulation took the "quake already running" path with another amplitude, and the fighters' checksum (instance, damage, stocks, posture x/y, status kind) differed for the quake's 11 frames, then agreed again. Which field moved was not isolated; the in-set difference at the first burst frame is in the fighters' model matrices (translations about 3.5 units apart).

**Fix (gp-v12, `b79fe057cf`, the default):** gp-v11 plus 0x805B69E0+0x280. The same replay with gp-v12: 0 differences (gp-v11: 11). Ground truth with gp-v12: Mario/Marth FD sync test and dedupe misprediction test identical for 7,919 and 7,932 frames, Ice Climbers/Olimar sync test identical for 21,766 frames. On the final build (`gprbw-w`, gp-v12 and sound bookkeeping by default): sync tests R.O.B./Olimar 28,439, Peach/Game & Watch PS2 17,030 and Mario/Marth FD with items 19,595 frames, 0 mismatches (three more runs failed to start under load); dual-core Mario/Marth FD sessions (`s11`) 24,152, 28,793, 26,048, 22,492 and 11,650 frames to game set, 0 mismatches, traces identical, and one ended by `peer timed out` at 3,491 frames (0 mismatches).

### Sound: stop and re-attach (Slippi-style bookkeeping)

`dedupe_resim_sounds` (`980673530a`) now does what Slippi's sound handling does:
- every sound a frame starts is recorded with the game's handle (`GprbSoundAttachHook`, a Start hook at `detail_SetupSound`'s attach path 0x801C9C70, plus the allocation hooks);
- a resimulated pass that starts a sound id an earlier run of the same frame started, while that sound still plays, does not start it again: `detail_SetupSound` continues at its attach path with the old sound, so **the game's handle is re-attached** to the playing sound. If another handle took the sound over in the meantime (`sndSystem::playSE` keeps its own 16-slot handle table, outside the set, and a resimulation picks a different slot), that handle gives it up first;
- sounds an earlier run started that the corrected run does not start are **stopped** at the top of the next loop: a guest call of the sound's `Stop(0)` (vtable +0x18), with the CPU registers saved and restored around it.

**How it is verified (every instance is muted).** `gprb_sound_state` walks the sound archive player's three instance managers (wave, sequence, stream: SAP+0x38/0x60/0x88) and lists every allocated sound: id, general handle (+0x8), whether a handle owns it, and sound ids allocated more than once. `gprb_mispredict.py --sound-sample` samples it twice a second during a dual-core misprediction test (Mario/Marth FD, `distance` 4, both ports mispredicted, the same recorded input in every mode), and the session status counts re-attaches, moves, stops and sounds already gone.

Dual core, `distance` 4, Mario/Marth FD, both ports mispredicted on every frame's first run (input of 3 frames earlier), 7,930 frames, about 270 samples each (raw `run/qa/gprbw/f-fmp*.json`):

| Mode | Sounds playing (avg / max) | Sound ids allocated twice at once (avg / max) | Orphans | Resimulated allocations | Gameplay vs ground truth |
|---|---|---|---|---|---|
| `play` (resimulations start sounds again) | 3.99 / 18 | **1.79 / 12** | 0 | 3,072 started again | identical |
| `suppress` (Brawlback: resimulations get no channel) | 4.42 / 11 | 2.41 / 7 (the mispredicted runs' sounds are never stopped) | 0 | 3,105 suppressed | identical |
| `dedupe` (stop + re-attach) | 1.55 / 6 | **0.03 / 1** | 0 | 2,926 matched an earlier run: 2,656 re-attached, 270 already finished (not restarted); 146 new; 15 stopped (29 already finished) | identical |

"Allocated twice at once" is a sound playing twice; it is what a player hears as a doubled sound. "Orphans" are sounds no handle owns: none in any mode (sounds the game starts through `sndSystem::playSE` are owned by its own handle table). The 2,656 re-attaches also show that the game's handles end up on the playing sounds, so the game can still stop or change them (a charge loop, a held move). This test is far harsher than a session: every frame is rolled back and resimulated with changed input.

`dedupe` is now the default of network sessions (`d31f69fc54`) and of the online backend; `suppress_resim_sounds` still selects Brawlback's behaviour, `dedupe_resim_sounds=false` the old one. The dual-core sessions `s8`/`s10` above ran with it.

**What this cannot show:** audible artefacts (a stop cuts a sound without a fade; a re-attached sound keeps the pitch or volume the discarded run gave it). Those need ears on an unmuted instance.

## Phase 7: the real online path (the default backend, from the online CSS)

**Merged** into `rollback-fixes` (`dolphin/`, merge `df44299556`, 2026-10-07; the worktree `dolphin-gprb` is unchanged). On `rollback-fixes`:
- `aedcf62a51`: the gameplay session is **the default online backend** (`[Online] SessionBackend = gameplay`; `netplay`, the synchronized reboot, stays selectable). DolphinQt's main window and DolphinNoGUI's `main` register it at start-up (`Gprb::RegisterOnlineBackend()` = `RegisterFactory("gameplay", Gprb::MakeOnlineBackend)`), before `SelectConfigured()`. The lobby and PPOM version 2 (SESSION, LOCAL), below.
- `da2cf9b595`: Brawl Online branding in DolphinQt.

- `b882222f2b`: the render window's title starts with Brawl Online.

Game side: `game-code` `pponline` `441eb31`, `3c0fdb6` (`docs/game-code.md` §11). Harness: `harness/tools/online_set.py` (the set, shared by the test and the launcher run).

### How a real session runs now

No reboot and no Dolphin window change: each player's game stays on its own online CSS.

1. **Matchmaking** (Direct, from the in-game keypad) hands the connected peer to `Gprb::Session` on the punched port (Phase 4, "Online hand-off"). The game's search already locked the player in for game 1; the lock-in reaches the session through LOCAL.
2. **The lobby** (`Gprb::Session`, Slippi's `MATCH_SELECTIONS`): both peers send their lock-in (gmCharacterKind, costume, stage pick, the game number it is for, ready) in their control messages. When every player is locked in for the next game, the **host decides the setup**: P1 = the host, P2 = the joiner; the stage is the losing player's pick (Direct), else any pick, else random from the match's `stages` list (the server's list for the mode, P+ v3.2's legal list in `server/config/rulesets.json`; `Gprb::Session::DefaultStages()` if the server sends none). Since branch `unranked` (2026-10-07, merged as `4944245954`) the random stage comes from Slippi's stage pool (no stage again until the list is used up). A pick is played as it is, even outside the list: Slippi does not restrict Direct's stage select. The joiner takes the host's setup. The winner of each game is read from the state both peers end on (more stocks, then less damage), so it is the same on both.
3. **SESSION** (written by GameBridge only outside matches, the same on both machines) gets the setup; each game leaves its CSS for the match, builds it with the Versus sequence's own setup from SESSION, and loads (`docs/game-code.md` §11).
4. **The match** starts exactly as in Phases 4-6: at the first pass through `scMelee` both seed the RNGs from the session seed and the match index and the joiner takes the host's init block; at the first simulation frame the barrier compares the setup hash and copies the host's frame counters, RNGs, serial counter and task order; the countdown runs without rollback; GekkoNet starts at frame 240 behind the start barrier; region mode gp-v12.
5. **Game set:** both end on the same frame, the game goes straight back to the online CSS (no results screen), the connection stays, `match_index` + 1. START locks in for the next game; Direct's loser picks the stage on P+'s stage select first.
6. **Disconnects** (design 5.6): a peer that leaves (hold Z) or goes silent (7.2 s) ends the session; LOCAL `disconnected` is set; in a match the game plays the error sound, draws "DISCONNECTED" in red at the top of its HUD and, 90 frames later, ends the match as the pause screen's quit does (no "GAME!"), then goes back to the CSS; GameBridge's next `GET_MATCH_STATE` cleans up and reads IDLE. Dolphin's red OSD "DISCONNECTED" only stands in if the game did not show the text within 30 frames.

What the plugin must never do in a match, and does not: read the mailbox, or let anything that differs between the machines change game state. Its `.data`/`.bss` is in the region set; the mailbox and LOCAL are excluded (one range); SESSION is constant during a match.

### Results

All on this PC (shared with another agent's three Dolphin instances), both peers on localhost, dual core, D3D11, muted, P+'s rules shortened by the tests to 2 stocks / 2 minutes (written into the set rule on both CSSs; part of the setup both build).

Two-game Direct sets from the in-game menus (`online_set.play_set`). Rollbacks per peer are low: both peers are on localhost and the random macros predict well.

| Run | Game | Stage | Frames (both) | Rollbacks | Confirmed checksums | Traces |
|---|---|---|---|---|---|---|
| `test_direct_set_under_the_gameplay_session`, build `b882222f2b` (DolphinNoGUI) | 1 | 0x0C Frigate Husk (random) | 7,193 | 0, 3 | 7,178 compared, **0 mismatches** | identical |
| | 2 | 0x02 Final Destination (the loser's pick) | 1,361 | 0, 3 | 1,346, **0** | identical |
| the same test, build `da2cf9b595` | 1 | 0x01 Battlefield (random) | 1,427 | 1, 3 | 1,412, **0** | identical |
| | 2 | 0x02 Final Destination (the loser's pick) | 1,507 | 0, 1 | 1,492, **0** | identical |
| `e2e_launcher.py`, two launchers and two Qt `Dolphin.exe`, build `b882222f2b` (`run/artifacts/e2e-launcher/gpon-e2e-final-20261007-125651/`) | 1 | 0x05 Metal Cavern (random) | 1,593 | 0, 3 | 1,578, **0** | identical |
| | 2 | 0x02 Final Destination (Alice lost and picked) | 1,376 | 0, 5 | 1,361, **0** | identical |
| `e2e_launcher.py`, build `da2cf9b595` (`gpon-e2e-20261007-115717/`) | 1 | 0x2D Dream Land (random) | 4,086 | 11, 0 | 4,071, **0** | identical |
| | 2 | 0x02 Final Destination (Bob lost and picked) | 3,448 | 3, 0 | 3,433, **0** | identical |

Every game ended with game set on both peers on the same frame.

No reboot (checked by `harness/tools/online_set.py`): the emulated frame count, the plugin's own frame counter and the PPOM block address only moved forward / stayed put, no netplay session ran, and GameBridge never lost the block. Both games built the same `gmGlobalModeMelee` setup (compared field by field) with the same setup hash, and both were back on the online CSS after each game, still connected, with the lobby at the next game and the same winner.

Development runs of the same flow (a scratch driver, 1-3 games each): 10 more games, 847-2,584 frames, all to game set with 0 confirmed-checksum mismatches.

`test_opponent_leaves_in_the_middle_of_a_game`: the guest's Dolphin is closed in a match; the host sees the drop after 7.3-7.4 s, the game ends and the host is back on its CSS, idle. The same with the host's Dolphin closed (the guest survives): dropped after 7.4 s, back on the CSS.

The launcher path (`harness/tools/e2e_launcher.py`) logs in through both launchers, presses Play, and drives both Qt `Dolphin.exe` from the main menu through the set above: both Dolphins' windows are "Brawl Online" (main window) and "Brawl Online | Project+ Dolphin b882222f2b | JIT64 DC | Direct3D 11 | HLE | ..." (render window), no NetPlay window, muted.

**Regression runs on the final build** (`b882222f2b`, frozen as `run/bin/gpon-final`):
- `harness/tests/test_online_game.py`: 8 passed (the menus, recent codes, CSS lock, Unranked's server error, the set above, the in-match leave, the netplay fallback hand-off);
- `harness/tests/test_online.py`: 7 passed (the netplay hand-off test now selects `backend="netplay"`);
- `harness/tests/test_rollback.py`: 14 passed (whole-machine netplay rollback, sc and dc);
- sync tests (`gprb_synctest.py`, gp-v12, single core, `distance` 2): Fox/Falco Battlefield 3,689 frames, Peach/Game & Watch PS2 26,175 frames, both to game set, 0 desyncs;
- network sessions through netsim (`gprb_session.py`, `typical`): single core 4,506 confirmed frames, dual core 4,780, 7,451 and 2,058, all 0 mismatches with identical traces. Two more dual-core runs failed to start (the harness's first `pad_set` timed out at start-up while the machine also ran another agent's three instances; Phase 6 open issue 3 saw the same), and the pre-merge build started its one try.

### Open issues of Phase 7

1. **Real network conditions** were not exercised through the in-game flow: the matchmaking P2P link is direct on localhost (the netsim tests of Phases 4 and 6 drive `gprb_connect` directly). The session code is the same; a netsim between two in-game peers is still to do.
2. ~~The game-side gaps~~ (`docs/game-code.md` §11): done 2026-10-07, see "Slippi parity round" below.
3. Ranked and code-based Teams are not exercised (Unranked is since 2026-10-07: `harness/tests/test_online_unranked.py`, a two-game set from the in-game Unranked search); SESSION and the lobby are sized for 4 players, the session itself runs 2.

### Slippi parity round (2026-10-07)

Game side `game-code` `pponline` `7ca3a7e` (PPOM v3), Dolphin `rollback-fixes` `c27636d256`; details in `docs/game-code.md` §6 and §11.
- **Each player's own controls.** The name tag a player picks on the online CSS travels with the lock-in as the design's port values (the tag's name, rumble byte and P+'s 0x2D-byte controls layout): LOCAL `own` → `Gprb::Session::LockIn::port_values` → the control messages (`"pv"`) → the host's `MatchSetup` → SESSION `players[i].pv` on both machines. At the match start the game's hook on ipPadConfig's setter (`0x80110550`) gives each port its player's layout from SESSION, the same on both machines. `SetupKey` (compared at the barrier) now includes `g_PadConfig`'s GameCube layouts and player → pad map. Dolphin's own controller mapping is applied before the game reads the pad, so it reaches the peer as that player's input.
- **The CSS remembers** the character, costume and tag after a match and after the stage select.
- **Disconnect in a match:** DISCONNECTED in the game's HUD with the game's font, the LRAS-type end without "GAME!" (`stOperatorInfoMelee` flags `0x70`), Dolphin's OSD only as a fallback (`game_bridge_status.osd_disconnects`).
- **Direct's loser's stage select is not restricted** (Slippi parity); P+'s stage striking is kept behind a debug flag for Ranked.
- **Menus:** WITH FRIENDS → Direct / Teams, WITH ANYONE → Unranked / Ranked (Brawl's Wi-Fi OPTIONS page, labels in the game's font).

Results (`harness/tests/test_online_game.py`, DolphinNoGUI built from this change, both peers on localhost, dual core, D3D11, muted):

| Test | Result |
|---|---|
| `test_direct_set_under_the_gameplay_session` | game 1 Yoshi's Island (0x0D, random): 1,604 frames, rollbacks 0/3, 1,589 confirmed checksums, **0 mismatches**; game 2 Final Destination (the loser's pick): 1,440 frames, rollbacks 0/6, 1,425, **0**; both CSSs showed the character again after game 1 and after the stage select |
| `test_each_player_keeps_their_tag_controls` | A ("NoTap") held up by A's Dolphin mapping stayed on the ground, B (defaults) jumped, on both machines; **0 mismatches** over the confirmed frames |
| `test_opponent_leaves_in_the_middle_of_a_game` | dropped after 7.31 s; HUD text at once; quit flags 90 frames later; `scMelee` left 110 frames after the text; back on the CSS idle, character selected; no OSD message |
| the whole of `test_online_game.py` (9 tests, `rollback-fixes` `c27636d256`, plugin `7ca3a7e`) | **9 passed**; its set: game 1 741 frames (rollbacks 0/11, 726 confirmed, 0 mismatches), game 2 1,983 frames (0/1, 1,968, 0) |
| `test_online.py`, `test_rollback.py` (same build) | **7 passed**, **14 passed** |
| `e2e_launcher.py`, two launchers (rebuilt bundle) and two Qt `Dolphin.exe` (`run/artifacts/e2e-launcher/slp-e2e-20261007-144841/`) | game 1 Metal Cavern (0x05, random): 2,641 frames, rollbacks 0/2, 2,626 confirmed, **0 mismatches**; game 2 Final Destination (Bob lost and picked): 4,076 frames, 0/7, 4,061, **0**; Bob's CSS showed his character again after the stage select; both launchers' rules checkbox read "I accept the Brawl Online Rules" |

**The launcher label.** The earlier e2e runs still saw "PlusOnline Online Rules" because they drove a stale bundle: `launcher/release/app/dist` was built at 06:49-06:58, before the rename commit `6d0c6913` (09:37), and nothing checked it (`launcher_app_dir()` existed but was never called). Rebuilt with `npm run build`; `e2e_launcher.py` now refuses a bundle older than `launcher/`'s last commit or any file under `src/`/`locales/` (`--allow-stale-launcher` overrides) and records and checks the rules label (`summary.json` `rules_labels`).

### The `unranked` merge (2026-10-07)

Dolphin `rollback-fixes` `4944245954` merges branch `unranked` (`6449517bc1`): the server's `stages` list with Slippi's stage pool for every random stage (every Unranked game, Direct's game 1). Kept from `rollback-fixes`: GameBridge and the PPOM v3 SESSION/LOCAL layout (the branch never touched it). Dropped from `unranked`: replacing a Direct loser's pick outside the list with a random stage (Slippi does not restrict Direct). Plugin `pponline` `7ca3a7e`; DolphinNoGUI built from the merge, muted, D3D11, both peers on localhost.

| Test | Result |
|---|---|
| `test_online_unranked.py` (3 tests, two-stage server lists) | **3 passed**. Unranked: game 1 Smashville (2,363 frames, 0 mismatches), game 2 Dream Land (4,282, 0). Direct: game 1 Dream Land from the list (2,883, 0), game 2 Final Destination, the loser's pick from outside the list (2,399, 0) |
| `test_online_game.py` (9 tests) | 8 passed in the full run; `test_each_player_keeps_their_tag_controls` read A's stick at 0 on both machines once (A's Dolphin mapping did not reach the game) and passed when run again |
| `test_online.py`, `test_rollback.py` | **7 passed**; **14 passed** over two runs (the cases that failed did so connecting to their own harness port, see open issue 8) |
| `e2e_launcher.py` (`run/artifacts/e2e-launcher/mrg-e2e-20261007-162710/`) | game 1 Green Hill Zone (0x23, random): 755 frames, 0 mismatches; game 2 Final Destination (Bob lost and picked): 4,595 frames, 0 mismatches |
| `cargo test --workspace` (server) | all passed |

## Phase 8: the coverage sweep's failures (gp-v13 to gp-v19)

Worktree `dolphin-gprb`, branch `gameplay-rollback`, after merging `rollback-fixes` (`c27636d256` fast-forward, then `4944245954`, the `unranked` merge). The failures are the coverage sweep's (`docs/gprb-coverage.md`, gp-v12). Frozen build: `run/bin/gprb-e642b3ce98`. **Default region set: gp-v19** (C++ defaults and every tool).

### Method

Every failure the sweep kept a replay for (18 runs: the countdown savestate and the pass log in `run/qa/sweep2/work/<id>/`) was replayed exactly, with rollbacks as recorded, and compared with its ground truth (the same pass log flattened, `gprb_passlog.py flat`, no rollback). First, on the merged build with gp-v12, four of them were run again and reproduced the sweep's result to the frame (`f-wario-bowser` RNG at game frame 1706, `i-mario-marth` 4097, `v-zelda` 929, `m-yoshi` hung at session frame 97). A region set under test is passed to the replay as a JSON path, so no rebuild is needed for a region-set change.

Tools used (scratch scripts, the same harness commands as before):
- the replay against its ground truth with a stall detector (20 s without progress = hang, then the main thread's saved context and a back chain);
- `PPR_GPRB_PROBE` (now also r5/r6, and in every session phase) and `PPR_GPRB_RNG_LOG` (now also during the load and the countdown) on both runs;
- `cpu_trace` of one game frame in the flattened and in the rolled-back replay (`PPR_GPRB_INTERP_FROM`), diffed with resynchronisation after interrupts (interrupts arrive at different instructions in the two runs; the old diff stopped at the first one), loads and stores separately;
- region-set dumps (`PPR_GPRB_DUMP_FRAMES`) of both runs at a few frames, diffed per heap, and full dumps of one run at several frames, scanned for words outside the set that change and point into it;
- a list of every function-local static in the DOL (guard byte plus `__register_global_object`) and of every DOL object the set covers only in part.

Every dump was deleted after use.

### Family B: the early hang in mirror matches (gp-v13)

**Cause.** `fn_80174434` returns one of nw4r::ef's seven draw strategies: function-local statics in DOL .bss (`lbl_804A3E90`, 0x578 bytes), each built on first use behind a guard byte in .sbss (0x805A04F0-0x805A04F6). The guards are outside the set; `dolw-g1` restored four granules of the objects, among them strategy 1's vtable (0x804A3F64). When the first particle drawn with strategy 1 appeared in a frame that was then rolled back, the load put the pre-construction zeros back while the guard stayed set; the object was never built again. The next draw (`fn_801638D0` walks an emitter's particle managers, `fn_8016DE08` calls `strategy->vtable[3]`) called through a zero vtable: the main thread ran lowmem from 0 to 0x20 (`lr 0x80163918`). Mirrors hit it early because both fighters' effects use the same draw types. The replay with probes showed the zeroed object in the hung state; the partly covered statics list shows it is the only DOL static whose object and guard straddle the set.

**Fix.** gp-v13 removes 0x804A3E90+0x578 from the set (rendering state only); gp-v19 also excludes it byte-precisely, because a range added later shares a granule with it.

### Family D: items and Smash Balls (gp-v14 to gp-v16, the file IO wait)

Four separate causes, found in this order with the replays of `f-wario-bowser` and `i-mario-marth`:

1. **The camera subject list (gp-v14).** `cmSubjectList` (DOL .bss `lbl_8049DEDC`, `{count, first, last}`; `getSubjectByPlayerNo__13cmSubjectListFUl`) links the camera subjects of fighters and items, which live in the instance heaps. An item created or removed in a rolled-back frame left the head pointing at the discarded run's node: the re-created node was linked to itself. Flat and rolled-back runs then had different cameras from frame ~1000 (dumps: `gfCameraManager` and every view matrix), the Smash Ball, whose flight is bounded by the camera range (`stPositions` code among its RNG callers), moved differently, and at game frame 1706 the rolled-back run drew 5 extra randoms for a spawn the flat run did not make (RNG logs of both runs). The dumps also showed the self-linked node: `0x814CA340 → {0x814CA340, 0x814CA340}` against `{0x8049DEE0, 0x812C72C0}`. This list is also what Family A's mirror hang looped on (`fn_8009EFCC ← fn_8009EC5C`).
2. **Mid-match file loads (the file IO wait, gp-v15).** Items preload Pokémon and Assist Trophy resources during the match, and Final Smashes and transformations read fighter files (below). `gfFileIOManager` (`*0x8059FFF4`) queues requests; the main loop's IO update (`fn_80022F84`, called once per pass) hands them to the IO thread, which reads while the main thread waits for the retrace. So the frame on which a load completed depended on emulated time, which region mode does not rewind, and a rollback could restore the destination buffer under a read in flight. Now, at every loop top of a session (and before the base snapshot), while the IO manager has a queued request, the main thread runs the manager's update and waits one more retrace, as guest calls that return to the loop top (`RunIoWait`, GameplaySession.cpp). A load requested in frame N is complete before frame N+1 on every peer and in every pass, and nothing is in flight at a save or a load. gp-v15 adds the IO manager's queues and request pool (System FW 0x805BF860+0x5760, the manager 0x805CA1C0+0xA0): requests are allocated by the game and held for a few frames, and the pool must roll back with the handles. `gprb_status` counts `io_waits`, `io_wait_retraces`, `io_wait_timeouts` (600 retraces).
3. **The archive manager (gp-v16).** `gfArchiveManager` (System FW 0x805BB860, `*0x8059FFA0`) lists every loaded `gfArchive`; the archives live in the heaps they were loaded into. An archive loaded or released in a rolled-back frame (Pokémon and Assist Trophy preloads, transformation and Final Smash files) left the head stale and the re-created archive linked to itself (dumps: node 0x91A6A420 `{0x91A6A340, 0x91A6A340}` against `{0x91A6A340, 0x80BD4A00}`). `gfArchive::update`'s "File Clone" looks archives up in this list.
4. With 1-3, `f-wario-bowser` and `i-mario-marth` replay identically to their ground truth through game set (4,348 and 5,173 frames).

The three item hangs (`i-fox-falco` in nw4r g3d, the two `GXWaitDrawDone` waits of `f-ice_climbers-peach` and `f-olimar-lucario`) replay to the end of their logs without a hang and without a difference from the ground truth from gp-v17 on.

### Family A: Zelda/Sheik transformations (gp-v15 to gp-v17)

A transformation is a fighter change (`#fighter change begin` / `end`): both instances exist from the start, but the fighter's resource heap gets the other form's motion and model files (`FitSheikMotionEtc.pac` 3.1 MB and `FitSheik00.pac`, read from the SD card; or `File Clone` from a loaded archive). In the rolled-back replay every resimulation of the frame that started it issued the reads again and they completed at game frame 923 or 924 depending on the pass; `change end` was never logged, and at game frame 929 the active instance read garbage (x = 0x80789790). With the IO wait the reads complete at 923 in every pass; with the archive list (gp-v16) `change end` appears and the change completes; then a later render hang remained (game frame 1224, main thread at 0x20 with `lr 0x8019D990`, in `gfTaskScheduler::render → fn_8000E214 → fn_8000F4F0 → fn_8000EB1C`): the scene keeps per-model records in DOL .bss (`lbl_80494E00`, `lbl_80494EE8`, 0x80494E00+0x3C0) with pointers into the fighter's resource heap (0x915712A0 ↔ 0x915FB960 at each transformation) and instance heap, and `dolw-g1` restored only two of their granules. gp-v17 restores them whole. `v-zelda`, `v-sheik`, `m-zelda`, `m-sheik`, `f-zelda-sheik` then replay without a difference.

### Family C: Meta Knight (gp-v17)

Both Meta Knight hangs had the same signature as Family A's last one (`lr 0x8019D990`, `fn_8019D950` calling a g3d object's `vtable[3]` through a garbage pointer, from the render). With gp-v17 `v-meta_knight` and `m-meta_knight` replay to the end of their logs (921 and 725 session frames, where they had hung) without a hang or a difference.

### Family E: late drifts in mirrors (gp-v18)

`m-mewtwo` and `m-wolf` replay identically from gp-v17 on (Wolf's `changeStatus` messages pointed at articles; one of gp-v13 to gp-v17 covers them; not bisected further). `m-donkey_kong` still drifted at game frame 5644: P1's x stayed at -16.24 for five frames where play without rollback moved it. The traces of that frame were identical in the flat and the rolled-back run (loads and stores), so the state differed before the frame; the first run of the frame against its resimulation differed in two bytes at 0x805B75DA/DB (0x01 against 0x41), written by the stick conversion (`fn_8004897C`) and P+'s hook in it (0x80579398). They belong to the controller configuration object (System FW 0x805B7480+0x1E0, `*0x805A00C8`): the per-port button layouts, then per-port stick state the main thread updates every frame. gp-v18 adds it (and excludes the first 32 bytes of the next block, the bottom of a thread stack that shares the last granule). `m-donkey_kong` then replays identically through game set.

### Final Smashes (gp-v19, `PPR_GPRB_FORCE_FINAL`)

The sweep never saw a Final Smash. To force one, `PPR_GPRB_FORCE_FINAL=<port mask>` (diagnostics) makes the session call `ftManager::setFinal(entry id, false)` (sora_melee .text+0x10D878), as a broken Smash Ball does, for those ports at the last countdown frame, before the base snapshot; the ground-truth replay does the same. (Setting only the owner's final flag, `ftOwner::setFinal`'s bit, did not start one.) The random fighters then use it at their first neutral B.

With gp-v18, Wario/Bowser hung at game frame 815, when Wario-Man turned back into Wario: the main thread looped in `fn_8016EBB4` (`fn_8016EDA8 ← fn_8001ACC4`), nw4r::ef. The EffectSystem (DOL .bss `lbl_8049EDD8`, 0x5068 bytes: the effect, emitter and particle managers' activity lists and pools, whose objects live in the Effect heap) was covered by `dolw-g1` only in a few granules; Brawlback restores it whole. gp-v19 adds it and ef's resource list (`lbl_804A3E70`).

Forced Final Smashes on gp-v19, each a replay of a sweep pass log with both ports given their Final Smash, against its own ground truth:

| Pair (kept pass log) | Final Smash types | Evidence it ran | Ground truth without vs with the FS | Rolled back vs ground truth |
|---|---|---|---|---|
| Wario vs Bowser, FD | transformations (Wario-Man, Giga Bowser) | `#fighter change begin` at game frame 402, back at 812; game set at 2,881 instead of 4,348 | differ from game frame 332 | **identical**, 2,881 frames to game set (gp-v18: hang at 815) |
| Olimar vs Lucario, BF | cutscene (End of Day), beam (Aura Storm) | P1 status 274 (neutral B) becomes 278/279 at game frame 525 | differ from 525 | **identical**, 3,161 frames to game set |
| Mario vs Marth, FD | projectile (Mario Finale), cutscene (Critical Hit) | match ends at 4,182 instead of 5,173 | differ from 492 | **identical**, 4,182 frames to game set |
| Samus vs ZSS, BF | beam (Zero Laser), transformation (Power Suit) | match ends at 3,639 instead of 7,161 | differ from 404 | **identical**, 3,639 frames to game set |
| Zelda vs Sheik, BF | arrows (Light Arrow) | | differ from 405 | **identical** to the end of the log (2,154) |
| Ice Climbers vs Peach, FD | Iceberg, Peach Blossom | | differ from 627 | **identical** to the end of the log (1,971) |

The sweep has a new group for this, `ffs` (7 pairs, below).

### Results on the kept replays

All 18 kept replays, rolled back as recorded, against their ground truth (raw: the scratch results of this phase, summarised here):

| Run | gp-v12 (sweep) | gp-v19 |
|---|---|---|
| `m-yoshi`, `m-ice_climbers` (Family B) | hang at session frame 97 / 26 | to the end of the log, identical |
| `f-wario-bowser`, `f-samus-zero_suit_samus` (D) | drift at 1706 / 1662 | identical through game set |
| `i-mario-marth` (D) | drift at 4097 | identical through game set |
| `i-fox-falco`, `f-ice_climbers-peach`, `f-olimar-lucario` (D) | hang | to the end of the log, identical |
| `v-zelda`, `v-sheik` (A) | drift at 929 / 788 | identical (`v-sheik` through game set) |
| `m-zelda`, `m-sheik`, `f-zelda-sheik` (A) | hang | to the end of the log, identical |
| `v-meta_knight`, `m-meta_knight` (C) | hang | to the end of the log, identical |
| `m-donkey_kong`, `m-mewtwo`, `m-wolf` (E) | drift at 5644 / 5529 / 4088 | identical through game set |

(`m-sheik`'s log ends where it hung, at game frame 2115; the two runs differ only after it, when the replay has no more input.)

### The clock and the match start (open issue 7)

What reads the console clock around a match (every `bl` to `OSGetTime` 0x801E1B34 and `OSTicksToCalendarTime` 0x801E1D80 in the DOL, the loaded RELs and P+'s code area, with Smashville loaded):
- the calendar (`OSTicksToCalendarTime`): Smashville's clock face (`st_village` `fn_70_CFAC`: hour and minute → the two hands' angles, two floats in a stage object, drawn only), the stage select's date strings (`sora_menu_sel_stage`), and two P+ codes that name files by the date (0x80568A10, 0x8056A22C); the stage select also writes the clock-derived Smashville variant into the `gmGlobalModeMelee` init block, which the joiner already replaces with the host's;
- the time base alone (`OSGetTime`): OS, GX, sound, network and profiling code, and the P+ counters in `sora_melee` `fn_27_1E8FEC` (the known tick-derived values of Phase 3). None of these was found to feed `g_mtRand` or fighter state.

So no clock-derived value was found that reaches the RNG before the barrier on its own. The failure's trace has a better lead: at game frame 242 P1 stands at x = -49.41 on the host and +50.59 on the joiner, i.e. at the other start point. The stage's constructor (`__ct__7stMelee`, sora_melee .text+0x238BDC) shuffles the fighters' start points (`stMelee +0x1B4`, 32 swaps of `randi`, which uses `g_mtRand`) and reads the init block (game mode, players), all during the load, before the barrier copies the host's RNG. The joiner's init block and both peers' seeds are applied at the first loop top in `scMelee`; a path into the match that builds the stage within the pass that switches the scene (the online CSS's Versus setup, unlike the stage select's path used by `gprb_session.py`) would shuffle with an unseeded RNG and a stale init block. With RTCs on both sides of 15:00 (`gprb_session.py --rtc-a 14:57:00 --rtc-b 15:00:00`, new option), Smashville sessions through the stage select passed (two matches, 0 mismatches), and the RNG logs of both peers between the seeding and the barrier were identical (105 calls; the shuffle right after the seeding).

Changes (`0422682986`), all in the session:
- **The match start** (the host's init block for the joiner, the three seeds, the serial counter) now runs at the first of: a loop top with the change to `scMelee` pending, the first loop top in `scMelee`, or **stMelee's constructor** (HLE hook at 0x809435F0). In the stage-select path it still runs at the loop top, before the constructor (log: `gprb: stage constructed (match N, seeded true)`); where the constructor came first the log says `RNG seeded ... at the stage's construction`.
- **The barrier's sync block carries the start point table** (`*0x80B8A428 + 0x1B4`, 4 words); the joiner takes the host's and logs `joiner took the host's fighter start points (...)` if it differed. The fighters are created after the barrier (game frames 2 and 92) and enter at these points.
- Diagnostics: `PPR_GPRB_PROBE` logs r5 and r6 too, and probes and `PPR_GPRB_RNG_LOG` also cover the load and the countdown.

Not done: the original failure was not reproduced (it needs the online CSS path between two logged-in instances), so whether the constructor ran before the seeding there is not confirmed; the new log lines will say so the next time.

### Family F: dual-core sessions

No session-specific cause was looked for: the failing pairs were run again on the fixed build first (`gprb_sweep.py` session runs, `typical`, delay 2, raw `run/qa/gfx-sess1`): Fox/Falco, Lucario/Mewtwo, Samus/ZSS, Sonic/Knuckles and Zelda/Sheik in dual core, and Zelda/Sheik in single core, all to game set with 0 confirmed-checksum mismatches and identical traces (161-333 rollbacks per peer in dual core). Then all 30 sessions of the full sweep passed (below). Their failures on gp-v12 fit the families above: dual core makes 2-4 times as many rollbacks, so a rare restore fault (a stale list head when an object is created or freed in a rolled-back frame, a load in flight) hits more often; Zelda/Sheik's were Family A.

### The full sweep on gp-v19

`run/bin/gprb-e642b3ce98`, gp-v19, the same 136 runs as the gp-v12 sweep plus the 7 forced Final Smash runs (`ffs`); `gprb_sweep.py run --out run/qa/sweep3 --jobs 3 --retry-errors`; details and matrices in `docs/gprb-coverage.md`.

| Group | gp-v12 | gp-v19 |
|---|---|---|
| vs Fox | 38 / 41 | **41 / 41** |
| mirrors | 29 / 42 | **42 / 42** (2 after reruns, see open issue 9) |
| stages | 10 / 10 | **10 / 10** |
| all items | 1 / 6 | **6 / 6** |
| Smash Ball only | 0 / 7 | **7 / 7** |
| forced Final Smashes (new) | - | **7 / 7** |
| sessions, single core | 13 / 15 | **15 / 15** |
| sessions, dual core | 10 / 15 | **15 / 15** |
| **all** | **101 / 136** | **143 / 143** |

Every sync test also matched its no-rollback ground truth to game set. In the `ffs` runs the forms seen include Wario-Man and Giga Bowser, and the logs show the fighter changes (Zelda/Sheik, Wario/Bowser).

## Phase 9: the base snapshot race (open issue 9)

Dolphin `rollback-fixes`, `001dd0b2df` (on `7788a27738`). Frozen build: `run/bin/gprb-001dd0b2df`.

### Cause

Not guest state: a race in the snapshot code (`RollbackManager`, Brawlback's, used by every mode).
- A save keeps only the 64-byte granules written since the previous save (the JIT's dirty bitmap) in a ring of 8 slots. A **base snapshot** holds all of RAM as of the frame before the oldest slot.
- When the ring is full, `SaveFrame` takes the oldest slot's granules out and merges them into the base snapshot **on a job thread** (`m_eviction_job`), then returns.
- `LoadFrame` restores each granule written since the target frame from the newest slot at or before the target that holds it, and **from the base snapshot** when no slot in the ring does. It never waited for the eviction job. The next save waited for it; the load did not.
- In a session or sync test the load comes right after a save (the last pass of an update saves, the next update loads). With the sync test's distance 7 the target is the oldest slot, so most granules come from the base snapshot.

If the job had not finished, the load read the base snapshot while it was being written. A granule last written in the evicted slot's frame came back one frame older, or torn. On an idle machine the job finishes first almost every time. A job thread that is slow to wake or is preempted loses the race. Since `204a906682` (2026-10-06) idle job threads sleep instead of spinning, so a kick has to wake them. The test instances also run at below-normal priority. Under load, both make the lag longer.

This matches every observation of issue 9:
- flat replays never failed (no loads);
- clean replays were byte-identical (no lost race, same memory);
- region dumps of a crashing replay matched a passing one until it lost the race;
- the crashes clustered when the machine was loaded;
- they hit nw4r ef/g3d first: effect and particle objects are written every frame, so one frame of staleness there means stale list links and vtable pointers.

### Reproduction

New tool: `gprb_replay_stress.py`. It replays one pass log N times (J at a time), optionally with N CPU-burning processes at normal priority (`--burn`). Each replay is compared with the ground truth (the flattened log); a stall in the running phase counts as a hang and keeps `cpu_state` and the main thread's back chain. The log: a fresh `m-yoshi` sync test on the frozen gp-v19 build (`gprb_sweep.py --keep-work`, new option; it passed); 7,194 frames. Then the same log cut at session frame 2,400 (every failure below happened before session frame 2,210), so that 50 replays on a loaded machine fit in about an hour.

| Build | Log | Load | Replays | Result |
|---|---|---|---|---|
| fixed, old load (`PPR_GPRB_EVICT_NO_WAIT=1`) | full | idle | 4 | 4 identical; 0-1 loads per replay found the eviction unfinished |
| fixed, old load, eviction delayed 3 ms (`PPR_GPRB_EVICT_DELAY_US=3000`) | full | idle | 4 | **4 hangs, all at session frame 51** (`srr0 0x20`, `lr 0x8072AB3C`) |
| fixed, eviction delayed 3 ms | full | idle | 4 | 4 identical to game set (7,185 loads per replay waited for the job) |
| old (`run/bin/gprb-e642b3ce98`) | full | `--burn 12` | 25 | **25 hangs** at session frames 51-1,143 |
| old | 2,400 | `--burn 12` | 12 | **11 hangs** at 51-2,202, 1 harness timeout |
| fixed | full | `--burn 12` | 1 | identical to game set (640 loads waited) |
| fixed | 2,400 | `--burn 12` | 54 | **53 identical, 0 hangs, 0 drifts**, 1 harness timeout while loading the savestate (open issue 8); in each replay 13-191 of its 2,393 loads (51 on average) found the job unfinished and waited |

So host load turns the failure from intermittent into certain, and the wait makes it disappear. The old build's hangs under load have the same signatures as the original reports: `fn_8005C2B0 ← fn_80017618` (the effect manager's list walk, as in the sweep's `m-yoshi`/`m-ness` hangs), `fn_8005B6F0`, `fn_80163FAC`, `fn_8019A874` jumping to `0x4E6F6464`, `fn_8016C870`, `GXSetFog`, and jumps to lowmem `0x20`. The other leads did not need testing: the failing replays used the Null video backend (no EFB copies to RAM) and single core (no GPU thread), and the forced delay alone produced the crash on an idle machine.

### Fix (`001dd0b2df`)

- `LoadFrame` waits for the eviction job before it reads anything (`WaitForEviction`). This costs nothing when the job is done; under `--burn 12` the longest wait was 57 ms.
- The job system: a job's completion count is decremented with release order and read with acquire (it was relaxed, which is correct on x86 only by accident of the hardware, and matters on ARM64 Macs). `Job::finish` no longer writes `is_done` into a job after the count drops, when its waiter may already have released the job's block.
- Diagnostics: `PPR_GPRB_EVICT_DELAY_US=N` (every eviction first sleeps N µs) and `PPR_GPRB_EVICT_NO_WAIT=1` (the old load). `gprb_status` reports `load_evict_waits` (loads that found the job unfinished) and `load_evict_wait_us_max`.

The race is in Brawlback's own snapshot code, so whole-machine rollback (netplay) had it too. Nothing in it is specific to Windows or D3D11: the replays used the Null backend in single core, and the code is the same on every platform, so the Linux and macOS builds had it too.

### Regression check: mirrors and forced Final Smashes

The fixed build (`run/bin/gprb-001dd0b2df`, gp-v19) ran the sweep's `mirror` and `ffs` groups: `gprb_sweep.py run --out run/qa/sweep4-i9 --groups mirror,ffs --jobs 3 --build run/bin/gprb-001dd0b2df`. The machine was otherwise idle apart from two instances of another agent.

| Group | gp-v19 (`sweep3`) | `001dd0b2df` (`sweep4-i9`) |
|---|---|---|
| mirrors | 42 / 42 (`m-yoshi`, `m-ness` after reruns) | **42 / 42** |
| forced Final Smashes | 7 / 7 | **7 / 7** |

Every run reached game set (7,193 session frames) with no sync-test mismatch. Every mirror matched its ground truth to game set. One run (`m-diddy_kong`) first failed while booting to the CSS: the harness connection was reset (open issue 8). It passed on `--retry-errors`. The forms seen include Wario-Man, Giga Bowser and the Zelda/Sheik changes. The ground-truth comparison of `ff-wario-bowser` and `ff-olimar-lucario` covers only the first 62 frames and none, respectively, as it did on gp-v19 (196 and 96 frames). That is an existing limit of the trace for these two forced runs, not a change.

## Phase 10: installs ran without the deterministic GPU thread

Dolphin `rollback-fixes`, `bd9ba09eb6`. Build used: `run/bin/gpudet-fix` (`bd9ba09eb6` plus the uncommitted `WiiRoot.cpp` NetplaySave change that 0.1.28 shipped).

**Symptom.** Client 0.1.28, Unranked over the internet, dual core, D3D11: two games in a row froze within seconds of the start (2026-10-08). The host's log: `session running (host; dual core true, deterministic GPU thread false)`, then `GFX FIFO: Unknown Opcode (0xcc)`; the CPU thread kept running JIT code while the video thread waited for work. The peer's side ended with the peer gone.

**Cause.** The dual-core design (this document's definitions, `docs/rollback-fixes-status.md` round 3) needs `GPUDeterminismMode = fake-completion`. It came only from P+'s `GameSettings/ID-Project+ Netplay Launcher.ini`, which the harness template has and installs do not: the launcher takes only the SD card, the launcher DOLs and `Sys/NetplaySave` from P+'s release (`launcher/src/dolphin/install/pplus_release.ts`), and the Dolphin bundle ships `Sys/` only. The netplay layer's override (`NetPlayConfigLoader`) does not apply to gameplay sessions, which start in-game long after boot chose the mode. So every install ran "auto", i.e. off.

**Fix (`bd9ba09eb6`).** The fork's default is `fake-completion`. A session that still runs dual core without the deterministic GPU thread (an explicit `none`/`auto`) logs an error. `gprb_session.py` takes `--gpu <mode>|unset` (`unset` removes the key from the template's INIs, as on an install) and `--video`.

| Build | `--gpu` | Preset | Result |
|---|---|---|---|
| `gprb-001dd0b2df` | `auto` | typical | host hung at frame 89 (7 rollbacks), joiner `peer timed out` |
| `gprb-001dd0b2df` | `unset` | typical | host hung at frame 59 (7 rollbacks), joiner `peer timed out` |
| `gprb-001dd0b2df` | template (`fake-completion`) | typical | game set, 6,784 frames, 396/250 rollbacks, 0 mismatches |
| `gpudet-fix` | `unset` | typical | game set, 4,176 frames, 174/218 rollbacks, 0 mismatches |
| `gpudet-fix` | `unset` | bad_wifi | game set, 3,268 frames, 272/322 rollbacks, 0 mismatches |

All dual core, Null video, 2-minute rule, raw results in `run/qa/gpudet/`. In network sessions `gprb synctest: checksum differs from the first run` is not a symptom: a mispredicted first run differs from its resimulation by design (the passing runs above log 100-200 of them each, the cap). Workaround for an installed 0.1.28: put P+'s `ID-Project+ Netplay Launcher.ini` (in the fork's `Data/user/GameSettings/`) into `<User>/GameSettings/` on both machines; the session line then says `deterministic GPU thread true`.

## Phase 11: rollbacks no longer push the frames after them back

Monorepo `9ec95463` (with telemetry `be93d264`). Branch `netcode-pacing` in the fork's worktree `dolphin-netcode`.

**Symptom.** A real Unranked set between two nearby players (2026-10-08, the host's `dolphin.log`) rolled back on 26% and 40% of frames ("playable, but not as smooth as Slippi"). Harness sessions with the random macros roll back on 4-5%.

**Cause.** Region snapshots do not rewind the emulated ticks, so `CoreTimingManager::SetRollbackResimulating(false)` re-anchored the throttle at the presented frame after every re-run. Each rollback pushed that frame and every later one back by the re-run's wall-clock time (load, re-run frames, saves: about 4 ms for one frame on this PC). The peer that rolled back fell behind faster than time sync (at most -2%/+1%) could correct. The other peer then got its inputs later and rolled back more and deeper: a feedback loop. Brawlback's newest code (Nyx, 2026-10-08) restores the throttle reference after a load for the same reason; whole-machine loads rewind the ticks, so a plain restore works there.

**Fix.** `CoreTimingManager::BeginRollbackBurst` (called before every load) remembers the host time the timeline is due at; the presented frame after the re-run resumes there, skipping the ticks the re-run used. A re-run that fits into the frame's slack costs no time; a longer one is caught up as after any slow frame. `PPR_GPRB_OLD_THROTTLE=1` keeps the old behaviour. Also: the sync test's first-run comparison runs only in sync tests (it logged up to 200 WARN lines per online session), and the wait for the peer polls every ~100 us with the precision timer instead of `sleep_for(250 us)`.

**Telemetry.** Every 600 displayed frames and at the end, network sessions log a `gprb net:` line: rollbacks, re-run frames and depth, rollback cost, late frames (more than 2 ms behind the throttle's schedule), waits for the peer, frames ahead, time-sync speed, ping, jitter and bandwidth (GekkoNet), the local pad sample's age, the present interval spread, and "screen hitches" (presents that repeat or skip a refresh on a 59.94 Hz screen without VSync, averaged over four phases).

**Results.** `gprb_session.py` through netsim `typical` (40 +- 8 ms RTT, 0.5% loss), dual core, D3D11, 2-minute rule, Fox/Falco Battlefield, with a human-like input model (the stick changes every 1-3 frames while moving; scratch `busy_session.py`). Same build, the old behaviour by the environment variable. Raw: `run/qa/netcode/`.

| Run | Rollbacks (host / joiner) | Re-run frames | Depth avg / max | Frames ahead | Time-sync speed | Mismatches |
|---|---|---|---|---|---|---|
| old 1 | 9.6% / 13.8% | 1,053 / 1,629 | 1.7-1.8 / 5 | -3.2 .. 3.4 | at its limits (0.98-1.01) | 0 |
| new 1 | 7.3% / 7.1% | 553 / 532 | 1.05 / 3 | -0.4 .. 0.8 (one 2.2 at the start) | 0.993-1.002 | 0 |
| old 2 | 9.9% / 13.0% | 1,184 / 1,753 | 1.7-1.9 / 4 | -2.9 .. 3.0 | at its limits | 0 |
| new 2 | 8.0% / 6.8% | 454 / 378 | 1.05 / 2 | -1.2 .. 1.4 | 0.987-1.006 | 0 |

No waits for the peer in any of these runs. Screen hitches did not improve on this PC (old 10-12% of presents, new 12-13%; the present interval spread is 4-8 ms in every mode with two instances on one machine): the frame after a rollback is still shown late, and now the next one catches up. Orca 0.3.34's `PresentPacer` (present at a steady offset from each copy's VI time) was tried on top (`netcode-pacing` `0c98069d30`): spread 4.8-5.2 ms against 6.8-7.8, but hitches unchanged (12-13%), 2.9% late frames against 1.4%, and rollback costs up to 110 ms (the GPU thread sleeping before a present holds the CPU thread in dual core). Not merged.

Regression on main's build (`run/bin/main-9f8981ea`, with the ping line, music off and fake-completion): game set, 4,394 frames, 351/333 rollbacks (depth max 3/2), 0 mismatches; `0x90E60F34` read 0 on both peers during the match (1.0 on a build without the Music Off change).

## Phase 12: the Smashville desync (open issue 7)

**Symptom.** Smashville games of the online tests that differed from session frame 0: equal setup keys, the barrier passed, then the first RNG word different and nearly every confirmed checksum too, often with both games still reaching game set. Seen 2026-10-07 (`run/scratch/merge-unranked/`) and three times on 2026-10-08 (`run/scratch/bootflow-smashville-desync/`; the `ranked` worktree's `unr-a-0`/`unr-b-0` game 2 and `unr-a-2`/`unr-b-2`). In two of them the joiner logged `joiner took the host's fighter start points (0 1 2 3 -> 1 0 2 3)`: the two stage constructors had shuffled with different RNG states although both peers had logged the same seeds before.

**Cause: the stage music, not the clock.** When the match scene starts, the game picks the stage's song from P+'s tracklist (`PlayID[...] Index[...]` in the log), normally just before the session seeds the RNGs at the loop top. In every failing pair one peer's first pick was invalid (`PlayID[0] Index[261]`, `[6F54] Index[248]`, `[7] Index[242]`: indexes far past Smashville's 16 tracks, from whatever that machine had loaded before), and that peer picked again after the seeding (`PlayID[F000] Index[0]`, Animal Crossing's Title), drawing from `g_mtRand` before the stage was built. The other peer did not. So the start-point shuffle in stMelee's constructor and everything Smashville builds with `g_mtRand` while it loads started from different states; the barrier's copy of the host's RNG and start points comes after all of that. Different songs alone do no harm: passing runs had them too, picked before the seeding. With `PPR_GPRB_RNG_LOG`, passing runs' `mtRand` calls from the seeding to the session start were identical on both peers (one call between the seeding and the constructor, `sora_scene` `0x806D1D4C`).

**Fix.** At stMelee's constructor (the HLE hook from Phase 8) the session now seeds the three RNGs again when it has seeded at the loop top already: `gprb: match N RNG seeded again at the stage's construction (was ...)`. The `was` values show any draw made on one machine only. The object serial counter is not reset there (objects made since the loop top keep their serials). Network mode only, as before.

**Verification.** `PPR_GPRB_TEST_RNG_SKEW=1` (tests only) moves the joiner's `g_mtRand` on right after the loop-top seeding, as the second song pick does. `test_unranked_pairs_strangers_on_a_server_stage` with it, game 1 on Smashville:
- without the fix: desync from frame 0 (2,024 of 2,033 confirmed checksums), both games at game set: the failure as seen;
- with the fix: 3 of 3 passed (`fix1`-`fix3`), the joiner's `was` word different from the host's, then identical.

Without the variable, before the fix: 7 of 7 passed with `PPR_GPRB_RNG_LOG` (`ua1`-`ua4`, `ub1`-`ub3`, two at a time); the failures need the stale first pick, which these runs did not hit.

Final runs with the fix and the `-Oz` plugin (with the colour clash shades, `docs/game-code.md` §11; raw `run/scratch/sv-runs/`): the skewed test again (Smashville and Dream Land, 0 mismatches), `test_online_unranked.py` 3 of 3 (Unranked: Dream Land 4,121 frames, Smashville 1,304; Direct: Dream Land, then the loser's Final Destination; 0 mismatches each), and `test_online_ranked.py` (a 2-0 set, 0 mismatches, rated; an earlier 2-1 run played clean and only tripped two stale test assertions from the `ranked` merge, fixed: an instance name and the server's `game reported` log line).

## Phase 13: the deterministic GPU thread only during sessions

**Symptom.** Client 0.1.32 (2026-10-09): the menus and the character select felt sluggish ("the cursor moves a bit slow"). With `GPUDeterminismMode = none` the same install felt better.

**Cause.** Phase 10 made `fake-completion` the default and shipped it in `Sys/GameSettings/ID-Project+ Netplay Launcher.ini`, so the deterministic GPU thread ran all the time, menus included. Before that, installs ran `auto`, i.e. without it outside netplay and movies.

**Fix.** The default is `auto` again and the shipped INI no longer sets the mode. In this fork `auto` also means the deterministic GPU thread while a gameplay rollback session runs: `FifoManager::SetRollbackSessionDeterminism(true)` at the top of `StartRunning` (on the CPU thread, at the session's frame 0, before the first `SaveFrame` takes the base snapshot), `false` in `EndRunning` (after `EndRegionMode`). The switch makes the GPU thread idle first (`SyncGPU`, `FlushGpu`), then pauses its loop as `PauseAndLock` does (`EmulatorState(false)`, `Wait`), so a wakeup from another thread cannot run the FIFO while the mode flips, and calls `UpdateWantDeterminism`. Only the FIFO mode changes: Dolphin's "want determinism" (IOS, JIT FMA) is untouched, so there is no mid-game JIT change. An explicit `fake-completion` still keeps it on everywhere; an explicit `none` still logs the Phase 10 error at session start.

Machine state outside the region set at the switch (a pending CP interrupt, a partial command left in the FIFO) can differ between the peers, as it already did with different menu paths; what rollback keeps equal is the region set, and the checksums compare it from the first frames.

**Verification** (a clean worktree with only this change, `--gpu unset` = the install's default, 2-minute rule): `typical` dual core, 2 matches in a row (the mode off between them, on again): game set both, 0 mismatches; `bad_wifi` dual core, 2 matches (343/339 and 735/754 rollbacks): game set both, 0 mismatches; `typical` single core (the switch does nothing): game set, 0 mismatches. Both peers logged `deterministic GPU thread true` at every dual-core session start.

## Phase 14: sound through rollbacks

**Symptom.** At high ping, sound effects cut out during rollbacks and sound odd.

**Causes.**
1. **The sound system ran through resimulated passes.** Brawl's sound system is outside the region set (Phase 5), so it plays on through a rollback. Every resimulated frame still moved every voice and the music forward and woke the game's sound thread (envelopes, fades), and `AudioCommon::SendAIBuffer` dropped those samples (`15de378723`). So every rollback of N frames cut N × 16.7 ms out of every playing sound, and a sound that only the corrected input starts lost its first frames.
2. **Resimulated stops killed sounds that had not started yet.** `sndSystem` keeps its own 32-slot table of playing sounds (`sndSystem+0x2D4`, 0x20 bytes each, outside the set). A resimulated pass that calls "stop every SE of this owner" (`0x800765B0`) or `playSE`'s same-id restart stops the sounds that the first runs of this frame and later frames started. In the timeline being re-run, those sounds do not exist yet. The dedupe (Phase 6) then finds them gone and does not start them again. Seen in a trace: a 170 ms sound cut after 12 ms, started again from its beginning a rollback later, then cut again. In a rollback-every-frame test this happened to 51 of 1,284 resimulated sound starts.
3. **Mispredicted sounds were stopped with `Stop(0)`**, mid-waveform (a click).

**Fix (gameplay sessions only).**
- **The audio clock waits for resimulated passes** (`CoreTimingManager::SetRollbackAudioWaits`, set by the session). `SystemTimers::AudioDMACallback` waits out a pause instead of running the audio DMA. With no DMA there is no AI interrupt, no AX frame (no mix, no voice moves) and no wake-up of the sound thread, so voices, envelopes, fades and sequences all wait. The audio advances only with the presented frames, as the console's does on Slippi, where rollback re-runs the game's frame inside one emulated frame. A rollback pauses every sound instead of cutting a piece out of it, and a sound that only the corrected input starts is heard from its beginning, at most the rollback's depth late. `SendAIBuffer` no longer drops samples during resimulation in this mode: whatever plays then belongs to the presented timeline. The full-memory rollback (`RollbackManager` without region mode) rolls the sound system back and keeps the old drop.
- **How long the pause is.** Each resimulated pass adds one field when it starts, so the pause lines up with the passes. When the pass ends, its measured length replaces that field only if it differs from a field by more than 10%: lag frames run about 1.9 fields, and the frames that catch up after them 0.29 or 0.82. Smaller differences are not frame lengths. The first pass after a load is about 0.5% short, and the presented pass after it is that much longer (per position in an update: 0.9948, 1.0000, 1.0000, 1.0051 fields). Each rule was checked against the ideal (an update's length minus the ground-truth length of the frame it presents, from a `--no-rollback` trace), as cumulative error in fields over a minute of rollback-every-frame updates:

| Pause per resimulated pass | Mario/Marth FD (733 updates) | 4 players BF (2,232 updates) |
|---|---|---|
| while the pass runs (its measured length) | -3.76 (drift: sounds 26 AX frames late after 20 s) | -14.7 |
| one field | 0.002 | -6.0 (steps at lag frames) |
| measured length rounded to fields | 0.002 | +6.0 |
| one field, measured length if more than 10% off (**used**) | 0.002 | -0.16 (never more than 0.22 off) |
| field boundaries that fall in resimulated passes | no drift, but the pause no longer lines up with the passes (waveform match 0.68) | |

- **Resimulated stops of sounds that have not started yet are ignored.** `GprbSoundStopHook` replaces `nw4r::snd::detail::BasicSound::Stop` (`0x801BC684`, shared by every sound type) and asks `Gprb::Session::OnSoundStop`. In a resimulated pass, a sound that an earlier run of this frame started and this pass has not started yet (`snd_remaining`), or that a later frame started, keeps playing. If the corrected run does not start it, it is stopped at the end of its frame like any other mispredicted sound. New status counter: `sound_stops_ignored`.
- **Mispredicted sounds fade out over 3 frames** (`SND_STOP_FADE_FRAMES`) instead of `Stop(0)`.
- `PPR_GPRB_RESIM_AUDIO=advance` keeps the old behaviour, for A/B runs.

A first version froze only the AX voices (DSP HLE) during resimulation. That left two problems. The game's sound thread kept running (envelopes and fades went ahead of the waveforms). The guest plays each mix one AX frame later, so the first AX frame after a re-run was silence and the last one before it was dropped. Replaced by the paused audio clock.

**Verification.** `gprb_mispredict.py --dump-audio DIR` copies the DSP dump (what a player hears) of each run. `harness/tools/gprb_audio.py` compares a rollback run with the `--no-rollback` ground truth of the same recording. It reports the waveform match per half second (strict: a sound 3 ms early drops it), the drift, and band levels (32 bands, 32 ms windows, ±32 ms tolerance): the median difference in dB and the share of loud moments more than 6 dB quieter (cut off) or louder (doubled). The harness plays no music (`SDStreamOpen Failed: …/X02.brstm`: the BRSTMs are not on its SD image), so these are sound effects only. Every run below is a misprediction test, `distance` 4: every frame is rolled back and 3 frames are re-run, far more than a session. Gameplay traces were identical to the ground truth in every run, old and new.

| Fixture (3,600 frames, dual core) | Input | Old: cut off / doubled / median diff | New: cut off / doubled / median diff | New waveform match |
|---|---|---|---|---|
| Mario/Marth FD | no misprediction | 22% / 2.2% / 5.5 dB | **0.03% / 0% / 0.0 dB** | 1.00 (sample-exact) |
| Mario/Marth FD | both ports mispredicted every frame | 21% / 2.4% / 5.5 dB | 0.19% / 0.40% / 0.23 dB | 0.96 |
| 4 players BF (Zelda, ICs, Olimar, Peach) | no misprediction | | 0.19% / 0.03% / 1.3 dB | 0.77 (3 ms offset) |
| 4 players BF | all ports mispredicted every frame | 58% / 0.6% / 9.8 dB | 0.19% / 0.38% / 1.5 dB | 0.76 |
| Ice Climbers/Charizard FD, items (`ics-cd`) | both ports mispredicted every frame | | 0.08% / 0.05% / 0.03 dB | 0.97 |
| Peach (`peach-cd`) | both ports mispredicted every frame | | 0.08% / 0.16% / 0.0 dB | 0.99 |
| 2v2 Smashville (R.O.B./Wario vs Bowser/Peach) | no misprediction | | 0.03% / 0.03% / 0.01 dB | 1.00 |
| 2v2 Smashville | all ports mispredicted every frame | | 1.4% / 0.38% / 0.71 dB | 0.83 |
| Mario/Marth FD, **single core** | both ports mispredicted every frame | | 0.19% / 0.27% / 0.23 dB | 0.96 |

The 4-player match drifts by at most 3 ms over the minute (against -99 ms with a field per pass, and +200 ms with rounded lengths). Its waveform match stays below 1 because of that offset, which nobody hears. With misprediction, the remaining differences are the expected ones: a sound only the corrected input starts plays up to 3 frames late, and a sound only the mispredicted run started is heard for a frame, then fades.

Network sessions (`gprb_session.py`, two independently booted instances, dual core, 2-minute rule, Fox/Falco Battlefield): `typical` game set on both, 2,132 confirmed checksums, 0 mismatches; `bad_wifi` (435 rollbacks, up to 6 frames deep, 317 waits for the peer) game set on both, 4,398 checksums, 0 mismatches; both peers' traces identical. Raw: `run/qa/sfx/`.

**Not measured here:** music (absent in the harness), and listening. The numbers say the cut-outs are gone, but ears on an unmuted session should confirm it.

## Phase 15: "desync at frame 0" after offline 4-player Versus (staging, 2026-10-10)

**Symptom.** Staging (Mac host, Windows joiner, a room's 1v1): game 8 ended at once with `gprb: desync at frame 0 (local 22c43180, remote d0526b01)`; equal setup keys, the barrier passed, the same RNG seeds, the same song-independent loads in the countdown. Game 7 between the same two machines ran 4,327 frames clean. Between the two games the Windows player had played offline 4-player Versus matches.

**Cause: the confirmed checksum read fighter entries the match does not use.** `ftEntryManager` (0x80624780) keeps 9 entries; a match fills the first n in port order. When the next match has fewer players, Brawl frees the extra entries (`m_entryId` = -1) but leaves their player number (+0x58) and instance index. `ReadFighterFields` (the session's per-frame checksum, `FrameChecksum`, and the game results) took every entry with a player number below 4, so after a 4-player match a 2-player match's entries 2 and 3 counted as P3 and P4, with stale owner and fighter pointers: on that machine only (a machine that never played 4 players has 0xCCCCCCCC there). Measured with one instance (a 4-player Versus match, then a 2-player one): entries 2 and 3 read `entry_id 0xffffffff, player 2 / 3, instance 0`. Neither the platforms (a Mac/Windows `xplat_trace.py` run on Smashville and two Mac/Windows gameplay sessions on Smashville, fixed and host clocks: identical through game set) nor the costumes (a Windows/Windows mirror match, Game & Watch costume 2 on both, Smashville: clean) were involved.

**Fix** (`GameplayRollback.cpp ReadFighterFields`): only the match's own entries count: the first n, n = the players in the match's setup (`gmGlobalModeMelee`, human or CPU), each for a port the setup has, never a freed one (entry id -1), the first entry for a port winning.

**Test.** `harness/tests/test_online_mirror.py::test_direct_after_an_offline_four_player_match`: A plays offline 4-player Versus, then a Direct game with B. Before the fix: both end at once with `desync` (`desync at frame 0`, as in staging); with it: game set on both, 652 confirmed frames, 0 mismatches. (Rollback's whole-machine `CalculateDesyncChecksums` still hashes entries 0-3 by index: the netplay fallback boots both machines together, so their tables are the same.)

## Phase 16: stutter online, not offline (prod, 2026-10-10)

**Symptom.** A prod set with halfjaw (52-100 ms RTT): "stuttery, microstutters all the time, choppy", visible, only online; local Versus is smooth. The netcode was fine (16% of frames rolled back at depth 1, 0 desyncs, 1-4 late frames per 600), but the presents were not: interval sd ~7 ms and 1,368 screen hitches in 5,655 frames (24%).

**Cause.** This fork presents with Immediate XFB and Rush Frame Presentation: a frame is shown when the GPU thread runs its XFB copy, and the CPU sleeps once per frame, at the first throttle (an SI poll) after the GPU thread presented. In a session the GPU thread is deterministic: it gets the FIFO only when the CPU thread reads it, so the present raced the racing CPU, and the first poll after it changed from frame to frame. `PPR_PRESENT_LOG` traces (two windowed instances, `--platform win32`; headless instances never present) showed two modes alternating every 4-7 frames:

- the sleep at a poll just after the XFB copy, before the session's loop top (which reads the local pad): the next frame then ran from its input to its copy in ~2 ms;
- the sleep at a poll after the next frame was rendered: that frame waited ~10 ms before its copy, its input already read.

Brawl's copies are exactly one VI field apart in emulated time (16.683 ms, sd 0.17), so the presents took the poll's drift: the CPU's lead over the copy's due time went 11, 12, 13, 14 ms and snapped back, one frame in ~6 shown 2-4 ms late, and the local input's age at the present jumped between ~2 and ~15 ms. Rollback re-runs (~4 ms a frame) added more. Offline the GPU thread is not deterministic and sees the copy as soon as the CPU writes it.

Not the fix: Dolphin's Smooth Early Presentation (an earlier headless A/B was a no-op, the presenter returns before its sleep when headless); throttling at the loop top to its own due time (the loop top's emulated position against the copy varies with the frame's work: 36% hitches); a wake-up anchored to the previous copy plus a field minus an adaptive lead (better on one peer, frames bunched on the other).

**Fix** (`[Core] RollbackPresentPacing`, default on, active while a session runs with Immediate XFB and Rush in single core or deterministic dual core):

- `CoreTimingManager::OnPresentedXFBCopy`: the CPU thread re-arms Rush's throttle when it sends a presented XFB copy (the FIFO preprocessor in deterministic dual core, the copy itself in single core) instead of the GPU thread at its present. The sleep is then always at the first poll after the copy: VI-locked, before the loop top, and every frame runs from its input to its present in one go.
- `PresentStats::PresentHoldUntil`: the presenter holds each paced frame until its copy's due time (throttle clock, taken with the copy decision) plus the 95th percentile of the last 120 frames' arrivals after theirs, at most 8 ms. A frame that rolled back comes ~4 ms late; the hold keeps the presents a field apart and shows only the rare slower frame late. A rolling window rather than a quantile tracker: after a stall the tracker stayed high for ~30 s.
- The `gprb net:` line reports the holds: `held N (avg X ms, max Y ms)`.

**Results.** `gprb_session.py --cpu dc --video D3D11 --platform win32 --minutes 2`, Fox/Falco Battlefield, off (`--config Dolphin.Core.RollbackPresentPacing=False`) and on back to back, both peers, match frames only (raw: `run/scratch/pacing/ab-*`):

| Link | Presents > 2 ms off a field | Screen hitches | Local input to present (median, p90) | Rollbacks |
|---|---|---|---|---|
| typical (40 +- 8 ms), off | 17.3%, 17.7% | 7.8%, 8.2% | 15.0, 18.2 ms | 94, 97 |
| typical, on | 2.1%, 2.5% | 1.4%, 1.1% | 3.9, 5.7 ms | 24, 19 |
| cross_country (80 +- 5 ms), off | 29.3%, 29.3% | 13.4%, 13.3% | 14.8, 18.7 ms | 385, 368 |
| cross_country, on (shorter match) | 3.3%, 2.8% | 1.7%, 1.7% | 6.1, 8.1 / 5.7, 6.8 ms | 243, 248 |

The holds averaged 2.0 ms (typical) and 3.5-4.0 ms (cross_country). 0 mismatches in every run; a single-core session ran clean (present sd 1.8 ms); `test_online_unranked.py` 4 of 4 on the final build. "Late frames" now count more (the rollback re-run comes after the frame's one sleep); they no longer delay the presents beyond the hold.

## Open issues

Resolved in Phase 6: the dual-core divergence (ground-collision list heads, gp-v11), the Peach article crash and the `GXWaitDrawDone` stalls (the GX FIFO ring tail), the 11-frame sync-test bursts (camera quake controller, gp-v12), stopping and re-attaching sounds. Resolved in Phase 8: every failure of the coverage sweep (gp-v13 to gp-v19, the file IO wait). Resolved in Phase 9: the nondeterministic render/effect hang (a load read the base snapshot while the eviction job was still merging into it). Resolved in Phase 12: the Smashville desync from frame 0 (issue 7).

1. **Sync tests must be checked against the ground truth.** Their own check (first run against resimulation) missed a fault that changed the game within seconds. `gprb_mispredict.py` with a recording, `--no-rollback`, and a comparison of the traces is the stronger test; only Mario/Marth FD and Ice Climbers/Olimar PS2 have recordings so far. The closed-loop scenario sync tests (`gprb_synctest.py`) cannot be compared this way.
2. **State outside the set that only rare events touch** is still found one case at a time. `gprb_memdiff.py scan` finds global list heads that point into the set (Phase 8 found three more: camera subjects, archives, and the IO manager's requests, all only touched when objects are created or freed mid-match); DOL objects that the set covers only in part (`dolw-g1`) caused two more (draw strategies, scene records), and the rest of them (nw4r g3d's statics `lbl_804A4540`, `lbl_804A5514`, `lbl_804A7F40`, `lbl_804A9A80`, and a few small ones) are still covered in part; counters and flags need a census (`PPR_GPRB_CENSUS`) and a reason. Known and left alone: the task-id counter `gUnk8059c66c` (ids of objects created in resimulated frames differ between peers; no effect found).
3. **Sessions end with `peer timed out` when the machine is overloaded**: 4 of the 10 `s8` sessions ended between 237 and 13,457 frames, both peers on the same frame and with 0 mismatches, while 10+ other Dolphin instances ran. The GekkoNet silence limit (7.2 s at delay 2) was exceeded by stalls of a starved process, not by the network.
4. The rollback count in the sessions is low (random macros predict well). A harder input model would stress deeper rollbacks between peers; the misprediction test covers that on one instance.
5. ~~The online backend is not registered outside the harness.~~ Done in Phase 7: both frontends register it, and it is the default.
6. ~~The coverage sweep still defaults to an old region set.~~ Every tool defaults to gp-v19 and the sweep to `run/bin/gprb-e642b3ce98` (Phase 8).
7. ~~**A desync from frame 0 on Smashville, seen once** (2026-10-07, the first `test_online_unranked.py` run after the `unranked` merge; log `run/scratch/merge-unranked/unranked.log`). Equal setup keys, the barrier passed, the RNGs seeded alike (`4f960b68 6b6c6b20 5048c23a`), yet at frame 0 (game frame 240) the first RNG word differed (`1240120447` against `2104839129`; the other two equal) and the fighters from frame 2; the host ended at "game set" after 573 frames, the joiner with "peer timed out". It happened at 14:59:2x local time, under a minute before the hour, and Smashville's lighting follows the console clock: the two instances' RTCs may have been on either side of an hour boundary. Two later Smashville games ran identically. Not reproduced. Phase 8 ("The clock and the match start"): no clock-derived value was found that reaches the RNG before the barrier; P1 had entered at the other start point, which the stage's constructor shuffles with `g_mtRand` during the load. The match start now also runs at the stage's constructor if it has not run yet, and the joiner takes the host's start points at the barrier; new log lines show either if it happens again.~~ Resolved in Phase 12: not the clock but a second song pick, with `g_mtRand`, on one machine only after the seeding; the RNGs are seeded again at the stage's construction.
8. **Localhost TCP connections sometimes time out on this machine** while several agents run Dolphins: a harness port another process answered ("another harness client is already connected"), a fresh harness port that never accepted, and the portable Postgres of `OnlineBackend` refusing or timing out its first connections (`CREATE DATABASE`, the services' pools). Runs on 2026-10-07 used one portable Postgres cluster started by hand and `PPHARNESS_PG_URL` (the harness's external-Postgres mode), which held up. `OnlineBackend`'s external-Postgres mode passed the URL before psql's options, which psql on Windows ignores; fixed.
9. ~~**A rare hang in render and effect code that a replay does not always reproduce** (Phase 8). In the gp-v19 sweep `m-yoshi` and `m-ness` first failed for harness reasons, then hung when run again (`m-yoshi` at session frame 4,270 and `m-ness` at 3,865, both in `fn_8005C2B0` ← `fn_80017618`, the effect manager's list walk), then passed when run a third time. Their pass logs (`run/qa/sweep3/work/m-yoshi/`, `m-ness/`; the hung runs' JSON in `run/qa/sweep3-hangs/`) replayed exactly: `m-ness` 3 of 3 to the end; `m-yoshi` 18 of 25 to the end, 7 crashed at session frames 130-1,901 in nw4r ef/g3d code (`fn_8005B6F0` calling a list node's `vtable+0x14`, `fn_80163FAC`, `fn_8019A874` jumping to `0x4E6F6464`), with gp-v18 as with gp-v19; the flattened log never crashed. Two replays that did not crash were byte-identical in all of MEM1 and MEM2 at session frames 300 and 600, and region dumps of a crashing and a passing replay were identical at 50-400. So something in a rolled-back run is not determined by the emulated state alone (host timing, or a host thread touching guest memory), and it shows in effect/render objects. The crashes came in clusters while this machine was loaded (other agents' instances).~~ Resolved in Phase 9: not guest state but a race in the snapshot code. `LoadFrame` read the base snapshot without waiting for the eviction job that `SaveFrame` had just kicked; under host load the job lost the race. On a loaded machine the old build hung in 25 of 25 replays of an `m-yoshi` log, and the fixed build (`001dd0b2df`) replayed its first 2,400 frames identically in 53 of 54 replays (the other was a harness timeout before the start).

## Harness commands

| Command | What it does |
|---|---|
| `frame_trace`, `frame_trace_config` | per-frame rows (frame counters, RNGs, fighters, game set, resim flag) |
| `game_pads` | record, inject and anchor the per-frame pad slots |
| `cpu_trace` | interpreter instruction trace (pc, op, ea, value), optional all threads, N occurrences |
| `mem_chunk_hashes`, `disasm`, `timing_nudge` | memory hashes by chunk, disassembly, emulated-time nudges |
| `gprb_synctest` | arm a sync test (`distance`, `region_set`, `hash_regions`, `start_frame`, `suppress_resim_sounds`, `dedupe_resim_sounds`; Phase 6: `inject_input`, `mispredict_ports`/`mispredict_offset`/`mispredict_every`, `replay_path`, `no_rollback`) |
| `gprb_connect` | network session (`role`, `port`, `host`, `remote_port`, `region_set`, `delay`, `start_frame`, sound options) |
| `gprb_set_selections`, `gprb_status`, `gprb_checksums`, `gprb_stop` | selections, status (phase, peer, `disconnected`, sound counters, save/load cost), confirmed checksums, leave |
| `mm_search_direct` / `mm_search` with `backend="gameplay"` | online hand-off to `Gprb::Session` |
| `gprb_samples` | chunk hashes (and watched bytes) of the region set every N confirmed frames |
| `gprb_census` | granules outside the set written during the match (`PPR_GPRB_CENSUS=1`) |
| `gprb_status` (Phase 9 fields) | `load_evict_waits` (loads that found the base snapshot's eviction job unfinished and waited for it), `load_evict_wait_us_max` |
| `gpu_state` | CP FIFO, PI FIFO, PE control, interrupt cause/mask, deterministic-GPU flag |
| `gprb_sound_state` | the sound archive player's allocated sounds (id, handle, owned/orphaned, duplicate ids) |
| `gprb_status` (Phase 14 field) | `sound_stops_ignored` (resimulated stops of sounds the re-run timeline had not started yet) |

Diagnostics through environment variables:
- `PPR_GPRB_DIFF_FRAME=N`: byte diff of the region set, first run against resimulation.
- `PPR_GPRB_RNG_LOG=1`: every `mtRand` call, with 8 stack levels.
- `PPR_GPRB_PROBE=addr,…`: registers (r3-r6, r12, ctr, lr) at those addresses, in every phase of a session (the load and the countdown too).
- `PPR_GPRB_INPUT_LOG`: per-pass input.
- `PPR_GPRB_PASS_LOG=path`: every GekkoNet update of a network session, for `replay_path` (`<path>.<host|join>.m<match>`).
- `PPR_GPRB_DUMP_FRAMES=f1,…` + `PPR_GPRB_DUMP_DIR=dir` (+ `PPR_GPRB_DUMP_FULL=1`): the region set (all of MEM1/MEM2) at every save of those session frames.
- `PPR_GPRB_CENSUS=1`: record non-set granules written during the match (`gprb_census`).
- `PPR_GPRB_INTERP_FROM=<game frame>`: JIT up to that frame, then the interpreter (and a break the harness resumes), for `cpu_trace` of late frames.
- `PPR_GPRB_FORCE_FINAL=<port mask>`: those ports get their Final Smash at the last countdown frame (Phase 8; the sweep's `ffs` group).
- `PPR_GPRB_EVICT_DELAY_US=N`: every eviction job (the oldest slot merged into the base snapshot) sleeps N µs first, as on a starved machine (Phase 9).
- `PPR_GPRB_EVICT_NO_WAIT=1`: loads do not wait for the eviction job (the behaviour before Phase 9, to reproduce the race).
- `PPR_GPRB_RESIM_AUDIO=advance`: the sound system runs through resimulated passes and their samples are dropped (the behaviour before Phase 14, for A/B runs).

## Commits (`dolphin-gprb`, branch `gameplay-rollback`)

| SHA | What |
|---|---|
| `008e2b663b` | frame-anchored pads, determinism tooling, region mode scaffolding |
| `3a3a9fb0a5` | merge `rollback-fixes` `a1f9ec2685` |
| `55a1f22d82` | region-mode sync test runs (pacing, countdown, stdio, XF) |
| `2605f5b123` | gp-v8, input from the frame |
| `fb111dc1ba` | session end at game set, resim sound counters, quieter per-frame logs |
| `a3b61add52` | GekkoNet handshake from the start barrier |
| `f8156f2735` | resim sound dedupe, next-match guard, setup key, RNG log, aux FIFO guard |
| `ee7a1273cb` | gp-v9 (camera manager), host's init block, register probes |
| `ecbd88b92e` | merge `rollback-fixes` `d36794a6e1` (online client); gameplay online backend |
| `fe35002ead` | session ends when GekkoNet drops a silent peer; gp-v9 by default |
| `e8c3bc2000` | never restore the GX FIFO ring tail (region-set `exclude`); gp-v10 (FIFO tail, slow manager) |
| `980673530a` | pass logs and replays, misprediction sync test, dumps, census, samples, `gpu_state`, `gprb_sound_state`; sound bookkeeping (re-attach, stop) |
| `fade07580e` | merge `rollback-fixes` `ad474c0358` (GameBridge, recent codes, Qt session backend); the PPOM mailbox is excluded from region sets |
| `e7b2076039` | gp-v11 (ground-collision list heads) as the default; sync-test `no_rollback` (ground truth) |
| `d31f69fc54` | sound bookkeeping (`dedupe_resim_sounds`) by default in network sessions |
| `b79fe057cf` | gp-v12 (camera quake controller) as the default; sync-test pass logs; `PPR_GPRB_INTERP_FROM` |
| (fast-forward) | `rollback-fixes` `c27636d256` (Phase 7, the online CSS path) |
| `5870549d59` | gp-v13: nw4r::ef's draw strategies are never restored (Family B) |
| `04d894ad28` | gp-v14: the camera subject list head |
| `6960df9843` | the file IO wait at the loop top (`RunIoWait`); gp-v15: the IO manager's queues and request pool |
| `54e2de9199` | gp-v16: the archive manager's list |
| `d8958f4ea2` | gp-v17: the scene's per-model records, whole |
| `fa8f2e39cf` | gp-v18: the controller configuration's per-port stick state |
| `fd0e89973e` | gp-v19: nw4r::ef's EffectSystem whole, the draw strategies excluded byte-precisely; `PPR_GPRB_FORCE_FINAL` |
| `0422682986` | match start before the stage is built (stMelee constructor hook); start points in the barrier's sync block; probes and RNG log in every phase |
| `e3f0451ed1` | merge `rollback-fixes` `4944245954` (`unranked`) |
| `e642b3ce98` | gp-v19 as the default (frozen as `run/bin/gprb-e642b3ce98`) |
| `7788a27738` | merge `rollback-fixes` `50e800b9d6` (online default hosts) |

## Commits (`dolphin/`, branch `rollback-fixes`, Phases 7 and 9)

| SHA | What |
|---|---|
| `df44299556` | merge `gameplay-rollback` (`b79fe057cf`) into `rollback-fixes` |
| `aedcf62a51` | the gameplay session as the default online backend (registered by both frontends); the lobby; PPOM v2 SESSION/LOCAL in GameBridge; IDLE after a disconnect; OSD DISCONNECTED |
| `da2cf9b595` | DolphinQt: Brawl Online main window title and About; no Project+ Discord link |
| `b882222f2b` | the render window's title starts with Brawl Online |
| `c27636d256` | PPOM v3: per-player port values through the lobby into SESSION, the applied layouts in the setup key; the OSD DISCONNECTED only as a fallback |
| `4944245954` | merge `unranked` (`6449517bc1`): the server's stage list with Slippi's stage pool for every random stage (Unranked, Direct's game 1); Direct's loser's pick is not restricted to the list; PPOM v3 kept |
| `001dd0b2df` | Phase 9: a load waits for the base snapshot's eviction job; job completion with release/acquire order, no write to a finished job; `PPR_GPRB_EVICT_DELAY_US`, `PPR_GPRB_EVICT_NO_WAIT`, `load_evict_waits` (frozen as `run/bin/gprb-001dd0b2df`) |
| `bd9ba09eb6` | Phase 10: `GPUDeterminismMode` defaults to fake-completion; an error when a session runs dual core without the deterministic GPU thread |
