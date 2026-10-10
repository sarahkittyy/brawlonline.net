# Harness control protocol (v1)

This is the contract between the C++ harness server inside our Dolphin fork (`dolphin/Source/Core/Core/Harness/`) and the Python driver (`harness/`). Change this file first, then both sides.

## Purpose

Let automated scripts (and Claude) run Dolphin instances end to end with no human at the keyboard. The scripts can:

- boot P+
- drive the virtual GameCube controllers
- read game memory
- take screenshots
- host and join netplay sessions (including rollback)
- compare game state between instances to detect desyncs

## Enabling

- Command line: `--harness-port <port>` (both `Dolphin.exe` and `DolphinNoGUI.exe`).
- Environment variable: `PPR_HARNESS_PORT=<port>` (used if the flag is absent).
- The server listens on **127.0.0.1 only**.
- If neither is set, the harness code does nothing and costs nothing.

## Transport

- TCP. Each message is a single line of UTF-8 JSON terminated by `\n`.
- Request: `{"id": <int>, "cmd": "<name>", ...args}`.
- Response: `{"id": <same>, "ok": true, "result": {...}}` or `{"id": <same>, "ok": false, "error": "<message>"}`.
- Only one client at a time. A second connection is refused with an error line and then closed.
- Requests are handled in order. A blocking command (`wait_frame`) blocks only that connection; the emulator keeps running.
- Addresses are numbers (decimal in JSON; the Python side may accept `0x` strings and convert). Byte data is lowercase hex strings.

## Frame numbering

- `frame` is the number of VI fields (`Movie::GetCurrentFrame()` or the equivalent VI counter) since boot. It increases at 59.94 Hz while the game runs, including on menus.
- `input_polls` is the number of times the game polled the GameCube pads since boot.
- Pad scripts are scheduled on `input_polls`, so they are exact per game frame regardless of lag.

## Commands

