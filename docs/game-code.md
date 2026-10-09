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

**Heap budget.** The plugin lives in P+'s Syringe heap (heap 60, `0x10000` bytes at `0x817BA5A0`), with `sy_core.rel` and P+'s four plugins. `gfModule::create` needs the REL's sections plus its `.bss` in one block of that heap (the relocations are not kept); the hooks' trampolines are allocated elsewhere. Measured on 2026-10-08 by growing the REL header's `.bss` size (offset `0x20`) of a built plugin by K bytes and booting it (`Loaded plugin (PPOnline` or the error below in Dolphin's log):
- the plugin before the boot redirect (`.text 0x9984`) loaded with up to `0x100` more bytes and failed with `0x180`;
- with the boot redirect (`boot_menu.cpp`) it needed about `0x3E8` more and **did not load at all**: Dolphin's log shows `gfModule::create Error : Can't Alloc Heap Buffer`, nothing of the plugin runs, and the game boots as plain P+;
- the fix: `boot_menu.cpp`, `ppom.cpp` and `match_hud.cpp` are built with `-Os` (`PPOnline/Makefile`; the rest of the plugin is built without optimisation). The plugin now needs `0x98` bytes less than before the redirect and loads with up to `0x180` more bytes (fails at `0x200`).

So about 400 bytes of section + `.bss` growth are left. Anything bigger must make room first, e.g. more files built with `-Os` (files whose inline hooks read the Syriinge stub's frame through `__builtin_frame_address` need their `volatile` local, as `boot_menu.cpp` and `netmenu.cpp` have; `boot_menu.cpp`'s two such hooks run correctly at `-Os`). Check the Dolphin log for `Loaded plugin (PPOnline` after any change.

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
| muProcWifiAnybody page enter | `sora_menu_main+0x2E4F8` | replace | The page is WITH FRIENDS' now (BASIC VERSUS = Direct, TEAM BATTLE = Teams). Notes that B from it must land on WITH FRIENDS |
| muProcWifiAnybody title | `sora_menu_main+0x2E760` | inline | Its title id `0x12` (WITH ANYONE) becomes `0x11` (WITH FRIENDS) |
| muProcWifiAnybody A press | `sora_menu_main+0x2E934`, `+0x2EB70` | inline | The decision picked: `0x1E` BASIC VERSUS = Direct (`sqNetAnyOkiraku`), `0x1F` TEAM BATTLE = Teams (`sqNetAnyTeamMelee`) |
| muProcWifi A press | `sora_menu_main+0x168B8` (and `+0x166B8`, `+0x16518`) | inline | WITH FRIENDS: page id `0x1A` (Brawl's friend roster) becomes `0x1B` (the two-button page) |
| muProcWifi A press | `sora_menu_main+0x168C4` (and `+0x166C4`, `+0x16524`) | inline | WITH ANYONE: page id `0x1B` becomes `0x1C`, Brawl's Wi-Fi OPTIONS page, the Unranked / Ranked page (`anyone_menu.cpp`, §6) |
| muProcWifi enter, cursor | `sora_menu_main+0x1640C` | inline | Entered from a scene with the friends flag set: cursor on WITH FRIENDS (`this+0x42` and the stub's saved r31) |
| muProcWifi enter, back from page `0x1B` / `0x1C` | `sora_menu_main+0x1639C`, `+0x163C4` | inline | B from WITH FRIENDS' page: cursor on WITH FRIENDS (the game puts it on WITH ANYONE); from the Unranked / Ranked page: on WITH ANYONE (the game puts it on the hidden OPTIONS button) |
| muProcOptWifi (Unranked / Ranked page) | `sora_menu_main+0x2EFA4` enter, `+0x2F3B8` update, `+0x30290` A, `+0x2F2E8` title; DOL `0x8014FEDC` | replace / inline | Its two buttons shown offline, labelled "Unranked" / "Ranked" in the game's font, value pills hidden, title WITH ANYONE, A = decision `0x1E` with mode Unranked or Ranked (§6) |
| Wi-Fi rules written | `sora_scene+0x36EA0` (`sqNetAnyOkiraku` state 0), `+0x3B81C` (`sqNetAnyTeamMelee`) | simple | Replace the Wi-Fi rules with the online rules (§6, rules) |
| Leave the CSS for the menus | `sora_scene+0x3770C` | simple | `setNextSequence("sqMenuMain", 0x1C)`: `0x1F` after Direct (ONLINE page), else unchanged (`0x1C` = WITH ANYONE page, BASIC highlighted; Teams' own exit uses `0x1D`, TEAM highlighted) |
| CSS keypad update call | `sel_char+0x18F30` | simple | Our wrapper of `MuSelctChrNameEntry::update` for the connect-code keypad and its recent codes (§7) |
| CSS character lock | `sel_char+0x726C` (B pressed), `+0x7280` (costume buttons), `+0x75B4` (A on the coin) | simple | While locked in, B does not take the coin back, X/Y do not change the costume, A does not pick up or drop the coin (§6, Input on the online CSS) |
| Wi-Fi CSS countdown, timer, network error, disconnect panel | `sel_char+0x4220`, `+0x56A8`, `+0x53A4`, `+0x4A70` | simple | Ported from Gen 1. The disconnect-panel hook is now one naked hook, without Gen 1's `SaveRegs` |
| Wi-Fi SSS countdown, network error | `sel_stage+0x141C`, `+0x30F0` | simple | Ported from Gen 1 |
| sqNetAnyOkiraku state 3 (stage select) | `sora_scene+0x36F64` | simple | Online match: no stage select, set the match up at once; Direct loser's pick: open P+'s stage select the Versus way (§11) |
| sqNetAnyOkiraku state 4 (after the stage select) | `sora_scene+0x36F8C` | simple | The loser's pick: keep the stage, back to the CSS locked in with it |
| sqNetAnyOkiraku state 5 (training room setup) | `sora_scene+0x36FDC` | simple | Fallback: set the online match up instead of Brawl's training room |
| sqNetAnyOkiraku state 10 (after the match) | `sora_scene+0x37580` | simple | Straight back to the online CSS, no results screen (Slippi); the player's character, costume and tag are written back for the CSS (§11) |
| ipPadConfig setter call | `0x80110550` (DOL) | simple | Each port's controls from SESSION's port values at the match start (§11, per-player controls) |
| gfApplication frame loop, after drawing | `0x8001792C` (DOL) | inline | DISCONNECTED in the match HUD; the 90-frame end before the LRAS-type end (§11) |
| Stage select (debug only) | `sel_stage+0x4EDC`, `+0x525C`, `+0x54C0`, `+0x4CF4` | simple | P+'s stage striking as an "allowed stages" filter, `CFG_SSS_LEGAL` only; no online mode uses it (§11) |
| `gfSceneManager::setNextSequence` | `0x8002D640` (DOL) | replace | The boot: P+'s "Boot Directly to CSS" default case (`sqVsMelee`, 0 from `sqBoot`) becomes the Start case (`sqPrizeCheck`, `0x14`), and the `sqMenuMain` that follows gets 30, the ONLINE page (§13) |
| scTitle's process | `sora_scene+0xECA4`, `+0xECB0` | inline | On that boot only: the opening movie (state 3) and the "press Start" logo (12) become 16, the title's exit (§13) |
| CSS ITEM / STAGE buttons (`buttonProcInAllArea`) | `sel_char+0x7CCC` | simple | Online: A on ITEM (button `0x19`) or STAGE (`0x1A`) opens nothing (P+'s item and stage switches; §6, Settings on the online CSS) |
| CSS READY TO FIGHT banner hit test | `sel_char+0xDDB0` | simple | Online: the hand never finds the banner, so A on it cannot start Brawl's own countdown (§6, The READY TO FIGHT banner) |

Notes:
- P+ v3.2's `sora_menu_main.rel` is vanilla's with the same `.text` size. Its 152 non-relocation word differences are confined to a few P+ edits (`reltool.py diff`), so Gen 1's offsets hold.
- P+ keeps `sel_char` (`.text 0x806828C4`), `sel_stage` (`0x806B0984`), `sora_menu_name` (`0x8067406C`), `sora_scene` (`0x806BB554`) and others resident. That is why Gen 1's absolute jump-back addresses are valid on P+.
- **Resident modules still need module hooks.** They are loaded after the plugins, so an absolute `sySimpleHook` on their code is overwritten when the module loads (checked: the patch log shows it, memory has the original).
- **Never patch an instruction that carries a relocation against a module loaded later.** The `bl MuSelctChrNameEntry::update` at `sel_char+0x18F34` is relocated against module 16. The loader applied that relocation after Syriinge's patch and kept the opcode bits, which turned the patch into `b update` (no link) and hung the game. The hook now sits on the `addi` before it.
- `sora_menu_main` is loaded into MenuInstance at `0x81164C00` while in the menus.
- **Gen 1 bug, not ported.** `turnOffSSSTimer` is installed on `sel_char+0x35A4`, which is a `blr`, but jumps into `sel_stage+0x35A8`. It was meant for `sel_stage+0x35A4`.
- **Online CSS files are built for size** (`Makefile`: `code_entry.o online_menu.o online_match.o stage_legal.o: CXXFLAGS += -Os`): the plugin is loaded into the Syringe heap, which has `0xB2E0` bytes free for it. On 2026-10-08 (online CSS settings, own code, '#' key) it needs `0xA984` at load: about `0x95C` left (measured by growing `.bss` until `gfModule::create` says "Can't Alloc Heap Buffer": `+0x800` loads, `+0xA00` does not). Before `-Os` the CSS work did not load at all. Since Ranked's game setup (2026-10-08) every file is `-Os` (`anyone_menu`, `online`, `rel`, `netmenu` too: 0x998 bytes less); the plugin then needs about `0xACE9` (sections + `.bss`, the sum of the REL header's section sizes and `.bss`), about `0x5F0` left. The menus (netmenu's inline hooks read the stub frame through a `volatile` local and keep working) are covered by `test_online_ranked.py` and `test_online_unranked.py`.

### PPOM block (design §5.2)

`include/ppom.h` is the contract, and `tools/gamecode/ppom.py` mirrors it.
- The block lives in the plugin's `.data`. It is 0x674 bytes, aligned to 32, and big-endian.
- Header: magic `"PPOM"`, version 1, then offset/size pairs for MAILBOX, SESSION, LOCAL and DEBUG. The header is 0x24 bytes.
- **How Dolphin finds it** (`Source/Core/Core/Online/GameBridge.cpp`): it walks the game's `OSModuleInfo` list (`0x800030C8`) to the module with our REL id **20560** (`PPOnline/Makefile` `RELID`; do not change it), then looks for the header in that module's data sections only. It does this once per boot. After that it checks the magic word each frame. The harness tools (`ppom.py`) still scan the Syringe heap (`0x817BA5A0..+0x10000`), which is fine for debugging.
- **Version 2** (gameplay session): MAILBOX, then LOCAL right after it (Dolphin excludes both from rollback as one range), SESSION and DEBUG. §11 has SESSION and LOCAL.
- **Version 3** (2026-10-07): each player's port values (name tag and its controls, `PortValues`, 0x3C bytes) in LOCAL (`own`, game-written with the lock-in) and in SESSION (`players[i].pv`); LOCAL grows to 0x80 (with `hudDisconnected` at +0x7C), a SESSION player to 0x80 and SESSION to 0x210. Dolphin and the plugin must speak the same version: GameBridge refuses another (`PPHARNESS_PLUGIN` in the tests picks a plugin build for an older Dolphin).

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
| `0xC1 GP_FETCH_STEP` | none; response `GameStep {active, type (1 strike, 2 pick, 3 characters, 4 done), myTurn, count, toSss, mayLock, nKinds, seconds, kinds u8[40], text u16[64]}` (0xB0): Ranked's game setup step (Dolphin `Online/GameSetup.cpp`, Slippi's `GP_FETCH_STEP`). Asked every frame (one at a time) on the Ranked CSS and stage select while connected. |
| `0xC0 GP_COMPLETE_STEP` | `{kind u8}`, no response: this player strikes (X), bans or picks (A) the stage `kind` (Slippi's `GP_COMPLETE_STEP`). Dolphin refuses it out of turn or for a stage that is not selectable. |
| `0xE3 GET_RANK` | none; response `{state u8 (0 unknown, 1 a set's result on its way, 2 ready), hasChange u8, pad[2], rating f32, setsPlayed u32, change f32}` (0x10). The rating is Elo (no tiers); `change` is the last ranked set's (Slippi's `RankInfo`, `ExiSlippi.h:159-167`). |

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

DEBUG `scratch` words the tests read: `[0]` `0xBE` requests sent; `[1]` CSS lock (bit 0) and the CSS phase (`<< 4`); `[2]` Z accepts `<< 24`, last answer found `<< 16`, its index; `[3]` online rules applied (count in the low half, Code Menu lines forced `<< 16`); `[4]` keypad 1 open, 2 OK, 3 closed; `[5]` the hand's mode (8 = keypad); `[6]`-`[9]` menu hooks (`netmenu.cpp`); `[12]` bit 31 the own connect code is shown; `[14]` shared: from the boot until the first online CSS, the boot (`boot_menu.cpp`, §13): bit 0 redirected, bits 1/2 the title's movie/logo skipped, bit 3 the ONLINE page opened, bits 16-23 the argument `sqTitle` asked `sqMenuMain` for (0); on the online CSS, the status line's layout `<< 24` (1 one line, 2 two lines, 3 one small line) and its width in font units (all 16 words are taken, so the two share it; the boot test reads it before any CSS); `[15]` keypad keys refused `<< 16`, keypad page `<< 8`, ITEM/STAGE presses refused (low byte), Code Menu opens closed again `<< 24`. DEBUG `cfg` `CFG_TEST_RULES` (`1 << 6`, `ppom.allow_test_rules`) lets the online rules keep the harness's time and stock count (short test games).

---

## 6. Screens (Slippi → ours)

Screenshots are in `run/artifacts/game-code/screens/`, from the last runs of `online_direct.txt` (01-10), `online_unranked.txt` (11-20) and `online_teams.txt` (21-25). Vanilla P+ references are in `explore/`; the step-by-step discovery shots are in `wifi/`. Slippi's strings below come from its sources (`refs/slippi-ssbm-asm`, `slippi-ssbm-c`, Dolphin) and from its `SdMenu.usd` / `SdSlChr.usd` patches.

| # (design §5.4) | Slippi | What you see now | Status | Screenshot |
|---|---|---|---|---|
| 1 | Slippi boots to its main menu with Online Play; Melee asks nothing about saves | Play boots straight to the ONLINE page (WITH FRIENDS highlighted), no save prompt (the launcher seeds the save), no title, no Versus CSS; B goes to the main menu (§13) | done | `run/design/boot-flow/` |
| 1 | 1P menu "Online Play": "Compete against online opponents." | Main menu PLAY ONLINE with Slippi's description. The account is not printed: Slippi's online menu does not show it. | done | `01-main-play-online.png` |
| 1 | (no Slippi equivalent) | Brawl's connect dialog, "Connected." and the first-time "Choose a profile name." keypad are skipped: PLAY ONLINE opens the ONLINE page at once. | done | `02-online-direct.png` |
| 1 | Online submenu: Ranked "Play ranked matches.", Unranked "Play unranked matches.", Direct "Play a specific person.", Teams "Play teams games." | ONLINE page: WITH FRIENDS ("Play a specific person.") opens the code-based modes on Brawl's two-button page, retitled WITH FRIENDS: BASIC VERSUS = Direct ("Play a specific person."), TEAM BATTLE = Teams ("Play teams games."). WITH ANYONE ("Compete against online opponents.") opens the matchmaking modes on Brawl's Wi-Fi OPTIONS page, whose buttons are labelled in the game's font: "Unranked" / "Ranked" with Slippi's descriptions (below). | done | `menu-modes/game-m/` |
| 2 | CSS: "Select your character" → "Press START to search / enter code" → "Searching for opponent / ABCD#123" → "Connecting to …" → "Playing: …"; errors in red; Z cancels, hold Z disconnects | Brawl's Wi-Fi CSS with P+'s competitive rules. Header art hidden; one status line with Slippi's strings (below). START works once a character is picked. | done | `03`-`09`, `13`-`19`, `22`, `24` |
| 3 | Connect-code entry: name-tag keyboard in code mode, 8 characters with '#', START confirms; recent codes as grey completion text, L/R scroll, Z accepts | Brawl's name keypad in code mode: 8 characters, alphabet and digits only, upper case, '#' key, unusable keys refused, START confirms, the name tag is untouched; the newest matching recent code in grey after the typed characters, L/R older/newer, Z accepts and jumps to OK (§7). | done | `05`, `06`, `23`; `recent-codes/` |
| 1 | Leaving: hold B on the CSS → online menu, cursor on the mode; CLEANUP_CONNECTION on menu load | Brawl's hold B (or LEAVE) → the page the mode was picked on, cursor on it; `0xBA` on every menu load. | done | `10`, `20`, `25` |
| 4 | Opponent found: both locked in, the match starts; no opponent's character on the CSS (Slippi shows it only on its VS splash) | The match starts from the CSS once both are locked in; "Waiting on opponent" / "Playing: <name>" | done (§11) | `gameplay-set/` |
| 5 | Stage: Unranked random, Direct random then loser picks, Teams random then P1 picks | Game 1 random from P+'s legal list (the host's Dolphin draws it); Direct game 2+: the loser presses START, picks on P+'s stage select, comes back locked in | done for 1v1 (§11) | `gameplay-set/` |
| 8 | Results → back to the CSS still connected | Straight back to the online CSS after game set (Slippi skips the results screen online), still connected; START locks in for the next game | done (§11) | `gameplay-set/` |
| 9 | "DISCONNECTED", back to the CSS | In a match: the error sound, "DISCONNECTED" in red at the top of the HUD in the game's font, after 90 frames the game ends as the pause screen's quit (no "GAME!"), back to the CSS idle with the character still selected. On the CSS: back to the idle prompt, no text (Slippi). Dolphin's red OSD message only stands in if the game does not show the text | done (§11) | `gameplay-leave/` |
| 7 | Rank info on the CSS (Slippi's rank box) | Ranked's CSS shows the Elo rating, and after a set its change ("1523 (+14)"), where Direct shows the own code (below); no tiers, no badge | done (2026-10-08) | `run/artifacts/game-bridge/ranked-set/` |
| 6 | Ranked game setup (Slippi's `Scenes/Ranked/GameSetup.c`: strikes, ban, counterpick, characters) | On P+'s own stage select with P+'s stage striking (below) | done (2026-10-08) | `run/artifacts/game-bridge/ranked-set/` |
| 10 | Chat | not started | — | — |

**CSS status line.** The Brawl CSS has one text window in its header (the rule line). Slippi shows a mode header, three status lines and a hint; we show the one line that carries the state, in Slippi's words (`LoadCSSText.asm:95-157`):
- idle: "Select your character", then "Press START to search" (Unranked) or "Press START to enter code" (Direct, Teams);
- "Searching for opponent" / "Searching for ADGJ#123", "Connecting to opponent" / "Connecting to ADGJ#123", "Playing: <name>";
- an error: the text from Dolphin or the server, in Slippi's red (`FF0000`), e.g. "Teams isn't available yet." (the server's texts are in `server/crates/mm/src/messages.rs`).
- Not shown: "<Mode> Mode", the hint line ("Press Z to cancel", "Hold Z to disconnect", "Press Z to clear error") and "Use D-Pad to Chat".

**The status line is never squeezed (2026-10-08).** The rule line's window (352 units wide) narrows its font to fit (MuMsg width mode 0), so "Ranked is not supported yet. Only Direct and Unranked work for now." came out at about a third of the font's width. Now:
- the window spans the whole bar (x `-318..330`, over the hidden rule numeral, mode title and ITEM/STAGE buttons), about 38 average characters at the normal size;
- the plugin measures the text with the window's own font (`ut::Font::GetGlyph` through the `Message`, vtable `+0x50`, the glyph's advance at `+6`; a font unit is a window unit at scale 1) and lays it out itself (`layoutStatus`): one line at the normal size if it fits; else two lines at 0.7 split at the space that balances them; a single long word on one line at 0.7; a text too long even for that is cut at a space with "..." on the second line. The window's own narrowing never runs. MuMsg window settings: `MuMsg+0xC`, 0x48 bytes each: `+0` flags, `+4..+0x10` rect (y up), `+0x2C` line spacing, `+0x30/+0x34` scale;
- every server text was shortened to fit one line (`messages.rs`, with a test that each fits 38 characters; e.g. "Ranked isn't available yet.", "No opponent found. Try again.", "Update to 1.2.3 to play online."). Dolphin's own texts (Slippi's, `Online/Matchmaking.cpp`: "Failed to connect to mm server", "Invalid response when getting mm status"...) are not changed: they fit or wrap; the longest, Teams' "Could not connect to players: ..." with three 15-character names, wraps and is cut with "...".
- Texts from Dolphin, the server and the opponent's name: bytes below `0x20` and above `0x7E` become spaces before they reach the message system (it reads control bytes as commands).
- Verified by `test_online_css.py::test_status_line_is_never_squeezed`: every `messages.rs` text (parsed from the source) and Dolphin's texts, served on an Unranked CSS, each layout read back from DEBUG `[14]`; screenshots `run/design/online-css/status-line/`.

**Own connect code (2026-10-08).** Slippi's CSS shows the player's own code only in Direct (`LoadCSSText.asm`, "display connect code (for direct only)"); ours in both code-based modes, Direct and Teams, so that it can be read out to a friend. It is printed in white between LEAVE and the status bar, right-aligned against the bar, in window 1 of the rule line's `MuMsg` (8 windows, only window 0 used), attached to the same node with `MuMsg::attachScnMdlSimple` (`0x800B8C90`) at window 0's size: no new model, art or allocation. The code comes from `GET_ONLINE_STATUS` (Dolphin reads it from `user.json`); only `A-Z a-z 0-9 #` are taken. Unranked shows none (Slippi). Screenshot `run/design/online-css/css-locked/direct-06-own-code.png`.

**Ranked game setup (2026-10-08).** Slippi's ranked flow (slippi-ssbm-c `Scenes/Ranked/GameSetup.c`, read 2026-10-08), on P+'s stage select instead of Slippi's own screen:
- **Game 1** (and the game after a draw): the characters are the ones locked in before the search. Both games go to the stage select by themselves; only the five starters (the server's `ranked.starters`: Battlefield, Final Destination, Smashville, Dream Land, Pokémon Stadium 2) can be chosen, the rest are struck with P+'s own art. Player 1 (the host) strikes one with X, player 2 two, player 1 one (Slippi's 1-2-1); the stage left is played and both games leave the stage select by themselves.
- **Games 2+**: both go to the stage select; the winner of the last game bans one stage (X), then the loser picks the stage (A), never the stage they last won on once they have won a game (Slippi's "Dave's stupid rule"). Back on the CSS, the winner may change character and locks in first; the loser's START does nothing until then, and the loser's line names the winner's choice ("Opponent picked Fox"). Slippi changes characters in the same order.
- **Turns**: only the player whose turn it is can strike or pick; the other's X and A do nothing (START, Z and B are off on a ranked stage select). Every strike shows on both machines at once. The header line says whose turn it is and the seconds left (Slippi's step timers: strikes 30 s, the last game-1 strike 10 s, ban and pick 30 s); when a player's time runs out, their Dolphin strikes or picks a random selectable stage for them.
- **How**: Dolphin runs the steps (`Online/GameSetup.cpp`): each player's own strikes and picks travel in its gameplay-session control messages, and both machines merge them the same way since only one player acts at a time. The host sets the match up with the stage the steps decided, whatever the stage select picked. The plugin asks for the step every frame (`GP_FETCH_STEP`), writes the selectable stages into P+'s strike table (`stage_legal.cpp`), sends X/A as `GP_COMPLETE_STEP`, and prints the line in window 0 of the stage select's message object (created by `sel_stage+0xC38`), moved into the header strip next to "STAGE SELECT". P+'s strike code (Random.asm's buttonProc hook) only runs while `gfSceneManager+0x284` is 1 (as after the Versus CSS); the online sequence opens the stage select with 0, so the plugin sets it while a ranked stage select runs.
- Verified by `harness/tests/test_online_ranked.py` (two instances, a whole set from the menus).
- Not done: Slippi's colour step (same character and colour) and its step for the opponent's stall (Slippi disconnects after a step's time plus a grace).

**Ranked rating (2026-10-08).** Ranked's CSS shows the player's rating in the same window: the Elo number rounded, and once a ranked set has been rated its change, e.g. `1523 (+14)` (the user chose Elo with no rank tiers, design §4.3). The plugin asks Dolphin with `GET_RANK` (`0xE3`) once a second while it is on the Ranked CSS (`online_menu.cpp` `pollRank`/`onRank`); Dolphin answers from the rating it read at login and, after a set, from the server's result (`Online/Ranked.cpp`). A ranked set is best of three: after the deciding game the games come back to the CSS, Dolphin closes the connection on the next `GET_MATCH_STATE` (the CSS reads as idle again, Slippi's ranked set end), and the change appears once both reports reached the server.

**Settings on the online CSS (2026-10-08).** Slippi's online CSS offers no rules, items or stage settings; the match is set up from fixed values. What the Brawl/P+ Wi-Fi CSS still offered (live, Direct and Unranked; `run/design/online-css/explore/`):
- **ITEM** (right end of the bar, `MenSelchrState0005`, muSelCharTask `+0x420`): P+'s item switch, frequency NONE/LOW/MEDIUM/HIGH and each item; it writes the menu record (`+0x810` frequency, `+0x818` switches).
- **STAGE** (`MenSelchrState0006`, `+0x424`): P+'s random stage switch and hazard switch (saved to the SD card on exit).
- **P+'s Code Menu** (L + R + D-pad Down): Debug Mode and its displays, Special Modes (Random Angle, War, Big Head, flight, hitstun/hitlag/SDI/shield/staling/jumpsquat modifiers), each player's codes (character select incl. transformations, infinite shield, percent select, input buffer, automatic L-cancelling), Alternate Stages, Tag-Based Costumes, Endless Friendlies... It lives at `0x804E0000` (`dnet.cmnu`, `0x2520` bytes) and P+'s codes read its lines directly.
- The rule bar is not a button on the Wi-Fi CSS. The name tag button stays (each player's controls, §11). Direct's loser's stage select has P+'s Z hazard toggle.
- **What reached a match before** (two-instance Direct, old plugin; `test_stale_settings_do_not_reach_the_match` and the exploration runs): items set to HIGH on the host's online CSS → item frequency 3 in both machines' match (the joiner takes the host's match init block), while each machine's item switches differed. Settings left from offline play (hazards off, Big Head, hitstun x0.5, P1 infinite shield) reached the match on that machine only: hazards and the Code Menu values differed between the two machines. The session did not notice (none of it is in its setup key) and played on with divergent rules.
- **Now (Slippi's way):** ITEM, STAGE and the READY banner are hidden (`nwSMSetVisibility`, every frame) and inert (`sel_char+0x7CCC`, `+0xDDB0`); the Code Menu is closed again in the frame it opens on the online CSS and the online stage select (its control code runs inside the pad update, before our tick, so masking the pads cannot stop it: the plugin undoes the open as its B does: freeze flag `0x805B8A08` from `0x804E006C`, `0x805B6DF8` from `0x804E0074`, state `0x804E0034 = 0`; only its open sound is heard); Z is masked on the online stage select. And every online match setup forces the ruleset right before `sqVsMelee`'s setup reads it (`OnlineMenu::applyRules`, also when the Wi-Fi sequence starts): P+'s set rule (16 bytes, below), pause only in Direct, item frequency 0 with P+'s default item switch (`00 06 00 00 00 00 00 00`), hazards on (stage select data `GameGlobal+0x14`, `+0x25`, which P+ copies into `gmGlobalModeMelee+0x29` bit `0x20`; also for Direct's loser's pick, whose hazard toggle is not part of what the other machine gets), and **every Code Menu value line at its default** (85 lines in P+ v3.2, found by fudgepop's line layout: `+0` size, `+2` type, `+6` text offset, `+8` value, `+0x10` default, a selection's options pointer at `+0x18`; defaults copied at boot, because the Character Select lines keep the current character in `+0x10` during a match). The player's own values (set rule, items, hazard, Code Menu) are saved on the way in and put back when the menus load.
- Direct's loser still picks the stage (P+'s stage select, every stage).
- **Not in the session's setup key yet:** the Code Menu values, item switches and hazard bit are outside the range Dolphin compares at the barrier (`gmGlobalModeMelee +0x08..+0x28`, players, pad layouts). Both machines force the same values, so they agree; adding them to the setup key is a Dolphin change (not done here).
- Verified by `test_online_css.py`: `test_online_css_settings_are_locked` (ITEM/STAGE hidden and inert, the Code Menu combo refused and the CSS not frozen, A on the banner idle and searching) and `test_stale_settings_do_not_reach_the_match` (two instances, A with stale offline items/hazards/stocks/Code Menu values: identical forced setup on both, Code Menu at its defaults on both, 0 checksum mismatches, A's own settings back in the menus). Screenshots `run/design/online-css/css-locked/`, `stale-settings/`.

**The READY TO FIGHT banner (2026-10-08).** A on Brawl's READY TO FIGHT banner on the online CSS hung the game. Root cause: the banner is the hand's target 5 (`sel_char+0xDDB0` hit test); A on it starts the CSS's own countdown, which writes the selections (`setToGlobal`) and leaves the CSS with exit code 1. `sqNetAnyOkiraku` then goes to state 3 with no online match and no loser's pick armed, so our hook let Brawl's **network stage vote** run (`scSelStage` with the Wi-Fi mode), which waits for votes that never come: the game sat frozen. Fixed three ways: the banner model (`MenSelchrReady`, muSelCharTask `+0x3C4`) is hidden online; its hit test is skipped online (the hand never finds it, so A on it does nothing in any state: idle, searching, connected, between games); and state 3 with nothing armed while the online CSS is up goes straight back to the CSS (DEBUG `[10]` low byte `0x3F`). Verified by `test_online_css_settings_are_locked` (idle, searching; Direct and Unranked) and `test_ready_banner_between_games` (two instances, after game 1); screenshots `run/design/online-css/banner-between/`.

**CSS header art (item 1).** "HOME-RUN CONTEST" is the Wi-Fi mode title texture `MenSelchrTitleW` (muSelCharTask+0x418) and the "2" is the rule numeral `MenSelchrRnum1/2` (+0x158/+0x15C). Both are images with no text slot, so they are hidden every frame on the online CSS (`nwSMSetVisibility`, `0x80043D20`). The status line keeps the rule line's own window.

**Input on the online CSS (Slippi `HandleInputsOnCSS.asm`).**
- START: lock in and search (Unranked) or open the code keypad (Direct, Teams), only once a character is picked. START is removed from all of the game's pad statuses on the online CSS, so Brawl's own "READY TO FIGHT" start never runs.
- Z: cancels a search or clears an error (`0xBA`, back sound); hold Z 48 frames (`DISCONNECT_HOLD_DELAY 0x30`) disconnects when connected. Error sound when an error arrives, back sound when a connection ends.
- Buttons come from the pad system's own pressed/held fields (`gfPadSystem+0x244`), so short presses are not missed when a game frame spans several pad reads.
- B: Brawl's own. A press takes the coin back (not while locked in, below); holding B leaves the CSS (Slippi keeps Melee's hold B, unblocked in every state). LEAVE does the same. The menus load with `0xBA`, as Slippi's `OnMenuLoad.asm` does, and reopen where the mode was picked: WITH FRIENDS' page with BASIC VERSUS (Direct) or TEAM BATTLE (Teams) highlighted (`sqMenuMain` entries `0x1C` / `0x1D`), or the ONLINE page with WITH ANYONE highlighted after Unranked and Ranked (`0x1F`).
- **Character lock** (Slippi `PreventAPressCharUnselect.asm`, `PreventBPressCharUnselect.asm`, `PreventColorChange.asm`). Slippi is locked in from the lock-in that starts a search (`MSRB_IS_LOCAL_PLAYER_READY`, i.e. `local_selections.is_character_selected`) until `CLEANUP_CONNECTION` clears it: Z on a search or an error, hold Z when connected, the menus loading, or Dolphin's own cleanup when the peer goes. While locked, A and B on the character do nothing and the costume does not change; hold B still leaves. Ours is locked while the CSS is searching, connecting, connected or shows an error (an error stays locked until Z clears it, as on Slippi). `mmState 0` (idle) unlocks.
  - Brawl's CSS handles one player's pad in `sel_char+0x6FFC` (its pad is a `getSysPadStatus` copy).
  - `+0x726C` computes "B pressed" for `+0x74F4`, which takes the coin back to the hand: our hook makes it 0 while locked.
  - `+0x7280` tests the two costume buttons (X/Y on the GameCube pad): while locked we jump to `+0x74F4`.
  - `+0x75B4` calls `+0x8500`, which handles A over the character grid (drop the held coin on a character, pick up a placed coin): skipped while locked.
  - Holding B to leave is the hand's own counter (`sel_char+0x1B794`, held B of the sys pad) and is not touched. A on LEAVE and the other buttons goes through `+0x7A94`/`+0x883C` and still works.
  - Verified: `run/artifacts/game-code/css-lock/game-l/` (searching: A, B, X, Y change nothing; after Z, B takes the coin back and A puts it down; locked again, hold B leaves) and `test_online_unranked.py::test_unranked_search_ends_with_the_server_error` (locked while the error shows, unlocked after Z).

**Rules (item 2).** Slippi uses one fixed ruleset in every online mode (`EXI_DeviceSlippi.cpp:2114-2133`): 4 stocks, 8 minutes, items off, real pause only in Direct. P+ v3.2's competitive defaults are its codeset's "Default Settings Modifier" (`RSBE01.txt`, `NETPLAY.txt`), which writes the set rule `0x9017F360 = 00 00 01 00 04 00 0A 00 08 01 01 00`: stock, 4 stocks, damage 1.0, 8-minute stock time limit (P+ uses `stockTimeMinutes` as the stock timer, `Rules.asm`), team attack on, pause on. Items off is item frequency 0 in the menu record (`getGlobalRecordMenuDatap()[0]`, what the ITEM screen edits). The plugin writes these right after `sqNetAnyOkiraku`/`sqNetAnyTeamMelee` write Brawl's Wi-Fi rules (2-minute time, no pause), and again at every online match setup, sets pause on for Direct only, and puts the player's own settings back when the menus load (all forced settings: Settings on the online CSS, above).

**Stage choice (item 2).** Slippi's game picks no stage on the CSS for game 1 in any mode: Unranked, Direct and Teams all lock in with "random", and the stage comes from the server's `stages` list (Dolphin's fallback is FoD, PS, YS, DL, BF, FD), drawn without repeats. After a game, Unranked stays random, Direct's loser picks on the SSS (a draw: both pick), Teams' port-1 player picks, and Ranked uses its strike screen (`HandleInputsOnCSS.asm:160-163, 259-307`, `main.asm:452-491, 757-764`, `EXI_DeviceSlippi.cpp:2179-2186, 2407-2418, 2746-2764`). In our game the CSS never leaves for Brawl's SSS on its own (START is ours), so game 1 is "random" as on Slippi. The pick itself belongs to the session: the server's `stages` list in `Online::Match` (design §5.5), drawn from P+'s legal list. **P+ v3.2's own legal list** is its random-stage switch "Default" preset (`/Project+/pf/stage/switch/Switch00.rss`, identical to the netplay `SwitchFF.rss`): Battlefield, Final Destination, Dream Land, Pokémon Stadium 2, Smashville, Yoshi's Island, Fountain of Dreams, Green Hill Zone, Wario Land, Frigate Husk, Temple of Time, Metal Cavern, Bowser's Castle, Delfino's Secret, Luigi's Mansion (15 on; Yoshi's Story, Castle Siege, Sky Sanctuary Zone, Golden Temple, Ceres Space Colony and Distant Planet listed but off). The other presets (PMBR, 2023/2024 Proposed, Midwest, Australia, Japan) are in `Switch01-06.rss`. Loser's pick on Brawl's SSS for Direct game 2+ needs the results → CSS flow (screen 8), which does not exist yet.

**Teams (item 7).** Teams is code-based only, as on Slippi (user, 2026-10-07): TEAM BATTLE on WITH FRIENDS' page, with the same code keypad as Direct. Slippi has no "not supported yet" behaviour: unavailable options are only locked and skipped (`HandleOnlineLockedOptions.asm`), and server refusals are shown as errors. Our server refuses Teams, so the option stays and the server's text shows in red (`24-css-teams-error.png`). The Teams code help text (Slippi: "Enter any code to start a lobby...") has no place on our screen.

Other notes:
- **Status line style.** `MuMsg::printf` does not apply msbin style tags, so we set the colour ourselves with `setFontColor`.
- **Do not print a lone `" "` into the CSS rule window.** It froze the display: presents stopped while the game kept running. This was found by bisecting with `ppom.py cfg`.
- **WITH FRIENDS must not open Brawl's friend page.** Leaving muMenuMain from inside that page froze the display the same way, so the A-press hook reroutes it to the Anybody path instead.
- **Ranked** is the second entry of WITH ANYONE's page (below). Its search sends `FIND_OPPONENT mode=0`; until the server has Ranked it answers with its error, shown in red like Teams'.

**The mode pages (2026-10-07).** Slippi's online submenu lists Ranked, Unranked, Direct and Teams with one description line. Brawl's ONLINE page has two image buttons, and none of Brawl's button images says "Ranked", so the modes are split by how the opponent is found:
- **WITH FRIENDS** = the code-based modes. It opens `muProcWifiAnybody` (Brawl's two-button page, normally WITH ANYONE's), retitled WITH FRIENDS (title id `0x11`): BASIC VERSUS = Direct, TEAM BATTLE = Teams. Their art fits the labels. B goes back to the ONLINE page with WITH FRIENDS highlighted; leaving a Direct CSS reopens this page with BASIC VERSUS highlighted (`sqMenuMain` entry `0x1C`), a Teams CSS with TEAM BATTLE (`0x1D`).
- **WITH ANYONE** = matchmaking: Unranked and Ranked. The only page of `muMenuMain` whose button labels are printed text is Brawl's **Wi-Fi OPTIONS** page (`muProcOptWifi`, page `0x1C`): two wide bar buttons ("Allow Spectators", "Smash Service") whose labels are `MuMsg` windows 0 and 1 and whose values (Yes/No, Accept/Decline) sit on a pink pill in windows 2 and 3. Offline the game never shows that page (its entry and its two buttons need WiiConnect24), and P+ does not change it. `anyone_menu.cpp` opens it from WITH ANYONE, shows both buttons, prints "Unranked" / "Ranked" (and Slippi's "Play unranked matches." / "Play ranked matches.", message `0x645`/`0x646` of Slippi's `SdMenu.usd`, `OnMenuPrep.asm` `Data_OnlineSubmenuDescriptions`), prints the values empty and hides their pill (the button model's VIS0 entry for bone `button0`, constant-visible → invisible while the page is ours), titles it WITH ANYONE (`0x12`) and makes A the menu's own decision `0x1E` with mode Unranked or Ranked. Leaving the CSS goes back to the ONLINE page with WITH ANYONE highlighted (`sqMenuMain` `0x1F`); Slippi goes back into the mode list, but the game has no decision that reopens page `0x1C`.
- Candidates checked and rejected: the ONLINE and WITH ANYONE pages (labels and title are images; only the description is text), the connect dialog (text Yes/No, but a Wii-style pop-up), Records / Options > Controls / Rules (data and settings screens, checked statically only).
- Verified by `test_online_menus_match_the_modes` (screenshots `run/artifacts/game-code/menu-modes/game-m/`): each entry reaches the CSS in its mode, Ranked's search is `FIND_OPPONENT mode=0`, and the way back lands where the mode was picked.

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
- **Keys:** the key table (`0x8067BEB0`, shared by every keypad) gets upper-case strings for the alphabet keys and "＃" on the symbols key while the keypad is ours, restored on close. The keys that cannot be part of a code (`!?&%$`, `・,./~`, `-+×=`) are refused with the error sound.
- **The '#' key and the pages follow the code (2026-10-08, the user's design).** A code is 2-4 letters, '#', 1-4 digits (our server's rule; Slippi's codes are the same shape). The alphabet page's first key is '#': its "@()~;" texture label is hidden (VIS0 of the page model, bone `pPlane65`) and a "＃" is printed on it in the game's font (its own `MuMsg`, attached to that bone's node, created when the keypad opens and deleted when it closes: a block left in the MenuInstance heap hangs `scMemoryChange`). Typing '#' turns to the digits page at once; erasing the '#' turns back to the letters; the page key and the keypad's "Z RANDOM" tab are hidden (VIS0 `kirikae`, `randam`, `zz_Gc`) and the unused symbol keys' labels too. Refused with the error sound: a letter after four letters, '#' before two letters or a second time, a fifth digit, a symbol key. A digit is committed at once (no multi-tap on single-character keys, so "11" needs no move off the key). Slippi's keyboard is Melee's full keyboard and does not turn pages by itself; this is ours.
- **Placeholder:** the empty field shows "PLYR#123" in the suggestion grey (`8E9196`, half alpha); a recent-code suggestion takes its place.
- Verified by `test_online_css.py::test_code_keypad_hash_key_and_placeholder` (screenshots `run/design/online-css/keypad/`: placeholder, letters, '#' → digits, erase '#' → letters, refused keys) and `test_online_game.py::test_keypad_steps_cover_every_code_shape`.
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
5. **Unranked** (since 2026-10-07). START on the BASIC VERSUS CSS posts `0xB4 {mode 1}`. The server pairs two strangers from its Unranked queue, and the set runs as in item 6, every stage drawn from the server's Unranked list (`harness/tests/test_online_unranked.py`, screenshots in `unranked-set/`). A lone search ends at the server's ticket TTL with `mmState 5` and the server's text ("Search timed out: no opponent found within ..."); Z clears it (`unranked-timeout/`).
6. **The gameplay session (the default).** Both games stay on their online CSS; the match starts from there (§11). `test_direct_set_under_the_gameplay_session` plays a two-game Direct set; `test_opponent_leaves_in_the_middle_of_a_game` the disconnect.
7. **Hand-off to netplay (the fallback, `[Online] SessionBackend = netplay`).** Both Dolphins stop their games right after the match and boot P+ together under rollback. The plugin is loaded again and the block is found again, but servicing is paused while netplay runs (`direct-netplay/*/06-netplay-boot.png`).
8. **As a player does it: launcher and Dolphin.exe.** `harness/tools/e2e_launcher.py` runs the same flow from two built launchers. Each logs in through the launcher's quick start and presses Play. The launcher installs `PPOnline.rel` on its own patched copy of the user's SD card and passes it with `-C Dolphin.General.WiiSDCardPath=...`. Each `Dolphin.exe` (Qt, D3D11, muted, main window "Brawl Online") is then driven from the main menu to a two-game set under the gameplay session (`harness/tools/online_set.py`; `run/artifacts/e2e-launcher/<run>/`).

`drive.py mbx-serve` and `ppom.py serve` still work for experiments without a server. Turn Dolphin's servicing off first: `ppharness cmd --port P game_bridge_config enabled=false`.

---

## 9. Next steps

1. ~~The CSS after a match or the stage select~~, ~~DISCONNECTED in the HUD and an end without "GAME!"~~, ~~name tags and their controls as port values~~, ~~the mode pages with Ranked~~: done 2026-10-07 (§6, §11).
2. Ranked's set (strikes with P+'s stage striking, counterpicks, best of 3) and code-based Teams (4 peers) (§11, open issues).
3. Slippi's quick chat, the rank line on the CSS, DESYNC DETECTED.

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
  - `7ca3a7e`: Slippi parity in the online session (2026-10-07): per-player controls (PPOM v3 port values, the ipPadConfig hook), the CSS remembers the character, DISCONNECTED in the HUD and the end without "GAME!", P+'s stage striking behind a debug flag, the mode pages (WITH FRIENDS: Direct / Teams; WITH ANYONE: Unranked / Ranked).
- Top-level repo: `docs/game-code.md`, `tools/gamecode/`, `tools/sdcard/`. `.gitignore` already listed `/game-code/` and `/toolchains/`.

---

## 11. Online matches from the CSS (the gameplay session)

The real online path since 2026-10-07: matchmaking hands the connected opponent to the gameplay-only session (`Gprb::Session`, Dolphin's default `[Online] SessionBackend = gameplay`; design §5.1 C). Nothing reboots: both games stay on their own online CSS, agree on the match through Dolphin, and each starts it from there. Dolphin's side is in `docs/gameplay-rollback-status.md` (Phase 7); the plugin's is `source/online_match.cpp` and the connected part of `source/online_menu.cpp`. Slippi's flow is copied from `HandleInputsOnCSS.asm`, `LoadCSSText.asm` and `Slippi Online Scene/main.asm`.

### The flow, as a player sees it

1. **Search.** START on the CSS (Unranked) or the keypad's START (Direct) locks the player in for game 1 (Slippi's `FN_LOCK_IN_AND_SEARCH`) and searches. The lock-in goes to Dolphin through LOCAL.
2. **Connected.** Both are locked in, so the host's Dolphin decides game 1's setup at once: P1 = the host's lock-in, P2 = the guest's, the stage drawn at random from the server's `stages` list (`server/config/rulesets.json`: P+'s legal list, the 15 stages of `Switch00.rss`, see below), Slippi's stage pool: no stage again until every stage of the list has been played. Both Dolphins write it into SESSION. The CSS line reads "Playing: <name>" for a moment.
3. **The match.** Each game sees SESSION's setup for the game it is locked in for and leaves the CSS (the scene manager's exit code 1, the way every scene leaves). `sqNetAnyOkiraku` goes on to its stage select state, where our hook builds the match instead (below) and goes to `scMelee` through the load screen. Dolphin's session meets the other game at the first simulation frame (RNG, frame counters, the host's match init block, task order), runs the countdown without rollback and the game under rollback from frame 240.
4. **Game set.** Both sessions end on the same frame (`MAX_ROLLBACK_FRAMES + 12` after game set) and know the winner (more stocks, then less damage). The match's own end runs ("GAME!"), and state 10 sends the game straight back to the online CSS: Slippi online has no results screen.
5. **Between games.** Still connected, the character still selected (coin, costume and name tag as before the match). The line reads "Press START to lock in"; in Direct the loser of the last game reads "Press START to select stage" (a draw: both). START locks in for the next game; the loser's START opens P+'s stage select with every stage available (Slippi does not restrict Direct's pick), and picking a stage comes back to the CSS locked in with it (Slippi's `ExitSSSUponStageSelect`), the character still selected. Locked in, waiting: "Waiting on opponent". Once both are locked in, the host's Dolphin decides the setup (the loser's stage as picked, else random from the server's list with Slippi's stage pool) and the next match starts as in 3. Unranked is always random.
6. **Leaving.** Hold Z (48 frames) on the CSS: `CLEANUP_CONNECTION`; the opponent's Dolphin hears a "leave" at once and its game goes back to the idle prompt with the back sound (its next `GET_MATCH_STATE` reads IDLE, as on Slippi). Hold B leaves for the menus, which also cleans up.
7. **The opponent drops in a match** (closes Dolphin, loses the connection): after the silence limit (7.2 s at delay 2) Dolphin ends the rollback session and sets LOCAL `disconnected`. The game plays the error sound and draws "DISCONNECTED" in red at the top of its HUD (below); 90 frames later it ends the match as the pause screen's quit does (no "GAME!", no contest), and state 10 goes straight back to the CSS, idle, the character still selected. Dolphin's red OSD message only stands in when the game did not show the text within 30 frames (`osd_disconnects` in `game_bridge_status`).

### SESSION and LOCAL (PPOM v3)

Both live in the plugin's `.data` with the mailbox (`include/ppom.h`, mirrored by `tools/gamecode/ppom.py`; Dolphin: `Online/GameBridge.cpp` `SyncSession`, every frame at the frame-end hook, never while a match runs under rollback).

| Block | Who writes | Rollback | Contents |
|---|---|---|---|
| **LOCAL** (0x80, right after the mailbox) | Dolphin: `state` (0 none, 1 connecting, 2 connected, 3 in a match), `localPort` (0 host / P1, 1 guest), `remoteReady`, `disconnected`, `peerName`. The game: `lockIn`, `own` (its port values), `hudDisconnected` | excluded (one range with the mailbox) | what differs between the two machines |
| **SESSION** (0x210) | Dolphin only, never while a match runs | in the region set, constant during a match | `state` (0 none, 1 lobby, 2 the next game's setup is ready), `mode`, `game` (the next game, 1-based), `lastWinner` (port; 0xFE draw), `stageKind`, `asl`, `numPlayers`, `players[4]` {present, gmCharacterKind, costume, name, code, `pv` (port values)} by in-game port |

`LockIn` = {seq, ready, cssChar, charKind (gmCharacterKind, Random already drawn), costume, stagePick (0xFFFF none), asl, game}; with it the game writes `own` = `PortValues` {flags (1: a tag), rumble, tag name u16[5], the tag's controls layout (0x2D bytes)}. The game bumps `seq` on every change of either. SESSION is sized for four players so that code-based Teams can use it later; a session has two today.

### The match setup (`online_match.cpp`)

The Wi-Fi sequence's decide function (`sora_scene+0x36D74`) is a state machine on `seq+8` (jump table 0x80703B3C): 1/2 CSS, 3/4 stage select ("vote"), 5-7 Brawl's training room, 8-10 the network match, 11-13 results, 14/15 `scMemoryChange` (type `seq+0x11`, then `seq+0xC`), 16 back to the menus. Four module hooks run at the first instruction of states 3, 4, 5 and 10 (only non-volatile registers are live there: r15 = the sequence, r17 = loop again, r19 = the scene manager), call C, and either set the next state or run the original instruction.

- **State 3** with an online match armed: `gmSelCharData` (GameGlobal+0x10) players 0-3 are filled from SESSION, each from one fixed template (the empty record the online CSS starts with, `m_nameIndex` 0x78 = no name tag), with character, state human, colour and controller set; the others are "none". The local CSS's own records are saved first. P+'s alternate-stage buttons (0x800B9EA2) get SESSION's `asl`. Then **`sqVsMelee`'s own match setup** (`sora_scene` 0x806DCE94, `(seq, stageKind)`, which P+ also patches) builds `gmGlobalModeMelee` from them and the set rule, as a local Versus match does; the controller numbers are set to 1, 2 by port; then load screen and state 9 (`scMelee`).
- **State 3** for Direct's loser's pick: `gmSelCharData+4` (the menus' mode, 0x10 Wi-Fi) becomes 0 and the stage select is opened the Versus way, `setNextScene("scSelStage", 0)`. The Wi-Fi sequence opens it with 1 (Brawl's network stage vote), which waits for the other players' votes forever, and with mode 0x10 the stage select crashed.
- **State 4** after the pick: the stage (GameGlobal+0x14, +0x22, where `sqVsMelee`'s setup reads it) and the alternate-stage buttons are kept; back to the CSS (state 1 sets mode 0x10 again).
- **State 10** after the match: the saved CSS records are put back and the sequence goes to the CSS (load screen type 0xD, as state 13 does).

**Stages.** P+ v3.2's legal list as `srStageKind` (page 0 of `Switch00.rss` with the random bit set, through its slot table at +0x104): 0x01 Battlefield, 0x02 Final Destination, 0x03 Delfino's Secret, 0x04 Luigi's Mansion, 0x05 Metal Cavern, 0x06 Bowser's Castle, 0x09 Temple of Time, 0x0C Frigate Husk, 0x0D Yoshi's Island, 0x1C Wario Land, 0x1F Fountain of Dreams, 0x21 Smashville, 0x23 Green Hill Zone, 0x2D Dream Land, 0x2E Pokémon Stadium 2. P+'s stage file loader keys on the stage kind and the alternate-stage buttons (`Net-StageFiles.asm`), so these two pick the stage without the stage select.

**In a match** the plugin's tick does nothing but watch LOCAL's `disconnected` (which Dolphin sets only after it has ended the rollback session): no mailbox; nothing that differs between the machines may change the game while frames are resimulated. The plugin's `.data`/`.bss` is part of the region set (`rel_data`), except the mailbox and LOCAL.

DEBUG scratch for the tests: `[10]` the match flow (low byte: the last sequence hook, 3 setup, 0x33 stage select opened, 4 back from it with the exit code `<< 8`, 10 back from the match; `0x100 * game` when the CSS starts a match; `0x10000` disconnect seen, `0x20000` the game ended); `[11]` the last setup, `stage << 16 | P1 char << 8 | P2 char`.

### Each player's own controls (port values)

P+ players play with their name tag's controls (tap jump off, shield and jump remaps, P+'s C-stick "tilt"/"charge"...). Until 2026-10-07 the match setup cleared the tags so that both machines read the same controls, and everyone played on the defaults. Now each player's tag travels as the design's **port values** (5.1; Orca's `NameTags.h` carries the same bytes):
- **Picking.** The player picks a tag on the online CSS with Brawl's own name button (the Wi-Fi CSS keeps it: A on the name plate opens the tag list). Its save index is `muSelCharPlayerArea+0x1C8` (-1 none).
- **The tag's data** (P+'s format, `LegacyTE/CSSCustomControls.asm` "TAG FORMAT"): the save's tag records (`g_GameGlobal+0x28` → records, tag i at `+0xE0 + i * 0x124`, i.e. `0x90172E20` on P+): name u16[5], rumble `+0x0C`, controls `+0x14..+0x40` (GameCube 12 bytes: L, R, Z, D-pad up/side/down, A, B, C-stick, Y, X, then flags with tap jump `0x80` and P+'s "controls set" `0x70`; then Wii Remote 8, Nunchuk 12, Classic 13).
- **Exchange.** The lock-in carries `own` = the tag's `PortValues` (LOCAL); Dolphin sends it with the lock-in in its control messages (`"pv"`, hex); the host's match setup carries each player's values, and both Dolphins write them into SESSION `players[i].pv`.
- **Applying** (`online_match.cpp`). At the match start the game gives each player's controller its layout: `fn_801104C4(gmGlobalModeMelee*)` calls ipPadConfig's setter `fn_8004A2C8(g_PadConfig, player, pad, layout)` for players 0-3 at `0x80110550`, with the tag's layout (record `+0x18` = the tag index) or the default (`0x80406938`). `g_PadConfig` (`0x805B7480`, `ipPadConfig`, 0x1AC bytes: GameCube pads 12 bytes each at `pad * 0xC`, Wii Remotes 0x21 bytes from `0x30`, player → pad at `+0xB5`) is what every frame's input goes through. The match keeps no tags (both machines build identical records), so the hook at `0x80110550` passes each port the layout kept from SESSION at the setup instead, the same on both machines. Values from the other machine reach the game only as its menus could set them (actions up to `0xE`, flag bytes masked), as Orca does.
- **The check.** The setup key the session compares at the barrier now includes `g_PadConfig`'s GameCube layouts and its player → pad map, so two machines that applied different controls refuse the match.
- Name tags are not shown over the fighters online (the records keep "no tag"; Slippi online shows no tags either). The tag's rumble byte is not applied (rumble is local only). Only GameCube layouts matter today: the session's ports are GameCube pads.
- **Dolphin's own controller mapping** keeps working and needs nothing: Dolphin maps the physical controller (`GCPad::GetStatus`, Dolphin's controller config) before the game's pad thread reads the SI, and the session sends the game's pad slots (`PadsLatestRaw`), i.e. the mapped input. The tag's layout is then applied by the game on both machines.
- Slippi has no custom controls in Melee. What it carries per player besides the selections (character, colour, team, `SlippiPlayerSelections`) is the display name, the connect code and the player's 16 chat messages (from the matchmaking server); UCF is the same for everyone.
- Verified by `test_each_player_keeps_their_tag_controls` (below).

### The CSS remembers the character

Slippi keeps the character selected after a game and after the stage select. Brawl's Wi-Fi CSS writes no selection into `gmSelCharData` when it leaves, but when it starts it builds the player's panel from it: the character from the per-port byte `gmSelCharData+0x0A + 4 * port`, the state, colour and name tag from the player's record (`+0xB8 + 0x5C * port`: `+0x01` state, `+0x05` colour, `+0x18` tag). So the plugin notes the panel (character, costume, tag) whenever the CSS leaves for a match or the stage select, and writes it back before the CSS starts again (states 4 and 10, `OnlineMenu::restoreCss`): the coin is on the character, with its costume and tag. If the coin was in the hand when the CSS was left, the last lock-in's character is used.

### DISCONNECTED in the match, and the end without "GAME!"

Slippi (design 5.6): the error sound, "DISCONNECTED" in red (`FF0000FF`) centred near the top of the HUD until the scene ends, and an LRAS-type end (no "GAME!", a 90-frame end screen). Ours (`online_match.cpp`, `match_hud.cpp`):
- **The text** is drawn with the game's own text renderer, `ms::CharWriter` (the base of Brawl's `Message`/`MuMsg` text), and the game's resident system font (font kind 4, the one P+'s Code Menu prints with): red with a thin black edge, scale 0.9 of the 640x480 HUD space, centred, top at y=70 (below the timer). `MuMsg` itself cannot be used: a `MuMsg` window draws only when attached to a model node, and the match HUD has none free at the top centre (the only in-match `MuMsg` is the pause screen's song title). It is drawn from an inline hook at `0x8001792C` in gfApplication's frame loop, after the frame is drawn and before the copy to the XFB (P+'s Code Menu draws one instruction earlier the same way), with its own 2D projection and no depth test. Nothing is allocated; it draws only in `scMelee`.
- **The end.** Brawl's pause-screen quit (L+R+A+START) sets bits `0x30` in the flags byte of the match's `stOperatorInfoMelee` (`scMelee+0x68`, `+0x11B`); bit `0x40` (its own stop request, sora_melee text+0x257574) ends the match on the next frame, and with `0x30` the end type is 3: no "GAME!", no announcer, "No Contest". 90 frames after the text appears the plugin sets `0x70`; the scene leaves `scMelee` about 17 frames later through Brawl's ordinary end, and state 10 goes back to the CSS. The departed player is no longer killed (`ftManager::setDead`, which went through Brawl's game set and its "GAME!").
- **Dolphin's OSD** "DISCONNECTED" is now a fallback: the game sets LOCAL `hudDisconnected` when it draws the text; if it has not within 30 frames of a disconnect in a match, Dolphin shows its red OSD message (`osd_disconnects` in `game_bridge_status` counts them). A disconnect on the CSS shows no text, as on Slippi.
- Tests can trigger the flow in any match with DEBUG `cfg` `CFG_TEST_DISCONNECT` (`1 << 5`).

### The stage select

Slippi restricts no stage select: Direct's loser picks any stage, Unranked and Teams' game 1 draw from the server's list without a stage select, and only Ranked has its own strike screen. So Direct's loser gets P+'s whole stage select (the design's "restricted to the server list" in 5.4 row 5 was corrected on 2026-10-07). The mechanism that restricts it is kept in `stage_legal.cpp`, behind the debug flag `CFG_SSS_LEGAL` only, for Ranked's strike and counterpick steps later. It drives P+ v3.2's own **stage striking** (X on its stage select: a struck stage is drawn greyed with a white X, cannot be highlighted or picked, and P+'s random skips it): strike table `0x8042C822` (5 pages × 6 bytes, bit = page position), redraw trigger `0x8042C821`, random switch `0x8042C4E8`; hooks at `sel_stage+0x4EDC` (fill the table; B and X cannot change it), `+0x525C` (a struck stage cannot be taken on the frame the cursor enters it, a gap in P+'s own check), `+0x54C0` (stage-builder page), `+0x4CF4` (B still leaves). Verified with the flag in a Versus stage select (`run/artifacts/game-code/sss-legal/`): the 6 illegal stages of page 1 and pages 2-3 struck, A on them does nothing, legal stages pick and load, random picked only legal stages in 6 of 6. `online_set.lock_in_next` checks that nothing is struck on Direct's loser's stage select.

### Verified

`harness/tests/test_online_game.py` (plugin with PPOM v3, Dolphin `rollback-fixes` with this change, 2026-10-07):
- `test_direct_set_under_the_gameplay_session`: two games from the in-game Direct flow (screenshots `run/artifacts/game-bridge/gameplay-set/`), results in `docs/gameplay-rollback-status.md` Phase 7. Now also: after game 1, and after the loser's stage select, each CSS shows the player's character with the coin placed, the same costume and tag (`online_set.check_css_remembers`); nothing is struck on the loser's stage select.
- `test_each_player_keeps_their_tag_controls` (`tag-controls/`): A picks the tag "NoTap" (tap jump off) on the online CSS, B none. Both SESSIONs carry A's tag; both machines' `g_PadConfig` give A's pad tap jump off and B's on. In the match A's stick is held up by A's **Dolphin controller mapping** (`GCPadNew.ini` `Main Stick/Up = 1`, the harness lets go of the pad) and B's by the harness: on both machines both sticks read up, A stays on the ground and B jumps; the confirmed checksums of both peers agree.
- `test_opponent_leaves_in_the_middle_of_a_game` (`gameplay-leave/`): the drop after 7.3 s, DISCONNECTED in the HUD at once (`01-dropped-in-match.png`), the quit flags set 90 frames later (`02-ending-no-game.png`: no "GAME!"), `scMelee` left 110 frames after the text, back on the CSS idle with Fox still selected (`03-back-on-css-idle.png`), no OSD message.
- `test_online_menus_match_the_modes` (`run/artifacts/game-code/menu-modes/game-m/`): the mode pages, each mode's CSS and the way back (§6).

`harness/tests/test_online_css.py` (2026-10-08; status line, settings, banner, own code, keypad; §6, §7): `test_status_line_is_never_squeezed`, `test_online_css_settings_are_locked`, `test_stale_settings_do_not_reach_the_match` (two instances), `test_code_keypad_hash_key_and_placeholder`, `test_ready_banner_between_games` (two instances). Screenshots in `run/design/online-css/`. `_connected_direct(test_rules=False)` plays with the real online rules; `before_online` runs steps on the main menu first (offline settings).

`harness/tests/test_online_unranked.py` (Dolphin `rollback-fixes` after the `unranked` merge `4944245954`, plugin `7ca3a7e`, server ruleset with two-stage lists, 2026-10-07; `run/artifacts/game-bridge/unranked-set/`, `direct-server-stages/`, `unranked-timeout/`):
- `test_unranked_pairs_strangers_on_a_server_stage`: WITH ANYONE → Unranked (the Wi-Fi OPTIONS page), both search, the server pairs them, and the two games of the set are drawn from the server's list, one per stage (Slippi's stage pool).
- `test_direct_loser_picks_off_the_server_list`: Direct's game 1 is drawn from the server's Direct list; the loser's Final Destination pick for game 2 is played although the list does not have it.
- `test_unranked_search_ends_with_the_server_error`: a lone search ends with the server's timeout text; Z clears it.

