# Brawl / Project+ memory map for the test harness

This is the game knowledge the harness scripts need: how to tell which screen the game is on, how to read match state, how to get from boot into a match automatically, and which memory to hash when comparing instances. The code lives in `harness/ppharness/brawl.py` (stdlib only). Its offline tests are in `harness/tests/test_brawl.py`.

**Target.** Exactly one setup is covered:

- Disc: NTSC-U Rev 1 (`RSBE01`, MD5 d18726e6dfdc8bdbdad540b561051087). Its `main.dol` has the same function layout as Rev 2's, and its 126 `.rel` modules are byte-identical to Rev 2's.
- Mod: Project+ v3.2, booted through `Project+ Netplay Launcher.dol`, which loads `NETPLAY.GCT` and `NETBOOST.GCT` from `Wii/sd.raw`.

**Rev 1 versus Rev 2.** All 57 of the "original word" guards that Orca's Rev 2 patch files (`RSBE01.patches`, `PPLUS32.ini` KeepGameCode) check in the DOL match the Rev 1 `main.dol` byte for byte. So Rev 2 addresses from the decomp and from Orca apply unchanged. Orca's own scene reader refuses anything but revision 2 (`Harness.cpp:225`); ours must not gate on the revision.

## Provenance legend

Every address below carries one of these tags. The same tags are in `brawl.ADDRESSES`.

