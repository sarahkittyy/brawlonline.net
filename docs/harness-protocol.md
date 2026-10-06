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
| `status` | — | `{"state": "uninitialized"\|"starting"\|"running"\|"paused"\|"stopping", "frame": n, "input_polls": n, "game_id": str, "cpu_thread": bool, "video_backend": str, "netplay": null \| {...see netplay_status}}` |
| `read_mem` | `addr`, `len` (≤ 16 MiB) | `{"hex": "..."}`. Effective (virtual) addresses `0x80000000–0x817FFFFF` (MEM1) and `0x90000000–0x93FFFFFF` (MEM2) |
| `write_mem` | `addr`, `hex` | `{}` |
| `read_u32` | `addr` | `{"value": n}` (big-endian, as the game sees it) |
| `hash_mem` | `ranges`: `[[addr, len], ...]` | `{"xxh3_64": "16 hex chars"}`, one hash over all ranges concatenated in order |
| `pad_set` | `port` 0–3, `buttons`: list of `A B X Y Z L R START DUP DDOWN DLEFT DRIGHT`, `main`: `[x,y]` 0–255 (default 128,128), `c`: `[x,y]`, `l`: 0–255, `r`: 0–255 | `{}`. Persistent override of that port until `pad_clear` |
| `pad_clear` | `port` | `{}`. Hands the port back to the real or configured controller |
| `pad_script` | `port`, `frames`: `[{buttons, main, c, l, r, hold}, ...]` (`hold` = number of polls, default 1), `start`: `"next"` or an absolute `input_polls` value | `{"starts_at": n, "ends_at": n}`. Queued and played poll by poll; afterwards the port returns to its `pad_set` state, or to neutral (released, not cleared) if there is none |
| `pad_script_status` | `port` | `{"active": bool, "remaining": n}` |
| `wait_frame` | `frame` **or** `input_polls`, `timeout_ms` (default 10000) | `{"frame": n, "input_polls": n}` once reached; error `"timeout"` otherwise |
| `pause` / `resume` | — | `{}` |
| `frame_advance` | `n` (default 1) | `{"frame": n}` after advancing while paused |
| `screenshot` | `path` (absolute `.png`) | `{"path": ..., "width": w, "height": h}`. Error if the video backend is `Null`. Returns only after the file is written |
| `save_state` / `load_state` | `path` | `{}` (Dolphin savestates, for test setup only, never for rollback) |
| `netplay_host` | `port`, `game`: path to the `.dol`/`.iso` to boot, `name`, `rollback`: bool, `delay`: int (optional) | `{}`. Hosts and joins its own server as player 1 (headless; no Qt dialog) |
| `netplay_join` | `host`, `port`, `name` | `{}` |
| `netplay_start` | — | `{}` (host only; boots the game on every client) |
| `netplay_status` | — | `{"role": "host"\|"client"\|null, "connected": bool, "players": [{"pid", "name", "ping_ms"}], "game_running": bool, "rollback": {...counters if available: rollbacks, max_rollback_frames, desyncs_detected, frames_resimulated}}` |
| `netplay_leave` | — | `{}` |
| `log_mark` | `text` | `{}`. Writes `[HARNESS] <text>` into dolphin.log, used to correlate log lines with test steps |
| `quit` | — | `{}`, then a clean shutdown (flushes logs and exits the process) |

## Notes

- Pad injection happens at the emulated controller source: the GameCube pad status that the SI device reads. Netplay and rollback therefore see harness input exactly as if a physical controller produced it.
- `pad_set`/`pad_script` on a port with no SI device return an error. Tests configure the port as a "Standard Controller" in `GCPadNew.ini` or `Dolphin.ini` (`SIDevice0 = 6`).
- `quit` must leave the process with exit code 0 and flushed logs. Killing the process loses buffered log lines.

## Defaults

