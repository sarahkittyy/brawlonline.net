# Game-side code: toolchain, SD patching and the online menus

This covers the game-side half of design §5 (`docs/backend-design.md`): the C++ plugin that runs inside Project+ v3.2, how it is built and put on P+'s SD card, what the in-game online menus look like today, the connect-code entry research, and the game↔Dolphin mailbox prototype.

Paths:
- Code: `game-code/`, a separate git repo (gitignored here). It is a fork of `brawlback-asm` with full history; see `game-code/NOTICE`.
- Plugin: `game-code/PPOnline/`.
- Scripts: `tools/gamecode/` and `tools/sdcard/`.
- Screenshots: `run/artifacts/game-code/` (local only, `run/` is gitignored).

Everything here was verified on NTSC-U Rev 1 + P+ v3.2, booting the P+ Offline Launcher with headless D3D11 in dual core. The menu screenshots (§6) come from `run/bin/menu-7b2227fd5e` (a frozen copy of `dolphin/build/release/x64/Binaries` at `7b2227fd5e`, no GameBridge, so `drive.py mbx-serve` plays Dolphin's part). `harness/tests/test_online_game.py` passes (4/4) with `run/bin/menu3-42b2129395` (GameBridge, real servers). The Netplay Launcher also loads the plugin and shows the relabelled main menu (`screens/netplay-launcher-main.png`).

**Do not use `rollback-fixes` `d36794a6e1` for menu work.** With that build (`run/bin/menu2-d36794a6e1`) Dolphin's CPU thread blocked about 20 frames after the code keypad opened, in 2 runs of 3 (no CPU use, no presents, the harness times out; once the harness stopped answering too). The same plugin and inputs passed 6 of 6 on `7b2227fd5e` and passed the tests on `42b2129395`.

---

## 1. Toolchain

**What brawlback-asm actually uses.** Not devkitPPC. Syriinge plugins are built with:
- **kuribo-llvm**: DotKuribo's LLVM 13 fork with the `powerpc-gekko-ibm-kuribo-eabi` target, providing `clang` and `ld.lld`;
- **elf2rel**: Sammi-Husky's tool, which links the relocatable ELF against symbol maps (`BrawlHeaders/RSBE01.lst` plus each plugin's `EXTRA.lst`) and writes a Wii `.rel`.

`bbk.py setup` and brawlback-asm's CI fetch both from the `dol-rvl-toolchains` S3 mirror, pinned to kuribo-llvm `377c67c` and elf2rel `b44c714`. The repo's Syriinge README mentions devkitPro, but no build step uses it.

**Install (portable, nothing system-wide):**

```sh
python tools/gamecode/setup_toolchain.py      # stdlib only; same pins and mirror as bbk.py
```

This creates:
- `toolchains/kuribo-llvm-377c67c/bin/{clang,ld.lld}.exe` (231 MB);
- `toolchains/elf2rel-b44c714/elf2rel.exe`;
- `toolchains/MANIFEST.json`, with the URL and sha256 of each download.

The other prerequisites were already on this PC: GNU make 4.4.1 (Chocolatey) and a POSIX shell (Git Bash). The script works on Linux and macOS too, since the mirror has x86_64 Linux and arm64 macOS builds. Only Windows was run.

**Proof: upstream builds as-is.** `game-code` branch `savestate-efficiency` @ `2123165`, with its submodules (BrawlHeaders `12544f8`, OpenRVL `d6aa79c`, brawlback-common `33d728c`):

```sh
cd game-code && ./build.sh all     # = make LLVMDIR=../toolchains/... ELF2REL=... clean all
```

The build produces `Brawlback-Online.rel` (34,272 B) and `sy_core.rel` (7,920 B) with no errors. There are about 700 warnings, all from the headers: `__declspec` is ignored by clang.

Upstream quirk: incremental builds fail with `*.d: missing separator`. Its rule passes `-r` without `-c`, so clang writes a linked object into the `.d` dependency file. Clean builds work. Our `PPOnline/Makefile` uses `-MMD -MP -MF ... -c` instead.

**Building our plugin:**

```sh
cd game-code && ./build.sh          # -> PPOnline/PPOnline.rel (18.5 KB), then reltool check
```

`build.sh` fails the build if any `bl` calls itself without a relocation. This happens when a function is in neither the DOL symbol maps nor our code: we link with `--unresolved-symbols=ignore-all`, so the linker leaves a `bl .`, and the game hangs the first time the call runs. It bit us once: Brawl's DOL has no `memcmp`. The check is `python tools/gamecode/reltool.py check X.rel`.

---

## 2. How P+ v3.2 loads code, and where our plugin goes