| Tag | Meaning |
|---|---|
| **dol-verified** | Checked against the bytes of the Rev 1 `main.dol`, or of a Rev 1 `.rel` (identical to Rev 2's). The scratch tools parse the DOL header (offsets, addresses and sizes at 0x0/0x48/0x90) and disassemble. |
| **decomp** | From `brawl-decomp` `config/RSBE01_02/symbols.txt` (Rev 2 symbols). |
| **headers** | A struct offset from BrawlHeaders or BrawlHeaders-sammi (reverse engineered). |
| **orca** / **brawlback** / **pplus** | Taken from that project's code. Orca's values were found live under its harness on Rev 2 with P+ v3.2. P+ values are hard-coded in its v3.2 codeset source (`Project+/Source/**` on the SD card). |
| **live** | A heap object, or a value that exists only at runtime. Every one of them used by `brawl.py` has now been checked on the running game (Section 7); the table marks each **verified live (Rev 1, P+ v3.2)** or says what is still unknown. |

The SDA base r13 is `0x805A4420`. This is dol-verified: `__init_registers` at 0x8000422C does `lis r13,0x805A; ori r13,r13,0x4420`. Every `.sbss` global below is r13-relative.

Pointer values stored in the globals (the 0x805A.... words) are runtime data. Only the global's own address and the code that reads it are verified.

---

## 1. Scene detection

**Method** (orca `Harness.cpp:199` BrawlSceneName; brawlback `RollbackManager.cpp:335` IsCurrentSceneMelee):

```
manager  = u32[0x805A0060]         gfSceneManager*   (getInstance @0x8002D018 = lwz r3,-0x43C0(r13): dol-verified)
scene    = u32[manager + 0x4]      gfScene* m_currentScene
name     = cstr[u32[scene + 0x0]]  gfScene::m_sceneName, e.g. "scMelee"
sequence = cstr[u32[u32[manager + 0x10]]]   gfSequence* m_currentSequence -> m_sequenceName, e.g. "sqVsMelee"
next     = cstr[u32[u32[manager + 0x8]]]    queued next scene (if any)
```

These offsets come from BrawlHeaders `gf/gf_scene.h`: prev/current/next scene at 0/4/8, prev/current/next sequence at 0xC/0x10/0x14, and `processStep` at 0x288.

P+ itself reads the same chain. Its "Don't load fighter files on CSS" hook compares `gfSceneManager->currentScene->name` with `"scSelctCharacter"`, and its BOSS code compares `[[gfApplication+0xD4]+0x10]+0` with `"sqSpMelee"`.

**Validation.** Every pointer must be in MEM1 or MEM2, and the name must be printable and start with `sc`/`mu` (or `sq` for sequences). Otherwise the result is `Scene.UNKNOWN`. Until the game proper is running (for example while the P+ launcher DOL runs), 0x805A0060 holds unrelated data. `read_scene(read_mem)` returns a `SceneInfo(scene, name, sequence, next_name, scene_ptr, manager)`.

### Scene values

All names were found as strings in Rev 1 `sora_scene.rel` / `main.dol`; `muMenuMain` comes from Orca's logs.

| Screen | Scene name | `Scene` enum | Notes |
|---|---|---|---|
| Strap / health screen | `scStrap` | `STRAP` | Copies packs to NAND `/tmp`. |
| Boot / save check | `scBoot` | `BOOT` | P+'s "Create save file for Project+?" appears here only if the NAND has no Brawl save. The template user folder has one (`Wii/title/00010000/52534245/data/*.bin`). |
| Title | `scTitle` | `TITLE` | P+ shows it only if Start is held at boot. |
| Main menu (and the Group > Brawl page) | `muMenuMain` | `MAIN_MENU` | Brawl's Vs. menu is a page inside the main menu, not its own scene. |
| Character select | `scSelctCharacter` | `CSS` | The typo is the game's. Sequence `sqVsMelee`. |
| Stage select | `scSelStage` | `SSS` | |
| Loading / between scenes | `scMemoryChange` | `LOADING` | |
| In match | `scMelee` | `IN_MATCH` | Versus uses sequence `sqVsMelee`. Subspace also runs `scMelee`, so check the sequence. |
| Results | `scVsResult` | `RESULTS` | |
| Unlock announcement | `scPrizeInfo` | `PRIZE` | P+ skips it ("Skip prize unlock screen", op b 0x10 @ 0x806F5EB0). |
| Anything else | e.g. `scAdvMap`, `scReplay`, `scEnding` | `OTHER` | Full list in `brawl.OTHER_SCENES`. |

**P+ netplay-specific screens.** There are none. P+ v3.2 netplay is Dolphin netplay of local Versus, so the scenes are the same as offline.

**Boot order under P+.** Observed in a P+ log (`brawlback-asm` at `8faf04c^:Brawlback/Codes/Brawlback/memLocations.txt`):

```
start -> scStrap -> scMemoryChange -> scBoot -> [sqBoot -> sqVsMelee] -> scMemoryChange -> scSelctCharacter
```

This happens because of P+'s **"Boot Directly to CSS v5.4"** (`Source/Project+/BootToCSS.asm`, HOOK @ 0x806DD5F8 in `sqBoot::setNext`). It starts `sqVsMelee` instead of `sqPrizeCheck` unless some pad holds one of these at that moment (it reads pads at 0x805BA684 + 0x40·port):

| Held button | Boots to |
|---|---|
| L/R | `sqTraining` |
| Start | the title, then the main menu |
| Z | `sqReplay` |

**The harness must not hold L, R, Start or Z during scBoot.** Tapping A is safe. The port that triggers the code is also written to `gmSetRule+0x24` (the menu decision pad).

**Sequences.** Allowed in a netplay Versus session: `sqBoot`, `sqVsMelee`, `sqMenuMain`, `sqTitle`, `sqPrizeCheck`. Every other `sq*` name in `sora_scene.rel` is listed in `brawl.BANNED_SEQUENCES` (Section 4).

**Frame counters** (`read_frame_counters`):

| Counter | Read | Where it comes from |
|---|---|---|
| Since boot | `u32[[0x8059FFAC] + 0x100]` | gfApplication. 0x800174FC `lwz r3,0x100(r23)`, `addi r0,r3,1`, then 0x80017504 `stw r0,0x100(r23)`, with r23 = `this` from `mainLoop` @0x80016EE0. **dol-verified.** 0x80017504 is the frame hook that Orca (`FrameHook`/`FrameHookWord 0x90170100`) and Brawlback (`HLE.cpp:126`) use. The pointer value is runtime; it was 0x805B4FD8 in old Brawlback logs. |
| `frameCounter` | `u32[0x901812A4]` | g_GameFrame. **verified live (Rev 1, P+ v3.2)**: +1 per game frame (per `input_polls`). |
| `persistentFrameCounter` | `u32[0x901812B4]` | g_GameFrame. **verified live (Rev 1, P+ v3.2)**: +1 per game frame; it ran 40 ahead of `frameCounter` in one boot. Brawlback hashes this one. |
| In-match | `stOperatorRuleMelee` fields | Section 2. |

---

## 2. Match state

All of these are read by `read_match_state(read_mem)`. Struct offsets come from BrawlHeaders-sammi unless noted.

### Match setup: gmGlobalModeMelee

The setup is at `[[0x805A00E0]+0x08]` (live at 0x90180F20). It is written when the stage select exits, and the match reads it. **verified live (Rev 1, P+ v3.2)**: a Fox (P1) vs Falco (P2) stock match on Battlefield read game mode 0, rule 1, 2 players, stage 0x01, time limit 28800, players `07/00/04` and `15/00/04` (character/state/stocks), the other two ports state 3.

| Field | Offset | Notes |
|---|---|---|
| gameMode | `+0x08` byte >> 2 | 0 = Melee/Versus, 6 = Adventure, 0xD = Training |
| gameRule | `+0x09` >> 5 | 0 time, 1 stock |
| numPlayers | `(+0x09 >> 2) & 7` | |
| isStamina | `+0x0B & 0x20` | |
| item frequency | `+0x16` | |
| **stage kind** | `+0x1A` u16 | Orca reads the low byte at +0x1B. |
| **time limit** | `+0x20` s32 frames | Orca Results.h. **verified live (Rev 1, P+ v3.2)**: 28800 for 8 minutes. |
| **players[i]** | `+0x98 + i·0x5C` | `+0` gmCharacterKind, `+1` state (**0 human, 3 none** verified live (Rev 1, P+ v3.2); 1 CPU from Orca's probe, not seen in a setup here), `+4` stocks (s8), `+5` costume, `+7` controller (1 for P1, 2 for P2 in our runs) |

### Rules: gmSetRule

The rules are at `[[0x805A00E0]+0x1C]` (live at 0x9017F360).

| Field | Offset |
|---|---|
| rule | `+2` low 3 bits |
| time minutes | `+3` |
| stocks | `+4` |
| handicap | `+5` |
| damage ratio ×10 | `+6` |
| stage choice | `+7` |
| stock-match time limit (minutes) | `+8` |
| team attack | `+9` |
| pause | `+0xA` |

**verified live (Rev 1, P+ v3.2)**: on the CSS it read rule 1, 4 stocks, stock time limit 8, ratio 10, team attack 1, pause 1, as P+'s defaults below say.

**P+'s active NETPLAY.TXT "Default Settings Modifier"** writes these once at boot: `0417F360 00000100`, `0417F364 04000A00`, `0417F368 08010100`. That is stock, **4 stocks, 8 minutes**, ratio 1.0, team attack on, pause on. So the rules already match the test's needs; just assert them.

### Per player

Read through ftEntryManager (`read_players`).

```
entries   = u32[0x80624780]                     ftEntryManager (P+ hard-codes 0x80624780: FSMeter.asm; Brawlback too)   live
entry[i]  = entries + i·0x244                   ftEntry (size 0x244)
  +0x04 entryId, +0x0A u8 activeInstanceIndex, +0x28 ftOwner*, +0x30 {ftKind,Fighter*}[4], +0x58 playerNo, +0x5C gmCharacterKind
owner->data = u32[u32[entry+0x28]]
  damage  f32 @ data+0x24   ftOwner::getDamage   = sora_melee .text+0x111850: lwz r3,0(r3); lfs f1,0x24(r3)   dol-verified (REL)
  stocks  s32 @ data+0x34   ftOwner::getStockCount = .text+0x111B2C: lwz r3,0(r3); lwz r3,0x34(r3)     dol-verified (REL)
fighter   = u32[entry + 0x34 + 8·activeInstance]
  ftKind @ fighter+0x110                         P+ DoubleCherry.asm
enum      = u32[u32[fighter+0x60] + 0xD8]        soExternalValueAccesser::getLr/getStatusKind (.text+0x8CAB4/0x8CBF4)  dol-verified (REL)
posture   = u32[enum+0x0C]   pos x/y/z f32 @+0xC/+0x10/+0x14, prevPos @+0x18, lr (facing) @+0x40   headers
status    = u32[enum+0x70]   status kind (action state) s32 @+0x34                                 headers
motion    = u32[enum+0x08]   anim frame f32 @+0x40, motion kind @+0x58                              headers
```

- The REL `.text` base is 0x8070AA14 in P+, consistent with Brawlback's absolute addresses and the P+ log ("create Instance adr:0x8070a940 text:0x8070aa14").
- **Velocity.** BrawlHeaders documents no kinetic-energy velocity field. `PlayerState.vel_x/vel_y` uses `pos - prevPos`, which is the movement over the last frame.
- **Human or CPU** comes from the setup's `state` byte.

**verified live (Rev 1, P+ v3.2)** (Fox vs Falco on Battlefield, compared with the HUD and screenshots):

- `[0x80624780]` held 0x806232F0 (the entries), count 9, and `[0x80B87C48] == 0x80624780`. `ftEntry+0x58` (playerNo) = port, `+0x5C` = gmCharacterKind (7 / 0x15), `+0x04` entryId 0x10000 / 0x20001.
- Damage `data+0x24` read 32.52 when the HUD showed 32 % (the HUD drops the fraction), and went back to 0 on respawn. Stocks `data+0x34` went 4 → 3 → 2 with each self-destruct, matching the stock icons.
- Position: P1 spawned at (-38.8, 27.21) and P2 at (38.8, 27.21) on the side platforms; x grew while holding right; y was 0.0 on the main platform. prevPos (`+0x18`) equals pos while still. Facing `lr` (`+0x40`) is +1.0 / -1.0.
- Action state (`status+0x34`): 0 wait, 1 walk, 3 dash, 6 turn, 14 fall, 22 platform drop, 190 respawn platform, 267 dead (KO'd off the bottom). Motion kind and animation frame move with them.
- ftKind (`fighter+0x110`): Fox 0x06, Falco 0x13; Giga Bowser 0x30 and Wario-Man 0x31 (seen in the netplay QA probe).

### Match timer, start and end

`scMelee` is the current scene object (or live at 0x90FF50C0), and **`stOperatorRuleMelee = u32[scMelee+0x54]`** (verified live (Rev 1, P+ v3.2); the earlier `+0x58` was wrong: it points at another object whose name string is also "StOperatorRule", and its fields read 0). `+0x48`/`+0x4C` are "OperatorGo" and `+0x50` is `stOperatorDropItemMelee`.

| Field | Read | Notes |
|---|---|---|
| `m_isGameSet` | u8 `rule+0x74` | |
| `m_isStart` | u8 `rule+0xDF` | |
| `m_remainingFrameTime` | u32 `rule+0xE0` | the match timer, in frames |
| `m_framesElapsed` | u32 `rule+0xE4` | |
| `m_frameCounter` | u32 `rule+0xE8` | |
| `m_decisionKind` | `rule+0x254` | Reads 0xCCCCCCCC (uninitialised) during play and 1 after a stock game set. Meaning not confirmed; use gmResultInfo's decision instead. |

**verified live (Rev 1, P+ v3.2)**: `remaining + elapsed = 28800` at every read; `remaining` matched the HUD timer (7:59.54 at GO = 28795 frames; 6:05.42 ≈ 21940); `frameCounter` equals `elapsed`; `isStart` is 1 from GO; `isGameSet` became 1 when P2 lost the last stock, while the scene was still `scMelee`.

**Match ended:**
- `m_isGameSet != 0`, or
- the scene leaves `scMelee` (it goes to `scMemoryChange` and then `scVsResult`).

`wait_match_end` uses both.

### Results: gmResultInfo

The results are at `[[0x805A00E0]+0x18]` (live at 0x9017F420) and are filled before the first frame of `scVsResult`.

| Field | Offset |
|---|---|
| stage | `+0x0C` u16 |
| winning player | `+0x1F` |
| decision | `+0x1378`: 1 time up, 2 win, 9 no contest |
| players[i] | `+0x24 + i·0x2AC`: `+0` char, `+1` state, `+0xA` stocks, `+0xE` place (0-based), `+0x10` KOs, `+0x14` falls (self-destructs included), `+0x18` u16 self-destructs |

**verified live (Rev 1, P+ v3.2)**: after P2 self-destructed four times, decision 2, winning player 0, P1 stocks 4 place 0, P2 stocks 0 place 1, falls 4, SDs 4 (the results screen shows FALLS 0 and SDs -4). Leaving the results needs **two A presses from every human port**: the first opens that port's stats panel, the second marks it "ready for the next battle".

### Brawlback's desync checksum (exact)

Source: `Project-Plus-Dolphin-brawlback/Source/Core/Core/Rollback/RollbackManager.cpp:278-309, 368-397`. In code these are `BRAWLBACK_CHECKSUM_FIELDS`, `brawlback_checksum_ranges()` and `brawlback_checksum()`.

**Algorithm:**
- zlib CRC32 (initial value 0) over the raw big-endian bytes of these fields, in this order.
- The result is 0 unless the scene name is `scMelee`.
- It is computed at the 0x80017504 frame hook for each frame GekkoNet saves.
- GekkoNet compares only confirmed frames (current − 5 − 1).
- Pointer fields: start at `base`; for each offset, dereference and then add the offset. A null pointer skips the field.

| # | Field | Address / chain | Size |
|---|---|---|---|
| 1 | persistentFrameCounter | `0x901812B4` | 4 |
| 2-5 | P1-P4 damage | `0x80623324`, `0x80623568`, `0x806237AC`, `0x806239F0` | 4 each |
| 6-9 | P1-P4 stocks | `0x80623318`, `0x8062355C`, `0x806237A0`, `0x806239E4` | 4 each |
| 10-17 | P1-P4 X, Y | `[0x80624780] → +{0x34,0x278,0x4BC,0x700} → +0x60 → +0xD8 → +0xC → +0xC (X) / +0x10 (Y)` | 4 each |
| 18-21 | P1-P4 "total velocity" | `0x80494F30`, `0x8049DEE4`, `0x80494F98`, `0x80495000` | 8 each |

**Settled live: Brawlback's damage, stock and velocity fields are wrong** (verified live (Rev 1, P+ v3.2)). With the entries at 0x806232F0:

- "P1 damage" 0x80623324 is `entry+0x34`, instance 0's `Fighter*` (read 0x8126DFA0); "P1 stocks" 0x80623318 is `entry+0x28`, the `ftOwner*` (0x8128AE20). Same for P2 (0x812BFC40 / 0x812DCE20). They stayed constant while P2 went from 0 % to 32 % and from 4 stocks to 2, so they never reflect damage or stocks.
- The "velocity" words hold heap pointers (0x8126C660…) and did not change while both fighters ran across the stage.
- **Positions are right**: they matched `read_players` exactly.
- So Brawlback's checksum covers only the persistent frame counter and X/Y. A damage or stock desync with identical positions would go unnoticed.
- The fix uses Brawlback's own pointer-chain semantics: damage = `[0x80624780] +0x28+i·0x244 → +0 → +0x24`, stocks = `… → +0x34` (resolved live to 0x8128AE54 / 0x8128AE64 for P1). `brawl.CORRECTED_CHECKSUM_FIELDS` has the full corrected table, and `brawlback_checksum(read_mem, CORRECTED_CHECKSUM_FIELDS)` computes it.
- **Positions** still always follow `instances[0]`, not the active instance, so transformations (Zelda/Sheik, Giga Bowser) hash the wrong fighter.

---

## 3. Getting into a match automatically

### Recommended method: closed-loop controller input, then a memory check

Controller input is the only method that is deterministic by construction under netplay or rollback. Each instance drives only its own port, and both read identical game state. Memory is used to **steer** and to **verify**, never to change state.

`start_match_local(drv, {0: "fox", 1: "falco"}, "battlefield")` does the whole thing on one instance. For netplay, run the same building blocks per instance (see the example at the end of this section).

**Preconditions**:
- Each port is a Standard Controller (`SIDevice0 = 6`).
- Pin the RTC. Orca forces `Dolphin.Core.CustomRTCValue = 1735689600` (2025-01-01) because "at the console epoch Brawl never detects a GameCube controller after the launcher boots it" (`PPLUS32.ini`).

**Step by step.** All waits are keyed on scenes or memory, never on fixed timings.

1. **Boot to CSS** (`boot_to_css`): every 40 frames while the scene is `UNKNOWN`/`STRAP`/`BOOT`/`TITLE`/`MAIN_MENU`, tap A for 3 frames. Never hold L/R/Start/Z.
   - **verified live (Rev 1, P+ v3.2)**: with the template user's save, the Offline and Netplay launchers both reach `scSelctCharacter` (sequence `sqVsMelee`) with **no input at all**, at about input poll 400-470 (VI field ~640). Log order: `scStrap -> scMemoryChange -> scBoot -> sqBoot -> sqVsMelee -> scMemoryChange -> scSelctCharacter`.
   - BootToCSS lands directly on `scSelctCharacter`.
   - From the title or main menu, repeated A walks Group > Brawl. Orca's script `pplus-1v1.txt` is `@muMenuMain mash A`.
2. **Wait for the CSS** (`wait_css_ready`): until `[[[[0x805A0060]+4]+0x400]+0x44+4·port]` (the muSelCharPlayerArea) is readable for every port, then 20 frames more.
3. **Join** (`css_join`): while `area+0x1B4` (kind) is not 1 (human), tap A. Orca joins with A at about frame 60 of the CSS. Kind values (verified live (Rev 1, P+ v3.2)): 0 empty ("NONE" panel), 1 human ("PLAYER 1"), 2 CPU. A on a human's name tag turns it into a CPU (seen on screen), so A is only pressed with the hand over the grid.
4. **Pick a character, closed loop** (`css_pick_character(drv, port, CSS_ID["fox"])`). The reads come from orca `OnlineRules.h:84-98` and `Queue.cpp:409-560`:
   - **Reads:**
     - hand = `u32[area+0x1A8]`
     - hand position: `x = f32[hand+0x90]`, `y = f32[hand+0x94]` (y up)
     - hand target: `u32[hand+0x80]` (verified live (Rev 1, P+ v3.2)): 0 nothing, 1 a name tag / panel button (where the hand rests at the bottom), 3 over its own placed token, 4 the BACK button, 7 over the grid holding the token; 2 = over the grid with the token placed (Orca).
     - character under the hand: `u32[area+0x1B8]` (MuSelchkind; 0x28 = none). **verified live (Rev 1, P+ v3.2)**: moving up the left column read 0x12 (G&W), 0x0B (Ness), 0x08 (Pikachu), 0x07 (Fox), 0x00 (Mario), as on screen. Fox is at about (-20.8, 11.2), Falco (-12.4, 11.0).
     - `u8[area+0x1F8]` = token in hand; `u8[area+0x1F9]` = token flying
   - **Steering:** move along a serpentine scan of the grid (y 15 … −2.5, x −30 … 30; the hand moves 1.0 unit per frame at full tilt, verified live (Rev 1, P+ v3.2)). The stick law is Orca's SteerToward: full tilt from 3 units away, proportional nearer, with at least 30 stick units of offset.
   - **Input latency.** The hand starts moving 2 polls after a stick change offline, but **12-13 frames later in fixed-delay netplay** (the pad buffer). A held stick then overshoots by that much and the old picker orbited its target. The picker therefore measures the latency first (`measure_input_latency`), holds full tilt only while far, releases within `APPROACH_RADIUS` (grown to the overshoot it observes), waits until the hand has been still for longer than the latency (`_settle`), and finishes with short stick pulses. Scan waypoints have deadlines. The offline simulation covers 3 and 10 frames of delay.
   - **Learning:** every icon seen under the hand is added to `LEARNED_CSS` as a running centroid (the first sighting is at the icon's edge), so later picks go straight to the icon's middle.
   - **Pressing A:** when target = 7, character under the hand = the wanted one, and the hand has settled, tap A for 3 frames, then wait up to 20 frames for the token to drop (the press is delayed too in netplay).
   - **Verify:** token placed (`!in_hand && !flying`) and `area+0x1B8` = the wanted character.
   - **Wrong token placed:** tap B for exactly 1 frame to pick it back up. **Never hold B**: held B with the token in hand backs out of the CSS after about 31 frames.
5. **Start** (`css_start`): the host (port 0) taps Start every 30 frames until the scene is `scSelStage`. P+ needs at least 2 players in stock mode.
6. **Pick a stage, closed loop** (`sss_pick_stage(drv, STAGE_KIND["battlefield"])`):
   - **Reads:**
     - task = `[[[0x805A0060]+4]+0x3AC]`
     - cursor = `u32[task+0x200]`, with `x = f32[+0x3C]`, `y = f32[+0x40]` (verified live (Rev 1, P+ v3.2): it starts at (0.25, -18.45) on RANDOM, moves 1.6 units per frame, y runs up to 20.5)
     - page = `task+0x228`
     - **stage under the cursor = hovered item `u32[task+0x244]` − 2** (verified live (Rev 1, P+ v3.2)): 0 over nothing, page position + 2 over a stage icon (Battlefield, position 17, reads 19; FD, position 10, reads 12), 0x36 on RANDOM.
     - `s32[task+0x248]` (P+'s "current selection", what X strikes) is **sticky**: it keeps the last hovered position after the cursor leaves the icons, so it must not be used on its own. (It was the primary read before; that was wrong.)
   - **Target:** P+ v3.2 netplay loads `pf/stage/switch/SwitchFF.rss` into RSS_EXDATA. Page `p`'s list is at `0x8042C524 + 0x28·p` (`{count, slot[39]}`), and the slot → kind table is at `0x8042C5EC + 2·slot`. From that preset, **Battlefield (kind 0x01) is page 0 position 17, and Final Destination (0x02) is page 0 position 10**. The recipe reads the live tables, with the file as a fallback. **verified live (Rev 1, P+ v3.2)**: the live tables are filled by the time the SSS opens with the Offline launcher too (page 0 = 21 stages, 3 rows of 7); on screen position 10 is Final Destination and 17 is Battlefield.
   - **Steering:** the stage icons are 3 rows at y ≈ -3.2 / -9.1 / -15.0, x ≈ -18 … 22. The scan covers those rows first; positions are cached as centroids in `LEARNED_SSS`; same delay-tolerant approach as the CSS.
   - **Taken:** `task+0x224` leaves 0 (it reads **11** on P+ v3.2, verified live (Rev 1, P+ v3.2); "2" was wrong) and `task+0x258` holds the chosen kind. The screen exits a few frames later.
7. **Wait for GO** (`wait_match_start`): scene `scMelee` and `framesElapsed > 0` (or `m_isStart`).
8. **Verify** (`verify_match_setup`) the match setup:
   - game mode 0, rule 1 (stock)
   - stage kind
   - each port human, with the right gmCharacterKind and 4 stocks
   - time limit 8 × 3600 frames

**Character ids.** Use the CSS id (MuSelchkind) to pick and the gmCharacterKind to verify. `CSS_TO_CHAR_KIND` is byte 0 of each 16-byte entry of P+'s slot table at 0x80585B00 (verified live (Rev 1, P+ v3.2); P+ copies the Rev 1 DOL table at 0x80455458 there and extends it).

| Character | CSS id | gmCharacterKind | ftKind |
|---|---|---|---|
| Mario | 0x00 | 0x00 | 0x00 |
| Fox | 0x07 | 0x07 | 0x06 |
| Falco | 0x13 | 0x15 | 0x13 |

The P+ CSS icon order is at 0x80680DE0 (43 bytes, `PPLUS_CSS_ROSTER`, verified live (Rev 1, P+ v3.2)). P+'s extra ids, identified from the slot table and the CSS screen (verified live (Rev 1, P+ v3.2)):

| CSS id | Character | gmCharacterKind |
|---|---|---|
| 0x2A | Charizard (independent) | 0x1E |
| 0x2B | Squirtle (independent) | 0x20 |
| 0x2C | Ivysaur (independent) | 0x22 |
| 0x2D | Roy | 0x32 |
| 0x2E | Mewtwo | 0x33 |
| 0x30 | Knuckles | 0x35 |
| 0x36 / 0x37 / 0x38 | Wario-Man / solo Popo / Giga Bowser (hold-L slots) | 0x2D / 0x11 / 0x2C |

The vanilla Pokemon Trainer ids 0x1B-0x1E are not on P+'s CSS. Roy, Mewtwo and Knuckles have gmCharacterKinds above 0x2D, so the banned-character check now uses the set of kinds P+'s CSS can produce (`PPLUS_PLAYABLE_CHAR_KINDS`) instead of "anything from 0x2E up".

**Open-loop fallback.** These are Orca's working P+ v3.2 timings (`Tools/orca/inputs/pplus-1v1.txt`), in frames since entering the scene. They are useful as a sanity check only, because the menus drift.

```
@scBoot mash 30.. A every 40
@muMenuMain mash 30.. A every 40
@scSelctCharacter 60-63 P1 A ; 70-73 P2 A ; 90-116 both SY=255 (up 27 frames: "two rows up from the panel") ;
                  140-143 P1 A ; 150-153 P2 A ; 200-203 P1 START
@scSelStage 40-44 P1 SY=255 ; 60-63 P1 A
@scVsResult mash A (from 240) then START (from 600), both ports
```

### Memory writes (method a): what is possible and the desync risks

Memory writes are fast but **only safe for netplay under these rules:**
1. Both instances write the same bytes.
2. The memory has not yet been consumed by the game.
3. Writes are repeated idempotently every frame until the consuming scene starts. A rollback can restore a frame from before the write on one instance only, and the repeat covers that.

Until both instances have written, full-RAM hashes differ, so begin desync comparisons after the consuming scene starts. Better still, write in single-instance setup (for example before saving a savestate that both instances load), not mid-session.

| Helper | Writes | Consumed by | Status |
|---|---|---|---|
| `write_rules(drv, 4, 8)` | gmSetRule `+2` (low 3 bits = 1), `+4` = stocks, `+8` = minutes; record menu data `[[GG]+0x24]+0x810` = 0 (items None, as Orca does) | CSS/SSS when building the match setup | P+ already sets the rules; use this to force them. |
| `seed_css_record(drv, {port: (gmCharacterKind, state)})` | gmSelCharData `[[GG]+0x10] + 0xB8 + port·0x5C` (`+0` char, `+1` state) | CSS construction | P+ keeps this record ("CSS Selections Preserved in VS Mode", `b 0x3C @ 0x806DCA90`). **verified live (Rev 1, P+ v3.2): only partly works.** Written every frame of scBoot, both panels came up joined as PLAYER 1/2, but the tokens were **not** placed (hands still holding them, character "none"). After a results screen P+ does restore placed tokens, from a record that also has stocks (+4) and controller (+7) set. Use `css_pick_character`. |
| Not implemented (risky) | SSS `task+0x258` (chosen kind) between taking a stage and the exit about 6 frames later. Orca: "writing it before then changes which stage loads." | Stage load | Single-instance only. Racing the 6-frame window across two instances is not deterministic. |
| Not implemented (risky) | gmGlobalModeMelee players/stage during `scMemoryChange` | `scMelee` start | P+'s stage-file loader keys on other state; untested. |

**Netplay orchestration** (one HarnessClient per instance; host = port 0, client = port 1):

```python
import threading
from ppharness import brawl as B
def css(drv, port, who): B.wait_css_ready(drv, [port]); B.css_pick_character(drv, port, B.CSS_ID[who])
t = [threading.Thread(target=css, args=(host, 0, "fox")), threading.Thread(target=css, args=(client, 1, "falco"))]
[x.start() for x in t]; [x.join() for x in t]
B.css_start(host, 0)                                   # only the host presses Start
B.sss_pick_stage(host, B.STAGE_KIND["battlefield"], 0) # SSS cursor obeys any port (task+0x278 = 0xF0)
for d in (host, client): B.wait_match_start(d)
assert not B.verify_match_setup(host.read_mem, {0: 0x07, 1: 0x15}, 0x01)
```

If the C++ side maps a netplay client's local pad to a different SI port, pass `pad_port=` (the harness port to inject into) separately from `port` (the in-game panel whose memory is read).

---

## 4. Banned and unintended states (QA)

`check_banned(read_mem)` returns a list of violations; an empty list means clean. It flags:

- **Modes.**
  - Sequence in `BANNED_SEQUENCES`:
    - Single-player: `sqAdventure` (Subspace), `sqSingleBoss` (Boss Battles), `sqSingleSimple`, `sqSingleAllstar`, `sqEvent`, `sqTargetBreak`, `sqHomerun`, `sqKumite`.
    - Other modes: `sqTraining`, `sqSpMelee`, `sqReplay`, `sqEdit`, `sqDebugDefault`, every `sqNet*`, and others.
  - Any sequence outside the allowed set.
  - A scene starting `scAdv`, or any `OTHER` scene.
  - In a match: gmMeleeInitData gameMode ≠ 0, stage kind 0x3D (Subspace) or 0x34-0x3D (special stages), or stamina.
- **Giga Bowser and Wario-Man.** Three independent signals:
  - **Setup:** a player's gmCharacterKind is 0x2C (Giga) or 0x2D (WarioMan); any other kind that P+'s CSS cannot produce (alloys, bosses; not Roy 0x32, Mewtwo 0x33, Knuckles 0x35) is also flagged.
  - **Live fighter:** active ftKind is 0x30 (Giga) or 0x31 (WarioMan); 0x32-0x36 is also flagged. This catches Final Smash transformations and the Code Menu's on-the-fly "P1 Character Select", which lists Giga Bowser and Warioman (`dnet.cmnu` strings).
  - **CSS:** a panel on P+'s special slots. With "Hold Shield for Special Fighter" (`ProjectM/CSS.ASM`, hooks @0x8068482C/0x80685BE4), holding shield on Wario gives 0x36 (WarioMan), on Bowser 0x38 (Giga Bowser), and on Ice Climbers 0x37 (solo Popo, flagged as information only). **verified live (Rev 1, P+ v3.2)**: the P+ features page describes it as "hold L while going from the character screen to the stage screen", and that is what works: with Wario (P1) and Bowser (P2) picked and L held through Start, the match setup read 0x2D / 0x2C and the fighters ftKind 0x31 / 0x30, and the match showed Wario-Man vs Giga Bowser.
- **Debug.** The Code Menu (`pf/menu3/dnet.cmnu`, loaded at 0x804E0000, 0x2520 bytes) copies its debug lines every frame (`Net-CodeMenu.asm:1167-1190`) into:

  | Byte | Line | Copied from |
  |---|---|---|
  | 0x80583FFF | **Debug Mode** (bit 0 of u16 0x80583FFE, which "Debug Start Input" tests) | line value at 0x804E0D3C |
  | 0x80583FFD | Hitbox Display | 0x804E0D68 |
  | 0x80583FF7 | Collision Display | 0x804E0DB8 |
  | 0x80583FF9 | Stage Collisions | 0x804E0DEC |
  | 0x80583FFB | Camera Lock | 0x804E0E40 |

  Any non-zero byte is flagged. The Code Menu being open is flagged as `u32[0x804E0034] == 4`, which is what the "Print Code Menu" hook tests.

  **verified live (Rev 1, P+ v3.2)**: L + R + D-pad Down opens the Code Menu on the CSS and in a match, with both launchers ("Project+ Code Menu" offline, "Project+ Code Menu (Netplay)" with the Netplay launcher); `0x804E0034` reads 4 while it is drawn and 0 after B closes it. Turning Debug Mode on set 0x80583FFF to 1, Hitbox Display set 0x80583FFD to 1 (hurtboxes drawn yellow on screen), and the bytes went back to 0 when switched off. With Debug Mode on, **Start toggles a frame-advance freeze** instead of pausing, so a script that presses Start (for example on the results screen) freezes the game.
- **A stronger debug check.** Hash the whole Code Menu block across instances and against a fresh boot. This catches every toggle: Special Modes, per-player character switch, and the rest.

**Reachable from a netplay controller?** (`harness/tools/qa_reachability.py`, fixed-delay netplay of the P+ Netplay Launcher, joiner's controller only, 2026-10-06.) Yes, all of it, in vanilla P+ v3.2:

| Banned state | How | Result |
|---|---|---|
| Code Menu | L + R + D-pad Down, on the CSS and in a match | opens on both instances (it is game state); `check_banned` flags it |
| Debug Mode | Code Menu > Debug Mode Settings > Debug Mode: ON | 0x80583FFF = 1 on both; flagged |
| Giga Bowser / Wario-Man | pick Bowser / Wario, hold L while the host presses Start | match setup 0x2C / 0x2D, fighters 0x30 / 0x31; flagged four ways |
| Non-Versus modes | hold B on the CSS (token in hand) | both instances go to `muMenuMain` / `sqMenuMain`, from which every mode is one menu away |

So "netplay Versus only" is not enforced by P+ itself; it has to come from our build (or from detection and refusal).

---

## 5. Desync-relevant regions

Heap positions change per scene (memory layout), so resolve them at runtime from the heap table, then hash what you need.

**Heap table:** `g_HeapInfos` at 0x80494958 (dol-verified: 25 `lis 0x8049 / addi 0x4958` references). It holds 0x48 entries of `{char* name, gfMemoryPool* pool, u32 size, u32 arena}`.
- The `gfMemoryPool` constructor (0x80025B78, dol-verified) is built at the heap's start and stores `+0 name`, `+4 start`, `+8 end = start + size`. So a heap spans `[pool, pool+size)`.
- `read_heap_table(read_mem)` returns `Heap(id, name, start, end, arena)`.

**Gameplay set** (`gameplay_ranges(read_mem)`):
- **Heaps:**
  - `System`: ftManager 0x80629A00, ftEntryManager 0x80624780, other managers.
  - `Fighter1Instance`-`Fighter4Instance`, `FighterTechqniq`, `StageInstance`, `ItemInstance`, `WeaponInstance`, `EnemyInstance`, `Physics`, `InfoInstance`, `GameGlobal`, `GlobalMode`, `WiiPad`.
- **Static ranges:**
  - g_mtRand 0x805A00B8 (8 bytes; seed at +4, LCG `seed*0x41C64E6D+0x3039`)
  - g_mtRandOther 0x805A0420 (8)
  - g_GameFrame 0x901812A0 (0x18)
  - ftEntryManager (0x50), ftManager (0x160)
  - gmGlobalModeMelee (0x320), gmSetRule (0x88)
  - the scMelee object (0x100) and its stOperatorRuleMelee (0x25C)
- **`full=True` adds:** `Effect`, `OverlayCommon` (sora_melee code, data and bss, 5 MB), `OverlayStage`, `OverlayFighter1-4`, `Fighter1-4Resoruce2`, `StageResource`, `IteamResource`.
- **Excluded from every comparison** (`STATE_EXCLUDED_RANGES` = Brawlback's exclusions plus the disc ID):

  | Range | Contents |
  |---|---|
  | 0x804E7C00 + 0xC00 | AX audio buffers |
  | 0x8049A4EA + 0x1400 | AX voice parameter blocks |
  | 0x90000800 + 0x12C800 | framebuffers |
  | 0x80494938 + 0x20 | the disc's DVDDiskID. Rev 1 and Rev 2 `main.dol` differ in exactly one word (0x8001BC9C, the game version passed when this ID is filled in), so this is the only place a Rev 1 and a Rev 2 peer differ (`research/00-summary.md`, addendum). Both discs are supported. |

  `exclude_state_noise(ranges)` subtracts the same set from arbitrary ranges (e.g. whole MEM1).

- **Never hash:** `Sound`, `CopyFB`, `RenderFifo`, `Thread` (stacks), `Network`, `Replay`, `Tmp`, fonts, `SystemFW`.

**Brawlback-parity set:** `brawlback_checksum_ranges(read_mem)` covers the 21 fields above. Hash it with `hash_mem` to match what GekkoNet compares.

**Menu set** (`menu_ranges(read_mem)`):
- **Heaps:** `MenuInstance`, `OverlayMenu`, `GameGlobal`, `GlobalMode`, `WiiPad`, `System`.
- **Objects:**
  - gfSceneManager object (0x320)
  - GameGlobal (0x50), gmGlobalModeMelee, gmSelCharData (0x340), gmSetRule
  - gfPadSystem (`[0x805A0040]`, 0xB78; 0x805BACC0 in P+)
- **P+ data:**
  - P+ Code Menu block 0x804E0000 (0x2520)
  - P+ RSS_EXDATA 0x8042C4E8 (0x320)
  - debug flags 0x80583FF4 (12)
- **RNG seeds** (as in the gameplay set).

**Heap table verified live (Rev 1, P+ v3.2).** `read_heap_table` in a Battlefield match read 50 heaps, all matching the reference layout below where it overlaps (System 0x80611F60-0x80673460, OverlayCommon 0x80673460-0x80B8DB60, Effect from 0x80B8DB60, GameGlobal 0x90167400-0x90199800, WiiPad 0x90E61400-0x90E77500, GlobalMode 0x90FBAD00-0x90FF5C00). In-match instance heaps: Fighter1-4Instance 0x8123AB60 + i·0x52000 (to 0x81382B60), ItemInstance 0x81382B60-0x814CE460, StageInstance 0x814CE460-0x8154E560, Physics 0x8154E560-0x81601960, InfoInstance 0x81601960-0x81734D60, FighterTechqniq 0x92CABB00-0x92DC5600 (MEM2). There is no WeaponInstance or EnemyInstance heap in a Versus match.

**Reference layout.** Heap starts and ends from a P+ run (brawlback-asm memLocations.txt; an older P+ build, so treat as indicative):

| Heap | Start | End |
|---|---|---|
| System | 80611F60 | 80673460 |
| OverlayCommon | 80673460 | 80B8DB60 |
| Effect | 80B8DB60 | 80C23A60 |
| GameGlobal | 90167400 | 90199800 |
| WiiPad | 90E61400 | 90E77500 |
| GlobalMode | 90FDDC00 | 91018B00 |
| Physics (boot) | 81695A60 | 816E2860 |
| MenuInstance (CSS) | 8123AB60 | 815EDF60 |

Brawlback lists the in-match Fighter1/2Instance heaps at 8123AB60-8128CB60 and 8128CB60-812DEB60.

---

## 6. Address table

Generated from `brawl.ADDRESSES`. Struct offsets are in the sections above and in the constants in `brawl.py`.

"pplus" addresses are fixed by P+'s codeset, but their contents exist only at runtime.

| Name | Address | What | Source | Verification |
|---|---|---|---|---|
| `SCENE_MANAGER_PTR` | `0x805A0060` | gfSceneManager* (getInstance @0x8002D018 = lwz r3,-0x43C0(r13)) | decomp lbl_805A0060; orca Harness.cpp:199; brawlback RollbackManager.cpp:335 | dol-verified |
| `GAME_GLOBAL_PTR` | `0x805A00E0` | GameGlobal* g_GameGlobal (-0x4340(r13)) | decomp symbols.txt:30675; P+ BootToCSS.asm | dol-verified |
| `PAD_SYSTEM_PTR` | `0x805A0040` | gfPadSystem* g_gfPadSystem | decomp symbols.txt:30651 | decomp |
| `APPLICATION_PTR` | `0x8059FFAC` | gfApplication* g_gfApplication | decomp symbols.txt:30624 | decomp |
| `MTRAND_DEFAULT` | `0x805A00B8` | mtRand g_mtRand {vtable, s32 seed @+4} (8 bytes) | decomp symbols.txt:30669 + splits.txt; BrawlHeaders mt_prng.h | decomp |
| `MTRAND_OTHER` | `0x805A0420` | mtRand g_mtRandOther (8 bytes) | BrawlHeaders RSBE01.lst:26 | decomp |
| `HEAP_INFOS` | `0x80494958` | HeapInfo g_HeapInfos[0x48] {name*, gfMemoryPool*, size, arena} | decomp symbols.txt:27842 (25 lis/addi refs in Rev1 DOL); BrawlHeaders gf_memory_pool.h | dol-verified |
| `FRAME_HOOK` | `0x80017504` | end of the game loop: stw r0,0x100(r23) | orca PPLUS32.ini FrameHook; brawlback HLE.cpp:126 | dol-verified |
| `CSS_SLOT_TABLE_VANILLA` | `0x80455458` | 16-byte CSS slot entries indexed by MuSelchkind; byte0 = gmCharacterKind, byte1 = alt (0x0C Bowser -> 0x2C Giga, 0x15 Wario -> 0x2D WarioMan). P+ copies it to 0x80585B00 | Rev1 main.dol bytes; P+ ProjectM/CSS.ASM 'Move CSS Slots' | dol-verified |
| `FT_ENTRY_MANAGER` | `0x80624780` | ftEntryManager {ftEntry* entries @0, u32 count @4} (System heap) | P+ Community/FSMeter.asm; brawlback RollbackManager.cpp:278 / EXIBrawlback.h:145 | verified live (Rev 1, P+ v3.2) |
| `FT_MANAGER` | `0x80629A00` | ftManager (System heap) | P+ FSMeter.asm, IC-Basics.asm | verified live (Rev 1, P+ v3.2) |
| `G_FT_ENTRY_MANAGER_PTR` | `0x80B87C48` | sora_melee .bss g_ftEntryManager (should hold 0x80624780) | P+ NETPLAY.TXT .alias; decomp rels/sora_melee bss+0x2E88 with the module at 0x8070A940 | verified live (Rev 1, P+ v3.2) |
| `GAME_FRAME` | `0x901812A0` | GameFrame {+4 frameCounter, +0xC frameDelta, +0x14 persistentFrameCounter} | BrawlHeaders RSBE01.lst:21 g_GameFrame; brawlback RollbackManager.cpp:28 | verified live (Rev 1, P+ v3.2) |
| `SC_MELEE_OBJ` | `0x90FF50C0` | scMelee object (also reachable as the current scene in a match) | BrawlHeaders RSBE01.lst:22 | live |
| `GAME_GLOBAL_OBJ` | `0x90181300` | *g_GameGlobal | orca Results.h:24 | live |
| `MODE_MELEE_OBJ` | `0x90180F20` | gmGlobalModeMelee | orca Results.h; BrawlHeaders RSBE01.lst:24 | verified live (Rev 1, P+ v3.2) |
| `RESULT_INFO_OBJ` | `0x9017F420` | gmResultInfo | orca Results.h | verified live (Rev 1, P+ v3.2) |
| `SET_RULE_OBJ` | `0x9017F360` | gmSetRule (P+ 'Default Settings Modifier' writes it) | orca Results.h; P+ NETPLAY.TXT:353 | verified live (Rev 1, P+ v3.2) |
| `RECORD_MENU_DATA` | `0x9017BE50` | gmGlobalRecord+0x810 menu data (+0 item frequency) | orca RSBE01.patches / OnlineRules.cpp:59 | live |
| `RSS_EXDATA` | `0x8042C4E8` | P+ stage-switch data (0x320 bytes, loaded from pf/stage/switch/SwitchFF.rss on netplay) | P+ Source/Netplay/Net-Random.asm; orca RankedPPlus.h | pplus |
| `RSS_PAGES` | `0x8042C524` | STAGE_PAGES: per page {u8 count, u8 slot[39]}, stride 0x28, 5 pages | P+ Net-Random.asm | pplus |
| `RSS_SLOT_KINDS` | `0x8042C5EC` | slot -> {u8 stage kind, u8 cosmetic} (STAGE_SLOTS_COSMETIC) | P+ Net-Random.asm; orca RankedPPlus.cpp:71 | pplus |
| `SSS_CURRENT_PAGE` | `0x80496000` | P+ CURRENT_PAGE (u8) | P+ Net-Random.asm | pplus |
| `CSS_ROSTER` | `0x80680DE0` | P+ CSS icon order: 43 MuSelchkind bytes | P+ ProjectM/CSS.ASM 'CSS Roster Data v3&K' | pplus |
| `CODE_MENU_BASE` | `0x804E0000` | P+ Code Menu data (pf/menu3/dnet.cmnu, 0x2520 bytes) | Brawlback memLocations.txt (data.cmnu read to 0x804e0000); SD card | pplus |
| `CODE_MENU_STATE` | `0x804E0034` | Code Menu state word (4 = menu drawn) | P+ Net-CodeMenu.asm 'Print Code Menu' (cmpwi r31,4) | verified live (Rev 1, P+ v3.2) |
| `DEBUG_FLAGS` | `0x80583FF4` | P+ debug bytes 0x80583FF4..FFF (Code Menu copies its Debug Mode lines here every frame) | P+ Net-CodeMenu.asm:1167-1190, Debug/modifiedDebug.asm | pplus |
| `DEBUG_MODE_HALF` | `0x80583FFE` | u16, bit 0 = Debug Mode on (byte 0x80583FFF) | P+ Debug/modifiedDebug.asm 'Debug Start Input' | pplus |
| `BOOT_PADS` | `0x805BA684` | pads BootToCSS reads at sqBoot::setNext (+0x40*port, u32 buttons) | P+ Project+/BootToCSS.asm | live |

### Other things verified against Rev 1 bytes

| What | Evidence |
|---|---|
| ftOwner damage `data+0x24`, stocks `data+0x34` | Disassembly of `sora_melee.rel` .text+0x111850 / +0x111B2C. |
| Fighter → accesser `+0x60` → enumeration `+0xD8` → posture `+0xC` / status `+0x70` | `soExternalValueAccesser::getLr` / `getStatusKind`, .text+0x8CAB4 / +0x8CBF4. |
| gfMemoryPool layout (`+4` start, `+8` end) | Constructor at 0x80025B78. |
| Orca's pad "input lag fix" source words at 0x8002AD8C… | Match Rev 1; P+ already ships the same fix. |
| sora_scene sequence strings | With the module at 0x806BB480, `sqTraining` is at 0x80701870, which matches P+ BootToCSS's `loadTraining = 0x1870` relative to 0x80700000. This confirms the module layout P+ assumes. |

---

## 7. Live verification (checklist)

Done on 2026-10-06 with the frozen harness build `run/bin/harness-100b8fd189`: NTSC-U Rev 1 + P+ v3.2, Offline Launcher (and the Netplay Launcher for items 10 and 13), headless D3D11, every value compared with a screenshot. "Verified" below means **verified live (Rev 1, P+ v3.2)**.

| # | Item | Result |
|---|---|---|
| 1 | CSS kind values, hand fields | Verified. Kind 0 / 1 / 2 = none / human / CPU. Hand x/y move 1.0 unit per frame, 2 polls after the stick (12-13 frames in fixed-delay netplay). Hand target values 0, 1, 3, 4, 7 identified (Section 3). |
| 2 | Character under the hand | Verified (`area+0x1B8`), lags the hand by about a frame. P+ ids 0x2A-0x2E/0x30 identified. |
| 3 | SSS position under the cursor | **Was wrong.** It is the hovered item `task+0x244` − **2**; `task+0x248` is sticky. BF = page 0 pos 17, FD = page 0 pos 10, checked on screen. The "taken" state is **11**, not 2. `brawl.py` fixed. |
| 4 | RSS tables populated | Verified, also with the Offline launcher (page 0 = 21 stages). |
| 5 | Match setup | Verified: stage `+0x1A` (1 BF, 2 FD), time limit 28800, state 0 human / 3 none, stocks 4. |
| 6 | ftEntryManager | Verified: `[0x80624780]` = 0x806232F0, `[0x80B87C48]` = 0x80624780, `ftEntry+0x58` = port. |
| 7 | Per-player reads | Verified against the HUD: damage, stocks, position, prevPos, facing, action state, motion, ftKind (Section 2). |
| 8 | stOperatorRuleMelee | **Was wrong**: it is `scMelee+0x54`, not `+0x58`. isStart, remaining, elapsed, frame counter, isGameSet verified with the HUD timer; `+0x254` decision is unconfirmed. `brawl.py` fixed. |
| 9 | g_GameFrame, Brawlback checksum | Verified. **Brawlback's damage/stock/velocity checksum addresses are wrong**: they read constant pointers (`Fighter*`, `ftOwner*`, heap pointers) while the HUD showed 0 % → 32 % and 4 → 2 stocks. Positions are right. `CORRECTED_CHECKSUM_FIELDS` added. |
| 10 | Code Menu state word, debug bytes | Verified: 4 open / 0 closed; Debug Mode 0x80583FFF and Hitbox Display 0x80583FFD follow the menu. Also reachable in netplay (Section 4). |
| 11 | seed_css_record | Partly: humans come up joined, but the tokens are not placed. Not used by the recipes. |
| 12 | Heap table | Verified in a match (Section 5). |
| 13 | Boot path | Verified: CSS with no input at all (both launchers). |
| — | gmResultInfo | Verified, plus `+0x18` self-destructs; leaving the results needs two A presses per human port. |
| — | `SC_MELEE_OBJ` 0x90FF50C0, `GAME_GLOBAL_OBJ`, `RECORD_MENU_DATA`, `BOOT_PADS` | Not used by the parsers (they follow pointers). The scMelee object was at 0x90FD22C0 in two separate boots (CSS 0x90FD3300, SSS 0x90FD42C0, results 0x90FD2180), not at 0x90FF50C0, so `SC_MELEE_OBJ` is wrong for this build; the parsers read the scene manager's current scene instead. The other three are unchecked. |

Still unknown: the exact meaning of `stOperatorRuleMelee+0x254`, state 1 (CPU) in a live match setup, and ftManager (0x80629A00), which no parser reads.

**How the recipes were tested end to end:** `harness/tests/test_e2e_match.py` (offline Battlefield and FD, fixed-delay netplay, rollback netplay as an expected failure) and `harness/tools/qa_reachability.py`.
