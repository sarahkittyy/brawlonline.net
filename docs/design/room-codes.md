# Room codes: the host's Gecko / Action Replay codes in Other Games rooms

_2026-10-30. Built. Sarah's board ("AR/gecko codes ... hosting a game runs the game without codes", a player whose communities play netplay with codes; Sarah: "let the host set codes for the room. they should stay persistently ... some games have a list of gecko codes already from dolphin that we should expose ... minimal text in the uis"; on the design: "they need to apply the codes when they host. i like this ui a lot. go for it."). Mockups of the design: `run/codesdesign/*.png`; the built UI: `run/codesreal/*.png` (local, not in git)._

## 1. Before

A room session turned cheats off for everyone: `MakeNetSettings` set `ns.enable_cheats = false` (`MachineSession.cpp`), and the netplay config layer forced Dolphin's Enable Cheats off over every ini, `Sys/GameSettings` included. Offline the player's codes ran, so they looked lost online.

The player's own `[OnFrame]` patches (`User/GameSettings`) were not pinned: `PatchEngine::LoadPatches` read them on every machine, so two players with different patches for a game desynced. A session now reads `[OnFrame]` from Dolphin's own game inis only.

## 2. Decisions

| # | Topic | Decision |
|---|---|---|
| 1 | Where | **Other Games rooms only**, and only for a game booted from its disc. P+ (Play, Ranked, Unranked, Direct, P+ rooms) keeps its own codeset; a mod booted by its launcher (REX, Minus, Brawl Online's own P+) has none: the launcher installs its own Gecko code handler at 0x80001800, where Dolphin's goes, and the mod's codes are in it. |
| 2 | Who | **The host's codes run on every machine.** A joiner's own codes are never used (as Dolphin netplay's Sync codes). Joining needs nothing: no download, no question; the room's chip says which. |
| 3 | When | **Set before hosting** (Sarah's). The room runs the codes it started with; while it is open the game's dialog is read-only (a lock, the On list). To change them: leave, change, host again. |
| 4 | Where they're set | A **Codes** button on each game row in Your games (`<> 5`: the count on, their names on hover; `<> CODES` greyed with none). |
| 5 | Remembered | **Per game row** (`ModBuild.codes`, the launcher's settings) until changed. Host the same game again: the same codes. |
| 6 | Lists | Sections: **Yours** (pasted with +), **Your Dolphin** (read on every open from Brawl Online's Dolphin and the player's own Dolphins, the folders of 064d171f's save import), **Dolphin** (Dolphin's own list for the game, shipped with the launcher), **Database** (codes.rc24.xyz, the geckocodes.org mirror Dolphin's Download Codes uses; folded, fetched when first opened, kept a week). A code shows once, in the first list that has it. |
| 7 | First time | A game starts with the codes on in the player's own Dolphins, as Dolphin decides it (`+$`, `_Enabled`, or on by Dolphin itself and not `_Disabled`); a code the player's ini redefines stands for Dolphin's of that name. Not for a Brawl disc: its inis hold an old P+ netplay setup's codes. Seeded at the launcher's start and when a game is added, so the row's count shows them. |
| 8 | Row | Switch, name, author (Dolphin's `[author]`, an AR code's too), `AR` for Action Replay, chevron: the notes and the lines. Edit / delete only on Yours. A code with lines to fill in (`2834XXXX`) has its switch off and greyed ("Has lines to fill in"); so has one that no longer fits ("No room for more codes"). |
| 9 | Text | Title "Codes", the game and its ID, "N on". Search, All / On, +. Section names; tooltips: the files Your Dolphin's came from, "codes.rc24.xyz". |
| 10 | Paste (+) | "New code": Name, the lines, Gecko / Action Replay (an encrypted `XXXX-XXXX-XXXXX` line makes it AR; hex pairs are Gecko unless switched). A new code is on at once. |
| 11 | Rooms show them | The public list and the room panel: a purple chip `<> 5` beside Disc / SD card, the names on hover (8, then "+N more"). The host's chip in the room panel opens the locked dialog. |
| 12 | On screen | Dolphin's own "N cheat(s) enabled" at boot. Nothing new. |

## 3. Data

`ModBuild.codes?: { on: ModOnCode[]; yours: ModCode[] }`: `ModCode = { name, kind: "gecko" | "ar", lines, author?, notes? }`, `ModOnCode = ModCode & { source: "yours" | "own" | "dolphin" | "database" }`. An on code keeps its own copy of the lines: a Dolphin update, an edited ini or a changed database entry never changes a room's codes. `setCodes` keeps them checked (`cleanCodes`): runnable lines only, in order, as many as fit (below).

**Dolphin's list** (`launcher/src/dolphin/mods/dolphin_codes.json`): the `[Gecko]`, `[Gecko_Enabled]`, `[ActionReplay]`, `[ActionReplay_Enabled]` sections of upstream Dolphin's `Data/Sys/GameSettings` (as of 2026-09, from `refs/orca-netplay`), by file name (`GP6E01`, `GALE01r2`, `GAL`): 270 games, 630 KB. Made by `dolphin_codes_cli.ts`; run it again after a Dolphin merge. Dolphin's game settings themselves are not shipped: rooms pin their own (`MakeNetSettings`), and our Dolphin's `Sys/GameSettings` stays as it is.

**Limits** (`code_rules.ts`): at most 407 Gecko lines on (Dolphin's code list goes between `Sys/codehandler.bin`, 2880 bytes from 0x80001800, and 0x80003000; past that Dolphin skips a code), 256 codes and 2048 lines (Dolphin's `ParseCodes`), and 20,000 characters of JSON (the codes go on Dolphin's command line).

## 4. Flow

1. **Host.** `ModsManager.host` puts the game's on codes in `--mods-room`'s `codes` (`[{name, kind, lines}]`). `ModsRoom::FromEnvironment` parses them (`Machine::ParseCodes`: Gecko lines by `Gecko::DeserializeLine`, AR lines decrypted as Dolphin does), and `Arm` makes them the synced codes (`Gecko::UpdateSyncedCodes`, `ActionReplay::UpdateSyncedCodes`). `MakeNetSettings` turns cheats on with `sync_codes` as a netplay guest, so `SESSION_CODE_SYNC_OVERRIDE` makes `PatchEngine::LoadPatches` take the synced codes over every ini. Without codes nothing runs: the Gecko handler is installed only with a code to run (`RunCodeHandler`), so the machine is the same as with cheats off.
2. **mm.** `mods-create`'s `build.codes`: the names only (at most 64 of 48 characters, `MAX_CODES`, `MAX_CODE_NAME`); every `mods-state` and `/v1/rooms`' `modRooms` give them back with the build. No code lines go through mm. Dolphin writes them into `mods-room.json` (`room.build.codes`) for the launcher.
3. **Join.** The codes travel in the machine image (version 5: after the draw state, `PackCodes`), and every player's digest covers them, so a joiner runs only codes the players vouch for. The joiner makes them its synced and active codes before the state loads (`UseCodes`): the Gecko handler and its list are in the image's RAM, and AR codes run in Dolphin at each VI (`ActionReplay::RunAllActive`), so the joiner's AR list must be the host's. An image without codes stays version 4 with the same digest, so a joiner of an older build still joins a room without codes; it cannot join one with codes ("not a machine image of this version").
4. **Mid-game edits.** In a session the Cheats window cannot change this machine's codes (`Gecko::SetActiveCodes` and `ActionReplay::ApplyCodes` keep the synced ones while `SESSION_CODE_SYNC_OVERRIDE` is set; Dolphin's own netplay guests get the same).

**Determinism.** Gecko and AR codes run from `ApplyFramePatches`, a VI event: emulated time, the same on every machine and in every re-run. An AR code that fails is dropped from Dolphin's list (`RunAllActive`) and stays dropped through a rollback: the same on every machine (it fails on all), so it does not desync.

**Security.** Nothing new: a joiner already loads the host's whole RAM, so a code can do nothing an image could not; the codes are part of what every player vouches for.

## 5. Checked

- `harness/tests/test_mods_room.py::test_host_codes_run_on_every_machine`: Mario Party 6 (`PPHARNESS_GC_DISC`) against the local accounts + mm, a host with two Gecko codes (a marker and a counter that goes up every frame) and an AR marker; the joiner's `mods-room.json` lists the names, both machines have the markers and counters, 0 desyncs.
- `harness/tools/machine_join.py --codes`: Mario Party 6 with four of Dolphin's codes (one a `C2` ASM code) plus counters, at 120 ms ± 8 ms, two joins: 0 desyncs, the counters equal on both; without codes the Gecko list is not in RAM and the join is as before.
- `MachineCodesTest` (Dolphin unit tests), `codes.test.ts`, `mods_manager.test.ts`, `mods_view.test.tsx`, mm's `mods` tests.
