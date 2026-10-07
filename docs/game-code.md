# Game-side code: toolchain, SD patching and the online menus

This covers the game-side half of design §5 (`docs/backend-design.md`): the C++ plugin that runs inside Project+ v3.2, how it is built and put on P+'s SD card, what the in-game online menus look like today, the connect-code entry research, and the game↔Dolphin mailbox prototype.

Paths:
- Code: `game-code/`, a separate git repo (gitignored here). It is a fork of `brawlback-asm` with full history; see `game-code/NOTICE`.
- Plugin: `game-code/PPOnline/`.
- Scripts: `tools/gamecode/` and `tools/sdcard/`.
- Screenshots: `run/artifacts/game-code/` (local only, `run/` is gitignored).

Everything here was verified on NTSC-U Rev 1 + P+ v3.2, booting the P+ Offline Launcher with headless D3D11 in dual core. The menu screenshots (§6) come from `run/bin/menu-7b2227fd5e` (a frozen copy of `dolphin/build/release/x64/Binaries` at `7b2227fd5e`, no GameBridge, so `drive.py mbx-serve` plays Dolphin's part). `harness/tests/test_online_game.py` passes (6/6, including the recent-codes and character-lock tests) with `run/bin/menu4-9fe894ab5b` (`42b2129395` plus `9fe894ab5b`, which answers `0xBE`; GameBridge, real servers); the recent-codes and CSS-lock screenshots are in `run/artifacts/game-code/recent-codes/` and `css-lock/`. The Netplay Launcher also loads the plugin and shows the relabelled main menu (`screens/netplay-launcher-main.png`).

**Do not use `rollback-fixes` `d36794a6e1` for menu work.** With that build (`run/bin/menu2-d36794a6e1`) Dolphin's CPU thread blocked about 20 frames after the code keypad opened, in 2 runs of 3 (no CPU use, no presents, the harness times out; once the harness stopped answering too). The same plugin and inputs passed 6 of 6 on `7b2227fd5e` and passed the tests on `42b2129395`.

**Not reproduced on `menu4-9fe894ab5b` with plugin `9b1ba11`** (evidence in `run/artifacts/game-code/keypad-block-check/`):
- 12 keypad runs: boot, Direct CSS, keypad open, then 40 s (about 2,950 frames) watched on one connection, then 5 fresh connections each with a screenshot.
  - 10 of 10 completed runs passed. `status.frame`, `input_polls` and `presentation.presents` advanced about 60 per second in every sample, and the harness answered every new connection.
  - Runs 4 and 11 lost the harness connection: a connect or `status` timed out. Both times fall inside host-wide loopback outages that an independent probe recorded. The probe is a plain Python socket pair, not Dolphin; it saw 21 s connect timeouts from 07:57:10 to 07:59:30 and from 08:08:52 to 08:10:45. In the same windows the local accounts server and Postgres also timed out.
  - Thread stacks taken right after both failures show Dolphin running: the CPU thread in JIT code or in `CoreTiming::Throttle` → `PrecisionTimer::SleepUntil`, the video thread waiting for work in the GPU loop, and the harness threads idle in `select`. The stacks come from `tools/stacks.py` (dbghelp) and were symbolised against the next build's PDB by code matching, `tools/symb.py`.
- 15 minutes with the keypad open, a new harness connection per command (screenshot, `online_recent_codes`, 4 × 32 KB `read_mem`, `status`; 1,877 operations): no failure, and every status advanced.
- 3 × 150 s idle on the online CSS with a screenshot: all fine.
- Two early stalls are still unexplained. Both were in dev instances with the previous plugin, on the online CSS without the keypad, shortly after a screenshot. Dolphin's CPU use dropped to about 0, and the harness stayed occupied by a client that had already gone. Their stacks could not be taken: the Windows Kits `cdb` on this machine fails to start (`0xC0000138`). They did not come back in any of the runs above. `hunt.py` and `churn.py` in the evidence folder record CPU use and thread stacks automatically if it happens again.

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
| `ppom.py --port P find\|dump\|log\|cfg\|serve` | Finds the PPOM block and dumps the mailbox (requests decoded, including `0xBE`, and the last response) and debug counters. `log` shows the last 32 `MuMsg::printIndex`/`printf`/`create` calls with their callers, which is how message ids were found. `serve --codes A,B,...` also answers `0xBE` from that list with Dolphin's search (`ppom.suggest`). |
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
| CSS keypad update call | `sel_char+0x18F30` | simple | Our wrapper of `MuSelctChrNameEntry::update` for the connect-code keypad and its recent codes (§7) |
| CSS character lock | `sel_char+0x726C` (B pressed), `+0x7280` (costume buttons), `+0x75B4` (A on the coin) | simple | While locked in, B does not take the coin back, X/Y do not change the costume, A does not pick up or drop the coin (§6, Input on the online CSS) |
| Wi-Fi CSS countdown, timer, network error, disconnect panel | `sel_char+0x4220`, `+0x56A8`, `+0x53A4`, `+0x4A70` | simple | Ported from Gen 1. The disconnect-panel hook is now one naked hook, without Gen 1's `SaveRegs` |
| Wi-Fi SSS countdown, network error | `sel_stage+0x141C`, `+0x30F0` | simple | Ported from Gen 1 |
| sqNetAnyOkiraku state 3 (stage select) | `sora_scene+0x36F64` | simple | Online match: no stage select, set the match up at once; Direct loser's pick: open P+'s stage select the Versus way (§11) |
| sqNetAnyOkiraku state 4 (after the stage select) | `sora_scene+0x36F8C` | simple | The loser's pick: keep the stage, back to the CSS locked in with it |
| sqNetAnyOkiraku state 5 (training room setup) | `sora_scene+0x36FDC` | simple | Fallback: set the online match up instead of Brawl's training room |
| sqNetAnyOkiraku state 10 (after the match) | `sora_scene+0x37580` | simple | Straight back to the online CSS, no results screen (Slippi) |

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
- **Version 2** (gameplay session): MAILBOX, then LOCAL right after it (Dolphin excludes both from rollback as one range), SESSION and DEBUG. §11 has SESSION and LOCAL.

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
| `0xBE FETCH_CODE_SUGGESTION` | request `{mode u8, scroll u8, inputLen u8, pad, index u32, input u16[9]}` (0x1C); response `{found u8, len u8, pad[2], index u32, code u16[9]}` (0x1C) |

`0xBE` is Slippi's `handleNameEntryLoad` (§7, Recent codes):
- Request: `mode` is 2 (Direct) or 3 (Teams) and picks the history. `scroll` uses Slippi's `AutoComplete.s` constants: 0 none, 1 older (L), 2 newer (R), 3 reset (keypad opened, a character typed or deleted). `inputLen` is the number of typed characters, `input` those characters (UTF-16, full-width or ASCII). `index` is the current suggestion's index from the last answer (0 before any).
- Search: older looks from `index+1` on, newer from `index-1` down, reset from 0, none from `index`; codes that do not start with the input (case-insensitive) are skipped; if the scroll runs off the list, the current suggestion stays if it still matches. Logged out: never found.
- Response (`cmd 0xBE`, `status 0`): `found` 1 with the whole code in upper-case ASCII (`'A'` = `0x0041`, `'#'` = `0x0023`), its index in the history (newest = 0) and its length; `found` 0 echoes the input and the request's index.
- The histories are Slippi's files, one set per account: `<User>/Online/<uid>/direct-codes.json` and `teams-codes.json`. Dolphin adds a code when it gets a Direct/Teams `FIND_OPPONENT`. The harness command `online_recent_codes {mode, clear?, add?, prefix?, index?, scroll?}` lists, clears, seeds and queries them.

The game only touches the mailbox outside sessions: menus and the CSS before plug-in. DEBUG holds counters, the `cfg` feature flags and the message log. Neither DEBUG nor the mailbox may be included in state hashes. Dolphin excludes the mailbox from rollback state (`RollbackManager::SetMailboxRegion`) and never services it while netplay runs.

**Protocol rules the game must follow** (agreed with Dolphin's GameBridge, `pponline` `42036d6`):
1. Dolphin services requests at the frame-end boundary. It writes **at most one response per frame**, and only when `respSeen == respCount`, i.e. once the game has taken the previous response. `pollResponse()` every frame (the tick already does).
2. `FIND_OPPONENT` is answered with a `GET_MATCH_STATE` payload. A fresh search reads as `mmState 1`. A refusal reads as `mmState 5` with the text in `errorText`.
3. While searching or connected, **poll `GET_MATCH_STATE`**, as Slippi's CSS does every frame. Keep one poll in flight: post the next only after the answer arrives (re-post after 60 frames without one). The ring has 4 slots, so unthrottled posting overwrites requests.
4. Accept every `GET_MATCH_STATE` answer with `seq >= searchSeq` of the current search, not only the answer to the FIND itself. Forget the search (`searchSeq = 0`) on Z/cleanup and when leaving the menus.
5. Handle `mmState 0` (idle: cleaned up, peer gone) by going back to the idle prompt, as Slippi does (design §5.6). Handle `mmState 4` with `peerName`/`peerCode` as "opponent found".
6. `CLEANUP_CONNECTION` gets no answer. Commands Dolphin does not implement yet (`0xB6`, `0xB8`, `0xE3`) are answered with `status 0xFF`.
7. Responses to different commands share the one response slot: route them by `cmd` (and `seq`). The keypad keeps at most one `0xBE` in flight. What the player asks for meanwhile (a new character, L, R) waits in a small queue and is sent when the answer arrives, with the text and index of that moment. A request is sent again after 120 frames without an answer. A `0xBE` answer with `status != 0` means no suggestions.

DEBUG `scratch` words the tests read: `[0]` `0xBE` requests sent; `[1]` CSS lock (bit 0) and the CSS phase (`<< 4`); `[2]` Z accepts `<< 24`, last answer found `<< 16`, its index; `[3]` online rules applied; `[4]` keypad 1 open, 2 OK, 3 closed; `[5]` the hand's mode (8 = keypad); `[6]`-`[9]` menu hooks (`netmenu.cpp`).

---

## 6. Screens (Slippi → ours)

Screenshots are in `run/artifacts/game-code/screens/`, from the last runs of `online_direct.txt` (01-10), `online_unranked.txt` (11-20) and `online_teams.txt` (21-25). Vanilla P+ references are in `explore/`; the step-by-step discovery shots are in `wifi/`. Slippi's strings below come from its sources (`refs/slippi-ssbm-asm`, `slippi-ssbm-c`, Dolphin) and from its `SdMenu.usd` / `SdSlChr.usd` patches.

| # (design §5.4) | Slippi | What you see now | Status | Screenshot |
|---|---|---|---|---|
| 1 | 1P menu "Online Play": "Compete against online opponents." | Main menu PLAY ONLINE with Slippi's description. The account is not printed: Slippi's online menu does not show it. | done | `01-main-play-online.png` |
| 1 | (no Slippi equivalent) | Brawl's connect dialog, "Connected." and the first-time "Choose a profile name." keypad are skipped: PLAY ONLINE opens the ONLINE page at once. | done | `02-online-direct.png` |
| 1 | Online submenu: Direct "Play a specific person.", Unranked "Play unranked matches.", Teams "Play teams games." | ONLINE page: WITH FRIENDS = Direct ("Play a specific person."), WITH ANYONE ("Compete against online opponents.") opens Brawl's page with BASIC VERSUS = Unranked and TEAM BATTLE = Teams, with Slippi's descriptions. Button art unchanged. | done | `02`, `11`, `12`, `21` |
| 2 | CSS: "Select your character" → "Press START to search / enter code" → "Searching for opponent / ABCD#123" → "Connecting to …" → "Playing: …"; errors in red; Z cancels, hold Z disconnects | Brawl's Wi-Fi CSS with P+'s competitive rules. Header art hidden; one status line with Slippi's strings (below). START works once a character is picked. | done | `03`-`09`, `13`-`19`, `22`, `24` |
| 3 | Connect-code entry: name-tag keyboard in code mode, 8 characters with '#', START confirms; recent codes as grey completion text, L/R scroll, Z accepts | Brawl's name keypad in code mode: 8 characters, alphabet and digits only, upper case, '#' key, unusable keys refused, START confirms, the name tag is untouched; the newest matching recent code in grey after the typed characters, L/R older/newer, Z accepts and jumps to OK (§7). | done | `05`, `06`, `23`; `recent-codes/` |
| 1 | Leaving: hold B on the CSS → online menu, cursor on the mode; CLEANUP_CONNECTION on menu load | Brawl's hold B (or LEAVE) → the page the mode was picked on, cursor on it; `0xBA` on every menu load. | done | `10`, `20`, `25` |
| 4 | Opponent found: both locked in, the match starts; no opponent's character on the CSS (Slippi shows it only on its VS splash) | The match starts from the CSS once both are locked in; "Waiting on opponent" / "Playing: <name>" | done (§11) | `gameplay-set/` |
| 5 | Stage: Unranked random, Direct random then loser picks, Teams random then P1 picks | Game 1 random from P+'s legal list (the host's Dolphin draws it); Direct game 2+: the loser presses START, picks on P+'s stage select, comes back locked in | done for 1v1 (§11) | `gameplay-set/` |
| 8 | Results → back to the CSS still connected | Straight back to the online CSS after game set (Slippi skips the results screen online), still connected; START locks in for the next game | done (§11) | `gameplay-set/` |
| 9 | "DISCONNECTED", back to the CSS | In a match: the error sound, the game ends (Brawl's "GAME!"), back to the CSS idle; DISCONNECTED as a red Dolphin OSD message for now. On the CSS: back to the idle prompt | done, with the gaps in §11 | `gameplay-leave/` |
| 6, 7, 10 | Ranked setup, rank, chat | not started | — | — |

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
- B: Brawl's own. A press takes the coin back (not while locked in, below); holding B leaves the CSS (Slippi keeps Melee's hold B, unblocked in every state). LEAVE does the same. The menus load with `0xBA`, as Slippi's `OnMenuLoad.asm` does, and reopen where the mode was picked: WITH ANYONE with BASIC VERSUS or TEAM BATTLE highlighted, or the ONLINE page with WITH FRIENDS highlighted (`sqMenuMain` entry `0x1F` plus the cursor hook).
- **Character lock** (Slippi `PreventAPressCharUnselect.asm`, `PreventBPressCharUnselect.asm`, `PreventColorChange.asm`). Slippi is locked in from the lock-in that starts a search (`MSRB_IS_LOCAL_PLAYER_READY`, i.e. `local_selections.is_character_selected`) until `CLEANUP_CONNECTION` clears it: Z on a search or an error, hold Z when connected, the menus loading, or Dolphin's own cleanup when the peer goes. While locked, A and B on the character do nothing and the costume does not change; hold B still leaves. Ours is locked while the CSS is searching, connecting, connected or shows an error (an error stays locked until Z clears it, as on Slippi). `mmState 0` (idle) unlocks.
  - Brawl's CSS handles one player's pad in `sel_char+0x6FFC` (its pad is a `getSysPadStatus` copy).
  - `+0x726C` computes "B pressed" for `+0x74F4`, which takes the coin back to the hand: our hook makes it 0 while locked.
  - `+0x7280` tests the two costume buttons (X/Y on the GameCube pad): while locked we jump to `+0x74F4`.
  - `+0x75B4` calls `+0x8500`, which handles A over the character grid (drop the held coin on a character, pick up a placed coin): skipped while locked.
  - Holding B to leave is the hand's own counter (`sel_char+0x1B794`, held B of the sys pad) and is not touched. A on LEAVE and the other buttons goes through `+0x7A94`/`+0x883C` and still works.
  - Verified: `run/artifacts/game-code/css-lock/game-l/` (searching: A, B, X, Y change nothing; after Z, B takes the coin back and A puts it down; locked again, hold B leaves) and `test_unranked_shows_the_server_error` (locked while the error shows, unlocked after Z).

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

**Recent codes (item 5).** Slippi's history: Dolphin suggests the newest code that starts with what was typed (`0xBE FETCH_CODE_SUGGESTION`, `EXI_DeviceSlippi.cpp:1972-2106`), shown as grey completion text. History files are `direct-codes.json` / `teams-codes.json` (`{connectCode, lastPlayed}`, newest first, no limit), updated when a search starts. Ours, in `code_entry.cpp`, with the `0xBE` layout of §5:

| Slippi (`TextEntryScreen/`) | Ours |
|---|---|
| `NameEntryThinkOneShot.asm`: fetch (reset) as the keypad appears | `CodeEntry::open` asks (reset) with an empty input |
| `OnEnterText.asm`: a character typed → fetch (reset) | Any change of the keypad's text after its update (a character typed, cycled by multi-tap, erased, or cleared by holding B) → ask (reset) with the new text |
| `OnBPressAutoComplete.asm`: B with typed characters deletes one and fetches (reset); B on an empty field leaves | The keypad's own B (erase; on an empty field back to the CSS), then the change above asks again |
| `OnLPress.asm` / `OnRPress.asm`: L older, R newer; they replace Melee's L/R (cursor left/right in the text) | L older, R newer. Brawl's keypad has no L/R handling to replace: its decoder (`text+0x1670`) reads A, B, START and held B, its cursor code (`+0xBBC`) the d-pad/stick and START. L, R and Z are taken out of the pad copy the keypad gets. |
| `CheckTriggersAndZ.asm`: Z with a suggestion → the text is the suggestion, the selector goes to Confirm, success sound; without one → error sound | Z with a suggestion that adds characters → the keypad's text becomes the whole code (full-width, as its keys type it), the highlight moves to OK the way the keypad's cursor code moves it, success sound (SE 1); otherwise the error sound (SE 3) |
| `HandleAutocompleteText.asm`: characters past the typed count drawn in `0x8E9196` | The field is reprinted when it changes: `MuMsg::beginPrint`, the typed text in the window's own colour (white), then the `Message`'s two colour commands (`0x8006A170`/`0x8006A28C`) with `8E9196FF` and the rest of the code. Same window, same font, no new art. |
| `OnConfirmButtonHandler.asm`: Confirm with 0 typed → error; otherwise the suggestion is cut off at the typed count before the search | The keypad's buffer only ever holds the typed characters (the suggestion is only drawn), so START/OK sends exactly those; empty → error sound |

- Dolphin adds the code to the history when it gets the `FIND_OPPONENT` (as `startFindMatch` does), so the code just searched for is the newest suggestion next time.
- Teams uses `teams-codes.json` (`mode 3`). Logged out, or a Dolphin without `0xBE`: no suggestion, everything else works.
- The keypad still shows Brawl's "Z RANDOM" label above it (part of its model; the random name is off). Slippi shows its own L/R/Z help there; we have no such art, so the label stays.
- Verified with `test_recent_codes_on_the_keypad` against the real servers, screenshots in `run/artifacts/game-code/recent-codes/game-r/`: `01-keypad-suggestion` (ABCD#999 in grey from the seeded history), `02-typed-AD` (AD white, GJ#123 grey), `03`/`04-L-older` (ADGJ#123, CARL#123), `05-R-newer` (ADGJ#123), `06-z-accepted` (white, OK highlighted), `07-searching-accepted` ("Searching for ADGJ#123", Dolphin logged `FIND_OPPONENT mode=2 code='ADGJ#123'`), `08-typed-CA` (CA + RL#123 grey; START searched for `CA`), `09-no-history`.

**The keypad's internals** (`MuSelctChrNameEntry`, `sora_menu_name` text `+0x44C`..`+0x1A70`, from the disassembly of the live module):

| Offset | Meaning |
|---|---|
| `+0x00` | active |
| `+0x04`, `+0x08` | text buffer (UTF-8), max characters |
| `+0x24`..`+0x3C` | page list, page count (`+0x38`), current page (`+0x3C`) |
| `+0x40` | highlighted key: 0 erase, 1-11 characters, `0xC` page, `0xD` OK |
| `+0x44` | keypad model; the highlight is its material frame `key+1` (`setFrameMatCol`) |
| `+0x48`..`+0x58`, `+0x78` | page models, the current one |
| `+0x5C`, `+0x60` | selector model; underline model (frame `cursor+1`) |
| `+0x64`, `+0x70` | the text field's `MuMsg` and window |
| `+0x7C` | last character committed (0: multi-tap pending, the same key cycles it) |
| `+0x80` | where UP from OK goes back to (START sets `0xC`) |
| `+0x84` | cursor (character index) |
| `+0x8C`, `+0x90` | hold-B counter (35 frames clears the text), pad port |

`update` (`+0x998`) runs the input decoder (`+0x1670`: B → erase or cancel, A → key action, START on OK → OK, held B → clear), the action (`+0xE18`: 1 OK, 2 cancel, 3 character with multi-tap, 4 erase, 5 page, 6 clear) and the cursor (`+0xBBC`). Every edit reprints the field with `MuMsg::printf(+0x64, +0x70, text)`. The key table is `{isDakuten, utf8}` × 11 per page at `sec5+0x490` (`0x8067BEB0`). The random-name button is Z on the GameCube pad (mask table `sec4+0x48`), off in code mode.

---

## 8. Mailbox round trip: Dolphin plays its part

Dolphin services the mailbox itself (`GameBridge`, dolphin branch `game-bridge`; design §5.2 has the details). No harness server is involved. Verified by `harness/tests/test_online_game.py` against our `accounts` + `mm` servers. Each instance has its own `user.json` and its own copy of the SD card, patched with `PPOnline.rel` through `tools/sdcard/patch_sd.py`. Screenshots are in `run/artifacts/game-bridge/`.
1. **Main menu.** The plugin posts `0xBA` (Slippi cleans up on every menu load) and `0xB9`. Dolphin answers `0xB9` from `user.json` and the accounts lookup. The menus no longer print the account: Slippi's online menu does not show it either (`OnMenuLoad.asm:34-37`).
2. **Direct.** The test picks a character, START opens the keypad, and the test types the other account's code, '#' included. Typing needs these rules: the cursor does not wrap; UP from row 1 reaches backspace; the same key twice needs a move off and back, because there is no multi-tap timeout. START confirms and posts `0xB4 {mode 2, lockedChar, code}`. Dolphin starts the search and answers `mmState 1`, and the header reads "Searching for BOB#610" (`direct/game-a/05-searching.png`).
3. **Z cancels.** The game posts `0xBA` and Dolphin cleans up. The header goes back to "Press START to enter code" (`06-cancelled.png`).
4. **Match.** The other player searches, and START opens the keypad again for the second search. The server pairs them (mm log `matched`). Both games are told `mmState 4` with the peer's name, code and role, and show "Playing: bob" / "Playing: alice" (`07-opponent.png`). `Online::Session::Start` was called on both sides with the match (harness `record` backend).
5. **Unranked.** The server refuses the ticket. The game gets `mmState 5` with "Unranked is not supported yet. Only Direct works for now." and prints it in the header (`unranked/game-u/05-unranked-error.png`). Z clears the error.
6. **The gameplay session (the default).** Both games stay on their online CSS; the match starts from there (§11). `test_direct_set_under_the_gameplay_session` plays a two-game Direct set; `test_opponent_leaves_in_the_middle_of_a_game` the disconnect.
7. **Hand-off to netplay (the fallback, `[Online] SessionBackend = netplay`).** Both Dolphins stop their games right after the match and boot P+ together under rollback. The plugin is loaded again and the block is found again, but servicing is paused while netplay runs (`direct-netplay/*/06-netplay-boot.png`).
8. **As a player does it: launcher and Dolphin.exe.** `harness/tools/e2e_launcher.py` runs the same flow from two built launchers. Each logs in through the launcher's quick start and presses Play. The launcher installs `PPOnline.rel` on its own patched copy of the user's SD card and passes it with `-C Dolphin.General.WiiSDCardPath=...`. Each `Dolphin.exe` (Qt, D3D11, muted, main window "Brawl Online") is then driven from the main menu to a two-game set under the gameplay session (`harness/tools/online_set.py`; `run/artifacts/e2e-launcher/<run>/`).

`drive.py mbx-serve` and `ppom.py serve` still work for experiments without a server. Turn Dolphin's servicing off first: `ppharness cmd --port P game_bridge_config enabled=false`.

---

## 9. Next steps

1. The CSS after a match or the stage select: put the coin back on the character (§11, open issues).
2. DISCONNECTED in the game's HUD instead of Dolphin's OSD, and an LRAS-type end without "GAME!" (§11).
3. Name tags and their controls as the design's port values (§11).
4. Ranked placement once the user decides (§6). Teams goes to the code-based side (user, 2026-10-07); nothing here is built for it.

---

## 10. Repos and commits

- `game-code` (fork of brawlback-asm, branch `pponline`; upstream refs under `refs/remotes/upstream/*`):
  - `0a4c153`: plugin skeleton, PPOM block, hello world.
  - `fb4fd87`: Wi-Fi flow, mailbox, Direct code entry.
  - `42036d6`: `GET_MATCH_STATE` polling and the protocol rules for Dolphin's GameBridge (§5).
  - `75d3647`: Slippi-style menus: no connect dialogs, Teams entry, P+ competitive rules, header art hidden, Slippi's strings and CSS input, leaving the CSS, the 8-character code keypad.
  - `9b1ba11`: recent connect codes on the keypad (`0xBE FETCH_CODE_SUGGESTION`: grey completion, L/R, Z), the character lock while searching (A/B/costume), DEBUG scratch words for the tests.
  - `441eb31`: online matches from the CSS under the gameplay session (sqNetAnyOkiraku hooks, the match setup from SESSION, Direct's loser picks, back to the CSS after game set), PPOM v2 SESSION/LOCAL (§11).
  - `3c0fdb6`: the game after a disconnect ends through `ftManager::setDead`.
- Top-level repo: `docs/game-code.md`, `tools/gamecode/`, `tools/sdcard/`. `.gitignore` already listed `/game-code/` and `/toolchains/`.

---

## 11. Online matches from the CSS (the gameplay session)

The real online path since 2026-10-07: matchmaking hands the connected opponent to the gameplay-only session (`Gprb::Session`, Dolphin's default `[Online] SessionBackend = gameplay`; design §5.1 C). Nothing reboots: both games stay on their own online CSS, agree on the match through Dolphin, and each starts it from there. Dolphin's side is in `docs/gameplay-rollback-status.md` (Phase 7); the plugin's is `source/online_match.cpp` and the connected part of `source/online_menu.cpp`. Slippi's flow is copied from `HandleInputsOnCSS.asm`, `LoadCSSText.asm` and `Slippi Online Scene/main.asm`.

### The flow, as a player sees it

1. **Search.** START on the CSS (Unranked) or the keypad's START (Direct) locks the player in for game 1 (Slippi's `FN_LOCK_IN_AND_SEARCH`) and searches. The lock-in goes to Dolphin through LOCAL.
2. **Connected.** Both are locked in, so the host's Dolphin decides game 1's setup at once: P1 = the host's lock-in, P2 = the guest's, the stage drawn at random from the server's `stages` list (empty today, so P+'s legal list: the 15 stages of `Switch00.rss`, see below). Both Dolphins write it into SESSION. The CSS line reads "Playing: <name>" for a moment.
3. **The match.** Each game sees SESSION's setup for the game it is locked in for and leaves the CSS (the scene manager's exit code 1, the way every scene leaves). `sqNetAnyOkiraku` goes on to its stage select state, where our hook builds the match instead (below) and goes to `scMelee` through the load screen. Dolphin's session meets the other game at the first simulation frame (RNG, frame counters, the host's match init block, task order), runs the countdown without rollback and the game under rollback from frame 240.
4. **Game set.** Both sessions end on the same frame (`MAX_ROLLBACK_FRAMES + 12` after game set) and know the winner (more stocks, then less damage). The match's own end runs ("GAME!"), and state 10 sends the game straight back to the online CSS: Slippi online has no results screen.
5. **Between games.** Still connected. The line reads "Press START to lock in"; in Direct the loser of the last game reads "Press START to select stage" (a draw: both). START locks in for the next game; the loser's START opens P+'s stage select, and picking a stage comes back to the CSS locked in with it (Slippi's `ExitSSSUponStageSelect`). Locked in, waiting: "Waiting on opponent". Once both are locked in, the host's Dolphin decides the setup (the loser's stage, else random without repeating the last) and the next match starts as in 3. Unranked is always random.
6. **Leaving.** Hold Z (48 frames) on the CSS: `CLEANUP_CONNECTION`; the opponent's Dolphin hears a "leave" at once and its game goes back to the idle prompt with the back sound (its next `GET_MATCH_STATE` reads IDLE, as on Slippi). Hold B leaves for the menus, which also cleans up.
7. **The opponent drops in a match** (closes Dolphin, loses the connection): after the silence limit (7.2 s at delay 2) Dolphin ends the rollback session and sets LOCAL `disconnected`. The game plays the error sound and, 30 frames later, ends the match through its own end: each player who left is put on its last stock and dies the way a fighter's own code ends itself (`ftManager::setDead`, sora_melee text+0x10B604, reason 5, no killer), so the match reaches its ordinary game set at once and tears itself down. Two other ways failed: leaving the scene directly (the scene manager's exit code) hangs in `scMelee`'s teardown mid-match, and a standing fighter moved past the blast zone is put back on the ground; then straight back to the CSS, idle. Dolphin shows "DISCONNECTED" as a red OSD message.

### SESSION and LOCAL (PPOM v2)

Both live in the plugin's `.data` with the mailbox (`include/ppom.h`, mirrored by `tools/gamecode/ppom.py`; Dolphin: `Online/GameBridge.cpp` `SyncSession`, every frame at the frame-end hook, never while a match runs under rollback).

| Block | Who writes | Rollback | Contents |
|---|---|---|---|
| **LOCAL** (0x40, right after the mailbox) | Dolphin: `state` (0 none, 1 connecting, 2 connected, 3 in a match), `localPort` (0 host / P1, 1 guest), `remoteReady`, `disconnected`, `peerName`. The game: `lockIn` | excluded (one range with the mailbox) | what differs between the two machines |
| **SESSION** (0x110) | Dolphin only, never while a match runs | in the region set, constant during a match | `state` (0 none, 1 lobby, 2 the next game's setup is ready), `mode`, `game` (the next game, 1-based), `lastWinner` (port; 0xFE draw), `stageKind`, `asl`, `numPlayers`, `players[4]` {present, gmCharacterKind, costume, name, code} by in-game port |

`LockIn` = {seq, ready, cssChar, charKind (gmCharacterKind, Random already drawn), costume, stagePick (0xFFFF none), asl, game}. The game bumps `seq` on every change. SESSION is sized for four players so that code-based Teams can use it later; a session has two today.

### The match setup (`online_match.cpp`)

The Wi-Fi sequence's decide function (`sora_scene+0x36D74`) is a state machine on `seq+8` (jump table 0x80703B3C): 1/2 CSS, 3/4 stage select ("vote"), 5-7 Brawl's training room, 8-10 the network match, 11-13 results, 14/15 `scMemoryChange` (type `seq+0x11`, then `seq+0xC`), 16 back to the menus. Four module hooks run at the first instruction of states 3, 4, 5 and 10 (only non-volatile registers are live there: r15 = the sequence, r17 = loop again, r19 = the scene manager), call C, and either set the next state or run the original instruction.

- **State 3** with an online match armed: `gmSelCharData` (GameGlobal+0x10) players 0-3 are filled from SESSION, each from one fixed template (the empty record the online CSS starts with, `m_nameIndex` 0x78 = no name tag), with character, state human, colour and controller set; the others are "none". The local CSS's own records are saved first. P+'s alternate-stage buttons (0x800B9EA2) get SESSION's `asl`. Then **`sqVsMelee`'s own match setup** (`sora_scene` 0x806DCE94, `(seq, stageKind)`, which P+ also patches) builds `gmGlobalModeMelee` from them and the set rule, as a local Versus match does; the controller numbers are set to 1, 2 by port; then load screen and state 9 (`scMelee`).
- **State 3** for Direct's loser's pick: `gmSelCharData+4` (the menus' mode, 0x10 Wi-Fi) becomes 0 and the stage select is opened the Versus way, `setNextScene("scSelStage", 0)`. The Wi-Fi sequence opens it with 1 (Brawl's network stage vote), which waits for the other players' votes forever, and with mode 0x10 the stage select crashed.
- **State 4** after the pick: the stage (GameGlobal+0x14, +0x22, where `sqVsMelee`'s setup reads it) and the alternate-stage buttons are kept; back to the CSS (state 1 sets mode 0x10 again).
- **State 10** after the match: the saved CSS records are put back and the sequence goes to the CSS (load screen type 0xD, as state 13 does).

**Stages.** P+ v3.2's legal list as `srStageKind` (page 0 of `Switch00.rss` with the random bit set, through its slot table at +0x104): 0x01 Battlefield, 0x02 Final Destination, 0x03 Delfino's Secret, 0x04 Luigi's Mansion, 0x05 Metal Cavern, 0x06 Bowser's Castle, 0x09 Temple of Time, 0x0C Frigate Husk, 0x0D Yoshi's Island, 0x1C Wario Land, 0x1F Fountain of Dreams, 0x21 Smashville, 0x23 Green Hill Zone, 0x2D Dream Land, 0x2E Pokémon Stadium 2. P+'s stage file loader keys on the stage kind and the alternate-stage buttons (`Net-StageFiles.asm`), so these two pick the stage without the stage select.

**In a match** the plugin's tick does nothing but watch LOCAL's `disconnected` (which Dolphin sets only after it has ended the rollback session): no mailbox; nothing that differs between the machines may change the game while frames are resimulated. The plugin's `.data`/`.bss` is part of the region set (`rel_data`), except the mailbox and LOCAL.

DEBUG scratch for the tests: `[10]` the match flow (low byte: the last sequence hook, 3 setup, 0x33 stage select opened, 4 back from it with the exit code `<< 8`, 10 back from the match; `0x100 * game` when the CSS starts a match; `0x10000` disconnect seen, `0x20000` the game ended); `[11]` the last setup, `stage << 16 | P1 char << 8 | P2 char`.

### Verified

`harness/tests/test_online_game.py`:
- `test_direct_set_under_the_gameplay_session`: two games from the in-game Direct flow (screenshots `run/artifacts/game-bridge/gameplay-set/`), results in `docs/gameplay-rollback-status.md` Phase 7;
- `test_opponent_leaves_in_the_middle_of_a_game` (`gameplay-leave/`).

`harness/tools/e2e_launcher.py` plays the same set from the two launchers (`run/artifacts/e2e-launcher/<run>/`).

### Open issues

1. **The CSS forgets the placed coin.** Brawl's Wi-Fi CSS (mode 0x10) restores no selection when it comes back from a match or the stage select: the coin is in the hand and the line reads "Press START to lock in". START locks in with the character and costume of the last lock-in (picking another character first uses that one). Slippi shows the character still selected. Restoring the coin needs the CSS's player-area init (`sel_char+0x58B8`...) or the hand and coin objects; not done.
2. **No DISCONNECTED in the HUD.** Dolphin shows it as a red OSD message; Slippi draws it in the game. And the game ends with Brawl's "GAME!" (a game set the plugin causes), where Slippi ends without it.
3. **Default controls for everyone.** Name tags (and P+'s tag controls) are cleared from the match setup so that both machines read the same controls; the player's own tag and controls would be the design's port values (5.1).
4. **Ranked** (strikes) and **Teams** (code-based, later) are not built. The server's `stages` lists are empty, so the client's P+ legal list is used.
