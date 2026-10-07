# Gameplay-only rollback feasibility (Slippi-style session start)

Spike run 2026-10-06 on the frozen harness build `run/bin/harness-100b8fd189` (P+ v3.2, NTSC-U Rev 1, Offline Launcher, Windows 11). Tool: `harness/tools/gameplay_rollback.py`. Raw data: `run/qa/gprb/` (not in git).

**The session model under test.** Two players boot independently and use the menus independently. They connect at the CSS, exchange selections, and start the same match. Only gameplay state is saved and restored during the match.

**The questions:**
1. Can Brawl's match simulation be identical on two machines whose menu histories differ?
2. Which memory is "gameplay state"?

## Verdict

**Feasible with conditions.**

- **Single core, after syncing the RNGs at match start:** gameplay was bit-identical in every observable we compare (per-player position, damage, stocks, action, motion, animation frame, facing; the RNG seeds) for ~140 s of Peach vs Game & Watch on Pokémon Stadium 2. The second scenario (Fox vs Falco on Battlefield, B with a *different RTC*) was identical for the whole match.
- **Then a rare divergence appears** that this spike could not pin down (condition 2).
- **Different heap contents do not prevent that.** Both machines get the same heap table, the same REL load addresses, and the same block layout in every per-match heap.
- **A memory-only restore at a frame boundary reproduces the simulation**, measured on one instance, as long as the restored set is large enough. The minimum set we validated is ~45 MB of address space, of which ~0.5 MB actually changes in a 600-frame window.

**Conditions** (all are engineering work except condition 2, which still needs research):

1. **Sync the RNGs at match start.** Also sync the start of the simulation itself: the load before the first frame takes a different number of fields on each machine.
2. **Find and remove a late divergence that appears without any rollback.** It shows up after ~141 s in the long runs. Inputs, uninitialised memory and object serial numbers are ruled out. The leading hypothesis is the emulated-time phase of interrupt- or audio-driven code, such as P+'s code handler on AXNextFrame. See [Experiment 2, late divergence](#late-divergence-at-frame-8454).
3. **Restore more than "gameplay heaps".** The set must include:
   - the fighter and stage *resource* heaps (they hold live NW4R G3D model objects);
   - REL .data/.bss;
   - a few DOL globals.

   It must **not** restore OS, pad or audio state partially. Doing that hangs the game or breaks the input pipeline.
4. **Dual core needs separate work.** Two of eleven dual-core runs diverged in gameplay with identical inputs. One of those two had identical menu histories, so this is a dual-core problem, not a session-start problem. Six of eleven runs crashed under the harness. This repeats `docs/determinism-findings.md`.

Details and numbers follow.

## Method

### Setup

- Single core, fixed RTC (`Dolphin.Core.EnableCustomRTC`, value 0x67748580), video backend `Null`, muted.
- Two independent instances, offline, no netplay. The template forces `CPUThread = True` in `GameSettings/RSBE01.ini`; the tool rewrites it per run.
- Savestates are **only** test fixtures:
  - prep saves the start of each run;
  - one run saves frame 8420 for analysis.

  No rollback in this study uses them.

**Scenario 1: `run/qa/gprb/ps2`**

- P1 Peach, P2 Game & Watch, Pokémon Stadium 2 (it transforms), items off, 4 stocks, 8 minutes.
- These characters were chosen because they use RNG: turnip pulls, Judge, Oil Panic.

**Scenario 2: `run/qa/gprb/bf`**

- Fox vs Falco on Battlefield.
- B runs with a different custom RTC (0x69C9A3D0 against 0x67748580), as two real machines would.

### Menu histories

| Instance | History | Reaches the first scMelee field at |
|---|---|---|
| A | Boot straight to the CSS (P+ BootToCSS). P1 picks, then P2 picks. Start, pick the stage. | VI 1430 (PS2), 1316 (BF) |
| B | Boot to the CSS. Idle 137 frames. P1 picks Mario, P2 picks Ike, both drop the tokens. P1 picks Kirby and drops it. Hold B to back out to the main menu. Open Rules and close it. FIGHT! back to the CSS. Idle 53 frames. P2 picks first, then P1. Start. Linger 77 frames on the SSS. Pick the stage. | VI 2469 (PS2), 2355 (BF) |

### Two anchors for "match frame 0"

1. **`0`: the first VI field whose current scene is `scMelee`.**
   - The task asked for this one.
   - Here every per-match heap exists but is empty.
2. **`s`: the first field of the match simulation.**
   - Here `g_GameFrame.frameCounter` (0x901812A4) has been reset for the match and reads 1.
   - The load between the two anchors takes a different number of fields on each machine:

     | Scenario | A | B |
     |---|---|---|
     | PS2 | 36 fields | 41 fields |
     | BF | 41 fields | 50 fields |

   - The persistent frame counter and the gfApplication counter differ by the same amount.
   - From `s` on, both machines reach GO after 215 frames.
   - **So `s` is the anchor where two machines line up, and the rollback session has to start there**, behind a barrier like Slippi's start sync.

### Tool commands

All commands are `harness/tools/gameplay_rollback.py <command>`.

