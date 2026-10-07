# Offline determinism findings (P+ v3.2, Rev 1)

Measured 2026-10-06 with `harness/tools/determinism.py` on the frozen harness build `run/bin/harness-100b8fd189`, Windows 11, 8 cores shared with another agent's Dolphin instances. The question: with identical controller input from boot, do two Dolphin runs stay identical, and where do they diverge first? And does dual core make gameplay-state divergence worse?

> **Update (round 3, `docs/rollback-fixes-status.md` section 3).** On the current build (`a1f9ec2685` no longer drops GPU commands under pauses), dual core with Null video was bit-identical in whole MEM1/MEM2 in 2 of 3 pairs; the third differed only in boot-time timestamps. Cause 3 below is a render-side screen-colour probe: Brawl copies 4x4 EFB pixels into .bss 0x804951C0 and feeds their average into NW4R G3D light objects, so its value depends on when the GPU thread wrote it and on how the frame was rendered. With D3D11 even single core is not bit-identical (shader compile timing under `AsynchronousSkipRendering`). Player state, damage, stocks and RNG stayed equal in every run. New profiles: `dc-rtc-d3d11`, `sc-rtc-d3d11`.

## Method

1. **Record once** (`determinism.py record`, single core, fixed RTC). Boot the Offline Launcher with no input; P+ boots straight to the CSS. Then, with the closed-loop recipes, P1 picks Fox and P2 Falco, P1 picks Battlefield, and both play 1200 game frames (P1 "chase", P2 seeded "random"). The recording ran from VI field 461 (first input) to 2964, with GO at field 1760.

   Every pad change is submitted **while the emulation is paused at a VI field boundary** (pause, `frame_advance(1)`, read frame and poll, `pad_script` with start "next", resume), and the frame, poll, port, script and resulting start poll are logged (528 submissions).

   This matters because of how `pad_script` replacement works. A replacement takes effect immediately, the new script starts a poll or two later, and which of the two SI polls of a game frame latches which state depends on host timing. Without the pauses, a "replayed" run did not even reach the same character select. (See also protocol doc, proposed change 12: in that gap a port without `pad_set` falls back to the unbound real controller and the game unplugs it.)

2. **Replay on two instances in parallel** (`determinism.py compare`). Both get a neutral `pad_set` on ports 0/1. Then every recorded submission is replayed while both are paused at the same VI field, with the same absolute start poll. Each replay matches the recording exactly: same characters and stage, and the same final damage and stocks as the recording when the RNG seed matched.