| cmd | args | result |
|---|---|---|
| `ping` | — | `{"pong": true, "version": 1}` |
| `status` | — | `{"state": "uninitialized"\|"starting"\|"running"\|"paused"\|"stopping", "frame": n, "input_polls": n, "game_id": str, "cpu_thread": bool, "video_backend": str, "netplay": null \| {...see netplay_status}}`, plus `poll_source`, `si_polls`, `audio_muted` and `presentation` (see "Presentation counters" below) |
| `read_mem` | `addr`, `len` (≤ 16 MiB) | `{"hex": "..."}`. Effective (virtual) addresses `0x80000000–0x817FFFFF` (MEM1) and `0x90000000–0x93FFFFFF` (MEM2) |
| `write_mem` | `addr`, `hex` | `{}` |
| `read_u32` | `addr` | `{"value": n}` (big-endian, as the game sees it) |
| `hash_mem` | `ranges`: `[[addr, len], ...]` | `{"xxh3_64": "16 hex chars"}`, one hash over all ranges concatenated in order |
| `pad_set` | `port` 0–3, `buttons`: list of `A B X Y Z L R START DUP DDOWN DLEFT DRIGHT`, `main`: `[x,y]` 0–255 (default 128,128), `c`: `[x,y]`, `l`: 0–255, `r`: 0–255 | `{}`. Persistent override of that port until `pad_clear` |
| `pad_clear` | `port` | `{}`. Hands the port back to the real or configured controller |
| `pad_script` | `port`, `frames`: `[{buttons, main, c, l, r, hold}, ...]` (`hold` = number of polls, default 1), `start`: `"next"` or an absolute `input_polls` value | `{"starts_at": n, "ends_at": n}`. **Replaces** any script on that port; `ends_at` is exclusive. Played poll by poll; afterwards the port returns to its `pad_set` state, or to neutral (released, not cleared) if there is none. Until a replacing script starts, the port keeps presenting what it presented when the script was submitted (it stays overridden) |
| `pad_script_status` | `port` | `{"active": bool, "remaining": n}` |
| `wait_frame` | `frame` **or** `input_polls`, `timeout_ms` (default 10000) | `{"frame": n, "input_polls": n}` once reached; error `"timeout"` otherwise |
| `pause` / `resume` | — | `{}` |
| `frame_advance` | `n` (default 1) | `{"frame": n}` after advancing while paused |
| `screenshot` | `path` (absolute `.png`) | `{"path": ..., "width": w, "height": h}`. Error if the video backend is `Null`. Returns only after the file is written |
| `save_state` / `load_state` | `path` | `{}` (Dolphin savestates, for test setup only, never for rollback) |
| `netplay_host` | `port`, `game`: path to the `.dol`/`.iso` to boot, `name`, `rollback`: bool, `delay`: int (optional) | `{}`. Hosts and joins its own server as player 1 (headless; no Qt dialog). `delay`: with `rollback`, the rollback input delay, 1-9 frames from pad read to use (default 2, error outside 1-9); otherwise the fixed-delay pad buffer |
| `netplay_join` | `host`, `port`, `name` | `{}` |
| `netplay_start` | — | `{}` (host only; boots the game on every client) |
| `netplay_status` | — | `{"role": "host"\|"client"\|null, "connected": bool, "players": [{"pid", "name", "ping_ms"}], "game_running": bool, "rollback": {...}}`. The `rollback` fields are listed under "Rollback counters" below |
| `rollback_pad_history` | `since` (GekkoNet frame, default 0) | `{"frames": [[frame, buttons_p0..p3, crc32, gekko_checksum, legacy_checksum, frame_counter, fighters_checksum, logic_steps], ...]}`. See "Rollback debugging commands" |
| `rollback_timings` | `since` (sample index, default 0) | `{"next", "first", "save_us": [...], "sync_us": [...], "compiles": [...], "evict_us": [...], "dostate_us": [...], "ram_us": [...]}`: one sample per rollback snapshot (the last 32768). See "Rollback debugging commands" |
| `rollback_chunk_hashes` | `frame` (GekkoNet frame) | `{"frame", "chunk_size", "hashes": [...]}`. Needs `PPR_ROLLBACK_CHUNK_HASHES` or a sync test |
| `cpu_state` | — | `{"pc", "npc", "msr", "lr", "ctr", "srr0", "srr1", "sp", "exceptions", "pi_cause", "pi_mask", "vi_display_interrupts": [4], "current_thread"}` (debugging) |
| `netplay_leave` | — | `{}` |
| `online_status` | — | Login state from `<User>/Online/user.json`: `{"logged_in", "app_state" 0/1/2, "app_state_name", "uid", "display_name", "connect_code", "latest_version", "app_version", "has_play_key", "user_fetch", "watching", "user_json_path", "mm_server", "accounts_url", "ranked"}`. `ranked` is the ranked set (Dolphin `Online/Ranked.cpp`; `reporting`: this 1v1 match's games are reported, every mode but Teams; `setup`: Ranked's game setup, `Online/GameSetup.cpp`, with its last `view`): `{"active", "reporting", "setup", "match_id", "wins" [port 0, port 1], "local_port", "set_over", "peer_gone", "left_reported", "reports_sent", "reports_failed", "pending_jobs", "result_pending", "last_status", "rating", "change", "rank_state", "user_rating", "sets_played"}`. Never returns the play key. See "Online play" |
| `mm_search_direct` | `code` (the opponent's connect code), `session`: `"auto"` (default) \| `"none"`, optional `game`, `delay`, `auto_start`, `selections` | Starts a Direct search; returns `mm_status`. See "Online play" |
| `mm_search` | `mode`: `ranked`\|`unranked`\|`direct`\|`teams`\|`party`, `code` (optional), same options | Same for any mode (our server refuses all but Direct) |
| `mm_status` | — | `{"state", "state_code", "searching", "error", "error_source", "mode", "opponent_code", "server", "local_port", "lan_address", "tickets", "connect_attempts", "match": {...}, "handoff", "handoff_error", "session": {...}}`. See "Online play" |
| `mm_cancel` | — | Slippi's `CLEANUP_CONNECTION`: ends the search, the P2P link and the session; returns the fresh (idle) `mm_status` |
| `online_session_backend` | `backend`: a registered backend (`"gameplay"`, the default; `"netplay"`, `"record"`) \| `"none"` | Replaces the session backend that a connected match is handed to; returns `mm_status`. See "Online play" |
| `game_bridge_status` | — | Dolphin's side of the game's PPOM mailbox: `{"enabled", "hand_off", "found", "block", "mailbox", "mailbox_size", "module", "module_id", "frames", "locate_attempts", "netplay_paused", "requests", "responses", "lost", "deferred_frames", "osd_disconnects", "by_cmd": {name: count}, "last_request", "last_response"}`; once found also `"local"`, `"session"`, `"session_seq"`, `"local_seq"`, `"lock_in"` (the last lock-in read, with `"tag"`: the player's port values carry a name tag, and `"pad"`: the controller port whose START locked in). `osd_disconnects`: in-match disconnects the game did not show itself, so Dolphin showed its red OSD message. See "Online play" |
| `game_bridge_config` | optional `enabled`, `hand_off` (bools) | Servicing on/off (off lets `tools/gamecode/drive.py mbx-serve` play Dolphin's part); whether the game's FIND_OPPONENT hands the match to the session backend. Returns `game_bridge_status` |
| `rooms_status` | — | Rooms (`Online/Rooms.h`, docs/rooms-game-interface.md): the online connection to mm (`"connection"`: down/connecting/hello/online, `"connection_error"`, `"connection_hold"` after "Signed in from another game.", counters `connects`, `hellos`, `drops`, `sent`, `received`, `invalid`), the room phase (0 none, 1 joining, 2 in a room), `"pending"`, the status line (`"text"`, `"error"`, `"serial"`), the room's next game number `"game"` and stage `"pickers"`, `"join_seq"`, the game's `"screen"` and `"lock"`, `"room_game"` (`active`, `code`, `match_id`, `host`, `teams`, `saw_session`, `saw_match`, `started`, `done`, `failed`, `last_end`), the validated `"view"` (null outside a room), the launcher's `"launch_pending"`, `"last_request"`, `"requests_seen"`, and the last 40 `"log"` lines |
| `rooms_request` | `op`: `create` (`public`), `join` (`code`), `leave`, `slot` (`slot` 1-4, `open`), `teams` (`on`), `public` (`public`), `team` (`team` 0-2); or, standing in for the game plugin, `ready` (`ready`, `character`, `costume`: the lock-in; overrides the game's until `release_lock`), `release_lock`, `screen` (`screen`, ppom.h Screen) | An op as the room CSS would send it. Returns `rooms_status` |
| `online_recent_codes` | `mode`: `"direct"` (default) \| `"teams"`, optional `clear` (bool), `add` (a code or a list, oldest first), `prefix` with optional `index`, `scroll` (0 none, 1 older, 2 newer, 3 reset; default 3) | The logged-in account's recent connect codes (`<User>/Online/<uid>/direct-codes.json` / `teams-codes.json`): `{"mode", "path", "codes": [newest first], "suggestion"?: {"found", "index", "code"}}`. `suggestion` is what the keypad's `FETCH_CODE_SUGGESTION` (0xBE) would get for that input. Errors when no user.json has been read. See "Online play" |
| `log_mark` | `text` | `{}`. Writes `[HARNESS] <text>` into dolphin.log, used to correlate log lines with test steps |
| `quit` | — | `{}`, then a clean shutdown (flushes logs and exits the process) |

## Notes

- Pad injection happens at the emulated controller source: the GameCube pad status that the SI device reads. Netplay and rollback therefore see harness input exactly as if a physical controller produced it.
- `pad_set`/`pad_script` on a port with no SI device return an error. Tests configure the port as a "Standard Controller" in `GCPadNew.ini` or `Dolphin.ini` (`SIDevice0 = 6`).
- `quit` must leave the process with exit code 0 and flushed logs. Killing the process loses buffered log lines.

## Defaults

- **Audio is muted for harness sessions.** Whenever the harness is enabled (`--harness-port` or `PPR_HARNESS_PORT`), Dolphin forces `MAIN_AUDIO_MUTED` (`[DSP] Muted`) to true for that session only (current-run config layer; nothing is written to `Dolphin.ini`). The audio pipeline still runs, only the output is silent. Set `PPR_HARNESS_AUDIO=1` to keep the configured audio setting. `status` reports the effective value as `audio_muted`. (Temporary; we'll revert this later.)
- Test user directories should also have `[Input] BackgroundInput = False` (the template does), so the real keyboard/controller don't leak into test instances. Harness pad overrides are not affected by the input gate.

## Rollback

Everything in this section was added while fixing Brawlback's rollback (`rollback-fixes` branch of `dolphin/`). Brawlback had none of these counters or commands.

### Rollback counters (`netplay_status.rollback`)

| field | meaning |
|---|---|
| `enabled` | the session runs in rollback mode |
| `gekko_session`, `session_started` | a GekkoNet session exists / its handshake with the peer finished |
| `current_frame` | GekkoNet frame (this peer's local frame, possibly predicted) |
| `input_delay` | the running game's rollback input delay (frames from pad read to use), set at game start from the host's setting |
| `announced_delay` | the host's current setting (`netplay_host` `delay` or the Qt "Rollback Delay" box); used by the next game start |
| `max_rollback_window` | the build's `MAX_ROLLBACK_FRAMES`: the deepest rollback GekkoNet may ask for (its prediction window) |
| `rollbacks` | rollbacks performed (loads) |
| `max_rollback_frames` | frames resimulated by the deepest rollback so far |
| `frames_resimulated` | frames resimulated in total |
| `by_depth` | `[{"depth", "count", "load_us_total", "load_us_max", "resim_us_total", "resim_us_max"}, ...]` for each rollback depth seen: wall-clock cost of the load, and of resimulating the frames (end of the load to the start of the displayed frame, including the resimulated frames' snapshots) |
| `save_count`, `save_us_total`, `save_us_max` | rollback snapshots taken and their wall-clock cost |
| `load_count`, `load_us_total`, `load_us_max` | rollback loads and their wall-clock cost |
| `desyncs_detected`, `last_desync_frame` | GekkoNet checksum mismatches with the peer (or, in a sync test, between a frame and its re-run) |
| `frames_ahead` | GekkoNet's estimate, already halved: this peer's share of the gap |
| `time_sync_speed` | emulation speed factor applied by time sync, 0.98-1.01 |
| `stall_polls` | GekkoNet polls that gave no frame to advance; the CPU thread waits about 0.25 ms per poll |
| `synctest`, `synctest_mismatches` | sync test running; re-runs whose MEM1/MEM2 differed from the first run |
| `netplay_desync_reports` | Dolphin's own netplay desync messages |
| `save_sync_count`, `save_sync_us_total`, `save_sync_us_max` | snapshots that waited for the GPU thread (dual core), and the wait |
| `gpu_deterministic` | dual core with the deterministic GPU thread (always the case in rollback sessions since `05616daf01`) |
| `gpu_sync_compiles`, `gpu_sync_compile_us_total`, `gpu_sync_compile_us_max`, `gpu_sync_utility_compiles`, `gpu_sync_utility_compile_us_total`, `shader_compilation_mode` | pipelines the video thread compiled synchronously (all; EFB-copy/texture-conversion ones), and the configured mode |
| `peer_disconnects`, `skipped_loads`, `dropped_advances` | GekkoNet dropped the peer (the game is then stopped); rollbacks deeper than the snapshot ring; advances beyond one update's capacity. All should stay 0 |
| `stall_fallbacks` | stall waits a pause request (e.g. a harness read) turned into a guest-side spin |

### Rollback input delay

The GekkoNet input delay is a netplay setting (`NetPlay.RollbackDelay` in `Dolphin.ini`, 1-9, default 2), counted like Slippi's delay frames: frames from reading the controller to the game using the input. The default is the feel the fork had before (the local sample lags one frame, plus GekkoNet delay 1). The host's value is used by both peers: the server announces it to clients (`MessageID::RollbackDelay`, also on join) and sends it with the start-game settings. `netplay_host` takes it as `delay` in rollback mode and rejects values outside 1-9. Measured: a press scheduled at a known pad read lands 2 frames later with delay 4 than with delay 2, and 7 frames later with delay 9, on the same frame on both peers.

### Presentation counters (`status.presentation`)

During a rollback the resimulated frames are run and rendered in full but not presented; only the newest frame of each update reaches the screen. With Immediate XFB (this fork's default) the decision is made where the CPU thread processes the XFB copy; otherwise at the VI field. Counters (reset on every boot):

| field | meaning |
|---|---|
| `present_resimulated` | `PPR_ROLLBACK_PRESENT_RESIM=1` is set (old behaviour, for A/B runs) |
| `immediate_xfb` | Immediate XFB is on |
| `xfb_copies`, `xfb_copies_skipped` | XFB copies, and those of resimulated frames that were not presented |
| `xfb_fields`, `xfb_fields_skipped` | VI fields with an XFB, and those of resimulated frames |
| `copy_decision_misses` | dual core: XFB copies the GPU thread ran without a CPU-side decision (should stay 0) |
| `presents`, `duplicate_presents` | frames the presenter showed; of those, the same XFB as the previous one |
| `displayed_frames`, `displayed_frames_after_resim` | displayed (not resimulated) frames of the rollback loop; of those, frames right after a resimulation |
| `gpu_ram_writes` | `{efb_copies, efb_copies_deferred, efb_copy_flushes, xfb_copies, fills, bytes, readback_us_total, readback_us_max}`: what the video thread wrote to guest RAM, and how long EFB copy readbacks waited for the host GPU |
| `outputs_per_frame`, `outputs_per_frame_after_resim` | histograms (index 0-7, the last bucket is 7 or more): frames sent to the presenter per displayed frame. With the fix every displayed frame is in bucket 1 |

### Rollback debugging commands

- **`rollback_pad_history`**, args `since` (GekkoNet frame, default 0): `{"frames": [[frame, buttons_p0, buttons_p1, buttons_p2, buttons_p3, crc32, gekko_checksum, legacy_checksum, frame_counter, fighters_checksum, logic_steps], ...]}` for the last 2048 GekkoNet frames, oldest first. Per frame: the raw `gfPadStatus` slots (`0x805BAD00 + 0x40 * port`) the game actually used (the first u32 of each port, and a CRC32 over all four 0x40-byte slots). A resimulation overwrites the frame's entry, so once a frame is confirmed both peers must report identical rows. The last five columns are written when the frame's snapshot is taken: the GekkoNet checksum, Brawlback's old checksum, the `g_GameFrame` persistent frame counter, the fighters checksum (per port: active instance, damage, stocks, X/Y, status kind) and the number of game-logic steps (`gameProc` calls) the iteration ran. The checksums are 0 outside `scMelee`. Errors with "no netplay session" outside netplay.
- **`rollback_chunk_hashes`**, args `frame`: `{"frame", "chunk_size", "hashes": [16 hex chars, ...]}`, an XXH3 hash of each `chunk_size` bytes of MEM1 then MEM2 at the end of that GekkoNet frame (last simulation wins), kept for the last 256 frames. `chunk_size` is the size actually used. Only recorded with `PPR_ROLLBACK_CHUNK_HASHES` or in a sync test; errors otherwise. Costs a few ms per frame. Used to find where two peers diverged.
- **`rollback_timings`**, args `since`: per snapshot, the wall-clock save time, the time spent waiting for the GPU thread (the old drain, or with the split video state only a wait for a capture a whole ring behind), the synchronous pipeline compiles meanwhile, the eviction wait, the non-RAM DoState and the RAM-copy time. `next` is the index to pass next time; `first` the index of the first returned sample.
- **`rollback_pad_history`** also takes `raw: true`: each row then ends with the 256 raw bytes (hex) of the four `gfPadStatus` slots.
- **`cpu_state`**: `{"pc", "npc", "msr", "lr", "ctr", "srr0", "srr1", "sp", "exceptions", "pi_cause", "pi_mask", "vi_display_interrupts": [4], "current_thread"}`, read under a `CPUThreadGuard`. A hung game shows whether it spins with interrupts off (`msr` bit 0x8000 clear while `exceptions` has pending bits) or idles (`current_thread` 0).

### Environment switches

| variable | effect |
|---|---|
| `PPR_SYNCTEST=N` (N = 1 to the rollback window) | A host that starts rollback netplay alone runs a GekkoNet stress session: every frame, the state from N frames back is loaded and resimulated with the same inputs (GGPO's SyncTest). `rollback.synctest` is true and `desyncs_detected` counts checksum mismatches between a frame and its re-runs. Memory hashing is on: a re-run whose MEM1/MEM2 differs increments `synctest_mismatches` and logs `SYNCTEST: frame F ...` lines with the differing chunks and words. N = 1 only reloads the latest snapshot; use N >= 2. Holding L on port 0 while booting reaches P+'s Training mode (one player plus a CPU), a single-instance match. |
| `PPR_ROLLBACK_CHUNK_HASHES=1` or `=<bytes>` | record `rollback_chunk_hashes` (64 KiB chunks, or the given size, at least 64) |
| `PPR_ROLLBACK_SYNC_VIDEO=1` | dual core: drain the GPU thread for every snapshot and serialize the whole video state there, as before `5defc02845` (A/B runs) |
| `PPR_LOG_GPU_RAM_WRITES=<file>` | append one line per guest-RAM write by the video thread (Brawl match frame, kind, address, size) |
| `PPR_ROLLBACK_PRESENT_RESIM=1` | present resimulated frames again (the behaviour before `2025577415`), for before/after measurements |
| `PPR_HARNESS_PORT`, `PPR_HARNESS_AUDIO=1`, `PPR_HARNESS_POLL_SOURCE=si` | see above and "Deviations" |

## Online play

The online client (`Source/Core/Core/Online/`) logs in from `user.json` and finds a Direct opponent through our matchmaking server (`server/crates/mm`), as Slippi's Dolphin does with Slippi's. The game's own online menus drive the same calls (`Online::Client`) through the PPOM mailbox (`Online/GameBridge.cpp`, docs/backend-design.md 5.2); these commands drive them directly.

**Configuration** (`[Online]` in Dolphin.ini, or `-C Dolphin.Online.<Key>=<Value>`):

| key | default | meaning |
|---|---|---|
| `MatchmakingHost`, `MatchmakingPort` | `mm.brawlonline.net`, `43113` | the mm server |
| `UseDevServer` | `False` | Slippi's dev host: matchmaking on `127.0.0.1:MatchmakingPort`, accounts at `DevAccountsUrl` |
| `AccountsUrl`, `DevAccountsUrl` | `https://brawlonline.net`, `http://127.0.0.1:8080` | for the users-rest lookup after a login |
| `ForceNetplayPort`, `NetplayPort` | `False`, `2626` | Slippi's "Force Netplay Port"; otherwise a random port in 41000-50999 |
| `ForceLanIP`, `LanIP` | `False`, `""` | Slippi's "Force LAN IP" |

**`online_status`.** Dolphin polls for `<User>/Online/user.json` every 500 ms until it parses (`watching` is true meanwhile), then stops, as Slippi does: a file written after the login is not reread. After reading the file it fetches `GET <accounts>/user/{uid}?additionalFields=chatMessages,rank` and takes the display name, code and latest version from there (`user_fetch`: `not_fetched`, `fetching`, `fetched`, `error`; on error the file's values stay). `app_state` is Slippi's: 0 logged out, 1 logged in, 2 `latestVersion` is newer than `app_version`.

**`mm_status.state`** is Slippi's `ProcessState`; `state_code` is its value, which is what the game will read:

| state | code | meaning |
|---|---|---|
| `idle` | 0 | nothing (after `mm_cancel`) |
| `initializing` | 1 | binding the port, connecting to mm (20 × 500 ms), `create-ticket`, waiting 5 s for `create-ticket-resp` |
| `matchmaking` | 2 | waiting for `get-ticket-resp` (no client-side limit; the server expires tickets) |
| `opponent_connecting` | 3 | the 8 s P2P connect window. On failure Direct goes back to `initializing` with a new ticket (`tickets`, `connect_attempts` count them) |
| `connection_success` | 4 | connected; `handoff` says what happened next |
| `error` | 5 | `error` is the text the game shows: the server's own message, or Slippi's client messages ("Failed to connect to mm server", "Failed to join mm queue", "Lost connection to the mm server", ...) |

`error_source`: `create_ticket` (the server refused the ticket: bad play key, unsupported mode, own code, out of date, ...), `get_ticket` (the server ended the search: expired, replaced, too many failed connects), `connection` (lost mm while waiting; teams P2P failure), `client` (local or protocol failure).

`match` (from `opponent_connecting` on): `match_id`, `is_host` (Slippi's decider), `local_player_index` (0-based), `local_port` (the punched port), `players` (`uid`, `display_name`, `connect_code`, `port` 1-4, `is_local`, `is_bot`, `ip_address` as the server saw it, `ip_address_lan`), `stages`, `items`, `remote_addresses` (chosen with Slippi's rule: the LAN address when both share an external IP), `connected_addresses` (the source address the P2P connection came up with) and `connect_ms`.

**Hand-off.** With `session: "auto"` the connected match goes to the registered session backend (`Online::Session`, `OnlineSession.h`): by default the gameplay-only session (`backend: "gameplay"`, `Gprb::Session`; the games stay on their online CSS and start the match from there, `docs/gameplay-rollback-status.md` Phase 7; its state is in `gprb_status`). With `backend: "netplay"` it is whole-machine netplay, run without any netplay window: the decider hosts a rollback netplay session on its punched port, the other joins from its punched port, and the host boots `game` (default `<User>/Launcher/Project+ Netplay Launcher.dol`) once the guest is in (`auto_start`, default true; `delay` is the rollback input delay). A running game is stopped first. `handoff`: `started`, `failed` (`handoff_error`), or `kept` (no backend, or `session: "none"`: the P2P link stays open until `mm_cancel`). `session`: `{"backend": "netplay", "phase", "error", "detail": {"role", "local_port", "peer", "match_id", "game"}}`; `phase` goes `starting` → (`stopping_game`) → `hosting`/`joining` → `waiting_for_peer` → `started`/`joined` → `running`, or `error`, and `ended` after `mm_cancel`. The netplay session itself is then visible in `netplay_status`.

**Peer timeout.** A rollback session drops a peer that has been silent for Slippi's in-match limit, about 7.2 s at delay 2 (`Online::PeerSilenceTimeoutMs`: the 7-frame window plus 421 halted frames), instead of GekkoNet's 5 s; dolphin.log says `GekkoNet: disconnect timeout 7191 ms`. The game then stops on both sides (`peer_disconnects` = 1), with an OSD `DISCONNECTED` until the game shows it itself (docs/backend-design.md 5.6).

**The game's mailbox.** `game_bridge_status` shows whether Dolphin found the plugin's PPOM block (through the `OSModuleInfo` list, module id 20560), what the game asked last and what it was told (`last_request`/`last_response`, the same text as the `GameBridge:` lines in dolphin.log; `GET_MATCH_STATE` polls are logged only when the answer changes). `netplay_paused` is true while a netplay session runs: the mailbox is not serviced then. A search started from the game hands off to the session backend like `session: "auto"` (`game_bridge_config hand_off=false` keeps the link instead).

**Recent connect codes.** A Direct or Teams FIND_OPPONENT from the game adds the code to the logged-in account's history, as Slippi's `startFindMatch` does; the keypad asks for suggestions with `FETCH_CODE_SUGGESTION` (0xBE, layout in `docs/game-code.md` section 5). Slippi keeps `direct-codes.json` / `teams-codes.json` (`[{"connectCode", "lastPlayed"}]`, newest first, no limit) in its user folder next to `user.json`, shared by every account on that install (`slippi-rust-extensions` `user/src/lib.rs`, `direct_codes/mod.rs`); ours are the same files and format, one set per account: `<User>/Online/<uid>/`. `online_recent_codes` lists, seeds and queries them.

**`online_session_backend`.** Backends are named factories (`Online::Session::RegisterFactory`); the one in use at start is `[Online] SessionBackend` (default `gameplay`, registered by both frontends at start-up; `netplay` is the fallback). `netplay` exists once a frontend gave the headless netplay session its boot callback (`Online/NetPlaySession.h`): DolphinQt always does (its main window boots the game, and no NetPlay window opens), DolphinNoGUI under the harness. `record` is for tests of the game's menus: `Start` records the match (`session.phase` `held`, `detail`: `starts`, `role`, `local_port`, `match_id`, `peer_name`, `peer_code`, `selections`, `link_open`) and holds the P2P link, so the game keeps running on its character select instead of being stopped for the netplay boot; dolphin.log says `Online session (record): Start match <id>`. `none` unregisters (hand-off `kept`).

**Tests.** `harness/tests/test_online_game.py` drives two instances through the game's own menus (main menu → PLAY ONLINE → WITH FRIENDS → START → code on the keypad → OK) with the plugin on each instance's SD card, and checks the mailbox in game memory, `mm_status`, the mm server's log and screenshots; it also covers Unranked's server error, a two-game Direct set under the gameplay session (`harness/tools/online_set.py`), the opponent leaving in a match, and the hand-off to whole-machine netplay. `harness/tests/test_online.py` (markers `dolphin` and `server`) runs Postgres, `accounts` and `mm` locally (`ppharness/backend.py`), creates accounts over HTTP and the admin CLI, writes each instance's `Online/user.json` and drives these commands. The variant through netsim puts each client's *matchmaking* traffic through a proxy; the P2P traffic cannot go through it, because the server tells each client the other's real address (the proxy would need to be a NAT both clients route through).

## Deviations

Where the implementation differs from, or goes beyond, the contract above. Extra result fields are additive; clients can ignore them.

- **`input_polls` counts the game's own pad reads for Brawl/P+, not SI polls.** The SI polls the pads twice per field in Brawl (about 120 Hz), so counting SI polls would make one `pad_script` entry last half a game frame, and the game might never see it. Once Brawl's per-frame pad read starts running (`gfPadSystem::updateLow` -> `updateLowGC`, which this fork already hooks for Brawlback, early in boot during `scStrap`), `input_polls` counts those reads. It counts one per game frame, skips rollback resimulation passes, and doesn't advance while the game loop stalls. Before that point, and for any game whose ID doesn't start with `RSB`, it counts SI polls. `PPR_HARNESS_POLL_SOURCE=si` forces SI counting. `status` adds `poll_source` (`"si"`/`"game"`/`"unknown"`) and `si_polls`. Verified: a 3-poll press is visible to the game on exactly 3 consecutive frames.
- **`pad_script` `start: "next"`** resolves to the first pad read whose SI data has not been latched yet. That's usually `input_polls + 1` in Brawl, because the SI already latched data for the upcoming read. The returned `starts_at` is authoritative. An absolute `start` earlier than that is rejected with an error naming the earliest valid value.
- **Ports are local controller ports**, i.e. the `port` a local `GCPadN` would have. In netplay, the netplay pad map decides which in-game port a local port drives. With the default mapping, the host's local port 0 is in-game port 0, and the joiner's local port 0 is in-game port 1 (P2). `netplay_status` reports `pad_map` (in-game port -> pid), `local_pid` and `local_port_to_ingame_port`. During netplay the "no SI device" check is skipped, because the local configuration doesn't decide the SI devices then. Ports configured as "GameCube Adapter for Wii U" (`SIDevice = 12`) are also overridden. With no physical adapter, an overridden port counts as connected, but the game only notices the "plugged in" controller on its next re-probe. That took up to about 2 s in testing, so use Standard Controller ports (`SIDevice = 6`) for tests that start pressing buttons right away.
- **`log_mark`** writes through Dolphin's logger under the new `HARNESS` log type, which is force-enabled for harness sessions. The line therefore reads `hh:mm:ss:ms Core\Harness\HarnessServer.cpp:NNN N[HARNESS]: [HARNESS] <text>`. It contains the literal `[HARNESS] <text>`.
- **`frame_advance`** steps by VI fields. It sets a break at the target frame counter and resumes; it does not use Dolphin's own frame-step, which waits for a newly presented frame. It errors if the emulation isn't paused.
- **`screenshot`** returns an error while paused, because no new frames are presented then; `resume` first. With D3D at internal resolution 3x, a screenshot is the full internal-resolution frame (e.g. 2484x1440).
- **`save_state`** returns after the file is fully written. **`load_state`** is refused during netplay (Dolphin forbids it) and when the file doesn't exist.
- **`read_mem`/`write_mem`/`read_u32`/`hash_mem`** also accept the uncached mirrors `0xC0000000`/`0xD0000000`, and addresses given as `"0x..."` strings. They run under `Core::CPUThreadGuard`, so the CPU is paused briefly and all ranges in one `hash_mem` come from the same instant (not necessarily a frame boundary). `write_mem` invalidates the JIT cache for the range. In non-rollback netplay, a call can block for as long as the CPU thread is blocked waiting for remote input.
- **`wait_frame`** requires exactly one of `frame` and `input_polls`.
- **`status`**: `game_id` is `""` and `frame`/`input_polls` are 0 when nothing is running. The counters reset on every boot, including netplay boots. `cpu_thread` is whether the core runs in dual-core mode. The result also has `poll_source`, `si_polls` and `audio_muted`.
- **Second connection**: the refusal line has `"id": null`.
- **Netplay without a netplay UI.** The `netplay_*` commands drive the headless session in `Core/Online/NetPlaySession.cpp` (shared with online play's whole-machine backend). DolphinNoGUI provides it under the harness; `Dolphin.exe` provides it from its main window, so the commands work there too once the main window exists (a client that connects within the first second may get "this Dolphin frontend has no headless netplay"; retry) (refused while the Qt NetPlay window has a session of its own). Netplay runs with a direct connection only: no traversal, no UPnP.
  - `netplay_host`: `rollback: true` does what the Qt "Rollback [WIP]" network mode does: `NetPlay.NetworkMode = rollback` for this session, and host input authority off. Otherwise the mode is `fixeddelay`. In rollback mode `delay` is the rollback input delay (see "Rollback input delay"); otherwise it sets the fixed-delay pad buffer. `game` paths may use `\` or `/`. They are normalized to `/`, because the `.dol` game ID (`ID-<file name>`) is part of the SyncIdentifier.
  - `netplay_join` accepts an optional `game` path. Without it, the joiner looks for the host's game among `<User>/Launcher/*.dol|elf`, the configured ISO folders (non-recursive) and `DefaultISO`, as the Qt game list would.
  - `netplay_start` returns an error (retry) until every player has reported that they have the game.
  - `netplay_status` adds `game`, `last_error`, `local_pid`, `pad_map` and `local_port_to_ingame_port`. Its `rollback` object is described under "Rollback counters". Outside a session, `netplay_status` returns `role: null`, `connected: false`, empty `players`, `rollback: null`.
- **`rollback_pad_history`, `rollback_chunk_hashes`, `cpu_state`**: extra commands, see "Rollback debugging commands".
- **Process lifetime (NoGUI)**: with the harness enabled, `DolphinNoGUI` may be started without a game, and it doesn't exit when emulation stops (netplay stops and restarts games). It exits on `quit`, or on SIGINT/SIGTERM as before. Without the harness, behaviour is unchanged.
- **Qt**: the P+ startup update check (a modal "Update Available" dialog) is gone from the fork altogether (`64bfbdb681`); `MainWindow::CheckForUpdatesAuto` is a stub for our own update channel and is still skipped under the harness. If the harness port can't be bound, `Dolphin.exe` prints an error and exits with code 1 instead of showing a dialog. `quit` stops emulation and exits the Qt event loop with code 0.

## Proposed changes

Raised by the Python side (`harness/`) while building the client, the mock server and `compare_state` against v1, and by the verification agent (12-14). Items marked **Resolved** are done (1, 12, 13, 14); the rest are not implemented on the server yet. The Python side already copes with v1 as it is; each item says what it works around today.

1. **Resolved (the table now says so).** **`pad_script` replaces; say so.** The table says "Queued", but the server replaces the port's current script, and `ends_at` is exclusive (`ends_at = starts_at + total holds`; `input_polls == ends_at` means the script has finished). Proposal: change the table to "replaces any script on that port; `ends_at` is exclusive". The mock server already behaves like this.
2. **`hash_mem` has no length limit; say so.** Only `read_mem`/`write_mem` are limited to 16 MiB. The client enforces 16 MiB only for those. Proposal: document it, and list the uncached mirrors `0xC0000000`/`0xD0000000` in the address table.
3. **Stop exactly at a frame: `wait_frame` gets `pause: true`.** Comparing state across instances needs every instance stopped at the same frame. Today the client waits until a few frames before the target, sends `pause`, then `frame_advance`s one field at a time while it reads the key. That costs a round trip per frame, and it fails (and retries) if `pause` lands late. Proposal: `wait_frame {..., "pause": true}` breaks the CPU when the target is reached (the same mechanism `frame_advance` uses), so the stop is atomic. A `game_frame` target (GekkoNet frame) would make it work for netplay too.
4. **Confirmed-frame state hashes for rollback.** Pausing two rollback peers at the same GekkoNet frame can compare *speculative* state: the peer that is ahead has simulated that frame with predicted inputs, which a later rollback may still fix. A mismatch there is not proof of a desync (seen in practice: whole-MEM2 hashes at the same `current_frame` sometimes match and sometimes don't under the `good` preset). Proposal: a `state_hashes` command. The client configures ranges once (`{"ranges": [...], "keep": 600}`). The server then hashes them every time a frame becomes *confirmed* (all inputs known, no further rollback possible) and keeps a ring buffer of `{frame, xxh3_64}`. Comparing two instances is then exact and needs no pausing. This is what Slippi-style desync detection does.
5. **Expose the confirmed frame.** `netplay_status.rollback.current_frame` is useful as a frame key, but it doesn't say whether it is the local (possibly predicted) frame or the last confirmed one. Proposal: add `confirmed_frame` and document `current_frame` as the local frame.
6. **Machine-readable error codes.** Clients have to match English text today: `"timeout"`, `"not all players have the game (yet)..."` (the session helper retries `netplay_start` on that string), and `"another harness client is already connected"`. Proposal: add an optional `"code"` to error responses, e.g. `timeout`, `not_ready`, `busy`, `not_running`, `bad_args`, `not_paused`, and keep `error` as the human message.
7. **Per-player "has game" in `netplay_status`.** Then a client can wait for readiness instead of polling `netplay_start` for errors. Proposal: `players[].has_game: bool`.
8. **Deliver the refusal line reliably.** When the server writes the refusal and then closes immediately while the client's first request is still unread, Windows sends a TCP reset and the client can lose the refusal line. The client now checks for an unsolicited line before it sends anything, which covers the common case. Proposal for the server: `shutdown(SD_SEND)`, drain briefly, then close. The mock server does this.
9. **Detect reboots.** `frame`/`input_polls` reset on every boot, including netplay boots. A `wait_frame` sent just before a reboot can wait for the wrong boot's counter. Proposal: a `boot_id` (incremented per boot) in `status` and in the `wait_frame` result.
10. **Pause during netplay.** It works in practice (the rollback session survives a short pause on both peers), but the contract doesn't say so. Proposal: document that `pause`/`frame_advance` are allowed during netplay, and whether a long pause can make the peer time out.
11. **`netplay_join` completion.** The server returns after the ENet connection is established. Proposal: state that in the table, so clients don't poll `connected` after a successful `netplay_join`.
12. **Resolved in `8408ee9506`:** until a replacing script starts, the port keeps presenting what it presented when the script was submitted and stays overridden; a port that was never overridden still stays with the real controller. Checked offline on the CSS: X held by a long script, replaced by a 3-poll Y press 20 polls later: the game read 0 for those 20 polls before the fix, X after, then Y for exactly 3 polls. The Python workaround (a neutral `pad_set` first) is no longer needed but harmless. Original report: **A future-start `pad_script` drops the port to the real controller.** When `pad_script` replaces a running script and the new one starts in the future (`start: "next"` is usually the poll after next), `RecomputePort` sets `overriding = state.neutral` for the gap. A port that had a script but no `pad_set` is then *not* overridden for those polls, so the game reads the real controller, which the test template leaves unbound: Brawl sees the pad unplugged and drops that player from the CSS (seen live while recording inputs with pauses between submissions). The comment above that line says the opposite ("keeps whatever the port did before"). Proposal: during the gap keep presenting the previous script's last state (or neutral) and keep overriding. The Python side works around it by sending a persistent neutral `pad_set` on every driven port first.
13. **Resolved in `a1f9ec2685` (a Dolphin bug, not the harness):** in deterministic dual core, `Fifo::SyncGPU` assumed the GPU thread had executed everything the CPU preprocessed. While the emulator is paused the GPU loop does nothing, so a sync then (e.g. `PauseAndLock` from a `CPUThreadGuard` during a pause) dropped the unexecuted commands, and the GPU thread later resumed mid-stream. A debugger attached during a crashing run caught an access violation on the GPU thread in `VertexShaderManager::SetConstants` after `LoadIndexedXF`. `determinism.py compare --profile dc-rtc`: 2 of 5 runs crashed before the fix, 0 of 18 after; `pause`/`frame_advance` are safe in dual core now. Original report: **`pause`/`frame_advance` crash DolphinNoGUI in dual core.** The determinism tool pauses at a VI field boundary for every input change (hundreds of `pause` + `frame_advance` per run). In single core that ran clean many times. With `CPUThread = True` the process exited silently (no stderr, the log just stops) in 5 of 11 runs, once on the CSS and four times in a match, after a few hundred pauses; single core never crashed in 8 runs (`docs/determinism-findings.md`). Proposal: investigate the break-at-frame path with a separate CPU thread; until then tools should pause rarely in dual core.
14. **Resolved in `0e49c345e2`:** with `EnableCustomRTC` (and no movie or netplay), the RTC is now the custom value plus *emulated* seconds, like netplay and movies. Note: on the current build the old divergence did not reproduce in our check (six offline boots of the pre-fix binary, two of them at 25 % emulation speed so 15 s more host time passed, had identical `g_mtRand`/`g_mtRandOther` at field 400), so the fix is by construction rather than demonstrated by a before/after difference. Original report: **Offline "custom RTC" is not fixed.** `EnableCustomRTC` makes the RTC `CustomRTCValue + host seconds since SystemTimers::Init` (`EXI_DeviceIPL::GetEmulatedTime`, else branch), unlike netplay and movies, which add emulated ticks. Two offline boots therefore read RTC values one second apart whenever their boots straddle a second boundary differently, and Brawl's RNG seeds (g_mtRand, g_mtRandOther) and `__OSStartTime` differ from the strap screen on. This is in Dolphin's core, not the harness, but it is what makes offline determinism tests flaky. Proposal: when the custom RTC is enabled (or a harness session asks for determinism), use `custom + ticks / ticks_per_second` like netplay.