The chain, found on the SD card (`run/template-user/Wii/sd.raw`):
1. **Launcher DOL.**
   - `Launcher/Project+ Offline Launcher.dol` loads `/Project+/RSBE01.GCT` + `BOOST.GCT`.
   - `Project+ Netplay Launcher.dol` loads `NETPLAY.GCT` + `NETBOOST.GCT` (the second file comes from `Net-MultiGCT.asm`).
   - The `.txt` sources sit next to them, and `/Project+/Source/**` has every `.asm`. `GCTRealMate.exe` is on the card.
2. **Both codesets include `Source/Community/Syringe.asm`.**
   - "Syringe Core Loader" (HOOK @ `80018074`) creates heap 60 "Syringe" at `0x817BA5A0`, size `0x10000`. It shrinks MenuInstance to make room.
   - It loads `sy_core.rel` from **file 18 of `/Project+/pf/system/common2.pac`**. That archive is LZ11-compressed; the REL is module id 224.
3. **sy_core is Syriinge 0.6.0.**
   - Its strings include "[Syringe] Initializing. (ver. 0.6.0)" and "%spf/%s/*.rel".
   - It loads every `/Project+/pf/plugins/*.rel` (the mod folder is `/Project+/`).
   - P+ ships four plugins there: `AsyncRSP` (v0.5.0), `lavaInjectLoader` (0.6.5), `lavaNeutralSpawns` and `Physics`.
4. **Plugin ABI.**
   - `_prolog(CoreApi*)` runs the constructors, installs hooks through the `CoreApi` vtable, and returns a `PluginMeta*`.
   - That is the same convention as brawlback-asm's `savestate-efficiency` and `project-plus-fork` (`lib/Syriinge`, `SYRINGE_VERSION "0.6.0"`) and as P+'s own plugins (checked by disassembling `lavaInjectLoader._prolog`).
   - brawlback-asm `master` vendors 0.5.1, which uses fixed sy_core offsets instead. It would not load on P+.

**So adding our plugin is one file:** `/Project+/pf/plugins/PPOnline.rel`.
- Nothing else on the card changes, and P+'s codes and plugins keep working.
- Our module id is 20560, which is unique. AsyncRSP and Physics share 202, and lavaInjectLoader uses 8192.
- Dolphin's log confirms the load: `[Syringe] Loaded plugin (PPOnline, v0.1.0)`.

**Hook conflicts.** `python tools/gamecode/pplus_hooks.py ADDR...` parses every `HOOK/op/CODE @` and raw gecko line of the four codesets, following `.include`s (3,176 patch sites). It reports any patch within 0x10 bytes of the given addresses. Every DOL address we hook is clear. The closest is P+'s `CODE @ $800B91C8` inside `MuMsg::printIndex`; we only replace that function's entry, at `0x800B91B8`.

---

## 3. SD card patching

```sh
python tools/sdcard/patch_sd.py SRC DST --plugin game-code/PPOnline/PPOnline.rel     # copy + patch
python tools/sdcard/patch_sd.py --in-place IMAGE --add local.bin=/Project+/x/y.bin   # never the template
python tools/sdcard/patch_sd.py --list IMAGE /Project+/pf/plugins
python tools/sdcard/patch_sd.py --check IMAGE                                         # FAT consistency
```

`tools/sdcard/fat32.py` is a small stdlib FAT32 reader and writer. It handles Dolphin's partitionless images, MBR images and VFAT long names. It can:
- create directories;
- add, replace and delete files;
- update both FATs and the FSInfo free count.

The read side mirrors the launcher's `launcher/src/brawl_assets/fat32.ts`.

`patch_sd.py` copies the source image with the harness's fast copy, then patches only the copy. It refuses to write `run/template-user/Wii/sd.raw` itself. After writing it re-reads every added file and runs a FAT check: every chain valid, no shared clusters, no lost chains.

The writer was checked against an independent implementation (pyfatfs, in a scratch dir only). A test image with a long-name file, a new nested directory and the 34 KB Brawlback rel was listed and read back byte-identical.

`tools/gamecode/ppboot.py` builds on this for testing. It lets the harness copy the template into `run/instances/<name>-<n>`, patches that copy's `sd.raw`, and then launches Dolphin.

---

## 4. Test tooling

