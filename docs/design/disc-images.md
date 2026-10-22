# Disc images: any format, and Brawl Rev 1 with Rev 2 in Other Games rooms (design)

Status: implemented on 2026-10-21. The user's request (2026-10-21): "i'd love support for rvz, wbfs. i've seen some people have more luck with the dolphin conversion than nkit in prod rn. and i'd love it if people running alt brawl mods could play on different brawl disks. like some guy with rex and rev1 could play with some guy with rex and rev2/wbfs/somethin else".

## 1. The problem

The launcher checked a Brawl disc by the MD5 of the whole file: only a full, uncompressed redump ISO passed. RVZ and WBFS were turned away by their extension (a rule from the upstream Slippi launcher, `3940f2ea`, 2024), and so was every image converted back to an ISO by Dolphin or NKit from a scrubbed source, though the game reads the same data from all of them. Other Games rooms named a disc by the SHA-1 of its contents, so an ISO and a WBFS of one disc, or Rev 1 and Rev 2, could not share a room.

## 2. A disc's game data

What the game reads is the disc's game partition. A Wii partition is hashed in a tree: each block's H0-H2 hashes, the H3 table over them, and the H3 table's hash in the partition's TMD. Once every block the game uses checks out against that tree, the game reads exactly what the TMD says, whatever file holds the disc.

`DolphinTool gamedata -i <image> [-p]` (`dolphin/Source/Core/DolphinTool/GameDataCommand.cpp`) runs Dolphin's `VolumeVerifier` over the image (any format Dolphin reads) and prints JSON: the game ID, revision, the game partition's offset, the TMD's SHA-1, whether the H3 table matches the TMD, and how many used and unused blocks fail their hashes (`VolumeVerifier::GetBlockErrors`, added for this). **`game_data`** is the SHA-1 of `"game-data-v1\n"`, the game ID, the partition's offset (8 bytes, big endian) and the TMD, and is null unless the H3 table checks out and no used block fails. The offset is part of it because it sets the emulated loading times: Dolphin's own verifier warns that a moved partition cannot play NetPlay with a good dump. The junk filling, the update partition and the image format are not part of it. A GameCube disc has no hashes: its `game_data` is null.

Measured with this PC's discs (2026-10-21):

