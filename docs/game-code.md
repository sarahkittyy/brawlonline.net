# Game-side code: toolchain, SD patching and the online menus

This covers the game-side half of design §5 (`docs/backend-design.md`): the C++ plugin that runs inside Project+ v3.2, how it is built and put on P+'s SD card, what the in-game online menus look like today, the connect-code entry research, and the game↔Dolphin mailbox prototype.

Paths:
- Code: `game-code/`, a separate git repo (gitignored here). It is a fork of `brawlback-asm` with full history; see `game-code/NOTICE`.
- Plugin: `game-code/PPOnline/`.
- Scripts: `tools/gamecode/` and `tools/sdcard/`.
- Screenshots: `run/artifacts/game-code/` (local only, `run/` is gitignored).

Everything here was verified on NTSC-U Rev 1 + P+ v3.2, booting the P+ Offline Launcher with headless D3D11 in dual core, with the Dolphin build `run/bin/menu-7b2227fd5e` (a frozen copy of `dolphin/build/release/x64/Binaries` at `7b2227fd5e`). The Netplay Launcher also loads the plugin and shows the relabelled main menu (`screens/netplay-launcher-main.png`).

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
| `drive.py --port P STEPS...` | The step language: `until SCENE`, `tap BTN N`, `hold`, `wait`, `shot`, `mem`, `@scenario.txt`. Also `mbx-serve` / `mbx-dump`, which act as the Dolphin side of the mailbox. |
| `scenarios/to_online.txt`, `online_unranked.txt`, `online_direct.txt` | The verification runs that produced the screenshots below. |
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
| muProcWifiAnybody page enter | `sora_menu_main+0x2E4F8` | simple | Gen 1's `SkipDirectlyToCSS`: leave muMenuMain with decision `0x1E`, which lands in `sqNetAnyOkiraku` → the Wi-Fi CSS |
| muProcWifi A press | `sora_menu_main+0x168B8` (and `+0x166B8`, `+0x16518`) | inline | Turn page id `0x1A` (friends) into `0x1B` (anybody) and remember that FRIENDS was picked |
| Wi-Fi CSS countdown, timer, network error, disconnect panel | `sel_char+0x4220`, `+0x56A8`, `+0x53A4`, `+0x4A70` | simple | Ported from Gen 1. The disconnect-panel hook is now one naked hook, without Gen 1's `SaveRegs` |
| Wi-Fi SSS countdown, network error | `sel_stage+0x141C`, `+0x30F0` | simple | Ported from Gen 1; not exercised yet |

Notes:
- P+ v3.2's `sora_menu_main.rel` is vanilla's with the same `.text` size. Its 152 non-relocation word differences are confined to a few P+ edits (`reltool.py diff`), so Gen 1's offsets hold.
- P+ keeps `sel_char` (`.text 0x806828C4`), `sel_stage` (`0x806B0984`), `sora_menu_name` (`0x8067406C`), `sora_scene` (`0x806BB554`) and others resident. That is why Gen 1's absolute jump-back addresses are valid on P+.
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

Screenshots are in `run/artifacts/game-code/screens/` from the last runs of `online_unranked.txt` and `online_direct.txt`. Vanilla P+ references are in `explore/`; the step-by-step discovery shots are in `wifi/`.

| # (design §5.4) | Slippi | What you see now | Status | Screenshot |
|---|---|---|---|---|
| 0 | hello world | Main-menu PLAY ONLINE description replaced through `MuMsg` in the game's font | done | `hello/main-online-hover.png` |
| 1 | Online menu entry | Main menu: PLAY ONLINE description = "Play online: Ranked, Unranked, Direct, Teams. (Sarah, SARA#001)". The account line comes from mailbox `0xB9`. | done | `screens/01-main-play-online.png` |
| 1 | (no Slippi equivalent) | Brawl's connect dialog relabelled "Connect to online play?" (Yes/No), then "Connected." The WFC login is faked, so nothing goes online and no error is shown. | works, **not Slippi UX yet** | `02-connect-dialog.png`, `03-connected.png` |
| 1 | (no Slippi equivalent) | First-time Wi-Fi "Choose a profile name." keypad. It appears once per save. | still shown | `04-profile-name-keypad.png` |
| 1 | Online submenu | ONLINE page: WITH FRIENDS = DIRECT, WITH ANYONE = UNRANKED. Button art unchanged, descriptions relabelled. | done | `05-online-direct.png`, `06-online-unranked.png` |
| 2 | CSS with status | Brawl's Wi-Fi CSS (`sqNetAnyOkiraku`/`scSelctCharacter`), no countdown, no network-error dialog, Brawl's own "Seeking..." panels. Status in the header rule line: "Unranked: START to search" → "Searching for opponent" → "Playing: Opponent". | done (prototype) | `07-css-unranked.png`, `08-…-searching.png`, `09-…-found.png` |
| 3 | Connect-code entry | Direct CSS: "Direct: START to enter code". START opens Brawl's name keypad on the CSS; type letters, then digits; OK → "Searching for EG#123" → "Playing: Friend". | prototype | `10-css-direct.png` … `15-direct-found.png` |
| 4-10 | Opponent on CSS, stage choice, ranked setup, rank, results, disconnect, chat | not started | — | — |