| Command | What it does |
|---|---|
| `prep` | Runs A and B in parallel. Each stops on the first scMelee field: dumps MEM1/MEM2 (zlib), the heap table, the REL module list (from the OS module list at 0x800030C8) and a savestate. Then steps field by field to GO, logging the counters, and dumps again. |
| `simstart` | Loads the frame-0 savestates and steps each instance to `s`. Dumps and saves state there. |
| `diff` | Compares A and B per memory region at `0`, `s` and GO. |
| `play` | **Experiments 2 and 4.** Loads A and B at `s` (or at `0`) and writes the sync values into both. Then steps both one game frame at a time and compares the observables every frame. |
| `synctest` | **Experiment 3.** One instance, region save and restore. |
| `replay` | **Experiment 4.** Dual core: the same input with few pauses. |

**How `diff` labels memory:**
- The region map labels every byte: DOL sections from the decomp's `symbols.txt`; REL sections from the module list (they are carved out of the heap they live in); every heap from `g_HeapInfos`.
- For every heap it also walks the allocator, which we reverse engineered from the dumps:
  - each block has a 0x20-byte header: `{+0 pool, +4 size incl. header, +8 next free}`;
  - `pool+0x20` points at a sentinel;
  - the free list starts at the sentinel's `+8`;
  - blocks are contiguous;
  - allocations come from the top.

  Every differing byte is therefore classified as **in an allocated block** or **in free memory**. It also checks whether the block layouts of A and B are identical.

**How `play` drives input:**
- Input is closed loop: `flows.Fighter` "random" macros, plus side-B, down-B, up-B, grabs and aerials. Decisions are made from instance A's state.
- **The same `pad_script` goes to both instances at the same relative poll while both are paused.** So the inputs are identical even after the states diverge.
- Every 300 frames it also hashes `brawl.gameplay_ranges()`.

**How `synctest` works:**
1. Load A at `s` and play N frames with recorded or scripted input.
2. Pause on a VI field boundary at game frame G. Save the region set with `read_mem` (the whole of MEM1/MEM2 is dumped, for diagnosis).
3. Play K game frames of scripted input, then dump again.
4. Restore the region set, writing only the bytes that changed. That equals a full copy and keeps write_mem's JIT invalidation away from unchanged code.
5. Replay the same K frames. Input is scheduled when the game is at G+2 on both passes, keyed on the game's own frame counter.
6. Compare at G+K:
   - the observables;
   - every region inside the set;
   - every region outside it;
   - which memory outside the set was written during the window.

**CPU state in `synctest`.** The harness cannot restore registers. The save and restore both happen paused on a VI field boundary in the same phase of the frame. On both passes the next field took the same number of fields to advance the game frame. So the CPU context is equivalent in practice. This mirrors Slippi's trick of always saving and loading at the same code location. The real implementation should save and load at the frame hook 0x80017504 / 0x8001727C, as Brawlback does.

## Experiment 1: what differs at match start

### Equal on both machines (both scenarios, all three anchors)

- **The heap table** (`g_HeapInfos`, 50 heaps): same names, start and end. The per-scene memory layout does not depend on history.
- **The REL modules:** 15 modules with identical load addresses (PS2):
  - `sora_scene`, `sora_melee` (0x8070A940);
  - five menu modules that P+ keeps resident: `sora_menu_name`, `sel_char_access`, `sel_char`, `rule`, `sel_stage`;
  - `ft_peach`, `ft_gamewatch`, `st_*`;
  - five Syringe plugin modules in the `Syringe` heap.
- **The block layout of every per-match heap.**
  - Instance heaps: Fighter1-4Instance, ItemInstance, StageInstance, Physics, InfoInstance, FighterTechqniq.
  - Resource heaps: Fighter*Resoruce(2), StageResoruce, IteamResource.
  - At `0` all of them are empty (one free block).
  - At `s` and GO they hold the same blocks at the same addresses on A and B: 121 blocks in ItemInstance, 356 in StageInstance, 1381 in InfoInstance.
- **Two heap layouts that changed mid-match under different seeds:** AssistFigureResource (15 against 8 blocks at GO) and PokemonResource (43 against 41). See Experiment 2.

### Differences

Scenario PS2, no sync yet. Bytes that differ between A and B:

