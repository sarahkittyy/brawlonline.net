# Gameplay-only rollback: status

Branch `gameplay-rollback` in the worktree `dolphin-gprb/` (off `rollback-fixes`, merged with `rollback-fixes` at `a1f9ec2685`, at `d36794a6e1` (the online client) and at `ad474c0358` (GameBridge, recent codes, Qt session backend)). Not pushed. Head: `b79fe057cf`. **Default region set: gp-v12** (Phase 6). **Merged into `rollback-fixes`** (`df44299556`, 2026-10-07), where the gameplay session is now the default online backend and starts matches from the game's own online CSS (Phase 7).

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
   - Both create the GekkoNet game session (delay 2, prediction window 7) and complete its handshake.
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
2. **The lobby** (`Gprb::Session`, Slippi's `MATCH_SELECTIONS`): both peers send their lock-in (gmCharacterKind, costume, stage pick, the game number it is for, ready) in their control messages. When every player is locked in for the next game, the **host decides the setup**: P1 = the host, P2 = the joiner; the stage is the losing player's pick (Direct), else any pick, else random from the match's `stages` list (the server's list for the mode, P+ v3.2's legal list in `server/config/rulesets.json`; `Gprb::Session::DefaultStages()` if the server sends none). Since branch `unranked` (2026-10-07) the random stage comes from Slippi's stage pool (no stage again until the list is used up), and a pick outside the list is ignored. The joiner takes the host's setup. The winner of each game is read from the state both peers end on (more stocks, then less damage), so it is the same on both.
3. **SESSION** (written by GameBridge only outside matches, the same on both machines) gets the setup; each game leaves its CSS for the match, builds it with the Versus sequence's own setup from SESSION, and loads (`docs/game-code.md` §11).
4. **The match** starts exactly as in Phases 4-6: at the first pass through `scMelee` both seed the RNGs from the session seed and the match index and the joiner takes the host's init block; at the first simulation frame the barrier compares the setup hash and copies the host's frame counters, RNGs, serial counter and task order; the countdown runs without rollback; GekkoNet starts at frame 240 behind the start barrier; region mode gp-v12.
5. **Game set:** both end on the same frame, the game goes straight back to the online CSS (no results screen), the connection stays, `match_index` + 1. START locks in for the next game; Direct's loser picks the stage on P+'s stage select first.
6. **Disconnects** (design 5.6): a peer that leaves (hold Z) or goes silent (7.2 s) ends the session; LOCAL `disconnected` is set; in a match the game plays the error sound and ends the game through its own game set (the departed player dies on its last stock), then goes back to the CSS; GameBridge's next `GET_MATCH_STATE` cleans up and reads IDLE. A red OSD "DISCONNECTED" stands in for the HUD text.

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
2. The game-side gaps (`docs/game-code.md` §11): the CSS does not show the coin placed again after a match or the stage select (START locks in with the last character); DISCONNECTED is an OSD message, and the disconnect end shows Brawl's "GAME!"; every player has the default controls (no name tags, no port values yet).
3. Ranked and code-based Teams are not exercised (Unranked is since 2026-10-07: `harness/tests/test_online_unranked.py`, a two-game set from the in-game Unranked search); SESSION and the lobby are sized for 4 players, the session itself runs 2.

## Open issues

Resolved in Phase 6: the dual-core divergence (ground-collision list heads, gp-v11), the Peach article crash and the `GXWaitDrawDone` stalls (the GX FIFO ring tail), the 11-frame sync-test bursts (camera quake controller, gp-v12), stopping and re-attaching sounds.

1. **Sync tests must be checked against the ground truth.** Their own check (first run against resimulation) missed a fault that changed the game within seconds. `gprb_mispredict.py` with a recording, `--no-rollback`, and a comparison of the traces is the stronger test; only Mario/Marth FD and Ice Climbers/Olimar PS2 have recordings so far. The closed-loop scenario sync tests (`gprb_synctest.py`) cannot be compared this way.
2. **State outside the set that only rare events touch** is still found one case at a time. `gprb_memdiff.py scan` finds global list heads that point into the set; counters and flags need a census (`PPR_GPRB_CENSUS`) and a reason. Known and left alone: the task-id counter `gUnk8059c66c` (ids of objects created in resimulated frames differ between peers; no effect found).
3. **Sessions end with `peer timed out` when the machine is overloaded**: 4 of the 10 `s8` sessions ended between 237 and 13,457 frames, both peers on the same frame and with 0 mismatches, while 10+ other Dolphin instances ran. The GekkoNet silence limit (7.2 s at delay 2) was exceeded by stalls of a starved process, not by the network.
4. The rollback count in the sessions is low (random macros predict well). A harder input model would stress deeper rollbacks between peers; the misprediction test covers that on one instance.
5. ~~The online backend is not registered outside the harness.~~ Done in Phase 7: both frontends register it, and it is the default.
6. The coverage sweep (`gprb_sweep.py`, another agent's tool) still defaults to gp-v9, while the session and sync-test tools it uses now default to gp-v11.

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
| `gpu_state` | CP FIFO, PI FIFO, PE control, interrupt cause/mask, deterministic-GPU flag |
| `gprb_sound_state` | the sound archive player's allocated sounds (id, handle, owned/orphaned, duplicate ids) |

Diagnostics through environment variables:
- `PPR_GPRB_DIFF_FRAME=N`: byte diff of the region set, first run against resimulation.
- `PPR_GPRB_RNG_LOG=1`: every `mtRand` call, with 8 stack levels.
- `PPR_GPRB_PROBE=addr,…`: registers at those addresses.
- `PPR_GPRB_INPUT_LOG`: per-pass input.
- `PPR_GPRB_PASS_LOG=path`: every GekkoNet update of a network session, for `replay_path` (`<path>.<host|join>.m<match>`).
- `PPR_GPRB_DUMP_FRAMES=f1,…` + `PPR_GPRB_DUMP_DIR=dir` (+ `PPR_GPRB_DUMP_FULL=1`): the region set (all of MEM1/MEM2) at every save of those session frames.
- `PPR_GPRB_CENSUS=1`: record non-set granules written during the match (`gprb_census`).
- `PPR_GPRB_INTERP_FROM=<game frame>`: JIT up to that frame, then the interpreter (and a break the harness resumes), for `cpu_trace` of late frames.

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

## Commits (`dolphin/`, branch `rollback-fixes`, Phase 7)

| SHA | What |
|---|---|
| `df44299556` | merge `gameplay-rollback` (`b79fe057cf`) into `rollback-fixes` |
| `aedcf62a51` | the gameplay session as the default online backend (registered by both frontends); the lobby; PPOM v2 SESSION/LOCAL in GameBridge; IDLE after a disconnect; OSD DISCONNECTED |
| `da2cf9b595` | DolphinQt: Brawl Online main window title and About; no Project+ Discord link |
| `b882222f2b` | the render window's title starts with Brawl Online |