| Image | `game_data` | Unused blocks failing |
|---|---|---|
| Brawl NTSC-U Rev 1, redump ISO (`D:\Roms\Wii\SSBB_NTSC2.iso`) | `0d79fc54…` | 0 |
| Rev 1, scrubbed RVZ (DolphinTool `--scrub`) | `0d79fc54…` | 8088 |
| Brawl NTSC-U Rev 2, redump ISO | `e6ae2404…` | 0 |
| Rev 2, RVZ (zstd 5, 128 KiB) | `e6ae2404…` | 0 |
| Rev 2, scrubbed ISO | `e6ae2404…` | 8088 |
| Rev 2, WBFS (2 MiB clusters, all-zero clusters left out, as USB loaders' tools write it) | `e6ae2404…` | 8088 |
| that WBFS missing one used cluster | null (64 used blocks fail) | 8088 |
| the scrubbed ISO with one byte of game data changed | null (1 used block fails) | 8088 |
| New Super Mario Bros. Wii (USA, Rev 2) | `87033344…` | 0 |
| Mario Kart Wii (USA) | `479693e6…` | 0 |

Reading a whole Brawl disc took 22 s from a cold file (the MD5 took ~25 s), 2-4 s from the OS cache. `-p` prints `progress <done> <total>` lines on stderr, so the launcher shows a bar for every format.

Not tested: NKit images (no NKit tool here). Whatever they are, the same rule decides: an NKit image whose game partition is where the disc has it and checks out is accepted; one that moved it is not. Some WBFS tools scrub Brawl's Masterpieces partitions entirely; those are not part of `game_data`, so such a WBFS is accepted, and a player who opens Vault > Masterpieces in a room with it would desync.

## 3. P+ (Settings > Game, quick start)

`launcher/src/main/verify_iso.ts`: a disc is accepted when its `game_data` is NTSC-U Brawl Rev 1's or Rev 2's (`BRAWL_NTSC_U_GAME_DATA`, `launcher/src/dolphin/game_data.ts`). A plain image whose header is another game is still refused at once, without reading 8 GB. If DolphinTool cannot run (missing, blocked by an antivirus), a plain image falls back to the old MD5 check; any other format is then refused ("DolphinTool is needed to read it"). The check runs with the bundled Dolphin's DolphinTool (`check_iso.ts` `isoGameDataReader`, after `bundledDolphinReady`). The verification cache has a version now (`v: 2`): a result from before (an RVZ refused by its name, a scrubbed ISO by its MD5) is checked again.

The quick start's ISO step and Settings > Game take `.iso .rvz .wbfs .wia .gcz .ciso` (`DISC_IMAGE_EXTENSIONS`); 7z archives still get "7z files must be extracted to be used in Dolphin.".

P+ online play already ran Rev 1 against Rev 2 (its rollback covers the game's memory, not the disc ID). A WBFS or an RVZ gives the game the same bytes at the same offsets as the ISO, so nothing else changes for P+.

## 4. Other Games rooms

A build's disc part (`launcher/src/dolphin/mods/disc_info.ts` `discIdentity`) is the disc's `game_data`, and NTSC-U Brawl Rev 1 and Rev 2 are one disc, `BRAWL_NTSC_U_DISC` (the SHA-1 of `"disc: NTSC-U Brawl, Rev 1 or Rev 2"`), labelled `RSBE01` in rooms. A GameCube disc, or a Wii disc whose game data does not check out, is still its contents' SHA-1. So a REX player on a Rev 1 ISO joins a REX room hosted on a Rev 2 WBFS, if the launcher (`.elf`/`.dol`) is the same and both or neither have an SD card (the card is synced as before). mm compares the disc part as before (`common::mods::join_mismatch`); only its message changed ("Make sure you both have the same version of the game.").

The build hash is v4 (`brawl-mods-build-v4`; `buildHashKey` `v4|`), so every game is hashed again once after the update: at the launcher's start (`ModsManager.rehashChanged`, one game at a time in the background, every game whose `hashKey` is not today's; a join by code waits for it). Until 2026-10-22 that happened only when the game was hosted or joined, and the room list matched rooms by the stored v3 parts, so a player's REX showed every REX room as another disc (and the plain Brawl disc beside it as the one to fix, "add an SD card") until they removed the game and added it again. Two games that are now one disc (Brawl Rev 1 and Rev 2 of one mod) both stay; a new or edited game is still refused as a duplicate. A game's save store is named by its build hash (`<User>/Saves/<hash16>`, `Core/WiiRoot.cpp`): when a game's hash changes, its store moves to the new hash (`save_import.ts` `moveSaveStore`), unless the new hash has one already. Known disc names (`known_builds.ts`) are keyed by the new parts.

Players on a launcher from before this cannot share a room with players on this one: their disc parts differ (content SHA-1 against game data), and Dolphin's machine-session protocol is 3 now (below), so the session refuses the join too.

## 5. Rev 1 and Rev 2 in one machine session

The two discs have the same game files; `main.dol` differs in one word, the revision the game checks at boot. Booted in Dolphin (single core, the same fixed RTC, no input) to frame 1500 and diffed: two Rev 1 boots are byte for byte the same, and Rev 1 against Rev 2 differ in exactly 4 bytes of MEM1 and none of MEM2, every one the revision (1 or 2):

| Address | What |
|---|---|
| `0x80000007` | the disc ID's revision in low memory |
| `0x8001BC9F` | the operand of `main.dol`'s revision check |
| `0x8049493F` | the DVD library's copy of the disc ID |
| `0x804E4585` | the game's copy of the disc ID |

A whole-machine session checksums all of RAM, so those bytes made every check fail: 340 desyncs in ~9000 frames with the build before this. `RollbackManager::SetChecksumMasks` takes byte ranges the checksums read as zero (`ForChecksum`, in the change, slice, full and digest checksums); they still roll back, unlike excluded regions. `MachineSession` sets `BRAWL_DISC_REVISION` (the four bytes) for a Brawl session (`Boundary::BrawlLoop`) at its first boundary and clears it at `Stop`. The machine-session `PROTOCOL_VERSION` is 3, since the checksums changed. A late joiner loads the room's machine image, revision bytes included; its own disc only differs in the revision byte of its header and that word of `main.dol`, which the game does not read again.

Tested (2026-10-21, `harness/tools`):

| Run | Discs | Result |
|---|---|---|
| `machine_pace.py --mode pair --iso2`, random input, 90 s after a 60 s warm-up, build before this | Rev 1 ISO, Rev 2 ISO | 340 desyncs in 8981 frames |
| the same, this build | Rev 1 ISO, Rev 2 ISO | 0 desyncs in 8983 frames |
| `machine_desync.py --match --joiner-iso`, 60 ms RTT: a late join at the character select, then a match to its end | host Rev 2 WBFS, joiner Rev 1 scrubbed RVZ | 0 desyncs in 14546 frames (match 6116-13722), ~450 rollbacks each |
| `machine_desync.py --together --match`: both boot together, then a match to its end | Rev 1 ISO, Rev 2 WBFS | 0 desyncs in 11827 frames (match 3409-11085), 412 and 539 rollbacks |
| `machine_desync.py --together`, REX 0½ Hotfix 1's netplay launcher and SD card, random input | Rev 1 ISO, Rev 2 WBFS | 0 desyncs in 10455 frames |

Not run: macOS and Linux (`dolphin-tool gamedata` builds there with the rest of DolphinTool), the packaged launcher's quick start with an RVZ or a WBFS, a room through mm with real clients.
