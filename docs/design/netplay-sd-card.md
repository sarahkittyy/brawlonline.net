# The SD card in netplay

Status: built (2026-10-14). Dolphin side: `Core/NetPlaySDCard.{h,cpp}`, `Core/IOS/SDIO/SDCardStorage.{h,cpp}`; tests: `UnitTests/Core/NetPlaySDCardTest.cpp`, `harness/tests/test_netplay_sd.py`.

## 0. What the user asked for (2026-10-14)

The question was how safe Dolphin netplay ("any Wii game", the NetPlay window's Rollback mode) is with strangers in public lobbies. Rollback itself sends only inputs and a 4-byte checksum, and every rollback state stays on the player's own machine. The risk was what the host sends at the start: its save data, its cheat codes, and its settings, including "allow SD writes". A host's code (or a save that exploits the game) runs inside the guest's emulated Wii and could rewrite the guest's own `sd.raw`, which is the P+ card that Brawl Online boots, ranked included.

Decided:
- **The host must never be able to write to a guest's SD card.** Every write during a netplay session goes to a throwaway layer.
- **The SD card follows the same rules as the NAND saves** (the netplay window's Data menu):

  | Host's Data setting | Card everyone plays on | After the session |
  |---|---|---|
  | No Save Data | each player's own card | everyone throws the writes away |
  | Load Host's Save Data Only (default) | the host's card | everyone throws the writes away |
  | Load and Write Host's Save Data | the host's card | the **host** keeps the writes; guests throw them away |

  So 4 friends can play NSMB (a NAND save) or anything that saves to SD, quit, and continue the run with the same host another day, while each guest's own saves stay untouched.
- **Host's card, fetched on demand.** A guest does not download the host's whole card. It compares the host's manifest with its own card and fetches only the chunks that differ, when the game first reads them. A few come before the boot, and the rest stream in the background at a gentle rate. A giant card ("a billion Brawl replays") costs nothing until the game opens those files.
- **Codes stay allowed** (an unlocked roster, Mario Kart cheats). The guest is not asked for permission: nobody can judge a code from its lines, and with every write thrown away a code can only change that session's game. The guest is shown the host's names for its codes instead ("Host's codes: ..."). A name is the host's label and is never checked.

## 1. How it works

**The session card** (`SessionCard`, an `SDCardStorage`). At boot the netplay client gives the game an SD card of its own (`BootSessionData::SetSDCardSession`; Core's emulation thread installs it before the hardware starts). The emulated card (`SDIOSlot0`) reads and writes it instead of the image file:
- Writes go to an in-memory layer of 64 KiB chunks. A write to part of a chunk copies the rest of the chunk first.
- The layer rolls back with the machine. Every write increments a counter, which `SDIOSlot0::DoState` keeps in the savestate, and loading a state undoes the writes made after it. Rollback's in-memory states use `DoState`. Before this change, a mispredicted frame that wrote to the card left the write in the image. History older than 10 s of emulated time is dropped; rollback never goes back that far. The counter is serialized only while a session card is installed, so states made outside netplay keep their format (`STATE_VERSION` is unchanged and the QA fixtures in `run/qa` still load).
- At the end (`SessionCard::Finish`, after the hardware shut down and before Dolphin's SD folder sync) the layer is written into the image only when it is the host's and the host chose "Load and Write". The SD folder (folder sync) is unpacked again only then.

**The host's card.** With "Load Host's Save Data" on a Wii game, the SD card is one more item of the save sync (`SyncSaveDataID::SDCardManifest`):
1. The server builds a **manifest** of its image on a thread of its own: one XXH3-128 hash per 64 KiB chunk, and which chunks are **zero**. A zero chunk is either all zero bytes or one no FAT cluster in use touches (`FindFreeChunks` reads FAT16/FAT32, with or without an MBR). Free space reads as zeros on every machine, the host's included, so deleted files are never sent. Hashes are cached per image in `User/Cache/NetPlaySD/` (keyed by path, valid while size and modification time match). With folder sync, the folder is packed first.
2. A guest hashes its own card (cached the same way) and plays on: zeros for zero chunks, its own chunk where the hashes match (same length too), else the host's chunk, fetched (`SDCardChunkRequest` → `SDCardChunkData`).
3. Before reporting the sync done, the guest fetches up to 32 MiB of the differing chunks, lowest first (the FAT and directories are near the start). Once the game runs, the rest streams at 256 KiB/s, up to 2 GiB. A read that needs a chunk not yet here waits for it. Emulated time does not move meanwhile, so every machine sees the same bytes at the same emulated moment, and GekkoNet is polled so the peer does not take the wait for a disconnect. The OSD says "Loading the host's SD card...". A read waits at most 60 s, then fails.
4. The host plays on the same card its guests see: its own image, with the manifest's zero chunks reading as zero.

The "SD card inserted" setting is now the host's too (`NetSettings::sd_card_inserted`), so a guest with its card out no longer differs from the host.

**What is checked** (the other side is a stranger):
- Guest side: the manifest (size at most 64 GiB, exactly one entry per chunk, known entry kinds, compressed sizes capped), every chunk (index, length, and its hash against the manifest; a wrong chunk is asked for again, and after 3 wrong copies it counts as unavailable), and the fetched chunks go to a temporary file in `User/Cache/NetPlaySD/` that is deleted with the session. Nothing from the host reaches the guest's own files.
- Host side: requests are answered on a thread of their own, so disk reads never hold up the netplay thread (at most 64 chunks per request, at most 256 queued per guest). Only chunks the manifest says hold data are sent, and only while they still match their hash. A bad request is ignored, not kicked.

**Codes.** `SyncCodeID::GeckoData`/`ARData` carry the code names after the lines (`WriteCodeNames`): at most 255 names of at most 64 bytes, control characters removed, never cut inside a UTF-8 character. The guest shows them in the chat and on screen at boot. An older host sends none.

## 2. What it costs

- First game with a card: hashing it (about 1-2 s per GB from an SSD; cached after that).
- Two players on the same P+ build: the manifest (about 0.6 MB for a 2 GB card, compressed) and next to nothing to fetch.
- A guest whose card differs a lot: 32 MiB before the boot, then short stalls when the game first reads something different (one round trip plus the chunk), and a background stream of 256 KiB/s.

## 3. Not done

- The launcher does not check the whole `sd.raw` against its manifest at start (only the plugin). With every netplay write thrown away this hole no longer needs it; the planned build-hash check (`backend-design.md` §6) would cover a card changed some other way.
- Riivolution redirected saves and GameCube memory cards were already synced as temporary copies and are unchanged.