Known gaps on these screens:
- **CSS header.** The art says "HOME-RUN CONTEST" next to LEAVE, and the rule line shows a "2" numeral. Both are art from the Wi-Fi mode: the mode title is a texture, and the numeral is the rule count. We print our status after the numeral. Fix: load P+'s competitive ruleset into the Wi-Fi rules (Gen 1's `Get/SetRulesFromCSSBoot`, not ported yet), so it reads "4-stock" and the line becomes ours.
- **Status line style.** `MuMsg::printf` does not apply msbin style tags, so we set the colour to the original line's black with `setFontColor`. Longer texts are clipped by the fixed, right-aligned window, so the strings are kept short.
- **Do not print a lone `" "` into the CSS rule window.** It froze the display: presents stopped while the game kept running. This was found by bisecting with `ppom.py cfg`.
- **WITH FRIENDS must not open Brawl's friend page.** Leaving muMenuMain from inside that page froze the display the same way, so the A-press hook reroutes it to the Anybody path instead.
- **Leaving the online CSS is not handled yet.** Holding B with no token does nothing; LEAVE was not tested. Gen 1's `ExitWifiCSSReturnsToDirectOrQuickplayScreen` (`sora_scene+0x3770C/0x37708`) is the starting point.
- **Description strings.** Mode names and order follow Slippi (Online.s). Slippi's exact description strings live in its patched Melee menu files, which are not in `refs/`. Ours are placeholders to replace verbatim once someone extracts them from a Slippi ISO patch.

**Ranked and Teams placement.**
- P+'s ONLINE page has two buttons whose labels are art, so we can't add a third button without new art.
- WITH ANYONE in vanilla Brawl opens a page with BASIC VERSUS and TEAM BATTLE (`wifi/b1-anyone.png`). We currently skip that page so that WITH ANYONE = UNRANKED.
- **Proposal for P4:** keep that page. BASIC VERSUS = UNRANKED, TEAM BATTLE = TEAMS. RANKED goes on the same page as a third option once a reusable button panel is identified (the vanilla page layout may have hidden slots). Until then RANKED is listed in the menu text only, like Slippi's locked items.
- **This needs the user's decision.** The alternative (one button toggling Unranked/Ranked) would be a new flow.

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

**Prototype: Direct's START opens it from the CSS.** This mirrors Slippi, which opens Melee's name-tag keyboard from the CSS.
- The CSS opens "New entry" with `sel_char+0x18E1C`: `area+0x400 = area+0x1DC`, then `helper(area+0x370).open(NULL, NULL, 5)`. It then sets the hand to mode 8 with `sel_char+0x1A348`.
- `code_entry.cpp` does the same two calls, but with `open(NULL, ourBuffer, 7)`. It re-asks for mode 8 for up to 30 frames, because the hand sometimes refuses right after START.
- It reads our buffer each frame. When the keypad closes, it converts full-width text to ASCII and inserts `#` between the letters and digits ("EG123" → "EG#123"), then starts the search.
- Verified: `11-direct-code-keypad.png` → `13-direct-code-typed.png` → `14-direct-searching.png`. The mailbox received `FIND_OPPONENT mode=direct code='EG#123'`.

Prototype limits:
- The key labels show lower case and symbols.
- The underline stops at 5.
- OK also runs the CSS's own new-name path. The player's name tag shows "EG123" afterwards, and the reserved-name check runs.
- The CSS copies the result into a stack buffer sized for 5 characters, so we cap at 7 (21 UTF-8 bytes + NUL). Codes of 8 typed characters would need the next step.
- L/R history (mailbox `0xBE`) and Z-to-accept are not wired.