3. **Checkpoints**, every 30 VI fields before GO and every 10 after it: pause both at the same field and hash each labelled region separately:
   - `brawl.menu_ranges()` on menus and `brawl.gameplay_ranges()` in the match (heaps by name, ftEntryManager/ftManager, scMelee and stOperatorRuleMelee, gmGlobalModeMelee/gmSetRule/gmSelCharData, gfPadSystem, Code Menu, RSS);
   - the two RNG seeds, `g_GameFrame`, and per-player state (character, ftKind, damage, stocks, x/y, action, animation frame);
   - for reference, whole MEM1 and whole MEM2.

   The first differing checkpoint is bisected down to 64-byte chunks with `hash_mem`.

   `STATE_EXCLUDED_RANGES` (Brawlback's audio and framebuffer exclusions, plus the Rev 1/Rev 2 DVDDiskID at 0x80494938) are never hashed.

Profiles (`--profile`):

- RTC: host clock (the Dolphin default), or fixed with `-C Dolphin.Core.EnableCustomRTC=True -C Dolphin.Core.CustomRTCValue=0x67748580`.
- `CPUThread`: single or dual core. The template's `GameSettings/RSBE01.ini` forces `CPUThread = True`, so the tool rewrites it per profile.
- `GPUDeterminismMode`: P+'s launcher game INI sets `fake-completion`. Game INIs override `-C`, so the tool edits the instance's INI for `auto` or `none`.

## Results

Each row is one pair of runs replaying the same recording. "Identical" means every labelled region, the RNG seeds, the frame counters, per-player state **and whole MEM1/MEM2** hashed equal at every checkpoint (175-179 of them, VI fields 120-2960).

| Profile | Runs | Result |
|---|---|---|
| single core, fixed RTC | 2 pairs | **Identical**, and identical to the recording (Fox 9 %/3 stocks, Falco 0 %/3 at the end). |
| single core, host RTC | 1 pair (started together) | **Identical** to each other. Different game outcome from the fixed-RTC recording (Fox 0 stocks, Falco 2): the clock seeds the RNG, and the RNG reaches gameplay. |
| single core, fixed RTC, no input (idle) | 2 pairs | One pair identical. In the other, the boots straddled a wall-clock second: `__OSStartTime` differed by 1 s at field 30, and both RNG seeds from the strap screen (field 240) on. See cause 1. |
| single core, one pre-scheduled script per port (pauses only at checkpoints) | 1 pair | Gameplay regions identical (one transient difference in the instance heaps while the match loaded); OS timestamps in .bss differ by one tick from field 240. See cause 2. |
| **dual core**, fake-completion (P+ INI), host RTC | 2 runs | One pair **identical**, including whole MEM1/MEM2. The other run crashed (see below). |
| **dual core**, fake-completion (P+ INI), fixed RTC | 6 runs | 1 identical. 3 diverged inside the match in the System heap (cause 3), first at fields 1710, 2120 and 2260, without visible player-state differences before the run ended. **1 diverged in gameplay**: StockResource (MEM2) at field 1200 on the CSS, MenuInstance at 1350 (SSS), the instance heaps at 1500 (match start), the System heap at 1990, **Fox's position at 1990-2010** (x -68.62 against -68.43, 250 frames into the match), the RNG at 2110. Then Falco's damage and the stocks differed. 1 crashed on the CSS before any difference. 4 of the 6 crashed at some point. |
| **dual core**, GPUDeterminismMode auto (Dolphin default), fixed RTC | 2 pairs | Players, RNG and the final result identical to the recording, but: OS timestamps in .bss from field 240, MenuInstance on the boot/SSS screens, and the in-match instance heaps (fighter/item/stage/physics/info) differed at **every** match checkpoint. |
| **dual core**, GPUDeterminismMode none, fixed RTC | 1 pair | The RTC second differed (cause 1), so the RNG differed from field 210; players and the result still identical to the recording. Whole MEM1/MEM2 differed throughout. |

**Crashes.** In dual core, `DolphinNoGUI` exited silently during `frame_advance` in 5 of 11 runs (always fake-completion profiles; one on the CSS, four in a match). That is about 400-600 pause/frame-advance cycles per run. Single core: 0 crashes in 8 runs. This is a harness/Dolphin issue (protocol doc, proposed change 13). It also limits how precisely dual core can be measured with this method.

## Causes found

### 1. The RTC: offline "custom RTC" still follows the host clock (Dolphin core)

The first difference between two otherwise identical single-core runs is at VI field 30, inside the launcher.

- **What differs:** `0x800030D8`, the OS start time (`__OSStartTime`), differs by 0x39EF8B0 timebase ticks, which is about 1.0 s.
- **What follows:** from the strap screen on (field 210-240), **both RNG seeds** differ: `g_mtRand` at 0x805A00BC and `g_mtRandOther` at 0x805A0424. So does everything derived from them, starting with gmGlobalModeMelee/gmSetRule/gmSelCharData in GameGlobal.
- **Why:** with `EnableCustomRTC`, Dolphin computes the RTC as `CustomRTCValue + (host seconds now - host seconds at SystemTimers::Init)` (`EXI_DeviceIPL::GetEmulatedTime`, offline branch). Two runs whose boots straddle a wall-clock second differently read RTC values one second apart.
- **How often:** in our runs this hit about half the pairs, even with both instances launched at the same moment. With the host clock RTC, runs started at different times always differ.
- **Netplay is not affected:** there `GetEmulatedTime` uses the host's RTC plus *emulated* ticks, and movies do the same.
- **Effect on play:** with these scripted inputs, Fox and Falco's positions, damage and stocks stayed identical even when the seeds differed. Only RNG-dependent things diverge (item spawns are off, Fox and Falco have little randomness). The recording made with the host-clock RTC at another time of day ended differently (Fox 0 stocks, Falco 2) from the fixed-RTC recording (3 / 3). That shows the seed does reach gameplay.
- **Fix (C++, one line):** when a custom RTC is set, use `custom + ticks / ticks_per_second`, as netplay does (protocol doc, proposed change 14).
- **Workaround today:** compare runs started together and check the RNG seeds first, or boot as a one-player netplay host.

### 2. Non-gameplay timestamps in bss (single core, fewer pauses)

Replaying the same input as one pre-scheduled script per port ("script" mode, pausing only at checkpoints every 120/30 fields) is not equivalent to the recording, because of the replacement semantics above: P1 ended up on Luigi and P2 on Link. Comparing the two script-mode runs with each other still showed something useful:

- **Gameplay regions were identical at every checkpoint but one:** a transient difference in the fighter, stage and item instance heaps at field 1440, during the match load.
- **Whole MEM1 differed from field 240 on**, in static .bss between 0x804DD540 and 0x804DE3F0. These are unnamed objects in the decomp, holding u64 timebase values such as `00AA49EF 1EC4F78B` against `…78C`, plus small counters that differ by one.

These look like OS or library timestamps taken one tick apart. The emulated time at which some event fired differed slightly between runs. They do not reach the labelled gameplay state, but they make whole-MEM1 hashing useless as a desync signal (as the README already said). The cause is not identified. A candidate is the CPU being stopped (pause / `CPUThreadGuard`) at different points relative to CoreTiming events. With pauses at identical points in both runs (events mode), MEM1 stayed bit-identical for the whole run.

### 3. Dual core: a System-heap object diverges inside the match, sometimes gameplay follows

In dual core with P+'s `fake-completion`, the first in-match difference was in the System heap in all four runs that diverged (fields 1710, 1990, 2120, 2260). In the two runs where it was narrowed down to chunks (1710, 2260) it was the same object both times:

- **Where:** the System heap, 0x80666938-0x80666BAC (System+0x549D8…+0x54C4C).
- **What is there:** an object whose vtable is 0x804662E8. Its methods are at 0x801ADxxx, in NW4R G3D code next to the `NodeTree` strings. So it is G3D scene/render-side state that lives in emulated RAM, not fighter logic.
- **What differs:** u32 counters (5 against 1), floats that look like normals (`bf594bd1` against `80000000`) and large floats (`d119036d` against `d1197330`).

In one of the six runs the divergence reached gameplay. It was preceded by different resource-load state on the menus: the StockResource heap was filled on one run and still empty on the other at field 1200, then MenuInstance differed on the SSS. Then it spread into the instance heaps, ftManager and Fox's position 250 frames into the match, and then the RNG.

**Hypothesis (not proven).** Something the CPU reads back from the GPU side is timing-dependent in dual core even with fake completion. EFB peeks and GX draw-sync/token reads, both used by NW4R G3D, are candidates. Asynchronous file loading (StockResource) racing the GPU thread is the other.

With `auto` (no GPU determinism in offline play), the in-match instance heaps differed at every checkpoint but players and RNG did not. That suggests most of what differs is render-side bookkeeping, and only occasionally does it feed back into the simulation.

**Caveat.** The method pauses and frame-advances the emulation at every input change, about 500 times per run. Pauses force CPU/GPU synchronisation points that a real session would not have, so the measured *rate* may not be the rate in play. That gameplay state *can* diverge in dual core with identical inputs is established. Single core showed no gameplay divergence beyond the RTC.

### Does dual core make gameplay-state divergence worse?

**Yes.**

- **Single core:** with a pinned RTC, every run was bit-identical (MEM1 and MEM2) for the whole 2500-field sequence.
- **Dual core with fake-completion:** 4 of 6 runs diverged in the match, 1 of them in player state. 4 of 6 also crashed under the harness's pause/frame-advance. Dual core with `auto` diverged in non-player heaps at every match checkpoint.

For rollback this matters twice. A desync check that hashes more than player state (as `gameplay_ranges` does) will flag the render-side differences. And the rare gameplay divergence is a true desync.

Since dual core is a hard requirement, the C++ side should find what the G3D object at 0x80666868 reads and make it deterministic, or exclude it from rollback state and desync checks if it is purely render-side. It should also confirm whether it reproduces without harness pauses: a two-instance netplay session in dual core comparing confirmed-frame hashes (proposed change 4) would answer that.

## Recommendations

1. **Fix the offline RTC** (proposed change 14): use emulated ticks with a custom RTC. Until then, check the RNG seeds before trusting an offline comparison, and record the RTC.
2. **Single core is the reference.** With fixed RTC and identical, pause-synchronised inputs it is bit-identical. Use it as the baseline for every other comparison.
3. **Dual core:** investigate the NW4R G3D object at System+0x549D8 (0x80666868 in this build) and the StockResource load difference on the CSS. Measure dual-core determinism over netplay with confirmed-frame hashes once the server has them, because the harness pauses used here also crash dual core (proposed change 13).
4. **Keep `fake-completion`.** `auto` was worse (instance heaps differed in every match checkpoint).
5. **Desync checks** should hash player state (corrected Brawlback fields plus action state) separately from heap-wide hashes, so render-side noise in dual core is told apart from real gameplay divergence. Exclude `STATE_EXCLUDED_RANGES`, which includes the Rev 1/Rev 2 DVDDiskID.
6. **Replaying input** needs submissions at identical emulation points. A pre-scheduled script per port is not equivalent to live `pad_script` replacement (proposed change 12).

Raw data: `run/qa/det-*.json` (not in git); the tool reproduces them:

```
python harness/tools/determinism.py record --out run/qa/timeline.json
python harness/tools/determinism.py matrix --timeline run/qa/timeline.json \
    --profiles sc-rtc,sc-hostrtc,dc-rtc,dc-rtc-gpuauto,dc-rtc-gpunone,dc-hostrtc --out run/qa/det.json
```