| Region | Size | @0 | @s | @GO | of which in allocated blocks (0 / s / GO) | Same block layout |
|---|---|---|---|---|---|---|
| dol.bss | 1.03 MB | 21,232 | 25,405 | 27,257 | n/a | n/a |
| dol.sbss / .sdata | 5 KB / 15 KB | 40 / 15 | 46 / 18 | 34 / 18 | n/a | n/a |
| main (boot) stack | 64 KB | 1,939 | 2,101 | 774 | n/a | n/a |
| System FW (gfPadSystem, OS objects) | 84 KB | 3,907 | 5,176 | 5,327 | all | yes |
| **System** (ftManager, ftEntryManager, managers) | 389 KB | 728 | 3,927 | 3,940 | 351 / 3,480 / 3,493 | **no** |
| Effect | 635 KB | 6 | 6 | 98 | all | yes, then no at GO |
| GameGlobal | 201 KB | 12 | 24 | 27 | all | yes |
| GlobalMode | 236 KB | 52,942 | 52,970 | 52,970 | 1,854 / 1,882 / 1,882 | yes |
| Fighter1/2Instance | 328 KB each | 0 | 0 | 58 / 54 | all | yes |
| ItemInstance | 1.3 MB | 253,188 | 165,556 | 165,556 | 0 / 30,904 / 30,904 | yes |
| StageInstance | 512 KB | 99,592 | 68,451 | 65,745 | 0 / 29,745 / 27,039 | yes |
| Physics | 717 KB | 71,862 | 71,863 | 71,863 | 0 / 1 / 1 | yes |
| InfoInstance (HUD) | 1.2 MB | 0 | 2 | 95 | all | yes |
| StageResoruce | 6.4 MB | 1,537,126 | 529,200 | 649,585 | 0 / 176,703 / 297,079 | yes, then no at GO |
| Fighter3/4Resoruce(2), FighterTechqniq, PokemonResource, OverlayMenu | ~14 MB | ~7.6 MB | same | same | 0 | yes |
| RenderFifo, Sound, Replay | | 208 K / 284 K / 0 | | 222 K / 431 K / 2 K | | yes |
| sora_melee/sora_scene .bss | 36 KB / 7 KB | 0 / 3 | 2 / 8 | 3 / 8 | n/a | n/a |
| dol.text, sora_melee.text, sel_char.text | | 2 | 2 | 6 + 4 + 4 | n/a | n/a |
| WiiPad, Fighter3/4Instance | | 0 | 0 | 0 | | yes |

Totals at `s`: 9.0 MB of 68 MB of heap address space differs. 8.8 MB of that is in free blocks or in heaps the match never uses. Also 27.6 KB of DOL statics and 10 bytes of REL statics.

### What the differences are

**1. Garbage in free memory (the bulk, about 9 MB).**
- Every per-match heap is carved over memory that the menus used. The bytes left in its free blocks are whatever the menus left there: CSS portraits, Mario, Ike and Kirby assets, and so on.
- A's untouched regions show the 0xCC debug fill where B's show leftover data.
- Harmless unless something reads memory it did not write (see 2 and 4).

**2. Uninitialised bytes inside allocated blocks (about 61 KB at `s`).**
- **ItemInstance and StageInstance** hold the same objects at the same addresses on both machines. 30.9 KB and 29.7 KB of their bytes still differ.
- Most of these are fields the constructors never write: CCCCCCCC on A, leftovers on B.
- The rest are **per-object serial numbers**. For example the word at `+0x48` of each 0x3D80-byte item-instance object reads 0x8E…0x90 on A and 0xA4…0xA6 on B. These come from a global creation counter that B advanced further on the menus (by 0x16).
- **StageResoruce:** the 4.5 MB PS2 archive block has a 176 KB hole that the loader never writes (+0x2A47E0…+0x2CFBA0). It holds 0xCC on A and leftover data on B.

**3. Allocation addresses differ in one heap, System.**
- B's menu trip left System fragmented: a 512-byte free block at 0x80615260.
- Every System allocation made for the match therefore lands 0x200 lower on B.
- Pointers to them differ by -0x200 in sora_melee.bss (0x80B8A540, 0x80B8A768), GlobalMode, and the ItemInstance and StageInstance objects that point into System.
- All other heaps match block for block. The match simulation stayed identical despite this (Experiment 2). Brawl does not appear to order anything by address in the paths we exercised.

**4. RNG state.**

| Variable | Address | A / B |
|---|---|---|
| `g_mtRand` seed | 0x805A00BC | differ at every anchor |
| `g_mtRandOther` seed | 0x805A0424 | differ at every anchor |
| libc `rand()` state `next` (`lwz/stw -0x44C8(r13)` in `rand`/`srand`) | 0x8059FF58 | see below |

- `g_mtPrngLogManager` (0x804977B4) is empty (zero) on both.
- The menus advance `g_mtRand`.
- libc `rand` is seeded from the clock:
  - with the same RTC it was equal at `0` (6836565) and was re-seeded during the scMelee load (256252672 on A);
  - with B on a different RTC (scenario BF) it already differed at `0`.
- **The RNG also chooses what gets loaded during the match.** With items off, the countdown still loads Assist Trophy and Pokéball Pokémon resources. With different seeds, A and B loaded different ones: different block layouts in AssistFigureResource and PokemonResource at GO. With the seeds synced, those heaps matched.

**5. Frame counters and timers.**
- `g_GameFrame.persistentFrameCounter` (0x901812B4) and `gfApplication+0x100` count from boot, so they differ by the menu time.
- `__OSStartTime` (0x800030D8) differed by 6 ticks between two boots started together.
- Timebase timestamps in dol.bss and .sbss differ.

**6. Menu leftovers in globals.**

