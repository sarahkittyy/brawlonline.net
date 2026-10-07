# Gameplay-only rollback: status

Branch `gameplay-rollback` in the worktree `dolphin-gprb/` (off `rollback-fixes`, merged with `rollback-fixes` at `a1f9ec2685`). Not pushed.

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