| Tool | Use |
|---|---|
| `ppboot.py run --plugin X.rel [--boot netplay] STEPS...` | Boot an isolated instance (muted, headless D3D11, dual core) with the plugin on its SD, run the steps, quit, and delete the dir. On failure the dir is kept with `sd.raw` removed. `boot`/`stop` cover interactive work. |
| `drive.py --port P STEPS...` | The step language: `until SCENE`, `tap BTN N`, `hold`, `wait`, `shot`, `mem`, `@scenario.txt`. Also `mbx-serve` / `mbx-dump`, which act as the Dolphin side of the mailbox (`mbx-serve MAX STATE NAME CODE ERROR TEXT...`; CODE `-` echoes the requested code). |
| `scenarios/to_online.txt`, `online_direct.txt`, `online_unranked.txt`, `online_teams.txt` | The verification runs that produced the screenshots in §6 (`ppboot.py run --plugin game-code/PPOnline/PPOnline.rel @online_direct.txt`). |
| `ppom.py --port P find\|dump\|log\|cfg\|serve` | Finds the PPOM block and dumps the mailbox and debug counters. `log` shows the last 32 `MuMsg::printIndex`/`printf`/`create` calls with their callers, which is how message ids were found. |
| `modules.py --port P` | Loaded RELs with live addresses, from the `OSModuleInfo` list at `0x800030C8`. |
| `reltool.py info\|dis\|relocs\|diff\|check\|dol` | REL/DOL inspection. Optional capstone for disassembly, installed with `pip --target run/scratch/gc/pylib`. |
| `brawlarc.py` | ARC `.pac` reader with LZ10/LZ11 decompression, used to find msbin message files. |
| `pplus_hooks.py`, `montage.py` | Hook-conflict check; screenshot contact sheets. |

The harness accepts one client at a time. That is why the mailbox server is a `drive.py` step, not a second process.

---

## 5. The plugin (`game-code/PPOnline`)