`harness/tools/e2e_launcher.py` plays the same set from the two launchers, through WITH FRIENDS → BASIC VERSUS, and checks the CSS after the loser's stage select (`run/artifacts/e2e-launcher/slp-e2e-20261007-144841/`). It now refuses a launcher bundle older than `launcher/`'s sources (an earlier run had driven a bundle built before the Brawl Online rename) and checks the rules label ("I accept the Brawl Online Rules").

### Open issues

1. ~~The CSS forgets the placed coin.~~ Done: the CSS remembers the character, costume and tag (above).
2. ~~No DISCONNECTED in the HUD; "GAME!" after a disconnect.~~ Done (above). The text is drawn with the game's text renderer and font, not a `MuMsg` window (none is free in the match HUD).
3. ~~Default controls for everyone.~~ Done: name tags and their controls as port values (above). Not carried: the tag's name over the fighter, rumble, Wii Remote / Classic layouts (the session's ports are GameCube pads).
4. **Ranked** (strikes, counterpicks, the set) and **Teams** (code-based, 4 peers) are not built; both modes are reachable from the menus and show the server's error. The server sends P+'s legal list as `stages` for Direct, Unranked and Ranked (since 2026-10-07). P+'s stage striking is ready for Ranked's strike screen (`CFG_SSS_LEGAL`).
5. Leaving an Unranked or Ranked CSS lands on the ONLINE page (WITH ANYONE highlighted), not back inside the Unranked / Ranked page as on Slippi: no menu decision reopens Brawl's Wi-Fi OPTIONS page.
6. A harness stall seen twice on 2026-10-07 while the machine was saturated (another agent's full Dolphin build, other agents' Dolphins, a game): the instance stopped answering `ping` and used almost no CPU. Thread stacks (`run/artifacts/game-code/stall-20261007/`): the first time the video thread waited inside the D3D11 driver and the CPU thread waited on it; the second time the process just got about 0.4 of a core. Both went away when the load dropped; the GPU probe of the test fixture skipped tests twice for the same reason.

