# Room codes: the host's Gecko / Action Replay codes in Other Games rooms (design)

_2026-10-30. Proposal, nothing built. Sarah's board ("AR/gecko codes ... hosting a game runs the game without codes", a player whose communities play netplay with codes; Sarah: "let the host set codes for the room. they should stay persistently ... some games have a list of gecko codes already from dolphin that we should expose ... minimal text in the uis"). Mockups (local, not in git): `run/codesdesign/*.png`, made from the launcher's own Other Games view (`run/codesdesign/mods_view_codes.tsx`, `codes.tsx`)._

## 1. Today

A room session turns cheats off for everyone: `MakeNetSettings` sets `ns.enable_cheats = false` (`MachineSession.cpp`), and the netplay config layer forces Dolphin's Enable Cheats off over every ini, `Sys/GameSettings` included. Offline the player's codes run, so they look lost online.

The player's own `[OnFrame]` patches (`User/GameSettings`) are **not** pinned: `PatchEngine::LoadPatches` reads them on every machine. Two players with different patches for a game desync. Fixed with this work either way (§4.4).

## 2. Decisions proposed

| # | Topic | Proposal |
|---|---|---|
| 1 | Where | **Other Games rooms only.** P+ (Play, Ranked, Unranked, Direct, P+ rooms) keeps its own codeset; the built-in P+ row has no Codes button. |
| 2 | Who | **The host's codes run on every machine.** A joiner's own codes are never used (as Dolphin netplay's Sync codes). Joining needs nothing: no download, no question. |
| 3 | Where they're set | A **Codes** button on each game row in Your games (`<> 6`: the count on; `<> CODES` greyed with none). One dialog per game. |
| 4 | Remembered | **Per game row**, in the launcher's mods settings, until changed. Host the same game again: same codes. |
| 5 | Lists in the dialog | Sections, top to bottom: **Yours** (pasted with +), **Your Dolphin** (read on every open from Brawl Online's Dolphin and the player's own Dolphin(s), the folders of 064d171f's save import; the first time, the ones on there are turned on here), **Dolphin** (Dolphin's own list for the game, shipped with the launcher), **Database** (codes.rc24.xyz, the geckocodes.org mirror Dolphin's Download Codes uses; folded, fetched when opened, cached). A code in two lists shows once, in the higher one. |
| 6 | Row | Switch, name, author (from Dolphin's `[author]`), `AR` tag for Action Replay (Gecko has none), chevron: the notes and the code lines. Edit / delete only on Yours. |
| 7 | Text | Title "Codes", the game and its ID, "N on". Search, All / On, +. Section names. Tooltips: the section's source (a path, "codes.rc24.xyz"). Nothing else. |
| 8 | Paste (+) | "New code": Name, the lines, Gecko / Action Replay picked from what's pasted (encrypted AR `XXXX-XXXX-XXXXX` → AR; hex pairs → Gecko, switchable). Cancel / Add. |
| 9 | Rooms show them | Public list and the room panel: a purple part chip `<> 6` beside Disc / SD card; hover lists the names (8, then "+N more"). |
| 10 | While hosting | **Locked** (the dialog opens read-only with a lock): the session runs from creation and never restarts. Changing codes = leave and host again. (Open question 1.) |
| 11 | On screen | Dolphin's own "6 cheat(s) enabled" at boot, on every machine. Nothing new. |

## 3. Data

`ModBuild.codes?: { on: CodeRef[]; yours: Code[] }`, where `Code = { name, kind: "gecko" | "ar", lines: string[], notes?, author? }` and `CodeRef = { source: "yours" | "own" | "dolphin" | "database", name, lines }`. An on code keeps its own copy of the lines: a Dolphin update, an edited ini or a changed database entry never changes a room's codes silently. If the source's lines differ later, the row shows the source's on hover and a click takes them (**default**).

**Dolphin's list.** Our fork ships one `Sys/GameSettings` file. Upstream Dolphin's has `[Gecko]` codes for 161 games (Mario Party 4-7 and 8, Melee's netplay set, Mario Kart Wii, ...) and `[ActionReplay]` for 851: 4,178 codes, 635 KB of ini text. A build step turns them into one JSON (`launcher/src/dolphin/mods/codes/dolphin.json`, by game ID and revision: `GALE01r2`), refreshed when we merge Dolphin.

## 4. Flow

1. **Host.** The launcher puts the row's on codes into the room request (`codes: [{name, kind, lines}]`) beside the build. Dolphin arms the session with them: `ns.enable_cheats = true`, `SESSION_CODE_SYNC_OVERRIDE`, `Gecko::UpdateSyncedCodes` / `ActionReplay::UpdateSyncedCodes`, so `LoadPatches` takes these and ignores the ini (the path Dolphin's netplay already has).
2. **mm.** `mods-create` gets `codes: [name]` (names only, up to 32, for the list); `mods-state` and `/v1/rooms`' `modRooms` give them back. No code lines go through mm. An older launcher ignores them.
3. **Join.** The codes travel with the machine image, P2P (header beside `sd_files_wire`). The joiner sets them as its synced codes, cheats on, before it loads the image. The Gecko handler and code list are in guest RAM already (`Gecko::DoState` keeps only "installed"); AR codes run in Dolphin (`ActionReplay::RunAllActive` at each VI), so the joiner's AR list must equal the host's, which this gives.
4. **Patches.** A session reads `[OnFrame]` from `Sys/GameSettings` only, never the player's own (§1).

**Determinism.** Gecko and AR run from `ApplyFramePatches`, a VI event: emulated time, the same on every machine and in every re-simulated pass. What still needs a test: a harness join (`machine_join.py`) on Mario Party 6 with a Gecko C2 (ASM) code, an AR code and a code that writes into code (`04` on `.text`), host and joiner, 0 desyncs, rollbacks forced with netsim.

**Limits.** Dolphin's Gecko code list has a fixed size in RAM (the installer says "Too many GeckoCodes! Ran out of storage space in Game RAM"); the dialog greys out the switches once the on codes would not fit, with the size on hover. A room carries at most 256 codes / 64 KB (**default**).

**Security.** Nothing new: a joiner already loads the host's whole RAM, so a code can do nothing an image could not (`brawl-mods-security.md`: "image mode: no codes" becomes "the host's codes, which write guest memory only").

## 5. Open questions

1. **Changing codes in a running room**: locked (§2.10, simplest), or the host changes them and every machine applies the new list at an agreed frame (like an epoch; Dolphin can swap codes live, the handler reinstalls itself)?
2. **The database** (codes.rc24.xyz, a third-party site; Dolphin itself uses it): include, or only Dolphin's list, the player's Dolphin and paste?
3. **Joiners**: is the chip enough, or should the join show the codes once (as the SD card question does)?