**Recommended next step.** Drive the same helper from our own gfTask, as `WifiCnctWnd` does at `sora_menu_main 0x38800–0x38A54`. That task creates the eight `MenSelchrW*` models from `mu_menumain.pac` and a MuMsg, then calls init with `line=NULL`. Then:
- open with max 8 and our buffer;
- limit pages to alphabet/digits (`helper+0x24 = {2,3}`, `+0x38 = 2`);
- point one key at "＃" while open, and restore it on close (the table is shared);
- handle L/R/Z ourselves before calling `update`.

This avoids the CSS name path and its 5-character stack buffer.

---

## 8. Mailbox round trip: Dolphin plays its part

Dolphin services the mailbox itself (`GameBridge`, dolphin branch `game-bridge`; design §5.2 has the details). No harness server is involved. Verified by `harness/tests/test_online_game.py` against our `accounts` + `mm` servers. Each instance has its own `user.json` and its own copy of the SD card, patched with `PPOnline.rel` through `tools/sdcard/patch_sd.py`. Screenshots are in `run/artifacts/game-bridge/`.
1. **Main menu.** The plugin posts `0xB9`. Dolphin answers from `user.json` and the accounts lookup. PLAY ONLINE then reads "(carl, CARL#322)" (`unranked/game-u/01-main-play-online.png`).
2. **Direct.** START opens the keypad, and the test types the other account's code. Typing needs these rules: the cursor does not wrap; UP from row 1 reaches backspace; the same key twice needs a move off and back, because there is no multi-tap timeout. OK posts `0xB4 {mode 2, code}`. Dolphin starts the search and answers `mmState 1`, and the header reads "Searching for BOB#610" (`direct/game-a/05-searching.png`).
3. **Z cancels.** The game posts `0xBA` and Dolphin cleans up. The header goes back to "Direct: START to search BOB#610" (`06-cancelled.png`).
4. **Match.** The other player searches and START searches again. The server pairs them (mm log `matched`). Both games are told `mmState 4` with the peer's name, code and role, and show "Playing: bob" / "Playing: alice" (`07-opponent.png`). `Online::Session::Start` was called on both sides with the match (harness `record` backend).
5. **Unranked.** The server refuses the ticket. The game gets `mmState 5` with "Unranked is not supported yet. Only Direct works for now." and prints it in the header (`unranked/game-u/05-unranked-error.png`). Z clears the error.
6. **Hand-off to netplay.** With the real backend (whole-machine netplay, design §5.1 A), both Dolphins stop their games right after the match and boot P+ together under rollback. The plugin is loaded again and the block is found again, but servicing is paused while netplay runs (`direct-netplay/*/06-netplay-boot.png`).

`drive.py mbx-serve` and `ppom.py serve` still work for experiments without a server. Turn Dolphin's servicing off first: `ppharness cmd --port P game_bridge_config enabled=false`.

---

## 9. Next steps

1. Skip Brawl's connect dialog and the first-time profile-name prompt, so PLAY ONLINE goes straight to the ONLINE page as on Slippi. These are `muWifiCnctWndTask` states (`sora_menu_main` rodata+0x1698 jump table).
2. CSS: put P+'s competitive rules into the Wi-Fi rules, handle LEAVE / "hold Z to disconnect", lock in on START (don't search without a character), and block A/B on the name tag while searching.
3. Own code-entry task (section 7), with history through `0xBE`.
4. Ranked/Teams placement once the user decides (section 6).
5. SESSION/LOCAL blocks. MAILBOX is serviced by Dolphin now (§8); SESSION/LOCAL need the gameplay-only session.

---

## 10. Repos and commits

- `game-code` (fork of brawlback-asm, branch `pponline`; upstream refs under `refs/remotes/upstream/*`):
  - `0a4c153`: plugin skeleton, PPOM block, hello world.
  - `fb4fd87`: Wi-Fi flow, mailbox, Direct code entry.
  - `42036d6`: `GET_MATCH_STATE` polling and the protocol rules for Dolphin's GameBridge (§5).
- Top-level repo: `docs/game-code.md`, `tools/gamecode/`, `tools/sdcard/`. `.gitignore` already listed `/game-code/` and `/toolchains/`.