---

## 12. Pending game-side changes

Found while adding Unranked matchmaking and the server's stage lists (Dolphin branch `unranked`, 2026-10-07; merged into `rollback-fixes` as `4944245954`). Nothing here blocks Unranked: the menu path already searches Unranked (WITH ANYONE → Unranked since the mode pages of §6; before, BASIC VERSUS; `0xB4 {mode 1}`), the CSS shows the server's errors, and the match setup takes the stage from SESSION.

1. ~~The stage select is not restricted to the server's list.~~ Not wanted: Slippi does not restrict Direct's loser's pick (coordinator's correction, 2026-10-07; §11 "The stage select"). Dolphin's branch `unranked` took the pick only if it was in the match's `stages`; that check was dropped at the merge (`4944245954`), so Dolphin plays the loser's pick as it is (`test_online_unranked.py::test_direct_loser_picks_off_the_server_list`). The game-side mechanism (P+'s stage striking) is kept behind `CFG_SSS_LEGAL` for Ranked.
2. **Unranked always draws the stage.** Correct as it is: the plugin opens no stage select in Unranked (`online_menu.cpp`, the loser's pick is Direct-only), as Slippi.
3. ~~**PPOM v3.**~~ Done: the merge (`4944245954`) kept `rollback-fixes`' GameBridge and its v3 SESSION/LOCAL layout; the `unranked` branch never touched the layout (its stage list reaches the game only as SESSION's `stageKind`). `test_online_unranked.py` now runs against the current plugin (`pponline` `7ca3a7e`) and its menu: WITH ANYONE → Unranked on the Wi-Fi OPTIONS page.