| Where | A | B | Notes |
|---|---|---|---|
| dol.data 0x80493EC8 / 0x80493EE4 | 0xC8E1 | 0x110D2 | Counters. |
| .sdata 0x8059C668, 0x8059CE48, 0x8059E900-0x8059E978 | | | Small counters and flags. |
| .sbss 0x805A01D4 … 0x805A0D28 | | | Counters, pointers to OS alarm objects (0x805B85E0 against 0x805BF420), audio buffer pointers. |
| sora_scene.bss 0x80708CBC | 0 | 0x90181300 (`g_GameGlobal`) | |
| GlobalMode, allocated | 1.9 KB | | Menu-mode state. |
| GameGlobal | 12-27 bytes | | |

**7. P+ code memory differs.**

| Address | A | B | When |
|---|---|---|---|
| 0x800B9F84 | `mtlr r0` (original) | 0x7C080000 | Whole match. The epilogue of a function just before `fn_800B9F90`. Something on B's menu path stored a zero halfword into code. B did not crash, so the function is not executed in a match. |
| 0x800E2188 | `nop` | `bl` | At GO. Converges later. |
| sora_melee.text 0x8076C988 / 0x8076C9A4 | Gecko hook branches | Gecko hook branches | Targets differ at GO. Converges later. |
| sel_char.text 0x8069B868 | Gecko hook branch | Gecko hook branch | Targets differ at GO. Converges later. |

- P+'s code handler runs on the AXNextFrame (audio) hook, not on the game frame (research 05 §3.14). So its patches land at an audio-frame phase that differs between machines.
- **A rollback session must hash the code sections at match start and compare them between peers.**
- Separately, sora_melee.text 0x807824D4 is a **variable embedded in P+ code** that changes during play (1 → 7). See Experiment 3.

**8. Render, audio and OS state.**
- RenderFifo, Sound, Replay, the main stack, SystemFW and the AX areas of dol.bss differ at every anchor.
- They never reach gameplay.

## Experiment 2: does it matter for gameplay?

`play`, single core, PS2 scenario. Both instances load their own state at `s`. Per-frame comparison of the observables, closed-loop input. 4,000 frames is 66.7 s (the first 215 frames are the countdown); the long runs are 9,000 frames (150 s).

