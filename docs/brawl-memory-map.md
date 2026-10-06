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
| **live** | A heap object, or a value that exists only at runtime. **Needs live verification.** |

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
| `frameCounter` | `u32[0x901812A4]` | g_GameFrame. **live** |
| `persistentFrameCounter` | `u32[0x901812B4]` | g_GameFrame. **live.** Brawlback hashes this one. |
| In-match | `stOperatorRuleMelee` fields | Section 2. |

---

## 2. Match state

All of these are read by `read_match_state(read_mem)`. Struct offsets come from BrawlHeaders-sammi unless noted.

### Match setup: gmGlobalModeMelee

The setup is at `[[0x805A00E0]+0x08]` (live at 0x90180F20). It is written when the stage select exits, and the match reads it.

| Field | Offset | Notes |
|---|---|---|
| gameMode | `+0x08` byte >> 2 | 0 = Melee/Versus, 6 = Adventure, 0xD = Training |
| gameRule | `+0x09` >> 5 | 0 time, 1 stock |
| numPlayers | `(+0x09 >> 2) & 7` | |
| isStamina | `+0x0B & 0x20` | |
| item frequency | `+0x16` | |
| **stage kind** | `+0x1A` u16 | Orca reads the low byte at +0x1B. |
| **time limit** | `+0x20` s32 frames | Orca Results.h. 8 min should read 28800: verify live. |
| **players[i]** | `+0x98 + i·0x5C` | `+0` gmCharacterKind, `+1` state (**0 human, 1 CPU, 3 none**: Orca's live probe `pplus-cpu-record-probe.txt`; BrawlHeaders has no enum), `+4` stocks (s8), `+5` costume, `+7` controller |

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

### Match timer, start and end

`scMelee` is the current scene object (or live at 0x90FF50C0), and `stOperatorRuleMelee = u32[scMelee+0x58]`.

| Field | Read | Notes |
|---|---|---|
| `m_isGameSet` | u8 `rule+0x74` | |
| `m_isStart` | u8 `rule+0xDF` | |
| `m_remainingFrameTime` | u32 `rule+0xE0` | the match timer, in frames |
| `m_framesElapsed` | u32 `rule+0xE4` | |
| `m_frameCounter` | u32 `rule+0xE8` | |
| `m_decisionKind` | `rule+0x254` | |

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
| players[i] | `+0x24 + i·0x2AC`: `+0` char, `+1` state, `+0xA` stocks, `+0xE` place, `+0x10` KOs, `+0x14` falls |

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

**Caveats found while cross-checking.** These are worth raising with whoever owns desync detection; they all need live verification.

- **Damage and stocks.** Brawlback's damage and stock addresses use the 0x244 ftEntry stride, but their relative offset (damage = stock + 0xC) does not match the REL-verified ftOwner accessors (damage at data+0x24, stocks at data+0x34). At most one of the two can be the ftOwnerData field. `read_players` uses the accessor offsets.
- **Velocity.** The "velocity" addresses are static DOL `.bss` (0x80494880+) with an irregular P2 stride. No source explains them.
- **Positions** always follow `instances[0]`, not the active instance. So transformations (Zelda/Sheik, Giga Bowser) hash the wrong fighter.

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
   - BootToCSS lands directly on `scSelctCharacter`.
   - From the title or main menu, repeated A walks Group > Brawl. Orca's script `pplus-1v1.txt` is `@muMenuMain mash A`.
2. **Wait for the CSS** (`wait_css_ready`): until `[[[[0x805A0060]+4]+0x400]+0x44+4·port]` (the muSelCharPlayerArea) is readable for every port, then 20 frames more.
3. **Join** (`css_join`): while `area+0x1B4` (kind) is not 1 (human), tap A. Orca joins with A at about frame 60 of the CSS. Kind values: 0 empty, 1 human, 2 CPU.
4. **Pick a character, closed loop** (`css_pick_character(drv, port, CSS_ID["fox"])`). The reads come from orca `OnlineRules.h:84-98` and `Queue.cpp:409-560`:
   - **Reads:**
     - hand = `u32[area+0x1A8]`
     - hand position: `x = f32[hand+0x90]`, `y = f32[hand+0x94]` (y up)
     - hand target: `u32[hand+0x80]`. 7 = over the grid holding the token; 2 = over the grid with the token placed.
     - character under the hand: `u32[area+0x1B8]` (MuSelchkind; 0x28 = none)
     - `u8[area+0x1F8]` = token in hand; `u8[area+0x1F9]` = token flying
   - **Steering:** move along a serpentine scan of the grid (y 15 … −2.5, x −30 … 30; the hand moves 1.0 unit per frame at full tilt in P+). The stick law is Orca's SteerToward: full tilt from 3 units away, proportional nearer, with at least 30 stick units of offset.
   - **Learning:** every icon seen under the hand is cached in `LEARNED_CSS`, so later picks go straight there.
   - **Pressing A:** when target = 7, character under the hand = the wanted one, and the hand moved less than 0.05 this frame, centre the stick and tap A for 3 frames. The character read lags the hand by a frame, so A must wait for a still hand.
   - **Verify:** token placed (`!in_hand && !flying`) and `area+0x1B8` = the wanted character.
   - **Wrong token placed:** tap B for exactly 1 frame to pick it back up. **Never hold B**: held B with the token in hand backs out of the CSS after about 31 frames.
5. **Start** (`css_start`): the host (port 0) taps Start every 30 frames until the scene is `scSelStage`. P+ needs at least 2 players in stock mode.
6. **Pick a stage, closed loop** (`sss_pick_stage(drv, STAGE_KIND["battlefield"])`):
   - **Reads:**
     - task = `[[[0x805A0060]+4]+0x3AC]`
     - cursor = `u32[task+0x200]`, with `x = f32[+0x3C]`, `y = f32[+0x40]` (range ±30 × ±19.5, 1.625 units per frame)
     - page = `task+0x228`
     - **page position under the cursor** = `s32[task+0x248]`. P+ calls it "Current selection on the stage selection screen" and strikes it with X. If it reads −1, fall back to the hovered item `task+0x244` − 1.
   - **Target:** P+ v3.2 netplay loads `pf/stage/switch/SwitchFF.rss` into RSS_EXDATA. Page `p`'s list is at `0x8042C524 + 0x28·p` (`{count, slot[39]}`), and the slot → kind table is at `0x8042C5EC + 2·slot`. From that preset, **Battlefield (kind 0x01) is page 0 position 17, and Final Destination (0x02) is page 0 position 10**. The recipe reads the live tables, with the file as a fallback.
   - **Steering:** scan (step 3 units), cache positions in `LEARNED_SSS`, and tap A when the cursor is on target and still.
   - **Taken:** `task+0x224` becomes 2 and `task+0x258` holds the chosen kind. The screen exits about 6 frames later.
7. **Wait for GO** (`wait_match_start`): scene `scMelee` and `framesElapsed > 0` (or `m_isStart`).
8. **Verify** (`verify_match_setup`) the match setup:
   - game mode 0, rule 1 (stock)
   - stage kind
   - each port human, with the right gmCharacterKind and 4 stocks
   - time limit 8 × 3600 frames

**Character ids.** Use the CSS id (MuSelchkind) to pick and the gmCharacterKind to verify. The `CSS_TO_CHAR_KIND` table is read from the Rev 1 DOL CSS slot table at 0x80455458, byte 0 of each 16-byte entry (dol-verified). P+ copies that table to 0x80585B00.

| Character | CSS id | gmCharacterKind | ftKind |
|---|---|---|---|
| Mario | 0x00 | 0x00 | 0x00 |
| Fox | 0x07 | 0x07 | 0x06 |
| Falco | 0x13 | 0x15 | 0x13 |

The P+ CSS icon order is at 0x80680DE0 (43 bytes, `PPLUS_CSS_ROSTER`). P+'s extra ids are 0x2A-0x2E and 0x30 (clones); their names need live confirmation.

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
| `seed_css_record(drv, {port: (gmCharacterKind, state)})` | gmSelCharData `[[GG]+0x10] + 0xB8 + port·0x5C` (`+0` char, `+1` state) | CSS construction | P+ keeps this record ("CSS Selections Preserved in VS Mode", `b 0x3C @ 0x806DCA90`), and Orca saw the CSS rebuild CPU panels from it. **Needs live verification** that human tokens come up already placed. Write during scStrap/scBoot. |
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
  - **Setup:** a player's gmCharacterKind is 0x2C (Giga) or 0x2D (WarioMan); 0x2E-0x3D (alloys, bosses) is also flagged.
  - **Live fighter:** active ftKind is 0x30 (Giga) or 0x31 (WarioMan); 0x32-0x36 is also flagged. This catches Final Smash transformations and the Code Menu's on-the-fly "P1 Character Select", which lists Giga Bowser and Warioman (`dnet.cmnu` strings).
  - **CSS:** a panel on P+'s special slots. With "Hold Shield for Special Fighter" (`ProjectM/CSS.ASM`, hooks @0x8068482C/0x80685BE4), holding shield on Wario gives 0x36 (WarioMan), on Bowser 0x38 (Giga Bowser), and on Ice Climbers 0x37 (solo Popo, flagged as information only).
- **Debug.** The Code Menu (`pf/menu3/dnet.cmnu`, loaded at 0x804E0000, 0x2520 bytes) copies its debug lines every frame (`Net-CodeMenu.asm:1167-1190`) into:

  | Byte | Line | Copied from |
  |---|---|---|
  | 0x80583FFF | **Debug Mode** (bit 0 of u16 0x80583FFE, which "Debug Start Input" tests) | line value at 0x804E0D3C |
  | 0x80583FFD | Hitbox Display | 0x804E0D68 |
  | 0x80583FF7 | Collision Display | 0x804E0DB8 |
  | 0x80583FF9 | Stage Collisions | 0x804E0DEC |
  | 0x80583FFB | Camera Lock | 0x804E0E40 |

  Any non-zero byte is flagged. The Code Menu being open is flagged as `u32[0x804E0034] == 4`, which is what the "Print Code Menu" hook tests; **needs live verification**.
- **A stronger debug check.** Hash the whole Code Menu block across instances and against a fresh boot. This catches every toggle: Special Modes, per-player character switch, and the rest.

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
- **Brawlback's exclusions are subtracted:**

  | Range | Contents |
  |---|---|
  | 0x804E7C00 + 0xC00 | AX audio buffers |
  | 0x8049A4EA + 0x1400 | AX voice parameter blocks |
  | 0x90000800 + 0x12C800 | framebuffers |

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
| `FT_ENTRY_MANAGER` | `0x80624780` | ftEntryManager {ftEntry* entries @0, u32 count @4} (System heap) | P+ Community/FSMeter.asm; brawlback RollbackManager.cpp:278 / EXIBrawlback.h:145 | live |
| `FT_MANAGER` | `0x80629A00` | ftManager (System heap) | P+ FSMeter.asm, IC-Basics.asm | live |
| `G_FT_ENTRY_MANAGER_PTR` | `0x80B87C48` | sora_melee .bss g_ftEntryManager (should hold 0x80624780) | P+ NETPLAY.TXT .alias; decomp rels/sora_melee bss+0x2E88 with the module at 0x8070A940 | live |
| `GAME_FRAME` | `0x901812A0` | GameFrame {+4 frameCounter, +0xC frameDelta, +0x14 persistentFrameCounter} | BrawlHeaders RSBE01.lst:21 g_GameFrame; brawlback RollbackManager.cpp:28 | live |
| `SC_MELEE_OBJ` | `0x90FF50C0` | scMelee object (also reachable as the current scene in a match) | BrawlHeaders RSBE01.lst:22 | live |
| `GAME_GLOBAL_OBJ` | `0x90181300` | *g_GameGlobal | orca Results.h:24 | live |
| `MODE_MELEE_OBJ` | `0x90180F20` | gmGlobalModeMelee | orca Results.h; BrawlHeaders RSBE01.lst:24 | live |
| `RESULT_INFO_OBJ` | `0x9017F420` | gmResultInfo | orca Results.h | live |
| `SET_RULE_OBJ` | `0x9017F360` | gmSetRule (P+ 'Default Settings Modifier' writes it) | orca Results.h; P+ NETPLAY.TXT:353 | live |
| `RECORD_MENU_DATA` | `0x9017BE50` | gmGlobalRecord+0x810 menu data (+0 item frequency) | orca RSBE01.patches / OnlineRules.cpp:59 | live |
| `RSS_EXDATA` | `0x8042C4E8` | P+ stage-switch data (0x320 bytes, loaded from pf/stage/switch/SwitchFF.rss on netplay) | P+ Source/Netplay/Net-Random.asm; orca RankedPPlus.h | pplus |
| `RSS_PAGES` | `0x8042C524` | STAGE_PAGES: per page {u8 count, u8 slot[39]}, stride 0x28, 5 pages | P+ Net-Random.asm | pplus |
| `RSS_SLOT_KINDS` | `0x8042C5EC` | slot -> {u8 stage kind, u8 cosmetic} (STAGE_SLOTS_COSMETIC) | P+ Net-Random.asm; orca RankedPPlus.cpp:71 | pplus |
| `SSS_CURRENT_PAGE` | `0x80496000` | P+ CURRENT_PAGE (u8) | P+ Net-Random.asm | pplus |
| `CSS_ROSTER` | `0x80680DE0` | P+ CSS icon order: 43 MuSelchkind bytes | P+ ProjectM/CSS.ASM 'CSS Roster Data v3&K' | pplus |
| `CODE_MENU_BASE` | `0x804E0000` | P+ Code Menu data (pf/menu3/dnet.cmnu, 0x2520 bytes) | Brawlback memLocations.txt (data.cmnu read to 0x804e0000); SD card | pplus |
| `CODE_MENU_STATE` | `0x804E0034` | Code Menu state word (4 = menu drawn) | P+ Net-CodeMenu.asm 'Print Code Menu' (cmpwi r31,4) | live |
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

## 7. Needs live verification (checklist)

Run `brawl.probe(client.read_mem)` on a live game at each screen and confirm:

1. **CSS: the "kind" values.** `area+0x1B4` = 1 after a port joins with A, and the hand fields `+0x80`/`+0x90`/`+0x94` move with the stick (about 1.0 unit per frame).
2. **CSS: the character under the hand.** `area+0x1B8` tracks the icon under the hand.
3. **SSS: position under the cursor.** `task+0x248` gives the page position under the cursor and matches the RSS page lists (Battlefield at 0/17, FD at 0/10 with SwitchFF). If it reads −1 while hovering, check `task+0x244 − 1` (`SSS_ITEM_BASE`).
4. **SSS: RSS tables populated.** RSS_EXDATA (0x8042C4E8) is filled from SwitchFF.rss by the time the SSS opens.
5. **Match setup.** gmGlobalModeMelee after the SSS: stage at `+0x1A`, time limit at `+0x20` (expect 28800), player state 0/1/3.
6. **ftEntryManager.** 0x80624780 holds the entries pointer, and `[0x80B87C48] == 0x80624780`. `ftEntry+0x58` (playerNo) equals the port.
7. **Per-player reads** while moving and taking damage: damage (`data+0x24`), stocks (`data+0x34`), position, prevPos (`+0x18`), lr (`+0x40`), status kind (`+0x34`), motion frame/kind, ftKind (`fighter+0x110`).
8. **stOperatorRuleMelee** (`scMelee+0x58`): isStart `+0xDF`, remaining `+0xE0`, elapsed `+0xE4`, isGameSet `+0x74`.
9. **g_GameFrame** at 0x901812A0 and its persistent counter. Compare with Brawlback's checksum fields and settle the damage/stock offset disagreement in Section 2.
10. **Code Menu.** The state word 0x804E0034 reads 4 when the menu is open, and the debug bytes at 0x80583FF4-FFF follow the Debug Mode lines.
11. **seed_css_record.** Do humans come up with their tokens placed?
12. **Heap table** contents per scene (names, starts, sizes), especially the in-match Fighter/Stage/Item instance heaps.
13. **Boot path.** With the template user's save present, no save prompt appears, and A-tapping reaches `scSelctCharacter` with no other input.
