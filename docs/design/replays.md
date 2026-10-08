# Replays: recording, file format, playback and launcher integration (design)

Status: design draft, 2026-10-08. Nothing here is implemented. The UI is designed separately in `docs/design/launcher-profile-ux.md`; this document is the system behind it.

User requirement: "all matches should be saved somewhere local and play-backable, with a ui, probably integrated with the rank / profile display."

Path prefixes used below:
- `GS` = `dolphin/Source/Core/Core/Rollback/GameplaySession.cpp`
- `GSH` = `dolphin/Source/Core/Core/Rollback/GameplaySession.h`
- `GR` = `dolphin/Source/Core/Core/Rollback/GameplayRollback.{h,cpp}`
- `PD` = `dolphin/Source/Core/Core/Rollback/PeerData.{h,cpp}`
- `GB` = `dolphin/Source/Core/Core/Online/GameBridge.cpp`
- `STATUS` = `docs/gameplay-rollback-status.md`
- `L` = `launcher/`
- `R04` = an earlier survey of Slippi's repositories and formats (not in this repository)
- `ISH` = `../refs/slippi-Ishiiruka/Source/Core`

## 1. Summary

| Question | Recommendation |
|---|---|
| What is recorded | Per game: the match setup and the host's sync values (everything the joiner takes from the host today), the final inputs of every frame from the first frame under rollback, a checksum per frame, compact per-player state per frame for stats, and metadata. Online modes first (Direct, Unranked, Ranked, later Teams). Offline Versus later (Phase R4). |
| Who records | **Both peers**, each into its own local folder. The game part of the two files is identical, so the files can be cross-checked. |
| File format | A **chunked binary container** (`.rep`, PNG/RIFF style): a small header, then self-delimiting chunks with a CRC each. Metadata (JSON) is the first chunk, the result is the last chunk plus a fixed trailer. Frame data is deflate-compressed in 1-second chunks. Never a savestate. |
| Recording path | Inside `Gprb::Session`: setup at the barrier, inputs and checksums from the final run of each frame, emitted once no rollback can reach the frame again (8 frames later). A writer thread does all file I/O. A crash loses at most the last second. |
| Playback | A new `Gprb::Session` mode. A fresh boot reaches the match through the plugin's normal online path (SESSION → `setupMatch`), and Dolphin then applies the recorded host values exactly as a joiner applies the host's: seeds, init-block variant, sync block, task order. The setup key is compared as at a live barrier. Then the recorded inputs are played without GekkoNet and without rollback. |
| Start contract | Slippi's `-i comm.json` (the launcher already writes it). |
| Seeking | In-memory keyframes taken during playback (our own buffers, never from a file), fast-forward through the existing "resimulating" mode (unthrottled, no audio, no presentation), and an 8-slot snapshot ring for instant frame-by-frame rewind. |
| Desync in playback | The recorded checksum of every frame is compared; the first mismatch is shown on screen and reported to the launcher. Playback continues. |
| Old builds | Refuse with a message that names what differs, with a "Play anyway" option. No archive of old builds for now. |
| Stats | Record per-frame player state (Slippi's post-frame idea, Brawl fields we have verified). Inputs and checksums alone give only duration, result and input rates. |
| First milestone (R1) | Record a Direct game on both peers and play it back from a fresh boot, no seeking. Pass criterion: playback's per-frame trace equals both live peers' traces through game set. |

## 2. What exists today

### 2.1 Pass logs (Dolphin commit `980673530a`)

`980673530a` ("Gameplay rollback: session replays, divergence diagnostics, sound bookkeeping", in `D:\code\pm_rollback\dolphin`) added pass logs, which are a debugging tool, not user replays.

| Property | Pass log | Consequence for user replays |
|---|---|---|
| Trigger | Environment variable `PPR_GPRB_PASS_LOG=path` (GS:2137-2147) | Needs a setting and a folder instead |
| File name | `<path>.<host\|join\|synctest>.m<match>` (GS:2146-2147) | Needs Slippi-style names and folders |
| Header | `'GPRH'`, `g_GameFrame` (0x18 bytes), the three RNG words, the serial counter (GS:2129-2133, 2150-2154). No app counter: a replay takes the replaying machine's own (GS:2204-2207) | Not enough to rebuild a match: no setup, no init block, no task order, no start points |
| Body | Every GekkoNet update: load depth, initial save, and every pass with its frame, save flag and all four pad slots, 0x100 bytes (GS:2157-2176). Mispredicted passes are included | User replays need only the final input per frame; `gprb_passlog.py flat` already derives it (`harness/tools/gprb_passlog.py:59-64, 93-96`) |
| Writes | `fwrite` on the CPU thread (GS:2164-2175) | Must move off the CPU thread: synchronous log writes once stalled saves for 5-17 s on a saturated disk (STATUS:212) |
| Replay | A sync test with `replay_path` runs the passes again (GSH:62-64, GS:2190-2254), starting from the peer's **countdown savestate** (STATUS:398-399) | A savestate is tied to the Dolphin build and must never be loaded from someone else. User replays must start from a fresh boot |
| Ground truth | `no_rollback`: every frame once, no loads (GSH:65-67, GS:1979-1988) | This is exactly what user playback is: confirmed inputs, each frame once |

The pass log proves the key property: replaying a peer's inputs reproduces that peer's game exactly, also in dual core (STATUS:399), and the flattened log reproduces the no-rollback ground truth, which every rolled-back run must equal (STATUS:448-456, 663-676).

### 2.2 Brawl's own replays (`06cab98c93`)

`06cab98c93` only moves P+ Dolphin's backup of Brawl's in-game replay save (`collect.vff`, the Vault) to `<User>/ReplayData` on Windows (`dolphin/Source/Core/Core/WiiRoot.cpp:235-273`, gated by `Config::SYSCONF_SAVE_REPLAYS`). That is the game's own replay feature: saved by the game from its results screen, and our online flow skips the results screen (`docs/game-code.md:402`). It is unrelated to this design. Keep it, and keep the names apart (`ReplayData` folder, `SYSCONF_SAVE_REPLAYS`) from ours (`[Online] ReplayDir`, `SaveReplays`).

### 2.3 Other existing pieces this design reuses

| Piece | Where | Use here |
|---|---|---|
| Match start: seeds from `MatchSeeds(session_seed, match_index)`, the host's init block for the joiner, serial counter | GS:672-680, GS:2816-2870, GS:3213-3223 | Playback applies the recorded values the same way |
| Barrier: setup key and hash, the host's sync block, the host's task order | GS:412-471, GS:3088-3145 | Playback compares the recorded setup key and applies the recorded sync block and task order |
| Sync block (frame counters, RNGs, serial, start points) | GS:120-131, GS:591-669 | Recorded in full |
| Countdown with neutral input, then GekkoNet from `start_frame` 240 | GS:2117-2127, GS:2918-3035; `start_frame` default GSH:86 | Playback runs the same countdown |
| Frame-anchored pads | GR.h:166-207 | Playback feeds file inputs through `PadsSetCurrent` like the session (GS:2433-2434) |
| Pacing: one logic step per pass while the session drives the loop | GS:3726-3730, `HLE/HLE_Misc.cpp:647-690`, STATUS:151 | Playback must report `DrivesLoop()` in the same phases |
| File IO wait at the loop top | GS:3384 ff., STATUS:624 | Runs unchanged in playback (same code path) |
| Per-frame checksum: frame counter, three RNGs, per-port fighter fields | GS:682-695, GR.cpp:178-218 | Recorded every frame, compared in playback |
| Checksum history ring (32,768 frames) | GS:64, GS:289, GS:1594-1600 | Source of the recorded checksums |
| GekkoNet saves every frame (`limited_saving = false`), prediction window = `MAX_ROLLBACK_FRAMES` (7) | GS:1398-1401 | Every frame has a checksum; rollbacks never reach further back than 7 frames |
| Load depth bound | GS:1995-2011 (a load deeper than `MAX_ROLLBACK_FRAMES` is refused) | Defines when a frame is final (section 5.2) |
| Winner rule: more stocks, then less damage | GS:1564-1576 | Same rule in the result chunk |
| Peer-data validation: characters, costumes, stages, init block merge, start-point permutation, pad sanitising, port values | PD.h:24-108, PD.cpp:153-229 | Every replay field goes through the same checks |
| Resimulating mode: unthrottled, audio dropped, presentation dropped | `Core/CoreTiming.cpp:399-414`, `AudioCommon/AudioCommon.cpp:208-211`, `HW/VideoInterface.cpp:877` | Fast-forward for seeking |
| Whole-machine snapshot code (device state via `State::SaveToBuffer` with RAM skipped, RAM by dirty granules) | `Rollback/DeltaSaveSlot.cpp:280-310`, `RollbackManager.h:109-121, 192-199` | Keyframes and the rewind ring |
| GekkoNet's own replay API (`gekko_start_recording`, `GekkoReplaySession`) | `Brawlback/include/gekkonet/GekkoLib/include/gekkonet.h:58-63, 235-248` | **Not used**: it keeps the whole recording in memory and returns it only at stop (`gekkonet.h:241-244`), has no metadata or checksums, and its `save_initial_state` puts a savestate in the replay |

## 3. What to record

### 3.1 Which games

| Mode | Recorded | Phase | Notes |
|---|---|---|---|
| Direct | yes | R1 | The tested path (STATUS:509-520) |
| Unranked | yes | R1 | Same session path, `sqNetAnyOkiraku` with mode Unranked (`docs/game-code.md:152`) |
| Ranked | yes | R1 for recording, R4 for upload | Same path once Ranked exists (`docs/game-code.md:482`) |
| Teams (4 players) | yes | after Teams exists | The format holds 4 ports from day one. The session runs 2 players today (`GameplayOnlineBackend.cpp:37-39`) |
| Offline Versus | optional | R4 | Not driven by `Gprb::Session` today, so it has neither anchored input nor sync values (2.3). Needs a local session mode and a playback path for arbitrary rules, items, CPUs and up to 4 players (section 10, open question 2) |

A recording starts at the barrier and covers one game. Games of a set are separate files linked by match id and game number.

### 3.2 Both peers record

- Each player keeps their own library; nothing travels over the network.
- The game part of the two files (setup, sync values, inputs, checksums) is identical by construction: both peers' final inputs are the confirmed inputs, and both computed the same checksums (STATUS:249-256, 528-539).
- Each file also carries what only its recorder knows: local port, role, local settings.
- For Ranked, the server can compare the two uploads (backend-design.md:321).

### 3.3 Contents per game

| Group | Fields | Source in Dolphin today |
|---|---|---|
| Versions | Dolphin build (scm string), `SIM_VERSION` (new, section 8.4), plugin build id and PPOM version, P+ release id, disc id and revision, region set name | new; region set GSH:79 |
| Match setup | mode, match id, game number, tiebreak index (0), number of players, per port: present, `gmCharacterKind`, costume, team, port values (0x3C), display name, connect code; stage kind, ASL buttons, last winner | `MatchSetup` GS:134-141, `LobbyPlayer` GSH:138-144, SESSION as GameBridge writes it GB:743-772, `Online::Match` (`GameplayOnlineBackend.cpp:27-76`) |
| Match start values | session seed, match index, the three seeds as applied, serial counter start (0x10000), the init block as used (0x20 bytes) | GS:2816-2870, GS:61 |
| Barrier values | setup key bytes and hash, the host's sync block (`g_GameFrame` 0x18, app counter, three RNG words, serial, start points), the host's task order (names) | GS:412-471, GS:120-131, GS:3091-3101 |
| Diagnostics | the host's whole `gmGlobalModeMelee` (0x320 bytes) at the barrier, compare only | `Addr::MODE_MELEE_SIZE` GR.h:51 |
| Timing | `start_frame` (240), number of countdown frames | GSH:86 |
| Inputs | per frame from `start_frame`, per playing port: the 0x40-byte `gfPadStatus` slot the frame ran with | the final pass's `ops.slots[i]` (GS:1950-1970, 2408) |
| Checksums | per frame: the 32-bit frame checksum; every 60 frames also its parts (three RNG words, fighters CRC) | `FrameRecord` GS:168-177, `history` GS:289 |
| Player state (stats) | per frame per port: active instance, ftKind, status kind, motion kind, animation frame, x, y, facing, damage, stocks | verified offsets `docs/brawl-memory-map.md:140-163`; partly read already by `ReadFighterFields` (GR.cpp:178-218) |
| Events | GekkoNet desync reports (frame, local and remote checksum), peer left, rollback summary | GS:1891-1908, GS:2674-2680 |
| Result | end reason (game set, LRAS quit, disconnect, stopped), game-set frame, last frame, winner port, per port stocks and damage at the end | GS:1544-1589, GS:2361-2380 |
| Metadata | start time, the fields of `launcher-profile-ux.md` §7 | section 4.3 |

Not recorded: IP addresses (privacy), the play key, chat (none in matches yet), savestates of any kind.

Countdown inputs (frames 1 to 239) are not recorded: they are the neutral slots of `NeutralSlots` (GS:2117-2127) on every machine.

## 4. File format

### 4.1 The options

| | Slippi `.slp` style event stream | Chunked container (recommended) |
|---|---|---|
| Structure | UBJSON object `{raw: [event bytes], metadata: {...}}`; first event is an event-size table, then game start, per-frame pre/post events (`refs/slippi-wiki/SPEC.md:18-41`) | Header, then `{type, flags, length, raw length, CRC}` chunks |
| Metadata | At the **end** of the file; `raw`'s length is 0 while the game is written (SPEC.md:29) | First chunk; the result is the last chunk plus a fixed trailer |
| Partial file | Parser must scan the event stream; no metadata | Every complete chunk is valid; metadata is always there |
| Corruption | Undetected | CRC per chunk; a torn last chunk is dropped |
| Compression | None (2-5 MB per game in Melee, backend-design.md:261) | Per chunk (deflate) |
| Extensibility | Unknown event ids skipped by the size table | Unknown chunk types skipped by length; a "critical" bit refuses playback instead |
| Live reading (mirror, spectate later) | Natural | Natural with 1-second chunks |
| Tooling | slippi-js, but our events would all be new anyway (R04:284, R04:464) | New small parser in C++ and TypeScript |

Recommendation: the chunked container. Our replay is mostly an input stream, not Melee's rich per-frame event set; what matters most here is crash safety, cheap metadata reads for an indexer that reads thousands of files (`launcher-profile-ux.md:258`), and hostile-input parsing, which a small fixed set of chunk types with hard caps makes simpler.

Rejected as well:
- **GekkoNet's replay blob:** see 2.3.
- **Dolphin's DTM movies** (`-m`, `UICommon/CommandLineParse.cpp:86`): whole-machine input from power-on. It would include every menu step, and it is exposed to the pad-thread race that frame anchoring fixed (STATUS:73-110).
- **Brawl's Vault replays:** see 2.2.

### 4.2 Layout

All container integers are little-endian. Guest data (pad slots, init block, SESSION fields) keep the guest's big-endian bytes.

**File header** (32 bytes):

| Offset | Size | Field |
|---|---|---|
| 0x00 | 8 | magic `89 42 4F 52 0D 0A 1A 0A` (`\x89BOR\r\n\x1A\n`, detects text-mode damage like PNG's) |
| 0x08 | 2 | format major (1). A reader refuses another major |
| 0x0A | 2 | format minor (0). A reader accepts a higher minor and skips what it does not know |
| 0x0C | 4 | header size (32) |
| 0x10 | 16 | `game_uid` (4.4) |

**Chunk header** (20 bytes), followed by `stored_len` bytes:

| Field | Size | Meaning |
|---|---|---|
| type | 4 | FourCC |
| flags | 4 | bit 0 deflate, bit 1 critical, others 0 |
| stored_len | 4 | bytes that follow |
| raw_len | 4 | bytes after decompression (= stored_len without bit 0) |
| crc32 | 4 | CRC-32 of the stored bytes |

**Chunks**, in file order:

| Type | Critical | Compressed | When written | Contents |
|---|---|---|---|---|
| `INFO` | no | no | at the barrier, first chunk | UTF-8 JSON, the metadata (4.3) |
| `SETP` | yes | no | at the barrier | match setup and match-start values (3.3), fixed binary layout with a field count |
| `SYNC` | yes | no | at the barrier | the host's sync block; the task order as `u16 lists`, per list `u16 n`, per task `u8 len` + name bytes |
| `FRMS` | yes | yes | every 60 confirmed frames, and at the end | `u32 first_frame` (game frame), `u16 count`, `u8 ports`, then per playing port `count × 0x40` pad bytes (each XORed with the previous frame's in the chunk), then `count × u32` checksums, then the checksum parts of frames with `frame % 60 == 0` |
| `POST` | no | yes | with each `FRMS` | a column list (`u8 field id`, `u8 type`), then the same frames' per-port rows, column by column |
| `EVNT` | no | yes | batched with `FRMS` | `(u32 frame, u8 type, u16 len, payload)` records |
| `ENDG` | no | no | at the end | UTF-8 JSON, the result |
| trailer | n/a | no | last 16 bytes | magic `BORT`, `u32` offset of `ENDG`, `u32` last frame, `u32` CRC-32 of the first 12 bytes |

Frame numbers are game frames (`g_GameFrame.frameCounter`, GR.h:44). The session's own frame numbers are game frame minus `start_frame` (STATUS:432, STATUS:781).

### 4.3 Metadata (`INFO`, and `ENDG`)

Field names follow `launcher-profile-ux.md:260-271` so the indexer maps them 1:1 to the existing columns (`L/src/database/schema.ts:88-127`).

```json
{
  "v": 1,
  "startTime": "2026-10-08T18:03:12.345Z",
  "utcOffsetMin": -240,
  "mode": "direct",
  "matchId": "mode.direct-2026-10-08T18:02:55.120Z-1a",
  "gameIndex": 2,
  "tiebreakIndex": 0,
  "isTeams": false,
  "stageId": 2,
  "asl": 0,
  "rules": { "stocks": 4, "minutes": 8, "items": false },
  "players": [
    { "port": 1, "uid": "…", "displayName": "Alice", "connectCode": "ALIC#123",
      "characterId": 7, "costume": 0, "teamId": null, "startStocks": 4, "tag": false }
  ],
  "recorder": { "port": 1, "role": "host" },
  "build": { "dolphin": "…", "sim": 1, "plugin": "…", "ppom": 4, "pplus": "…", "disc": "RSBE01 rev 1" },
  "session": { "regionSet": "gp-v19", "delay": 2, "cpu": "dual", "startFrame": 240 }
}
```

`ENDG`: `{ "endMethod": "game" | "lras" | "disconnect" | "stopped", "gameSetFrame", "lastFrame", "durationFrames", "winnerPort", "players": [{ "port", "stocksLeft", "damage", "isWinner" }], "rollbacks", "maxRollback", "desyncs", "inputsSha256" }`.

- `startTime` is UTC; the file name uses local time (4.5).
- The match id format is `mode.<mode>-<RFC 3339 ms>-<hex counter>` (`server/crates/mm/src/engine.rs:691-696`). It contains `:`, so it is never used in a path.
- `inputsSha256` covers the raw `FRMS` pad and checksum bytes; equal on both peers' files of one game.

### 4.4 Identity

- `game_uid` = first 16 bytes of SHA-256 over (match id, game number, session seed, match index, setup key). Both peers know all of these at the barrier, so both files carry the same `game_uid` from the first byte, and an in-progress file can already be linked.
- The launcher's set link stays `(matchId, gameIndex, tiebreakIndex)` (`launcher-profile-ux.md:10, 278-280`); `game_uid` deduplicates two copies of one game (for example both players' files in one shared folder).

### 4.5 Folders and names

Slippi's layout (R04:381; `launcher-profile-ux.md:242-256`):

```
<[Online] ReplayDir>                 default Documents/Brawl Online (L/src/settings/default_settings.ts:38)
  2026-10/                           if [Online] ReplayMonthlyFolders (default on, default_settings.ts:40)
    Game_20261008T140312.rep         local time at the barrier
```

- The launcher already writes `ReplayDir`, `SaveReplays` and `ReplayMonthlyFolders` into `[Online]` (`L/src/dolphin/config/config.ts:40-57`; `OnlineSettings.h:12-14` mentions them), but Dolphin does not define or read them yet. Add them to `Config/OnlineSettings.{h,cpp}`.
- The name comes only from the local clock. On a collision, add `_2`, `_3`.
- The file is created at the barrier under its final name (not `.part`): the user wants partial files to be playable, and the browser can list an in-progress game. `launcher-profile-ux.md:253` says "Dolphin writes the file when a game ends"; with this design the indexer must instead re-read a file whose size or trailer changes (section 9.1).
- Unwritable folder or a full disk: one OSD warning, recording stops for that game, the game is never blocked.

### 4.6 Disk usage (estimates, measure in R1)

| Part | Raw per frame (2 players) | Compressed, estimate | 3-minute game (~10,800 frames) | 8-minute game (28,800) |
|---|---|---|---|---|
| Pads | 128 B | 5-15 B | 50-160 KB | 150-430 KB |
| Checksums | 4 B (+16 B / 60 frames) | ~4 B (random) | ~45 KB | ~120 KB |
| Player state | ~48 B | 10-20 B | 110-220 KB | 290-580 KB |
| **Total** | | | **~0.2-0.45 MB** | **~0.55-1.1 MB** |

For comparison, Slippi's files are 2-5 MB (backend-design.md:261). The server's 10 MB upload cap (backend-design.md:243) holds any of ours.

### 4.7 Compression

Deflate (zlib) per chunk:
- Dolphin has it (`dolphin/Externals/zlib-ng`).
- The launcher runs on Electron 34 (`L/package.json:112`), whose Node has `zlib.inflateRawSync` with a `maxOutputLength` bound, but no zstd. zstd would need a JS decoder dependency for little gain on streams this small.

## 5. Recording

### 5.1 Where in Dolphin

A new `Gprb::Replay::Recorder`, called from `Gprb::Session`. All calls happen on the CPU thread with the session mutex held, and they only copy small records into a queue.

1. **Barrier passed** (GS:3136-3145): the host records its own values (`s.sync`, `s.task_order`, `s.init_block`); the joiner records the host's values it just applied (`s.peer.sync`, `s.peer.task_order`, `s.peer.init_block`). Both record the setup key (GS:3092, GS:412-442), the seeds (GS:2861), the setup (`s.setup`), and the match metadata. `GameplayOnlineBackend::Start` (`GameplayOnlineBackend.cpp:27-76`) must pass the match id, the mode and the players (uid, display name, connect code, in-game port order as in GB:683-690) into `ConnectOptions` (GSH:70-107). The writer creates the file and writes header, `INFO`, `SETP` and `SYNC`, then flushes.
2. **Every pass** (`RunFrame`, GS:2358-2434): store `ops.slots[i]` for frame `ops.adv_frame[i]` in a 16-entry ring indexed by frame; a later pass of the same frame overwrites it, so the ring holds each frame's final run.
3. **Every save** (`PerformQueuedSave`, GS:1764-1858): the checksum is already stored per frame in `s.history` (GS:1858, GS:1594-1600). At the same point, read the player-state fields for that frame into a ring (the same overwrite rule).
4. **Emit:** when the newest frame `N` first runs, frame `N - 8` is final (5.2). Push frame `N - 8`'s input, checksum and player state to the writer queue.
5. **Events:** GekkoNet desync events (GS:1902-1908) and the peer leaving go into `EVNT`.
6. **End** (`EndRunning`, GS:1544-1589): emit every frame up to the last final frame, then `ENDG` (reason, game-set frame, winner from GS:1566-1576), and the trailer; the writer flushes and calls `fsync`.

### 5.2 Only confirmed frames

- A load restores at most `MAX_ROLLBACK_FRAMES` (7) frames back: a deeper one is refused (GS:1995-2011), and GekkoNet's prediction window is 7 (GS:1398). When the newest frame is `N`, the deepest load restores the end of frame `N - 8` and resimulates from `N - 7`. So frame `N - 8` and older never run again, and their stored final run is what this machine played.
- In a session without desync, the final input of every frame is the confirmed input (GekkoNet only stops predicting a frame once it has the real input; its own recorder uses the same rule: `dolphin/Source/Core/Core/Brawlback/include/gekkonet/GekkoLib/src/replay.cpp:79-101` and `GetConfirmedFrame` in `.../GekkoLib/src/game_session.cpp:655-659`).
- The recording is therefore "what this machine finally simulated". In a healthy session both peers' recordings are the same, and the test plan checks it against the flattened pass log (section 11, test 5).
- Optional cross-check: a small accessor `gekko_confirmed_frame()` in the vendored GekkoNet, logged when it lags the window rule. Not required.

### 5.3 End of the recording

| End | Last recorded frame | Notes |
|---|---|---|
| Game set | the session ends `MAX_ROLLBACK_FRAMES + 12` (19) frames after game set (GS:65-68), so frames up to game set + 11 are final | Includes the game-set frame |
| LRAS quit (Direct has pause) | the session ends when the scene leaves `scMelee` (GS:2290-2295) | `endMethod: "lras"` when no game set was seen |
| Peer left or timed out | up to 8 frames before the session ended | `endMethod: "disconnect"` |
| Dolphin crash | the last flushed `FRMS` chunk | No `ENDG`, no trailer: "incomplete" |

### 5.4 Writer thread and crash safety

- **The CPU thread never writes files.** It pushes fixed-size records (about 200 bytes per frame) into a mutex-protected queue and returns.
- A writer thread wakes every 60 frames (or on end), builds `FRMS`/`POST`/`EVNT` chunks, compresses them, appends them, and calls `fflush`.
- If the disk stalls, the queue grows; a whole 8-minute game is under 10 MB raw. Above a 64 MB cap the recording is marked truncated and further frames are dropped; the game is never slowed.
- After a crash, every chunk whose CRC matches is valid. A torn last chunk is ignored. The loss is at most about one second of frames plus the 8 unconfirmed frames.
- Cost on the CPU thread: one 0x80-byte copy, one ring write and a few guest reads for player state per pass (target under 20 µs, measure in R1).

## 6. Playback

### 6.1 How a fresh boot reaches the identical first frame

The live session already makes two machines with different menu histories identical at the first simulation frame: the joiner takes the host's values (STATUS:220-235). **Playback is a third joiner whose host is the file.** Nothing new is written into the game that a live joiner does not write.

1. **Start.** The launcher writes `comm.json` and starts Dolphin with `-e <P+ launcher DOL> -i <comm.json>` and the playback User folder (6.2).
2. **Validate before boot.** Dolphin reads `comm.json` with the safe JSON parser (PD.h:39-50), opens the replay, and validates the whole file (section 8). It checks compatibility (8.4). On any failure: a dialog with the reason, and no boot.
3. **Arm.** `Gprb::Session::ArmPlayback(replay)` sets a new `Mode::Playback` (GS:71-76), phase `Armed`. `GetLobby()` (GS:2577-2599) returns the recorded setup, so GameBridge writes SESSION exactly as it did live (GB:743-772), rebuilt from validated fields, and LOCAL gets a new state `LS_PLAYBACK` (`game-code/PPOnline/include/ppom.h:183`).
4. **Boot fast.** Unthrottled, muted, no presentation until the barrier, with an OSD line "Loading replay…".
5. **The plugin starts the match** from the main menu (PPOM v4, 9.2 below): it opens the recorded mode's online CSS. In playback the CSS locks in for SESSION's game at once and leaves with exit code 1, as it does live once SESSION has the setup (`game-code/PPOnline/source/online_match.cpp:14-16`). State 3 runs `setupMatch` (`online_match.cpp:132-198`): `gmSelCharData` from SESSION, then `sqVsMelee`'s own setup. This is the live path, so the same setup comes out.
6. **Match start.** At the first loop top with `scMelee` pending, or at stMelee's constructor (GS:3057-3086, GS:3213-3223), Dolphin runs the joiner side of `ApplyMatchStart` (GS:2816-2870) with the file as the host: only the init block's stage variant is taken (`PeerData::MergeInitBlock`, PD.h:72-81), the three seeds and the serial counter are written.
7. **Barrier** (first simulation frame, `IsSimStart` GS:2102-2108):
   - compute `SetupKey` (GS:412-442) and compare it with the recorded key. Mismatch: stop, with "This replay's match could not be rebuilt here (setup differs)" and the differing bytes in the log;
   - `WriteSyncBlock` with the recorded sync block (GS:612-669; it already refuses start points that are not a permutation and non-finite frame deltas);
   - `ApplyTaskOrder` with the recorded names (GS:532-577; it only reorders this machine's own tasks and returns -1 when the task sets differ). In playback a -1 is logged and shown; playback continues and the checksums decide (risk, section 12);
   - take keyframe K0 (6.4); restore normal speed, audio and presentation.
8. **Countdown** exactly as live: `NeutralSlots` for the recorded ports (GS:2117-2127), the pacing hooks (`DrivesLoop`, GS:3726-3730), the IO wait (GS:2930-2932).
9. **Running from `start_frame`.** `StartRunning` (GS:2256-2279) with a file op source instead of GekkoNet: one advance per update, `save_after` on, no load, no region mode. The slots come from the file, every port is passed through `SanitizePad` (PD.cpp:212-229), and ports without a player read "no controller" as live (GS:1951-1954). This is the existing `no_rollback` replay path (GS:1979-1988, GS:2305-2313) fed by a different source.
10. **Check.** At each loop top, `FrameChecksum` (GS:682-695) of the previous frame is compared with the file (section 7).
11. **End.** After the last recorded frame:
    - game set: neutral input until the plugin's state 10 leaves the match (the end sequence runs as it does on each machine after a live session ends, STATUS:358-361). Then "Replay finished", and Dolphin waits for a new `commandId` or the next queue item. The next replay starts from the CSS without a reboot, because the barrier writes make any menu history equivalent;
    - disconnect, LRAS or incomplete: pause at the last frame with an OSD line naming the reason.

### 6.2 Starting the playback Dolphin

Use Slippi's contract unchanged: `-i <comm.json>` (`refs/slippi-wiki/COMM_SPEC.md:1-41`). The launcher already implements the writer side: `ReplayCommunication` (`L/src/dolphin/types.ts:5-25`), the comm file and `-i` (`L/src/dolphin/instance.ts:155-189`), `launchPlaybackDolphin` (`L/src/dolphin/manager.ts:138-170`), and file-open → playback + stats (`L/src/main/main.ts:316-330`).

| `comm.json` field | Ours |
|---|---|
| `mode` `normal` / `queue` | yes (`queue` plays a set: `launcher-profile-ux.md:166`) |
| `mode` `mirror` | later (spectating) |
| `replay`, `queue[].path` | path to a `.rep`; must be an existing regular file under 64 MiB |
| `startFrame`, `endFrame` | game frames (ours start at 1, Slippi's at -123: `refs/slippi-Ishiiruka/Externals/SlippiLib/SlippiGame.h:27`) |
| `commandId` | changing it restarts or switches the replay without a reboot |
| `rollbackDisplayMethod` | `off` only; `normal` needs pass logs (R4, open question 6) |
| `shouldResync`, `isRealTimeMode`, `outputOverlayFiles`, `gameStation` | ignored for now |

Dolphin side:
- `UICommon/CommandLineParse.cpp:85-131`: add `-i/--replay-comm` (free today).
- Playback needs the same disc, P+ files and plugin as Play. The launcher ships one Dolphin build for both (`L/src/dolphin/manager.ts:41-46`, `L/src/dolphin/install/paths.ts:52`). Give the playback instance the netplay install's patched SD card with SD writes off, instead of a second 2 GB copy (open question 8). `PlaybackDolphinInstance.play` must add the `-e` boot arguments it does not pass today (`instance.ts:182-187`).
- The playback User folder has no `user.json`, so nothing logs in or opens a socket.

### 6.3 What the game shows

- The match as played: HUD, stock icons, timer. **No in-game names.** Name tags over fighters are not used online (`docs/game-code.md:441`), and a tag index in `gmSelCharData` selects controls, so writing one would change the match.
- Names, codes, mode and game number go on a Dolphin overlay (ImGui, like the OSD) and in the window title ("Brawl Online Playback | Alice vs Bob | Direct · Game 2").
- A seek bar overlay, Slippi's: time "m:ss / m:ss", click or drag to seek, toggled by a hotkey (`ISH/DolphinWX/Frame.cpp:1640-1662`).

### 6.4 Controls, seeking and rewind

| Control | Default key (Slippi's) | How |
|---|---|---|
| Pause / play | Dolphin's pause hotkey | Pause **at the loop top**: the CPU thread waits on the host as `HostWait` does (GS:2085-2100), so emulated time stops at a frame boundary |
| Frame advance | Period (`ISH/Core/HotkeyManager.cpp:407-411`) | Run one pass, stop at the next loop top |
| Frame back | Comma (new) | Load from the rewind ring (below) |
| Jump ±5 s | Left / Right (`ISH/Core/HotkeyManager.cpp:415-423`; Slippi's 300 frames, `ISH/Core/Slippi/SlippiPlayback.cpp:228`) | Seek |
| Seek bar | mouse | Seek |
| Speed 0.25× to 2× | menu | Dolphin's emulation speed setting |
| Hide overlay | Slippi's "Toggle Seekbar" | |

**Keyframes** (in memory only, never written to a file, never loaded from one):
- K0 at the barrier (after the sync writes), K1 at `start_frame`, then every 600 frames (10 s; Slippi uses 900 frames, `ISH/Core/Slippi/SlippiPlayback.cpp:19`), captured at the loop top on the CPU thread.
- Capture = device state through `State::SaveToBuffer` with RAM skipped, plus the RAM granules written since the previous keyframe from the dirty bitmap: the split `DeltaSaveSlot` already uses (`DeltaSaveSlot.cpp:280-310`, `RollbackManager.h:119-121`). That keeps the CPU-thread hitch near the cost of a rollback save (0.55-0.9 ms, STATUS:205-208, STATUS:258) instead of a full Wii savestate (Orca's whole-machine keyframes were 26-30 MB compressed, backend-design.md:353).
- Compression and bookkeeping run on a worker thread. A load waits for every pending job first: the Phase 9 race was exactly a load reading a base snapshot while a job still wrote it (STATUS:719-756).
- Memory budget (default 1 GB, open question 3): over budget, every other old keyframe is dropped.
- A simpler first cut, if the delta store is late: full `State::SaveToBuffer` keyframes compressed on a worker, as Slippi does (`ISH/Core/Slippi/SlippiPlayback.cpp:165-206`), with a longer interval.

**Seek to frame T:**
1. Pause at the loop top.
2. If T is ahead and no keyframe lies in (current, T], fast-forward from here. Otherwise load the newest keyframe at or before T.
3. Fast-forward to T with `SetRollbackResimulating(true)`: unthrottled (`CoreTiming.cpp:399-403`), no audio (`AudioCommon.cpp:208-211`), no presentation (`VideoInterface.cpp:877`). Slippi overclocks to 4× instead (`ISH/Core/Slippi/SlippiPlayback.cpp:307-321`).
4. Compare frame T's checksum with the file, then resume or stay paused.

**Rewind ring:** during the last 8 frames of every fast-forward, and on every paused frame step, a whole-machine save goes into RollbackManager's 8-slot ring (`RollbackManager.h:32`). Up to 7 steps back are then instant; the next step refills the ring through a keyframe.

### 6.5 Replays from older builds

| Component | Affects the simulation? | Compatibility key |
|---|---|---|
| Disc (game id, revision) | yes | must match |
| P+ release (codes, files on the SD) | yes | must match (the launcher pins it by sha256, `L/PPLUS_PORTING.md:226`) |
| Plugin (`online_match.cpp` builds the setup) | yes | PPOM version + plugin build id must match |
| Dolphin session code (sync writes, pacing, IO wait, anchoring, countdown, sanitising) | yes | `SIM_VERSION` must match |
| Rest of Dolphin (UI, video backends) | no | recorded for diagnostics only |
| Region set | no: playback does not roll back | recorded for diagnosing live desyncs |

Recommendation: refuse with a message that names the differing component ("recorded with Project+ 3.2.1, this install has 3.2.2"), and offer "Play anyway" (the checksums show at once whether it still matches). Do not archive old builds now: the launcher ships Dolphin inside its package, and a P+ release change would also need the old 2 GB of P+ files. `SIM_VERSION` changes should become rare after release; small ones can keep the old behaviour behind a version switch. Server-side Ranked replays keep their build reference for disputes.

## 7. Desync detection in playback

- After every frame, its `FrameChecksum` (frame counter, three RNGs, fighters CRC; GS:682-695) is compared with the recorded value. Every 60 frames the parts are compared too, and, if `POST` is present, every player field: the report can then say "frame 5,180: Mario's x 12.40 against 12.52".
- **First mismatch:** a yellow OSD line, "Replay desynced at 1:23 (frame 5,180). From here the game may differ from what was played", a red mark on the seek bar, and a log line with the parts. Playback continues; it is still a valid game, just not this one.
- Dolphin writes `<comm file>.status.json` (`{ "replay", "frame", "desyncFrame", "reason" }`); the launcher stores `desync_frame` in its index and shows a badge.
- A desync that happened **live** (GekkoNet's report, recorded in `EVNT`) is shown at its frame and as a browser badge, independent of playback.
- No resync. Slippi's `shouldResync` corrects playback with recorded post-frame data (`COMM_SPEC.md:15`). Writing fighter state into Brawl memory is neither safe (peer-controlled bytes into the heaps) nor enough (most state is elsewhere). Non-goal.

## 8. Security

A shared replay comes from a stranger. Every byte is validated before use. Values reach game memory only through the checks a live joiner already applies to the host.

### 8.1 Container

| Check | Limit |
|---|---|
| File size | ≤ 64 MiB, else refuse |
| Magic, major version | exact; minor may be higher |
| Chunk lengths | `stored_len` ≤ bytes left; per type: `INFO` ≤ 64 KiB, `SETP` ≤ 4 KiB, `SYNC` ≤ 256 KiB, `FRMS`/`POST` raw ≤ 1 MiB, `EVNT` ≤ 64 KiB, `ENDG` ≤ 16 KiB; ≤ 10,000 chunks |
| Decompression | output bounded to exactly `raw_len` (zlib with a fixed output buffer; Node `maxOutputLength`); a different size is an error. No bombs |
| CRC | a mismatch ends the file there (crash rule); in the first chunks it refuses the file |
| Order and count | `INFO`, `SETP`, `SYNC` once each and before the first `FRMS`; unknown critical type → refuse; unknown non-critical → skip |
| Frames | contiguous from `start_frame`, `count` ≤ 600, total ≤ 216,000 (60 min), `ports` equal to `SETP`'s |
| Trailer | used only if its CRC matches and it points at a valid `ENDG`; else ignored |

### 8.2 Fields that reach the game

| Field | Check | Existing code |
|---|---|---|
| Number of players, ports | 1-4 (2 today), distinct ports 0-3 | `online_match.cpp:146` |
| Character | `PeerData::ValidCharKind`, then the plugin's `selectableCharKind` | PD.cpp:153-157, `online_match.cpp:150-156` |
| Costume | ≤ 0x1F | PD.h:64-65, `online_match.cpp:152` |
| Stage | `ValidStageKind`, then the plugin's `StageLegal::selectableKind` | PD.cpp:164-178, `online_match.cpp:146` |
| ASL buttons | only the bits the stage select produces | new |
| Port values | `SanitizePortValues`; layout bytes by the plugin's `layoutByte` | PD.h:95-105, `online_match.cpp:86-95` |
| Names, codes into SESSION | UTF-16, length-bounded (`NAME_LEN`, `CODE_LEN`), control characters removed | GB:761-762 |
| Init block | only the stage variant (≤ 0x0F) is taken, never stage kind, rules or time | PD.h:72-81, GS:2843-2859 |
| Seeds | any value; bit 31 cleared for the two mtRand words as `MatchSeeds` does | GS:677-678 |
| Sync block | as from a peer: only the frame counters and a finite frame delta of `g_GameFrame`; start points must be a permutation of ours | GS:612-669, GS:765-817 |
| Task order | ≤ `MAX_TASK_LISTS` lists, ≤ `MAX_TASKS_PER_LIST` names of ≤ 64 bytes; only reorders our own tasks | PD.h:30-33, GS:1210-1238, GS:532-577 |
| Pads | `SanitizePad` on every frame and port | PD.cpp:212-229 |
| `start_frame` | 60-600 | new |
| Setup key | compared, never written | GS:3111-3121 |
| `POST`, checksums, `INFO`, `ENDG` | never written to game memory; compare and display only | |

### 8.3 Paths and display

- No path comes from file contents: the file name is derived from the local clock (4.5), the match id is never part of a path (4.3), and downloads (if ever allowed) use the same rule (`launcher-profile-ux.md:281`).
- `comm.json` is parsed with the depth and number limits of `PeerData::JsonSafeToParse` (PD.h:43-46: picojson calls `std::abort` on `1e999`); `replay` must be an existing regular file, not a device or URL.
- Display strings (overlay, window title, launcher) are length-capped and stripped of control and bidi-override characters. React escapes text; never use `dangerouslySetInnerHTML` for them.
- The file never contains a savestate, and the format has no chunk type for one. Keyframes are created in memory during playback.

### 8.4 `SIM_VERSION`

A constant in `Gprb` (new), bumped by any change to the simulation path of a match: sync writes, pacing, the IO wait, anchoring, `NeutralSlots`, `start_frame` handling, `SanitizePad`. A golden-replay check in the nightly harness (11, test 9) catches a missed bump: the same stored replay must still give the same trace.

## 9. Launcher integration

### 9.1 Indexing (`readReplayFileInfo`)

`L/src/replays/replay_format.ts:27-38` returns only size and birth time today. Replace it with:
1. Read the first 64 KiB: header, `INFO` (CRC-checked JSON, safe parse).
2. Read the last 16 bytes: if the trailer is valid, read `ENDG` at its offset. Without a trailer, walk the chunk headers (lengths only, no decompression) to find the last frame: the file is in progress or incomplete.
3. Map to the existing tables (`L/src/database/schema.ts:88-127`): `session_id` = `matchId`, `game_number`, `tiebreak_number`, `is_ranked`, `is_teams`, `stage`, `start_time`, `mode`, `last_frame` = `durationFrames`; players: `port`, `character_id`, `character_color`, `team_id`, `is_winner`, `start_stocks`, `connect_code`, `display_name`, `user_id`.
4. New migration (next to `L/src/database/migrations/20231030T2041_initial.ts`): `game_uid`, `end_method`, `complete`, `playable` (compatibility), `desync_frame`, `local_port`, per player `stocks_left`, `end_damage`.
5. Re-index a file when its size or mtime changes (in-progress games).
6. Drop `.json` from `REPLAY_EXTENSIONS` (`replay_format.ts:10-11`) unless an export format is decided.

`characterId` is `gmCharacterKind`; fill `characterStockKeys` in `L/src/renderer/lib/utils.ts` (`L/PPLUS_PORTING.md:156`) and add a stage-id → name table (`launcher-profile-ux.md:273-274`).

### 9.2 Match history and profile

- Set → games: `game WHERE session_id = :matchId ORDER BY game_number, tiebreak_number` (`launcher-profile-ux.md:278`). "Watch set" = `comm.json` `queue` with the found files.
- Server history (Ranked, later all modes) joins on `matchId` + game index; a game with no row is "not on this computer" (`launcher-profile-ux.md:198-206`).
- Rank at match time: optional `players[].rank` in `INFO`, once the session gets it from matchmaking.

### 9.3 Stats page

| Stat | Inputs + checksums only | Needs per-frame player state |
|---|---|---|
| Duration, result, end method, stage, characters | yes (`ENDG`) | |
| Inputs per minute, per-button counts | yes | |
| Stocks lost, death frame, kill percent | | yes (stocks, damage) |
| Damage dealt and taken over time | | yes (damage) |
| Time per action state, positions, heat maps | | yes (status kind, x, y) |
| Openings, conversions, combos, punishes (slippi-js) | | yes, plus hitstun and last attacker (offsets not mapped yet, open question 5) |

Recommendation: record `POST` from R3 on (or from R1 if cheap). Without it, the stats page can never compute more than the first two rows without running Dolphin. A headless "stats pass" (play the replay unthrottled and read memory) is possible but slow (a full emulation per game) and needs the disc and P+ files. `calculateGameStats` (`L/src/replays/types.ts:51`) returns slippi-js's `StatsType`: fill the fields we can and leave the rest undefined.

### 9.4 File association

`.rep` is already registered (`L/electron-builder.json:18-22, 34-37`), and opening a file plays it and shows stats (`L/src/main/main.ts:316-330`). StarCraft also uses `.rep` (open question 1).

### 9.5 Upload (later, Ranked only)

Dolphin's reporter uploads the finished file to the signed `uploadUrl` (backend-design.md:112-115, 243), as Slippi's game reporter does. Chunks are already compressed, so no gzip. Server-side re-simulation for disputes would run a headless playback, which needs the disc and P+ files on the server (open question 9).

## 10. Phased plan

### R1: record and play back one game (smallest useful milestone)

Scope: Direct and Unranked recorded on both peers; play back from a fresh boot to the end; no seeking; checksum verification; the browser shows real metadata.

| Repo | Files | Change |
|---|---|---|
| dolphin | `Core/Rollback/ReplayFile.{h,cpp}` (new) | container writer and validating reader |
| dolphin | `Core/Rollback/ReplayRecorder.{h,cpp}` (new) | rings, emit rule, writer thread |
| dolphin | `Core/Rollback/GameplaySession.{h,cpp}` | `Mode::Playback`, `ArmPlayback`; recorder calls at the barrier (GS:3136-3145), in `RunFrame` (GS:2358-2434) and `PerformQueuedSave` (GS:1764-1858), in `EndRunning` (GS:1544-1589); file op source next to `ReplayNextUpdate` (GS:2190-2254); `ApplyMatchStart`/barrier with the file as host (GS:2816-2870, GS:3088-3145); `GetLobby` from the replay (GS:2577-2599); `ConnectOptions` gets match id, mode, players (GSH:70-107) |
| dolphin | `Core/Rollback/GameplayOnlineBackend.cpp:27-76` | pass the match metadata |
| dolphin | `Core/Online/GameBridge.cpp:641-773` | SESSION from the playback lobby; LOCAL `LS_PLAYBACK`; PPOM v4; plugin build id |
| dolphin | `Core/Config/OnlineSettings.{h,cpp}` | `ReplayDir`, `SaveReplays`, `ReplayMonthlyFolders` |
| dolphin | `UICommon/CommandLineParse.cpp`, DolphinQt and DolphinNoGUI start-up | `-i`, refusal dialog, "Replay finished" |
| dolphin | `Core/Harness/HarnessServer.cpp` | `replay_play {path}`, `replay_status` |
| dolphin | `Source/UnitTests/Core/Rollback/ReplayFileTest.cpp` (new), next to `OnlineSecurityTest` (`UnitTests/Core/CMakeLists.txt:53-62`) | round trip, truncation, mutation |
| game-code | `include/ppom.h`, `tools/gamecode/ppom.py` | PPOM v4: `LS_PLAYBACK`, plugin build id in the header |
| game-code | `source/online_menu.cpp`, `source/online.cpp` | in playback: open the recorded mode's CSS from the main menu; the CSS locks in for SESSION's game at once |
| game-code | `source/online_match.cpp` | state 10 in playback: back to the playback idle state instead of the CSS |
| launcher | `src/replays/replay_format.ts` (+ tests with hostile fixtures), database migration | 9.1 |
| launcher | `src/dolphin/instance.ts`, `src/dolphin/manager.ts` | boot arguments for playback; read-only SD |
| harness | `harness/tools/replay_check.py`, `harness/tests/test_replay.py` (new) | section 11 |

### R2: playback controls

Pause and step at the loop top, keyframe store, seek and fast-forward, rewind ring, overlay seek bar and hotkeys, `.status.json`, harness `replay_seek`/`replay_pause`. Files: `Core/Rollback/ReplayPlayback.{h,cpp}` (new), `RollbackManager` (a keyframe store with a long stride), `VideoCommon/OnScreenUI.cpp` (overlay), `Core/HotkeyManager.{h,cpp}`.

### R3: stats and history

`POST` recording and comparison, launcher stats (9.3), match-history links and "Watch set" (queue mode), desync and incomplete badges.

### R4: later

Ranked upload and server re-simulation; Teams (4 ports); offline Versus (a local session mode plus a playback setup path for any rules); optional pass-log chunk for rollback-faithful playback and bug reports; spectating via `mirror`; an old-build archive only if needed.

## 11. Test plan (existing harness)

Not run now (shared, CPU-bound machine). In order:

1. **Unit** (`ReplayFileTest`, ASan in CI): write/read round trip; the file cut at every byte offset reads as its longest valid prefix and never crashes; mutations of every field, sizes 0 / maximum / 4 GiB, a decompression bomb, an unknown critical chunk, duplicate `SETP`, non-contiguous frames, a bad trailer: each refused cleanly.
2. **Record:** `harness/tests/test_online_game.py::test_direct_set_under_the_gameplay_session` (two Direct games through the in-game flow, STATUS:571-580) with `-C Dolphin.Online.SaveReplays=True -C Dolphin.Online.ReplayDir=<instance>/replays` on both peers. Check: two files per game; equal `game_uid` and `inputsSha256`; `INFO` names, codes, characters and stage equal the lobby's; `ENDG` winner equals the session's `last_winner`; the last frame is at least the game-set frame.
3. **Play back:** a fresh DolphinNoGUI (another boot, so another menu history) with `-i comm.json` and `frame_trace_config enabled`. Wait for `replay_status` "ended". Compare its `frame_trace` rows (game frame, RNGs, fighters, game set; GR.h:142-163) from frame 1 to the last recorded frame with **both** live peers' rows (the test already compares the peers' traces, STATUS:528-541). Require identical rows and 0 checksum mismatches.
4. **Variants:** play back the host's and the joiner's file; single and dual core; Null and D3D11; a Smashville game across an RTC hour boundary (`gprb_session.py --rtc-a/--rtc-b`, STATUS:684).
5. **Only confirmed inputs:** record with `PPR_GPRB_PASS_LOG` set at the same time, `gprb_passlog.py flat` the pass log, and compare its final inputs with the `.rep` inputs frame by frame: equal. Under `typical` and `bad_wifi` netsim, which needs the in-game flow through netsim (STATUS:558, Phase 7 open issue 1).
6. **Crash safety:** kill one peer's Dolphin in a match. Its file plays to its last chunk (within about 1 s of the kill); the other peer's file ends with `endMethod: "disconnect"`.
7. **Seek (R2):** for 20 random targets, forward and back, plus repeated frame-back steps, the trace from each target on must equal the straight playback's rows. Repeat under load with `gprb_replay_stress.py --burn`-style CPU burners (STATUS:738-748): the keyframe store's worker must not reintroduce the Phase 9 race.
8. **Hostile files** (`harness/tests/test_replay_hostile.py`, new, like `test_peer_hostile.py`; and launcher vitest): mutated files never crash Dolphin or the launcher, are refused with a reason, and SESSION never holds an unvalidated value (read it with `ppom.py dump`).
9. **Golden replay (nightly):** one stored replay per region set family (a Final Smash game, a transformation game, an items game) must give the stored trace hash; a change means a missed `SIM_VERSION` bump.
10. **Cost:** recorder time per pass on the CPU thread under 20 µs; with the writer artificially stalled the game runs on and the queue grows.

## 12. Risks

| Risk | Mitigation |
|---|---|
| The playback menu path (main menu → online CSS without the ONLINE pages) leaves a different task set, so `ApplyTaskOrder` returns -1 | Test 3 logs the relink result. If needed, the plugin walks the same menu pages as a player, or the task-order sync learns to tolerate extra menu tasks |
| A setup field outside the setup key differs between live and playback | The whole `gmGlobalModeMelee` is recorded and diffed at the barrier (log only) |
| A region-set gap made the live game differ from the no-rollback ground truth | Playback shows the ground truth; the recorded checksums show the difference (it would also have been a live desync between peers) |
| Keyframe memory or hitch too large | Delta store (6.4), budget, interval setting; measure in R2 |
| An incompatible P+ or plugin update makes old replays unplayable | Clear refusal, "Play anyway"; keep `SIM_VERSION` stable after release |

## 13. Open questions

1. **Extension.** `.rep` is also StarCraft's. Switch to `.brep` before anything ships (the launcher's association and filter change in two places)?
2. **Offline Versus.** In scope? It needs a local session mode (offline lag would then slow the game instead of skipping frames, as online) and a setup path for any rules, items, CPUs and 4 players.
3. **Keyframe budget.** Default 1 GB and 10 s? Is a short hitch every 10 s acceptable in playback?
4. **Old builds.** Refuse with "Play anyway" (recommended), or keep an archive?
5. **Player-state fields.** Combos and punishes need hitstun, shield and last attacker; their offsets are not mapped (`docs/brawl-memory-map.md` has damage, stocks, position, facing, status, motion). Worth the reverse engineering for R3?
6. **Pass-log chunk.** Store every pass (with mispredictions) behind a setting, for Slippi's `rollbackDisplayMethod: normal` and for bug reports? It is about 0x100 bytes per pass and replays from a fresh boot like the rest.
7. **Privacy of shared files.** Files carry uids, names and connect codes (Slippi's carry names and codes too). Keep uids?
8. **Playback SD card.** Share the netplay install's patched card read-only (recommended) or keep a second 2 GB copy?
9. **Ranked disputes.** Who uploads (Dolphin, as Slippi), and may the server keep a disc and P+ files for re-simulation?
10. **Harness recordings.** `gprb_session.py` builds matches through the CSS/stage select with harness writes, not through SESSION (STATUS:163, STATUS:222-224). Should those recordings be playable (the harness would have to build its matches through SESSION), or are only real online games enough?