| Sync written into both at `s` | Frames | First difference |
|---|---|---|
| none | 4,000 | RNG at frame 1. G&W's motion kind (an idle animation pick) at 443. Damage, action and position at 3,913. |
| RNG (`g_mtRand`, `g_mtRandOther`, libc `next`, all taken from A) | 4,000 | Player state and RNG identical every frame. Only the boot-relative counters (persistent counter, gfApplication) differ. |
| RNG + frame counters (also `g_GameFrame` 0x18 bytes and `gfApplication+0x100`) | 4,000 | **Nothing**: every observable equal every frame. |
| RNG + frames, 9,000 frames | 9,000 | **P2's x at frame 8,454** (-64.068 against -64.128, action 4 against 5). RNG from 8,461. Same frame in four separate runs. |
| RNG + frames + **scrub** (A's free-block bytes copied into B in every per-match heap, and 0xCC wherever A still has the fill inside allocated blocks: 9.1 MB) | 8,484 | **Same: P2's x at 8,454.** |
| RNG + frames written at `0` (before the stage and fighters are created), then each instance steps to `s` | 9,000 | Player x at 8,549 (a different game, because the RNG state at `s` differs from the previous rows). |
| RNG + frames + **object serial counter** (.sdata 0x8059C668), all written at `0` | 8,579 | **Same: player x at 8,549** (P1 -77.199 against -77.299). The serial numbers were equal on both, checked at the end. |
| A against A (control) | 900 | Nothing. |

Scenario BF (Fox vs Falco on Battlefield, B with a different RTC; the match ended at frame 2,850):

| Sync | Result |
|---|---|
| none | RNG different from frame 1. **Player state identical for the whole match**: Fox and Falco draw little randomness in this play. |
| RNG | Identical for the whole match. |
| RNG + frames | Identical. The run hit the tool's time limit at frame ~1,300 because the machine was heavily loaded. |

**What this shows:**
- **Menu-history differences in heap contents, allocation addresses (System) and garbage do not by themselves change the simulation**: 4,000+ frames were identical once the RNGs were equal.
- **The RNGs are the only start-of-match sync the simulation needed in these runs.**
- **The frame counters matter only for checksums.** They are hashed by Brawlback's desync check and by `gameplay_ranges`.

### Late divergence at frame 8454

The long runs diverge after ~141 s even though nothing was rolled back:
- the RNGs were synced;
- the inputs were identical.

It happens whether the sync is written at `s` or at `0`.

**Eliminated, each with a dedicated run:**

| Candidate | How it was eliminated |
|---|---|
| Inputs | `harness/tools/gprb_late_divergence.py` loads both instances from savestates taken at frame 8,420 in the lockstep run and re-issues the recorded inputs. It reads the `gfPadStatus` slots the game consumed (0x805BAD00, 0x80 bytes) every frame: **identical on A and B through the divergence**. Input polls and game frames stay in lockstep (1:1) on both. |
| Uninitialised memory and leftover garbage | The scrub variant made all free memory and all fill-marked allocated words of the per-match heaps identical at `s`. **Same divergence, same frame.** |
| Object serial numbers | The .sdata counter 0x8059C668 hands every new item, stage or fighter object a serial number: A 0x37 / B 0x4D at `0`, because B's menus created 22 more objects. The only gameplay-heap granules that newly differ before the divergence (StageInstance 0x8154AB00 at 8,426, Fighter1Instance 0x812876CC at 8,440) are such serials (0x9C/0xB2, 0x92/0xA8). Syncing the counter at `0` made the serials equal on both. **Same divergence.** |
| The RNGs and gmGlobalModeMelee | Equal on both. |

**What the frame-by-frame probe shows** (`play --probe-from`, and the analysis script from the 8,420 fixtures):
- Before the divergence, nothing new differs in any gameplay heap, resource heap or REL static except those serials.
- The DOL statics only churn in the audio/AX and OS areas (0x804CA000-0x804CF000, 0x804DE5E0, 0x804E4000-0x804E7800, .sbss 0x805A07C0/0x805A09E0).
- Then, in a single frame, a fighter's position (one dash step: 0.06 and 0.1 units in the two games) and every object derived from it differ: System, Fighter2Instance, StageInstance, InfoInstance, Fighter2Resoruce.

**So no memory precursor was found inside what we probe.** The remaining differences between A and B are:
- (a) where System's match objects sit (0x200 lower on B);
- (b) the emulated-time phase. A and B reach the match after different amounts of emulated time, so the audio/AX interrupts, OS alarms and the P+ Gecko code handler (hooked on AXNextFrame, research 05 §3.14) run at different points relative to the game frame on the two machines;
- (c) audio state.

**Leading hypothesis: (b).**
- Something gameplay-relevant runs from an interrupt- or audio-cadence context: a P+ code in the code handler, or a sound callback that touches fighter state.
- Its effect lands one frame apart on the two machines when a frame boundary and an audio frame nearly coincide.
- This would also explain why it is rare (once in ~8,500 frames) and lands at a seemingly random moment.
- (a) remains possible if anything iterates System objects in address order.

**Next steps (C++/asm, not doable with the harness):**
- **Test (b) directly:** start B's match after shifting its emulated time by a fraction of a frame. Run the same seed many times and check whether the divergence frame moves with the phase.
- **If (b) holds:** move the P+ code handler hook to the game frame for online play (research 05 recommends this anyway), and audit P+ codes and sound callbacks that write fighter state.
- **Test (a):** compact System at match start (allocate a dummy on A, or fill B's hole) so both machines get the same addresses.

Until this is fixed, gameplay-only rollback desyncs in roughly one 2-3 minute match in a few. The desync checksum detects it, but cannot repair it.

## Experiment 3: which memory is "gameplay state"

`synctest`, one instance, PS2. N = 600 or 2,400 (the first 2,400 frames replay a recorded closed-loop game). K = 240-900. "Equal" means every observable is equal at G+K: RNG, per-player state, elapsed time and remaining time.

| Region set | Address space | Bytes restored | K | Result |
|---|---|---|---|---|
| `all`: MEM1+MEM2 minus Brawlback's AX/XFB exclusions and the disc ID | 91.0 MB | 0.89 / 1.11 MB | 240 / 600 | **Equal.** Only RenderFifo, Sound, AX state in dol.bss, Replay, stack and SystemFW bytes differ afterwards. |
| `all-av`: `all` minus Sound, RenderFifo, CopyFB, MEM2 below the first heap (XFB) | 75.9 MB | 0.50 MB | 600 | **Equal.** Remaining differences: audio (AX blocks after 0x8049B8EA, SystemFW, the stack). |
| `all-sys`: `all-av` minus lowmem, the boot stack, SystemFW, Thread, Network, Replay, Tmp | 74.0 MB | 0.33 MB | 240 | **Broken.** P1 ends up on the respawn platform. Most likely cause: restoring DOL OS state (alarm queue, pad alarm pointers) without the objects in SystemFW breaks the pad pipeline (see the DOL bisection below). |
| `all-sys-dol`: also without main.dol .data/.bss/.sdata/.sbss | 72.4 MB | 0.28 MB | 240 | RNG differs (it lives in .sbss). Players equal. |
| `heaps`: the gameplay heaps only (System, Effect, Fighter1-4Instance, FighterTechqniq, Stage/Item/Weapon/EnemyInstance, Physics, InfoInstance, GameGlobal, GlobalMode, WiiPad, FighterEffect) | 8.0 MB | 0.16 MB | 240 | **Game hangs.** |
| `candidate`: `heaps` + REL data/bss + all DOL writable sections | 10.5 MB | 0.21 MB | 240 | **Broken**, as `all-sys`. |
| `heaps` + REL data/bss + RNG | 8.9 MB | 0.16 MB / 0.29 MB | 240 / 600 | Equal at 240. **Hangs at 600.** |
| `all-av-res`: `all-av` minus resource heaps and code | 25.6 MB | 0.37 MB | 600 | **Hangs.** In the window, 60 KB of Fighter1Resoruce and 35 KB of Fighter2Resoruce were written, plus 4 bytes of sora_melee.text. |
| `all-av-res` + resource heaps (+ the code word) | 61.8 MB | 0.49 MB | 600, 900 @2400 | **Equal**, with and without the code word. |
| **`gp-v1`**: `heaps` + REL data/bss + resource heaps + RNG + 4 DOL clusters (below) + code word | **45.1 MB** | 0.42 / 0.51 MB | 600 / 900 @2400 | **Equal.** Remaining in-set differences after K=900: InfoInstance 851 B, WiiPad 98, System 46, GameGlobal 4, sora_melee.bss 3, Fighter1/2Instance 2+3. Analysed below. |

Bisecting the DOL bytes written during play, with `gp-v1` minus the DOL clusters as the base:

| DOL bytes written during play | Result |
|---|---|
| 0x80420680-0x804B0000 (55 granules) | Helps. Effect divergence 44.7 KB → 4.5 KB; Fighter2Instance 144 → 31 B. |
| — of which 0x80493EC0-0x80494FC0 | Fighter2Instance 144 → 31 B. 0x80494F30… are per-player structs: the ones Brawlback's checksum calls "velocity", holding a pointer into the fighter heap plus floats. |
| — of which 0x8049E000-0x804B0000 | Effect 44.7 KB → 4.5 KB. Floats and matrices: camera and effect globals. |
| — of which 0x80499000-0x8049E000 | No effect. AX/audio. |
| 0x804B0000-0x804E0000 | **Hang.** OS: alarm queue (pointers to 0x801D6xxx `OSAlarm` code), threads, timebase. |
| 0x804E0000-0x804F0000 | **Hang** after 110 frames. Audio buffers. |
| 0x804F0000-0x8059C420 | **Broken**: players diverge. Contains a pointer to `gfPadSystem` 0x805BACC0: pad and IO state. |
| .sdata/.sbss except the RNG | No effect. |

### What the residual differences in `gp-v1` are (K=900 at N=2400)

| Where | Values | What it is |
|---|---|---|
| Fighter2Instance 0x812C2E88/0x812C9988, System 0x8066193C | 0x800000xx | Handle IDs with a running counter (N 0x12 → pass 1 0x16 → pass 2 0x1D). Sound or effect handles numbered by a counter outside the set. This is the Slippi "sound handle" problem; it needs Slippi-style SFX bookkeeping. |
| ftOwner data +0x848 (P1, float 8 → 9) and +0x850 (P2); gmResultInfo (GameGlobal 0x9017F614…) | | Match statistics: the same values appear in the results block. Updated from something outside the set. Results screen only, as far as we can tell. **Unverified.** |
| InfoInstance | A 0x300-byte block allocated on the second pass only (heap top 0x7B020 → 0x7AD20); a System object (vtable 0x80466318) points at it | A HUD/G3D allocation triggered from outside the set. |
| WiiPad | Ring-buffer indices and pointers of the pad queue | Input plumbing, not simulation. |

### Candidate gameplay region set (`gp-v1`, `harness/tools/gprb-sets/gp-v1.json`)

Addresses are from the PS2 match. Heaps are resolved at runtime from `g_HeapInfos`; their positions are fixed for a Versus scene and were identical on both machines and in both scenarios.

**Include:**

| What | Address, size | Why |
|---|---|---|
| System | 0x80611F60, 0x61500 | ftManager, ftEntryManager, item/effect/stage managers. |
| Effect, FighterEffect | 0x80B8DB60, 0x9ED00; 0x914C9F00, 0x100 | Fighters and items hold effect handles. Effect state also diverged in the synctest whenever its DOL globals were missing. |
| Fighter1-4Instance | 0x8123AB60 + i·0x52000 | Fighters. |
| ItemInstance | 0x81382B60, 0x14B900 | |
| StageInstance | 0x814CE460, 0x80100 | |
| Physics | 0x8154E560, 0xB3400 | |
| InfoInstance | 0x81601960, 0x133400 | HUD, but allocated by gameplay. |
| FighterTechqniq | 0x92CABB00, 0x119B00 | |
| GameGlobal | 0x90167400, 0x32400 | `g_GameFrame`, gmGlobalModeMelee, gmResultInfo. |
| GlobalMode | 0x90FBAD00, 0x3AF00 | The scMelee object, stOperatorRuleMelee. |
| WiiPad | 0x90E61400, 0x16100 | Pad status the game reads. Keep it, then let the input layer overwrite the pad slots per frame. |
| **Resource heaps:** Fighter1-4Resoruce(2), StageResoruce, IteamResource, PokemonResource, AssistFigureResource, ItemExtraResource | ~36 MB of address space | **Not read-only.** Fighter resource heaps hold live objects: vtables 0x804663C8 and 0x8042B6C8, next to the NW4R G3D vtable 0x804662E8 named in the determinism doc, so most likely model and animation instances. About 100 KB were written in 600 frames. Excluding them hangs the game. Dirty-page tracking keeps the cost to what changes. |
| REL .data/.bss of every loaded module | sora_melee 0x80AD6854…0x80B8DAA8 (0xA5680 data + 0x8CE8 bss); sora_scene; resident P+ menu modules; ft_*; st_*; Syringe modules | Module statics. Their addresses come from the OS module list. |
| RNG | 0x805A00B8 (8), 0x805A0420 (8), 0x8059FF58 (4), 0x804977B4 (0xC, PRNG log) | |
| DOL gameplay globals | 0x80493EC0 (0x40), 0x80494F20 (0x40), 0x80494FA0 (0x20), 0x8049E020-0x804AFAA0 (21 granules: 0x8049E020, 0x8049EDE0, 0x804A3DE0-0x804A4000, 0x804A4580, 0x804A4680, 0x804A56C0-0x804A5760, 0x804A7F40, 0x804A9A80 (0xB20), 0x804AA640, 0x804AA6E0, 0x804AA740 (0x2E0), 0x804ADB20 (0x140), 0x804ADC80 (0x100), 0x804AFA80) | Per-player structs, camera, effect globals. **Found empirically**: these are the granules written in 600 frames that improved the replay. The list is a lower bound, because a granule that only changes in rarer situations was not observed. |
| P+ code-embedded variables | 0x807824D4 (4) in sora_melee.text | P+ Gecko codes keep state inside code. The full list needs the P+ codeset; see unknowns. |

**Exclude:**
- Sound, RenderFifo, CopyFB, MEM2 below GameGlobal (XFB/EFB copies).
- AX state in dol.bss: 0x804E7C00+0xC00 and 0x8049A4EA up to about **0x8049E000**. Brawlback's exclusion stops at 0x8049B8EA; the AX parameter blocks extend further.
- Thread, Network, Replay, Tmp, MeleeFont, CommonResource, InfoResource, StockResource, MenuInstance/OverlayMenu.
- All `.text` and `.rodata`, except the code-embedded variables.

**Needs special handling:**
- **OS state** (lowmem, the boot stack, SystemFW, the OS parts of dol.bss): restore all of it or none of it.
  - None: the frame-boundary save/load point keeps threads and alarms consistent. This is Slippi's approach and what `gp-v1` does.
  - All: Brawlback's current fork does this with a full `State::SaveToBuffer` plus dirty granules (`all`/`all-av` above).
  - Partial restores hung or broke input in every variant we tried.
- **Sound handles** in fighter and System objects: Slippi-style SFX bookkeeping.
- **Pad state:** the input layer must write each frame's pad slots (`gfPadStatus` 0x805BAD00 + 0x40·port) after a load, as Brawlback does.
- **Mid-match loads:** Assist Trophy and Pokémon resources are loaded during the countdown even with items off, and transformations on some stages. The RNG choice must be synced, and the load must never straddle a rollback window (preload, or synchronous loads).

**Size.** `gp-v1` is 45 MB of address space. In a 600-frame window 0.42 MB changed. With 64-byte dirty granules (Brawlback's `DeltaSaveSlot`), a per-frame snapshot is the per-frame dirty set, a few hundred KB. A full copy of the set would be 45 MB, which is too slow per frame.

## What must be synced at match start

1. **The start of the simulation.** Rollback frame 0 is the field where `g_GameFrame.frameCounter` is reset and reads 1, not the first scMelee field. The load before it takes a different number of fields on each machine (36 against 41, 41 against 50 in our runs). Both peers must wait at this barrier.
2. **The RNG state:**
   - `g_mtRand` seed 0x805A00BC;
   - `g_mtRandOther` seed 0x805A0424;
   - libc `rand` state 0x8059FF58.

   Write the agreed values before the match loads anything RNG-dependent. Brawlback's HLE hooks on `srandi` 0x8003FB4C and `srand` 0x803F8C5C do this whenever the game seeds. In our runs, writing at `0` or at `s` both worked for 140 s; writing at `0` is safer because the countdown already loads RNG-chosen resources.
3. **The frame counters**, for checksum parity only: `g_GameFrame` 0x901812A0 (0x18 bytes, including `persistentFrameCounter` 0x901812B4) and `gfApplication+0x100`. The simulation did not need them.
4. **The match setup.** gmGlobalModeMelee (characters, costumes, stage, rules) comes from the CSS exchange. It was identical here because both players picked the same. In the product, write it from the session and verify its hash on both peers before `s`.
5. **Not syncable by writing, so verify at match start:**
   - a hash of DOL and REL `.text`, minus the known code-embedded variables, against P+ code drift (difference 7);
   - the heap table and module list;
   - the block layout of the per-match heaps.
6. **Cheap hygiene:** the object serial counter (.sdata 0x8059C668). It did not cause the late divergence, but it is a menu-history difference that is stored into every match object.

## Experiment 4: dual core

`replay`: the same recorded closed-loop input on two instances with `CPUThread = True` and GPUDeterminismMode `fake-completion` (P+'s INI). States compared every 300/600 frames. Few pauses, because frequent `pause`/`frame_advance` crashed dual core before.

| Pair | Runs | Result |
|---|---|---|
| A / B | 7 | 3 identical to the end (frame 3,600-3,900, and identical to the single-core reference). **1 diverged by frame 600** (B differs from both A and the single-core reference: x off by 0.6, y by 1.7). 3 crashed after 1-8 checkpoints (`DolphinNoGUI` exits, 0xC0000005, with no difference before). |
| A / A (same history) | 4 | 1 identical to the end. **1 diverged by frame 600** (and then crashed). 2 crashed with no difference before. |

**What this shows:**
- **Dual core diverges in gameplay with identical histories as well.** It is the dual-core nondeterminism already described in `docs/determinism-findings.md` (cause 3), not a session-start effect.
- The crash rate under the harness is high even with only ~10 pauses per run. This is protocol proposed change 13, and it limits how precisely dual core can be measured with the harness.
- The fix belongs in the C++ side: find what the CPU reads back from the GPU thread. A rollback session in dual core would see these as desyncs.

## Effort and unknowns

**Effort** (one engineer who knows Dolphin and PPC asm):

| Work | Estimate |
|---|---|
| Session start: barrier at `s`, RNG write or hook, setup write and hash, code and heap-layout hash check | 1-2 weeks |
| Late divergence: phase experiment, then a game-frame hook for the P+ code handler (or whatever the experiment points to), then 30-minute soak tests (Peach/G&W/PS2 and a stage list) | 2-4 weeks |
| Region set in C++: dirty-granule snapshot of the set from `g_HeapInfos`, the module list and the DOL list; restore at the Brawlback frame hook; AX exclusion fix | 1-2 weeks on top of Brawlback's `DeltaSaveSlot` |
| SFX and handle bookkeeping, pad slot rewrite after load | 1-2 weeks (Slippi has the design) |
| Mid-match loads: preload or synchronous loads for Assist Trophies, Pokémon, stage transformations; audit legal stages and characters | 2-3 weeks |
| Dual-core determinism (GPU readback) | Unknown. Research first; 2+ weeks. |
| Total to a playable single-core prototype | About 8-12 weeks, assuming the late divergence is the code-handler phase |

**Biggest unknowns:**

1. **The late divergence.** Until its exact cause is found and fixed, the "identical simulation" claim holds for ~140 s, not for a whole match. If the cause is interrupt-phase dependence, it is not specific to gameplay-only rollback. It affects any scheme where the two peers' emulated time differs. (Brawlback's synchronized netplay boot avoids it because both peers boot together.)
2. **Coverage.** Two scenarios and one input policy. Other characters (Zelda/Sheik, Pokémon Trainer forms, Ice Climbers, Olimar), stages (Smashville's clock-driven events; Castle Siege), items on, 3-4 players and Final Smashes are untested. Each can add uninitialised reads, loads or RNG consumers.
3. **The DOL globals list is empirical.** Rarely-written globals are missing until a test writes them. An address-level audit with the decomp, or a run with `all-av` as the reference, is needed.
4. **P+ code-embedded state and Gecko timing.** The code handler runs at audio cadence. Code-embedded variables (0x807824D4) and the code drift of difference 7 show that P+ codes hold state outside the heaps. A full inventory needs the P+ codeset.
5. **Dual core** (Experiment 4).
6. **Cross-platform floating point** (x86-64 against ARM64): not tested here; research 05 §3.12.

## Reproduce

```
set PPHARNESS_DOLPHIN_DIR=D:\code\pm_rollback\run\bin\harness-100b8fd189
python harness/tools/gameplay_rollback.py prep --out run/qa/gprb/ps2
python harness/tools/gameplay_rollback.py simstart --dir run/qa/gprb/ps2
python harness/tools/gameplay_rollback.py diff --dir run/qa/gprb/ps2 --points 0,s,go
python harness/tools/gameplay_rollback.py play --dir run/qa/gprb/ps2 --sync none,rng,rng+frames --frames 4000 --json run/qa/gprb/ps2/play-AB-sc.json
python harness/tools/gameplay_rollback.py play --dir run/qa/gprb/ps2 --sync rng+frames --frames 9000 --seed play2 --probe-from 8380
python harness/tools/gameplay_rollback.py synctest --state run/qa/gprb/ps2/As.sav --pre-inputs run/qa/gprb/ps2/play-AB-sc.json --set all,all-av,harness/tools/gprb-sets/gp-v1.json --n 2400 --k 900
python harness/tools/gameplay_rollback.py replay --dir run/qa/gprb/ps2 --inputs run/qa/gprb/ps2/play-AB-sc.json --run-index 2 --cpu dc --frames 4000 --every 600
python harness/tools/gameplay_rollback.py prep --out run/qa/gprb/bf --chars fox,falco --stage battlefield --rtc-b 0x69C9A3D0
```

`tools/gameplay_rollback.py` needs numpy (`pip install numpy` into the venv). The harness itself stays stdlib-only.

Region sets are named (`all`, `all-av`, `all-av-res`, `all-sys`, `all-sys-dol`, `heaps`, `candidate`, `brawl_gameplay`) or a JSON file:

```
{"base": <set>, "add": [[addr, len, label]], "remove": [[addr, len]],
 "add_heaps": [...], "remove_heaps": [...], "add_rel": true, "add_dol": [...]}
```

The validated set `gp-v1` is `harness/tools/gprb-sets/gp-v1.json`. `harness/tools/gprb_late_divergence.py` is the analysis script for the late divergence.