- **Audio is muted for harness sessions.** Whenever the harness is enabled (`--harness-port` or `PPR_HARNESS_PORT`), Dolphin forces `MAIN_AUDIO_MUTED` (`[DSP] Muted`) to true for that session only (current-run config layer; nothing is written to `Dolphin.ini`). The audio pipeline still runs, only the output is silent. Set `PPR_HARNESS_AUDIO=1` to keep the configured audio setting. `status` reports the effective value as `audio_muted`. (Temporary; we'll revert this later.)
- Test user directories should also have `[Input] BackgroundInput = False` (the template does), so the real keyboard/controller don't leak into test instances. Harness pad overrides are not affected by the input gate.

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
- **Netplay is NoGUI only.** In `Dolphin.exe` the `netplay_*` commands return an error; use the Qt NetPlay dialog there. Netplay runs with a direct connection only: no traversal, no UPnP.
  - `netplay_host`: `rollback: true` does what the Qt "Rollback [WIP]" network mode does: `NetPlay.NetworkMode = rollback` for this session, and host input authority off. Otherwise the mode is `fixeddelay`. `delay` sets the fixed-delay pad buffer. GekkoNet's input delay is hard-coded to 2 frames in this fork (`InitGekkoSession`), so `delay` has no effect on rollback sessions. `game` paths may use `\` or `/`. They are normalized to `/`, because the `.dol` game ID (`ID-<file name>`) is part of the SyncIdentifier.
  - `netplay_join` accepts an optional `game` path. Without it, the joiner looks for the host's game among `<User>/Launcher/*.dol|elf`, the configured ISO folders (non-recursive) and `DefaultISO`, as the Qt game list would.
  - `netplay_start` returns an error (retry) until every player has reported that they have the game.
  - `netplay_status` adds `game`, `last_error`, `local_pid`, `pad_map` and `local_port_to_ingame_port`. `rollback` holds: `enabled`, `gekko_session`, `session_started`, `current_frame` (GekkoNet frame), `rollbacks`, `max_rollback_frames`, `frames_resimulated`, `desyncs_detected`, `last_desync_frame`, `frames_ahead`, and `netplay_desync_reports` (Dolphin's own desync messages). Brawlback didn't track these counters before; they were added to `NetPlayClient`. Outside a session, `netplay_status` returns `role: null`, `connected: false`, empty `players`, `rollback: null`.
  - `rollback_pad_history` (extra command), args `since` (GekkoNet frame, default 0): `{"frames": [[frame, buttons_p0, buttons_p1, buttons_p2, buttons_p3, crc32], ...]}` for the last 2048 GekkoNet frames, oldest first. Per frame it holds the raw `gfPadStatus` slots (`0x805BAD00 + 0x40 * port`) the game actually used: the first u32 of each port, and a CRC32 over all four 0x40-byte slots. A resimulation overwrites the frame's entry, so once a frame is confirmed both peers must report identical rows. Errors with "no netplay session" outside netplay.
- **Process lifetime (NoGUI)**: with the harness enabled, `DolphinNoGUI` may be started without a game, and it doesn't exit when emulation stops (netplay stops and restarts games). It exits on `quit`, or on SIGINT/SIGTERM as before. Without the harness, behaviour is unchanged.
- **Qt**: under the harness, the P+ startup update check (a modal "Update Available" dialog) is skipped. If the harness port can't be bound, `Dolphin.exe` prints an error and exits with code 1 instead of showing a dialog. `quit` stops emulation and exits the Qt event loop with code 0.

## Proposed changes

Raised by the Python side (`harness/`) while building the client, the mock server and `compare_state` against v1. Nothing below is implemented on the server yet. The Python side already copes with v1 as it is; each item says what it works around today.

1. **`pad_script` replaces; say so.** The table says "Queued", but the server replaces the port's current script, and `ends_at` is exclusive (`ends_at = starts_at + total holds`; `input_polls == ends_at` means the script has finished). Proposal: change the table to "replaces any script on that port; `ends_at` is exclusive". The mock server already behaves like this.
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
12. **A future-start `pad_script` drops the port to the real controller.** When `pad_script` replaces a running script and the new one starts in the future (`start: "next"` is usually the poll after next), `RecomputePort` sets `overriding = state.neutral` for the gap. A port that had a script but no `pad_set` is then *not* overridden for those polls, so the game reads the real controller, which the test template leaves unbound: Brawl sees the pad unplugged and drops that player from the CSS (seen live while recording inputs with pauses between submissions). The comment above that line says the opposite ("keeps whatever the port did before"). Proposal: during the gap keep presenting the previous script's last state (or neutral) and keep overriding. The Python side works around it by sending a persistent neutral `pad_set` on every driven port first.
13. **`pause`/`frame_advance` crash DolphinNoGUI in dual core.** The determinism tool pauses at a VI field boundary for every input change (hundreds of `pause` + `frame_advance` per run). In single core that ran clean many times. With `CPUThread = True` the process exited silently (no stderr, the log just stops) in 5 of 11 runs, once on the CSS and four times in a match, after a few hundred pauses; single core never crashed in 8 runs (`docs/determinism-findings.md`). Proposal: investigate the break-at-frame path with a separate CPU thread; until then tools should pause rarely in dual core.
14. **Offline "custom RTC" is not fixed.** `EnableCustomRTC` makes the RTC `CustomRTCValue + host seconds since SystemTimers::Init` (`EXI_DeviceIPL::GetEmulatedTime`, else branch), unlike netplay and movies, which add emulated ticks. Two offline boots therefore read RTC values one second apart whenever their boots straddle a second boundary differently, and Brawl's RNG seeds (g_mtRand, g_mtRandOther) and `__OSStartTime` differ from the strap screen on. This is in Dolphin's core, not the harness, but it is what makes offline determinism tests flaky. Proposal: when the custom RTC is enabled (or a harness session asks for determinism), use `custom + ticks / ticks_per_second` like netplay.