---

## 13. The boot: no save prompt, straight to the ONLINE page

Player reports (2026-10-08): P+ asked to create a save file on start-up, and Play opened P+'s single-player Versus character select, from which players had to know to back out to the online menu. Screenshots of every step below: `run/design/boot-flow/` (local).

### The save prompt (launcher)

**What asks, and when.** Brawl's boot save check (`scBoot`, the `muBootNandTask` of `sora_menu_boot`, state 5) opens P+'s "Create save file for Project+?" whenever the NAND has no Brawl save under `/title/00010000/52534245/data` (`0-before/01-empty-nand-save-prompt.png`, reproduced with an empty NAND). Play boots P+ from the User folder's own NAND (`<User>/Wii`; Dolphin's `Sys/NetplaySave` only goes into the temporary NAND of a Dolphin netplay session), and a new install's NAND is empty, so every new player met it on the first Play; with No, on every Play. Yes spends about 13 s writing a 14.6 MB save. The development User template already has a save (byte-identical to P+'s `NetplaySave`), which is why no test ever showed the prompt.

**The fix: the launcher seeds the save** (`launcher/src/dolphin/install/brawl_save.ts`, called by `DolphinManager.launchNetplayDolphin` before every Play that boots the game). When the NAND's Brawl save folder has no file, the save files of P+'s official Brawl save template are copied in: `<userData>/netplay/pplus/NetplaySave` (from P+'s release, already downloaded at setup), else `<User>/NetplaySave` or `<Dolphin>/Sys/NetplaySave`. Only the 10 data files (16.7 MB, well under a second): Dolphin writes the title's TMD itself when the disc boots. The copy goes to `data.seeding` and is renamed into place, so an interrupted copy is never taken for a save. A save folder with any file is never touched, so what the game writes stays. The NAND is Dolphin.ini's `[General] NANDRootPath` when set, else `<User>/Wii`. A failure is logged and Play goes on (the game then asks, as P+ does).

Why this rather than answering in the game (Orca's way: the boot check's own "continue without saving"): without a save the game keeps nothing, and name tags with their controls (which online play carries as port values, §11) would be lost at every restart; answering Yes in the game costs 13 s on the first boot. The template is P+'s own netplay save, the same for every player, with P+'s preset tags (Chrg1/2, Tilt1/2, ...).

**Online determinism.** Nothing online reads the save beyond what the session already syncs: the match setup (characters, stage, rules, items) comes from SESSION and the plugin's online rules, each player's controls travel as port values, and the setup key compared at the barrier (§11) refuses a match whose applied setup differs. Players had arbitrary saves before (the template, or one written by the game); the seeded template makes them more alike, not less.

**Verified** (harness instance booted through the launcher's boot path: Netplay Launcher DOL, patched SD with the plugin, the NAND seeded by the launcher's own `ensureBrawlSave` through `ts-node`):
- fresh NAND: `seeded` (10 files), no prompt, the ONLINE page first (`1-fresh-nand/01-first-screen.png`);
- the same NAND again: `present`, files unchanged, no prompt (`2-second-boot/01-first-screen.png`);
- a name tag "D" made in Versus > Names: the game rewrote `autosv0/1.bin` when leaving the screen; the next Play: `present`, the game's files unchanged, the tag still listed (`4-next-play-keeps-save/02-names-kept.png`);
- `brawl_save.test.ts` (7 tests: seed, never overwrite, partial save kept, Dolphin's empty folder filled, interrupted copy redone, template order, NAND root).

### Boot to the ONLINE page (plugin)

`boot_menu.cpp`, after Orca's "Boot and friends from the menus" (approach and addresses; ORCA.md, `PPLUS32.patches`). P+'s codeset boots to its Versus CSS through "Boot Directly to CSS v5.4" (`BootToCSS.asm`, a HOOK at `sqBoot::setNext` 0x806DD5F8 whose code is in `NETBOOST.GCT`/`BOOST.GCT`, read to 0x80550010): with no special input held, `setNextSequence("sqVsMelee", 0)` instead of the game's `setNextSequence("sqPrizeCheck", 0x14)`. Going to `sqMenuMain` straight from `sqBoot` runs out of the OverlayMenu heap (Orca), so the boot takes the game's own way to the menus, P+'s Start case:
1. The plugin replaces `gfSceneManager::setNextSequence` (0x8002D640). A call from `sqBoot` with `"sqVsMelee"`, 0 becomes `"sqPrizeCheck"` (the game's string at 0x80701C94), `0x14`, only while P+'s default case is exactly BootToCSS v5.4's words (Orca's guard: `41A0FF84 38951B54 38A00000 48000038 38951C94 38A00014` at 0x80559C20 in `NETBOOST.GCT`, 0x80559C30 in `BOOST.GCT`). The hook's branch is rewritten by the code handler every frame, so the plugin does not patch it; the codeset's memory is not written at all. Another codeset (a P+ update, a player's own boot code) keeps its boot; L/R (Training), Z (Replays) and Start (the title) at boot are P+'s as before.
2. On that way only, scTitle's opening movie and "press Start" logo become state 16, the title's exit with result 0 (inline hooks at `sora_scene+0xECA4`/`+0xECB0`, Orca's 0x806CA1F8/0x806CA204), so the title shows nothing.
3. `sqTitle` asks for `sqMenuMain` with 0; the plugin gives 30, the menu's argument for the ONLINE page with WITH FRIENDS highlighted (`sqMenuMain` turns 30/31 into the menu modes 34/35, the ONLINE page on WITH FRIENDS / WITH ANYONE).

After that the plugin leaves every sequence alone: B on the ONLINE page goes to the main menu (PLAY ONLINE highlighted), B there to P+'s title as in P+, leaving a CSS goes back where it did (§6), and "Configure Dolphin" (the user's own card, no plugin) is P+ as shipped.

The plugin first did not load with this code: see "Heap budget" in §2.

**Startup time.** P+'s Versus CSS appeared at application frame 220-221; the ONLINE page at 227-231 (one run 244), about 0.15 s later. With an empty NAND (no launcher) the save prompt still comes first, then the ONLINE page.

**Verified** (Netplay Launcher DOL unless noted, Dolphin `rollback-fixes` build of 2026-10-07, `run/design/boot-flow/`): the first screen on a fresh and on a second boot is the ONLINE page (`1-fresh-nand/01`, `2-second-boot/01`); B to the main menu (`1-fresh-nand/02`), PLAY ONLINE back to the ONLINE page (`03`), WITH ANYONE's Unranked / Ranked page (`04`), the Unranked CSS (`05`), hold B back to the ONLINE page with WITH ANYONE highlighted (`06`), B to the main menu (`07`); the Offline Launcher too (`5-offline-launcher/01-first-screen.png`). DEBUG `scratch[14]` = `0xB` on every boot. Before: `0-before/02-pplus-default-boot-versus-css.png`.

**Tests.** `to_main_menu` in `test_online_game.py` (and `e2e_launcher.py`, `scenarios/to_online.txt`) now expects the ONLINE page after the boot and checks `scratch[14]`; with `PPHARNESS_PLUGIN` set to an older plugin build it still takes P+'s Versus CSS route. Passed on 2026-10-08: `test_online_menus_match_the_modes`, `test_direct_from_the_game_menus`, `test_recent_codes_on_the_keypad`, `test_character_locked_while_searching`, `test_direct_set_under_the_gameplay_session`, `test_opponent_leaves_in_the_middle_of_a_game`, `test_each_player_keeps_their_tag_controls`, `test_direct_from_the_game_menus_hands_off_to_netplay` (second run; in the first, one instance stopped answering frame waits while entering the CSS, as in the machine's known stalls under load), `test_online_unranked.py` 3/3 (`test_direct_loser_picks_off_the_server_list` on its second run; the first desynced from session frame 0 on Smashville, below). `test_e2e_launcher.py` was not run.

**Noted, not changed:**
- The netplay fallback (`[Online] SessionBackend = netplay`) boots P+ again under Dolphin netplay with the plugin, so its players now land on the ONLINE page instead of P+'s Versus CSS (`direct-netplay/*/06-netplay-boot.png`). The default gameplay session never reboots.
- A Smashville desync from session frame 0 came back once in `test_direct_loser_picks_off_the_server_list` (game 1 on Smashville, equal setup keys, the barrier passed, the first RNG word different from frame 1; logs in `run/scratch/bootflow-smashville-desync/`): `docs/gameplay-rollback-status.md` open issue 7, which the session's match-start changes did not remove. The boot and the save do not reach the match setup; the rerun passed.