| Hook | Address | Kind | Purpose |
|---|---|---|---|
| `MuMsg::printIndex` | `0x800B91B8` | replace | Relabel message lines. A NULL msbin means the MuMsg's own data. Lines are matched by text content, scene and caller. |
| `gfPadSystem::updateSystem` | `0x8002A210` | replace | Per-frame tick: online menu state, mailbox polling, code entry |
| `MuMsg::beginPrint`, `MuMsg::create` | `0x800B8EE8`, `0x800B8930` | replace | Debug log of `printf` callers and message objects (`CFG_LOG`) |
| WFC/Wiimmfi login bypass | `0x8014B5F8/5FC`, `0x8014B4BC`, `0x8014B3B8`, `0x80033B48`, `0x800CCF70` | simple | Ported from Gen 1 `NetMenu`: logged in, friend code, connection, Mii render, matchmaking error |
| WifiCnctWnd created | `sora_menu_main+0x15288`, `+0x15460`, `+0x1674C`, `+0x1694C` | inline | Mark Brawl's connect window (connect dialog, "Connected.", first-time profile name) finished and connected (`wnd+0x138 = 0xC`, `+0x128 = 0`) as soon as the main menu creates it; the menu then opens the ONLINE page |
| muProcWifiAnybody page enter | `sora_menu_main+0x2E4F8` | replace | After WITH FRIENDS: Gen 1's `SkipDirectlyToCSS` (decision `0x1E` → `sqNetAnyOkiraku`), mode Direct. After WITH ANYONE: the original page (BASIC VERSUS / TEAM BATTLE) |
| muProcWifiAnybody A press | `sora_menu_main+0x2E934`, `+0x2EB70` | inline | The decision picked: `0x1E` BASIC VERSUS = Unranked (`sqNetAnyOkiraku`), `0x1F` TEAM BATTLE = Teams (`sqNetAnyTeamMelee`) |
| muProcWifi A press | `sora_menu_main+0x168B8` (and `+0x166B8`, `+0x16518`) | inline | Turn page id `0x1A` (friends) into `0x1B` (anybody) and remember that FRIENDS was picked |
| muProcWifi enter, cursor | `sora_menu_main+0x1640C` | inline | Back from a Direct CSS: cursor on WITH FRIENDS (`this+0x42` and the stub's saved r31) |
| Wi-Fi rules written | `sora_scene+0x36EA0` (`sqNetAnyOkiraku` state 0), `+0x3B81C` (`sqNetAnyTeamMelee`) | simple | Replace the Wi-Fi rules with the online rules (§6, rules) |
| Leave the CSS for the menus | `sora_scene+0x3770C` | simple | `setNextSequence("sqMenuMain", 0x1C)`: `0x1F` after Direct (ONLINE page), else unchanged (`0x1C` = WITH ANYONE page, BASIC highlighted; Teams' own exit uses `0x1D`, TEAM highlighted) |
| CSS keypad update call | `sel_char+0x18F30` | simple | Our wrapper of `MuSelctChrNameEntry::update` for the connect-code keypad (§7) |
| Wi-Fi CSS countdown, timer, network error, disconnect panel | `sel_char+0x4220`, `+0x56A8`, `+0x53A4`, `+0x4A70` | simple | Ported from Gen 1. The disconnect-panel hook is now one naked hook, without Gen 1's `SaveRegs` |
| Wi-Fi SSS countdown, network error | `sel_stage+0x141C`, `+0x30F0` | simple | Ported from Gen 1; not exercised yet |

Notes:
- P+ v3.2's `sora_menu_main.rel` is vanilla's with the same `.text` size. Its 152 non-relocation word differences are confined to a few P+ edits (`reltool.py diff`), so Gen 1's offsets hold.
- P+ keeps `sel_char` (`.text 0x806828C4`), `sel_stage` (`0x806B0984`), `sora_menu_name` (`0x8067406C`), `sora_scene` (`0x806BB554`) and others resident. That is why Gen 1's absolute jump-back addresses are valid on P+.
- **Resident modules still need module hooks.** They are loaded after the plugins, so an absolute `sySimpleHook` on their code is overwritten when the module loads (checked: the patch log shows it, memory has the original).
- **Never patch an instruction that carries a relocation against a module loaded later.** The `bl MuSelctChrNameEntry::update` at `sel_char+0x18F34` is relocated against module 16. The loader applied that relocation after Syriinge's patch and kept the opcode bits, which turned the patch into `b update` (no link) and hung the game. The hook now sits on the `addi` before it.
- `sora_menu_main` is loaded into MenuInstance at `0x81164C00` while in the menus.
- **Gen 1 bug, not ported.** `turnOffSSSTimer` is installed on `sel_char+0x35A4`, which is a `blr`, but jumps into `sel_stage+0x35A8`. It was meant for `sel_stage+0x35A4`.

### PPOM block (design §5.2)

`include/ppom.h` is the contract, and `tools/gamecode/ppom.py` mirrors it.
- The block lives in the plugin's `.data`. It is 0x674 bytes, aligned to 32, and big-endian.
- Header: magic `"PPOM"`, version 1, then offset/size pairs for MAILBOX, SESSION, LOCAL and DEBUG. The header is 0x24 bytes.
- **How Dolphin finds it** (`Source/Core/Core/Online/GameBridge.cpp`): it walks the game's `OSModuleInfo` list (`0x800030C8`) to the module with our REL id **20560** (`PPOnline/Makefile` `RELID`; do not change it), then looks for the header in that module's data sections only. It does this once per boot. After that it checks the magic word each frame. The harness tools (`ppom.py`) still scan the Syringe heap (`0x817BA5A0..+0x10000`), which is fine for debugging.
- SESSION and LOCAL are declared with size 0 in v1; only the mailbox is prototyped.

**Mailbox.**
- `reqWrite` (game), `reqRead` (Dolphin), `respCount` (Dolphin, bumped last) and `respSeen` (game).
- 4 request slots of 0x80 bytes: `seq`, `cmd`, payload 0x78.
- 1 response of 0x200 bytes: `seq` of the request it answers, `cmd`, `status`, payload 0x1F8.

Payloads follow design §5.3, with Slippi's command bytes. Text is UTF-16BE.

| Command | Payload |
|---|---|
| `0xB4 FIND_OPPONENT` | `{mode, lockedChar, costume, team, code[9]}`. Mode is 0 ranked, 1 unranked, 2 direct, 3 teams. |
| `0xB3 GET_MATCH_STATE` | `{mmState (Slippi ProcessState 0-5), role, sessionPhase, percent, peerName[16], peerCode[9], errorText[120]}` |
| `0xB9 GET_ONLINE_STATUS` | `{state, name[16], code[9]}` |
| `0xBA CLEANUP_CONNECTION` | none |

The game only touches the mailbox outside sessions: menus and the CSS before plug-in. DEBUG holds counters, the `cfg` feature flags and the message log. Neither DEBUG nor the mailbox may be included in state hashes. Dolphin excludes the mailbox from rollback state (`RollbackManager::SetMailboxRegion`) and never services it while netplay runs.

**Protocol rules the game must follow** (agreed with Dolphin's GameBridge, `pponline` `42036d6`):
1. Dolphin services requests at the frame-end boundary. It writes **at most one response per frame**, and only when `respSeen == respCount`, i.e. once the game has taken the previous response. `pollResponse()` every frame (the tick already does).
2. `FIND_OPPONENT` is answered with a `GET_MATCH_STATE` payload. A fresh search reads as `mmState 1`. A refusal reads as `mmState 5` with the text in `errorText`.
3. While searching or connected, **poll `GET_MATCH_STATE`**, as Slippi's CSS does every frame. Keep one poll in flight: post the next only after the answer arrives (re-post after 60 frames without one). The ring has 4 slots, so unthrottled posting overwrites requests.
4. Accept every `GET_MATCH_STATE` answer with `seq >= searchSeq` of the current search, not only the answer to the FIND itself. Forget the search (`searchSeq = 0`) on Z/cleanup and when leaving the menus.
5. Handle `mmState 0` (idle: cleaned up, peer gone) by going back to the idle prompt, as Slippi does (design §5.6). Handle `mmState 4` with `peerName`/`peerCode` as "opponent found".
6. `CLEANUP_CONNECTION` gets no answer. Commands Dolphin does not implement yet (`0xB6`, `0xB8`, `0xBE`, `0xE3`) are answered with `status 0xFF`.

---

## 6. Screens (Slippi → ours)

Screenshots are in `run/artifacts/game-code/screens/`, from the last runs of `online_direct.txt` (01-10), `online_unranked.txt` (11-20) and `online_teams.txt` (21-25). Vanilla P+ references are in `explore/`; the step-by-step discovery shots are in `wifi/`. Slippi's strings below come from its sources (`refs/slippi-ssbm-asm`, `slippi-ssbm-c`, Dolphin) and from its `SdMenu.usd` / `SdSlChr.usd` patches.

| # (design §5.4) | Slippi | What you see now | Status | Screenshot |
|---|---|---|---|---|
| 1 | 1P menu "Online Play": "Compete against online opponents." | Main menu PLAY ONLINE with Slippi's description. The account is not printed: Slippi's online menu does not show it. | done | `01-main-play-online.png` |
| 1 | (no Slippi equivalent) | Brawl's connect dialog, "Connected." and the first-time "Choose a profile name." keypad are skipped: PLAY ONLINE opens the ONLINE page at once. | done | `02-online-direct.png` |
| 1 | Online submenu: Direct "Play a specific person.", Unranked "Play unranked matches.", Teams "Play teams games." | ONLINE page: WITH FRIENDS = Direct ("Play a specific person."), WITH ANYONE ("Compete against online opponents.") opens Brawl's page with BASIC VERSUS = Unranked and TEAM BATTLE = Teams, with Slippi's descriptions. Button art unchanged. | done | `02`, `11`, `12`, `21` |
| 2 | CSS: "Select your character" → "Press START to search / enter code" → "Searching for opponent / ABCD#123" → "Connecting to …" → "Playing: …"; errors in red; Z cancels, hold Z disconnects | Brawl's Wi-Fi CSS with P+'s competitive rules. Header art hidden; one status line with Slippi's strings (below). START works once a character is picked. | done | `03`-`09`, `13`-`19`, `22`, `24` |
| 3 | Connect-code entry: name-tag keyboard in code mode, 8 characters with '#', START confirms | Brawl's name keypad in code mode: 8 characters, alphabet and digits only, upper case, '#' key, unusable keys refused, START confirms, the name tag is untouched (§7). | done; recent codes not done | `05`, `06`, `23` |
| 1 | Leaving: hold B on the CSS → online menu, cursor on the mode; CLEANUP_CONNECTION on menu load | Brawl's hold B (or LEAVE) → the page the mode was picked on, cursor on it; `0xBA` on every menu load. | done | `10`, `20`, `25` |
| 5 | Stage: Unranked random, Direct random then loser picks, Teams random then P1 picks | Rules are set; the stage is picked by the session, which does not exist in the game yet (see below). | open | — |
| 4, 6-10 | Opponent on CSS, ranked setup, rank, results, in-match disconnect, chat | not started | — | — |

**CSS status line.** The Brawl CSS has one text window in its header (the rule line). Slippi shows a mode header, three status lines and a hint; we show the one line that carries the state, in Slippi's words (`LoadCSSText.asm:95-157`):
- idle: "Select your character", then "Press START to search" (Unranked) or "Press START to enter code" (Direct, Teams);
- "Searching for opponent" / "Searching for ADGJ#123", "Connecting to opponent" / "Connecting to ADGJ#123", "Playing: <name>";
- an error: the text from Dolphin or the server, in Slippi's red (`FF0000`), e.g. "Teams is not supported yet. Only Direct works for now." (`24-css-teams-error.png`).
- Not shown: "<Mode> Mode", the hint line ("Press Z to cancel", "Hold Z to disconnect", "Press Z to clear error") and "Use D-Pad to Chat". The window narrows its font to fit, so long errors stay readable (54 characters in `24`); Slippi's 120-character errors would be very narrow.

**CSS header art (item 1).** "HOME-RUN CONTEST" is the Wi-Fi mode title texture `MenSelchrTitleW` (muSelCharTask+0x418) and the "2" is the rule numeral `MenSelchrRnum1/2` (+0x158/+0x15C). Both are images with no text slot, so they are hidden every frame on the online CSS (`nwSMSetVisibility`, `0x80043D20`). The status line keeps the rule line's own window.

**Input on the online CSS (Slippi `HandleInputsOnCSS.asm`).**
- START: lock in and search (Unranked) or open the code keypad (Direct, Teams), only once a character is picked. START is removed from all of the game's pad statuses on the online CSS, so Brawl's own "READY TO FIGHT" start never runs.
- Z: cancels a search or clears an error (`0xBA`, back sound); hold Z 48 frames (`DISCONNECT_HOLD_DELAY 0x30`) disconnects when connected. Error sound when an error arrives, back sound when a connection ends.
- Buttons come from the pad system's own pressed/held fields (`gfPadSystem+0x244`), so short presses are not missed when a game frame spans several pad reads.
- B: Brawl's own. A press takes the coin back; holding B leaves the CSS (Slippi keeps Melee's hold B, unblocked in every state). LEAVE does the same. The menus load with `0xBA`, as Slippi's `OnMenuLoad.asm` does, and reopen where the mode was picked: WITH ANYONE with BASIC VERSUS or TEAM BATTLE highlighted, or the ONLINE page with WITH FRIENDS highlighted (`sqMenuMain` entry `0x1F` plus the cursor hook).
- Not done: Slippi blocks A/B on the character while locked in (`PreventAPress/PreventBPressCharUnselect.asm`); ours still lets the coin move while searching.

**Rules (item 2).** Slippi uses one fixed ruleset in every online mode (`EXI_DeviceSlippi.cpp:2114-2133`): 4 stocks, 8 minutes, items off, real pause only in Direct. P+ v3.2's competitive defaults are its codeset's "Default Settings Modifier" (`RSBE01.txt`, `NETPLAY.txt`), which writes the set rule `0x9017F360 = 00 00 01 00 04 00 0A 00 08 01 01 00`: stock, 4 stocks, damage 1.0, 8-minute stock time limit (P+ uses `stockTimeMinutes` as the stock timer, `Rules.asm`), team attack on, pause on. Items off is item frequency 0 in the menu record (`getGlobalRecordMenuDatap()[0]`, what the ITEM screen edits). The plugin writes these right after `sqNetAnyOkiraku`/`sqNetAnyTeamMelee` write Brawl's Wi-Fi rules (2-minute time, no pause), sets pause on for Direct only, and puts the player's own set rule and item frequency back when the menus load.

**Stage choice (item 2).** Slippi's game picks no stage on the CSS for game 1 in any mode: Unranked, Direct and Teams all lock in with "random", and the stage comes from the server's `stages` list (Dolphin's fallback is FoD, PS, YS, DL, BF, FD), drawn without repeats. After a game, Unranked stays random, Direct's loser picks on the SSS (a draw: both pick), Teams' port-1 player picks, and Ranked uses its strike screen (`HandleInputsOnCSS.asm:160-163, 259-307`, `main.asm:452-491, 757-764`, `EXI_DeviceSlippi.cpp:2179-2186, 2407-2418, 2746-2764`). In our game the CSS never leaves for Brawl's SSS on its own (START is ours), so game 1 is "random" as on Slippi. The pick itself belongs to the session: the server's `stages` list in `Online::Match` (design §5.5), drawn from P+'s legal list. **P+ v3.2's own legal list** is its random-stage switch "Default" preset (`/Project+/pf/stage/switch/Switch00.rss`, identical to the netplay `SwitchFF.rss`): Battlefield, Final Destination, Dream Land, Pokémon Stadium 2, Smashville, Yoshi's Island, Fountain of Dreams, Green Hill Zone, Wario Land, Frigate Husk, Temple of Time, Metal Cavern, Bowser's Castle, Delfino's Secret, Luigi's Mansion (15 on; Yoshi's Story, Castle Siege, Sky Sanctuary Zone, Golden Temple, Ceres Space Colony and Distant Planet listed but off). The other presets (PMBR, 2023/2024 Proposed, Midwest, Australia, Japan) are in `Switch01-06.rss`. Loser's pick on Brawl's SSS for Direct game 2+ needs the results → CSS flow (screen 8), which does not exist yet.

**Teams (item 7).** Teams follows the design's mapping (TEAM BATTLE on the WITH ANYONE page) and uses the same code keypad as Direct. Slippi has no "not supported yet" behaviour: unavailable options are only locked and skipped (`HandleOnlineLockedOptions.asm`), and server refusals are shown as errors. Our server refuses Teams, so the option stays and the server's text shows in red (`24-css-teams-error.png`). The Teams code help text (Slippi: "Enter any code to start a lobby...") has no place on our screen.

Other notes:
- **Status line style.** `MuMsg::printf` does not apply msbin style tags, so we set the colour ourselves with `setFontColor`.
- **Do not print a lone `" "` into the CSS rule window.** It froze the display: presents stopped while the game kept running. This was found by bisecting with `ppom.py cfg`.
- **WITH FRIENDS must not open Brawl's friend page.** Leaving muMenuMain from inside that page froze the display the same way, so the A-press hook reroutes it to the Anybody path instead.
- **Ranked.** There is no third button without new art; Ranked stays out of the menus until the user decides where it goes (P4).

---

## 7. Connect-code entry: findings

Static research (sub-agent) plus live checks. Scratch notes are in `run/scratch/gc/codeentry/`.

**One keypad widget does all of Brawl's text entry.**
- It is `MuSelctChrNameEntry` (BrawlHeaders `mu/selchar/mu_select_character_name_entry.h`), a 0x94-byte helper in the **resident module `sora_menu_name` (id 16)**.
- Users: the CSS name tag "New entry", Vs > Names, the first-time Wi-Fi profile name, friend nicknames, and Stage Builder names.
- Its functions, as `.text` offsets with runtime addresses on P+:

  | Function | Offset → address |
  |---|---|
  | ctor | `0x3B4` → `0x80674420` |
  | init | `0x44C` |
  | `open(this, initial, extBuf, maxChars)` | `0x5A8` → `0x80674614` |
  | `update(this, pad, outUtf8, allowRandom, se)` | `0x998` (returns 0 OK / 1 cancel / 2 editing / 3 inactive) |
  | close | `0xAF8` |

- **Max length is a parameter.** Every caller passes 5, and Stage Builder passes 16 and 20. The 5-character limit lives in the callers and in the save record's name entry (`0x8004D334`, `cmpwi r3,5`), not in the widget. The underline animation only has 5 positions.
- **Layouts.**
  - It is a phone-style multi-tap keypad.
  - English pages cycle alphabet → accented → hiragana → katakana → **digits** (live, `codeentry/kb-cycle-*.png`).
  - The key table is 55 `{isDakuten, utf8}` entries at `0x8067BEB0` (5 pages × 11 keys).
  - There is **no `#`/`＃` anywhere**.
  - Output is full-width UTF-8 ("ＡＢＣ").
- **The friend-code pad** (`sora_menu_friend_list`, id 14, not resident) is numeric-only, 12 digits, and does not use this widget. It is not usable for codes.
- **Don't reuse Vs > Names** (`muNameTask`): its OK writes the save file's name list.

**Connect-code mode (`code_entry.cpp`).** Slippi opens Melee's name-tag keyboard from the CSS in "connect code mode" (`TextEntryScreen/*`). We open Brawl's keypad the way the CSS opens it for a new name tag:
- The CSS's "New entry" is `sel_char+0x18E1C`: `area+0x400 = area+0x1DC`, then `helper(area+0x370).open(NULL, NULL, 5)`, then the hand goes to mode 8 with `sel_char+0x1A348`. `code_entry.cpp` makes the same calls with `open(NULL, ourBuffer, 8)`. It re-asks for mode 8 for up to 30 frames, because the hand sometimes refuses right after START.
- **8 characters** with '#', as Slippi (`Allow8Characters.asm`). The text field's font narrows to fit (`MuMsg::setFontWidthModeAuto`); the underline animation only has 5 positions.
- **Pages:** alphabet and digits only (`helper+0x24` page list = {2, 3}, `+0x38` count = 2, `+0x3C` current), as Slippi forces the English layout. Restored on close.
- **Keys:** the key table (`0x8067BEB0`, shared by every keypad) gets upper-case strings for the alphabet keys and "＃" on the symbols key while the keypad is ours, restored on close. The labels are the game's textures and stay as they are, so the '#' key is labelled "@()~;". The keys that cannot be part of a code (`!?&%$`, `・,./~`, `-+×=`) are refused with the error sound.
- **Buttons:** START confirms (Slippi: "Start = A on Confirm"); Confirm with an empty field plays the error sound and stays (`OnConfirmButtonHandler.asm:30-42`); B deletes, and on an empty field goes back to the CSS (vanilla; also Slippi); Random is off.
- **The name tag is untouched.** The CSS's call of `update` (`sel_char+0x18F34`) goes through our wrapper, which gives the widget our own output buffer and turns OK into "cancel" for the CSS. The CSS never runs its new-name path, so no 8-character copy into its 5-character stack buffer and no reserved-name check either.
- The code is sent as typed, '#' included ("ADGJ#123"). The client does not check it; neither does Slippi's.
- Verified: `05-direct-code-keypad.png` → `06-direct-code-typed.png` → `07-direct-searching.png`; the mailbox received `FIND_OPPONENT mode=direct code='ADGJ#123' char=0x7`.

**Recent codes (item 5): not implemented.** Slippi's history: as you type, Dolphin suggests the newest matching code (`0xBE FETCH_CODE_SUGGESTION`, prefix-filtered, `EXI_DeviceSlippi.cpp:1992-2106`), shown as grey completion text; L/R scroll older/newer, Z accepts and jumps to Confirm (error sound without a suggestion), B on an empty field still leaves. History files are `direct-codes.json` / `teams-codes.json` (`{connectCode, lastPlayed}`, newest first, no limit), updated when a search starts. All of it needs `0xBE` in the mailbox, whose request/response layout the Dolphin side has not defined yet (GameBridge answers `0xBE` with `status 0xFF`), and the grey text needs a second colour in the keypad's text field. Without new art it is possible: the suggestion can be drawn in the field's own window in a grey font colour.

---

## 8. Mailbox round trip: Dolphin plays its part

Dolphin services the mailbox itself (`GameBridge`, dolphin branch `game-bridge`; design §5.2 has the details). No harness server is involved. Verified by `harness/tests/test_online_game.py` against our `accounts` + `mm` servers. Each instance has its own `user.json` and its own copy of the SD card, patched with `PPOnline.rel` through `tools/sdcard/patch_sd.py`. Screenshots are in `run/artifacts/game-bridge/`.
1. **Main menu.** The plugin posts `0xBA` (Slippi cleans up on every menu load) and `0xB9`. Dolphin answers `0xB9` from `user.json` and the accounts lookup. The menus no longer print the account: Slippi's online menu does not show it either (`OnMenuLoad.asm:34-37`).
2. **Direct.** The test picks a character, START opens the keypad, and the test types the other account's code, '#' included. Typing needs these rules: the cursor does not wrap; UP from row 1 reaches backspace; the same key twice needs a move off and back, because there is no multi-tap timeout. START confirms and posts `0xB4 {mode 2, lockedChar, code}`. Dolphin starts the search and answers `mmState 1`, and the header reads "Searching for BOB#610" (`direct/game-a/05-searching.png`).
3. **Z cancels.** The game posts `0xBA` and Dolphin cleans up. The header goes back to "Press START to enter code" (`06-cancelled.png`).
4. **Match.** The other player searches, and START opens the keypad again for the second search. The server pairs them (mm log `matched`). Both games are told `mmState 4` with the peer's name, code and role, and show "Playing: bob" / "Playing: alice" (`07-opponent.png`). `Online::Session::Start` was called on both sides with the match (harness `record` backend).
5. **Unranked.** The server refuses the ticket. The game gets `mmState 5` with "Unranked is not supported yet. Only Direct works for now." and prints it in the header (`unranked/game-u/05-unranked-error.png`). Z clears the error.
6. **Hand-off to netplay.** With the real backend (whole-machine netplay, design §5.1 A), both Dolphins stop their games right after the match and boot P+ together under rollback. The plugin is loaded again and the block is found again, but servicing is paused while netplay runs (`direct-netplay/*/06-netplay-boot.png`).

`drive.py mbx-serve` and `ppom.py serve` still work for experiments without a server. Turn Dolphin's servicing off first: `ppharness cmd --port P game_bridge_config enabled=false`.

---

## 9. Next steps

1. Recent codes through `0xBE` once its payload is agreed with the GameBridge side (§7).
2. CSS: block A/B on the character while locked in (Slippi `PreventAPress/PreventBPressCharUnselect.asm`); show the opponent on the CSS after plug-in (screen 4).
3. Stage choice in the session: random from the server list for game 1; Direct's loser picks on P+'s SSS after the results → CSS flow (screen 8).
4. Ranked placement once the user decides (§6).
5. SESSION/LOCAL blocks. MAILBOX is serviced by Dolphin now (§8); SESSION/LOCAL need the gameplay-only session.

---

## 10. Repos and commits

- `game-code` (fork of brawlback-asm, branch `pponline`; upstream refs under `refs/remotes/upstream/*`):
  - `0a4c153`: plugin skeleton, PPOM block, hello world.
  - `fb4fd87`: Wi-Fi flow, mailbox, Direct code entry.
  - `42036d6`: `GET_MATCH_STATE` polling and the protocol rules for Dolphin's GameBridge (§5).
  - `75d3647`: Slippi-style menus: no connect dialogs, Teams entry, P+ competitive rules, header art hidden, Slippi's strings and CSS input, leaving the CSS, the 8-character code keypad.
- Top-level repo: `docs/game-code.md`, `tools/gamecode/`, `tools/sdcard/`. `.gitignore` already listed `/game-code/` and `/toolchains/`.
