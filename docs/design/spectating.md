# Spectating

Status (2026-10-14): uploading, the relay, the safety tests (§4.5), the Spectate tab and watching live are built; spectator chat in Dolphin is not. mm's `MM_SPECTATE` stays `off` in production until it is turned on.

What was built, and where it differs from the plan below:
- **Upload and relay** (§3.1, §3.2): as planned, except the watch stream is server-sent events from accounts (which relays mm's loopback `GET /live/{stream}`), not a WebSocket to mm, so no new public listener or nginx location. `docs/spectate-protocol.md` is the wire protocol.
- **Launcher** (`launcher/src/spectate/`, `renderer/pages/home/spectate/`): Home > Spectate lists `/v1/live` every 3 s while open, newest set first, with each side's stock icons and names, the mode, "Game 2 · 1-0" and the stage, the current game's time ticking every second, and the eye with the spectator count. New rows slide in and pulse once. The player's own games say "Your game" and can't be watched; the watched one says "Watching". Auto play is a switch on the tab (off, remembered on the computer). Settings > Game has "Close Dolphin after a spectated game ends" (on). Not built: the "Most watched" sort, and "Save to Replays" for a watched set.
- **Watching** (§6.2): the launcher writes each game to `<playback User folder>/Spectate/<stream>/<place>.brep` and drives the playback Dolphin with a `mirror` comm file (`docs/spectate-protocol.md` §4). A spectator joining mid-set starts at the game being played (`/v1/live`'s `place`); the set's earlier games aren't downloaded. Dolphin's mirror mode: waits for a game's first frames; a game watched from its start (every next game of a set too) plays from its first frame at normal speed about 10 s behind the players, and only a game joined more than 20 s in catches up by itself (the user, 2026-10-14, below); catching up shows one frame in 15 with "Catching up to live · 2:14 / 4:05 · about 20 s"; at the live edge it plays 3 s behind (2-4 s by a 2 % speed nudge); "Buffering…" when frames run out, a LIVE / GO LIVE -12 s button and the End hotkey ("Go Live (Spectating)"), fast-forward stopping at the live edge, "The next game starts soon" between games, a set's next game following on, and at the stream's end the close countdown or Auto play.
- **A game whose upload stops** (players first, §4.3: the only uploader paused three times): mm says `bad` at once (2026-10-14; it used to wait for the match's next game), the launcher marks the game `cut`, and playback plays what came ("The rest of this game wasn't streamed.") and goes on with the next game.
- **Auto play** picks the newest game being played, and a game waiting for its set's next game only when none is (2026-10-14). While a picked game's first frames haven't come (the upload's 10 s hold), Dolphin shows "Game found, waiting for data...". Found in the harness, where three Dolphins on one PC made the uploader stop for most games.
- **Windows file sharing:** Dolphin's default open (`_tfopen_s`) refuses a file another program is writing, so a growing game file could only be read once its game had ended. `ReplayFile::ReadFile` now opens shared (`SharedAccess::ReadWrite`).
- Not built yet: spectator chat in Dolphin (§6.4; the server side exists), "Lost sync with this game" reports, the launcher coming to the front when Dolphin closes, and the version check's own message for spectating (a different build gets the replay's "recorded with another version" error, without Play anyway).

## 0. What the user asked for (2026-10-13)

- A **Spectate** tab on Home, next to Overview. It lists every live game the way Public Rooms lists rooms. A click watches one.
- Watching uses the playback Dolphin and has replay controls: pause, play, frame steps, ±5 s, seek, slow and fast. A **Go live** button jumps back to the live game.
- Frames a spectator already received stay cached on their computer. Scrubbing back and forth never asks the server again.
- **Players' games must not be affected in any way.** Under bad performance the replay and upload work is the first thing to throttle or stop. This has to be tested and verified before it ships.
- The playback Dolphin plays at the pinned 16:9, like the players' games, so a screen KO can't desync a replay or a spectated game (§7).

Decided by the user later the same day:
- **"Let others stream my games"** is an opt-out in the launcher's settings, on by default. Its description: "Streams inputs to the server for others to spectate you. Very minimal network load." It decides whether **this player's game uploads**. When one player has it on and another off, the game is streamed from the player who has it on. Only a game where every player has it off is not listed (§3.1, §6.3).
- **Ranked is listed** with no extra delay. **Direct is listed** too. Every mode gets the same short client-side buffer and nothing more.
- A spectator who joins **always catches up to live** on their own, with visible progress (§6.2).
- The list is **newest first** by default and refreshes often while the tab is open. It shows who is playing and how long the game has been going (§6.1).
- Clicking another game while watching switches the open playback Dolphin to it. It catches up again, so a switch is slow, which is accepted.
- Between games of a set, including a room's set, the spectator waits on "Game 2 starts soon" and watches the next game.
- At the end, the final few seconds are held, then the playback Dolphin closes. Launcher setting **"Close Dolphin after a spectated game ends"**, on by default.
- An **Auto play** toggle on the Spectate tab, off by default: when a watched stream ends, it starts spectating the newest live game at once (§6.2). It overrides the Close setting.
- The setting's description adds "Your opponent's game may still stream the match." This may be revisited if players push back.
- **Spectator chat:** a chat window shared by everyone watching a game. **Players in that game never see it.** It may be unencrypted, without the lock icon, if end-to-end encryption doesn't scale (§6.4).

## 1. Summary

Spectating is a live replay. The players' games already record every game's confirmed inputs and checksums (`FRMS` chunks, `replays.md` §4.2). Playback rebuilds the match from those inputs and checks every frame against the recorded checksum. For spectating:

1. One player's game sends those chunks to mm over the online connection it already keeps open. Other players who allow streaming send small digests of the same chunks, so mm can check the stream and switch uploaders if one stops.
2. mm keeps each live game's chunks in memory. It serves each spectator the game from frame 1 and then pushes new chunks as they arrive.
3. The spectator's launcher writes the chunks into a local `.brep`. The playback Dolphin follows that file in `mirror` mode (reserved in `replays.md` §6.2). It plays 3 s behind the newest frame so network jitter never shows as stutter.

The server never simulates, decodes or renders anything. A spectator never touches the players' machines.

## 2. Why inputs, not game state

Measured on 17 real `.brep` files from 2026-10-12/13 (Ranked, rooms, 1v1 and 2v2):

| Part of the file | 1v1 | 2v2 | Needed to watch |
|---|---|---|---|
| `FRMS` (pads XOR-delta + per-frame checksums, deflated) | **420-580 B/s** | **600-670 B/s** | yes |
| `INFO` + `SETP` + `SYNC` + `FTSP` (once per game) | 8-10 KB | 8-10 KB | yes (`FTSP` no) |
| `FTRM` (raw fighter memory, for future stats) | 3.5-6.5 KB/s | 12-14 KB/s | **no** |

An 8-minute 1v1 is about 280 KB of `FRMS`. About half of `FRMS` is per-frame checksums: 4 random bytes per frame that don't compress. A spectator stream could send every 10th checksum instead, at about 250 B/s, but this proposal starts with the file's chunks unchanged so one parser serves both.

The alternative, streaming state, has two problems. A playable state is the rollback region set, which is megabytes (whole-machine keyframes in Orca were 26-30 MB, `backend-design.md` §5.1). Raw fighter memory alone can't drive a Dolphin anyway.

## 3. Data path

### 3.1 Uploading (players' games → mm)

- **What:** the `.brep` chunks the replay writer already produces, minus `FTRM` and `FTSP`: `INFO`, `SETP`, `SYNC`, every `FRMS` (one per 60 confirmed frames), `EVNT`, `ENDG`. Nothing new is encoded. The writer already deflated each chunk.
- **Where:** on a new ENet channel of the **online connection** a logged-in game already keeps to mm (UDP 43113, `rooms-protocol.md`: "A running, logged-in game keeps one connection open"). That means no new socket, no TLS handshake and no new login. The connection is serviced by the `Rooms` thread (`Online/Rooms.cpp:1146`), never by the CPU thread.
- **Who:** only players whose "Let others stream my games" is on take part. Their game says so in its ticket. A player with it off sends nothing at all: no chunks and no digests. From those who have it on, mm picks one **primary** per game at the barrier: the player with the most headroom (§4.3), the host on a tie. Every other player with it on is a **witness**. In a 1v1 where only one player has it on, there is no witness, so no takeover and no cross-check. That stream is trusted as sent, and the game ends for spectators if its only uploader stops. A witness sends a 12-byte digest per chunk (`first_frame` plus a hash of the uncompressed pads and checksums), batched every 5 s, about 0.1 kbit/s. It keeps its own copy of the chunks in memory (≤ 0.5 MB per game, kept even with `SaveReplays` off).
- **Takeover:** mm sends `spectate-upload {from_frame}` to a witness when the primary goes quiet for 5 s or says it paused (§4.3). The witness sends from that frame through the same pacer. Spectators show "Buffering…" while the gap fills.
- **Validation:** if a digest differs from the primary's chunk, mm ends the stream ("This game can't be shown") and logs it. One modified client can't show spectators a fake game. Peers' confirmed inputs are byte-identical; the files' `inputsSha256` already matches.
- **Cost on the primary:** a 500-650 B chunk per second plus ENet/UDP headers ≈ **4.5-5.5 kbit/s up**. That is roughly 10 % on top of the game's own traffic (measured 2026-10-13: about 45-50 kbit/s per player, one datagram a frame; before that day's fix ~160 kbit/s from a joiner and ~740 from a host, `gameplay-rollback-status.md` Phase 26). §4 explains why that still can't hurt the game.

### 3.2 mm (relay)

- **Live store:** each live game holds its chunks in memory, plus the match's earlier games, so a set can be scrubbed. A game stays listed while it plays and for 10 min after its set ends. Nothing is written to disk.
- **Limits** (uploads are untrusted): a chunk ≤ 64 KB; a game ≤ 2 MB; an uploader ≤ 4 KB/s; frames contiguous. Headers, CRCs and sizes are checked as `ReplayFile` reads them (`replays.md` §8.1).
- **No server delay** (the user, 2026-10-13: not for Ranked either). mm relays each chunk as soon as it is validated. The only delay is the spectator's own buffer (§6.2), about 4-5 s behind the players in all: up to 1 s for a chunk to fill, the network, and the 3 s buffer.
- **Fan-out** runs in a tokio task, off mm's ENet thread. The ENet thread only hands each validated chunk to a `tokio::sync::broadcast`.
- **List:** `GET /v1/live`, served from a 1 s snapshot like `/v1/rooms`. For each stream it gives:
  - mode, ruleset and stage;
  - players (name, code, character, costume);
  - set score and game number;
  - the set's and the current game's start times (UTC), so the client can show elapsed time;
  - the spectator count.

  A game is listed when at least one of its players streams it (§3.1).
- **Watching:** `GET /v1/live/{stream}/ws` (WebSocket). The client sends `{from: <game>, <frame>}`. mm sends everything from there, then each new chunk as it arrives. On reconnect the client asks from its newest frame, so nothing is sent twice. Each spectator downloads each chunk exactly once.
- **nginx:** `location ^~ /v1/live` proxies to a new public axum listener in mm, with `Upgrade` headers for the WebSocket. A push doesn't deploy the nginx site; it is installed by hand on sarahvps2, so it has to go in before the matching server code ships.

### 3.3 Spectator (launcher + playback Dolphin)

- The **launcher** holds the WebSocket and appends the chunks to `<playback user folder>/Spectate/<stream>/<game>.brep`. The playback Dolphin stays as `replays.md` §6.2 has it, with no login and no sockets. It follows the growing file in `mirror` mode, as Slippi's mirror mode follows a growing `.slp`.
- The file **is** the client cache. Seeking back reads it, and so does the keyframe store (`ReplayKeyframes`, every 5 s, 768 MB cap). The server is never asked again.
- When the set ends, the files are replays the spectator can keep ("Save to Replays"). They have no `FTRM`, so their stats page says stats aren't available, as it already does for a `.brep`.

## 4. Players come first

The rule: **no part of spectating may make a player's game worse, and the test suite has to prove it.** Under any pressure, the order in which things give way is:

1. The **uploader** stops sending. Spectators fall behind, or a witness takes over.
2. The **replay writer** falls behind. Its queue grows and the recording is finished later.
3. Never: the CPU thread, the GPU thread, audio, or the P2P rollback traffic.

### 4.1 CPU

- The CPU thread's work is unchanged. It already copies each final frame into the recorder's queue (`ReplayRecorder.h`: "These only copy small records"). The uploader reads chunks the writer has finished, on the `Rooms` thread. The CPU thread never waits for, locks with, or allocates for the uploader.
- **Thread priorities** (also worth doing now, without spectating): the `Replay writer` thread runs at the lowest priority on every OS: `THREAD_PRIORITY_LOWEST` on Windows, `QOS_CLASS_UTILITY` on macOS, `nice 10` on Linux and Deck. When the machine is short of CPU, the OS runs the game's threads first and the writer waits. Today it runs at normal priority.
- **Queue cap:** with `FTRM` a 1v1 frame is ~4.6 KB raw, so a writer that is starved the whole game hits `MAX_QUEUED` (64 MB, `ReplayRecorder.cpp:38`) after about 4 minutes. Then the recording is marked truncated and the game goes on. That's acceptable, but it has to be measured (§4.4 T2) instead of assumed.
- Upload adds no compression and no new thread: one `enet_peer_send` of ready bytes per second.

### 4.2 Network

- **No new connection:** the online connection already exists and is already the heartbeat.
- **Pacer:** a token bucket at 2 KB/s with a 2 KB burst. The uploader also keeps at most 2 KB unacknowledged in ENet. On a congested uplink it fills almost no router queue. A backlog (after a pause or a takeover) goes through the same pacer and is never burst.
- **Small packets:** each chunk goes out as packets of at most 512 B (header included), at most 2 KB/s, and nothing more while 2 KB are unacknowledged. At 256 kbit/s up, a 512 B packet holds the link for 16 ms. (Built 2026-10-13: the pacer runs on the Rooms thread's 10 ms tick; sending right after the session's own input packets is not done.)
- **DSCP CS1** ("background") on the upload's packets where the OS allows it (Linux, macOS). Home routers mostly ignore it, so nothing relies on it.

### 4.3 Backoff (the uploader stops first)

The primary's uploader reads signals the session already has (`Gprb::Session::GetLinkHealth`: counters and averages the CPU thread publishes with relaxed atomics), every 10 ms, on the `Rooms` thread. **Any** of these pauses it at once (as built 2026-10-13, `Online/Spectate.h` `Options`):

| Signal | Pause when |
|---|---|
| Emulation speed (`frame`) | the displayed frames that ran late (over their schedule) in the last 2 s are more than twice the game's normal for 2 s plus 4 (a hitch is let through) |
| Time sync | not used: the session can't tell a wait for another player's input from its routine pacing polls (about 116 updates a second with no frame to run in normal play, measured 2026-10-13); the ping and the rollbacks show that trouble |
| Rollbacks (`rollback`) | the ~2 s average rollback depth is over the game's own average for its first 10 s + 1 frame |
| P2P latency (`rtt`) | the worst link's average ping is 20 ms over its average in the game's first 10 s (the uplink is queuing) |
| P2P loss | not measured: GekkoNet reports no loss; lost input packets show as stalls and rollbacks |
| Upload queue (`queue`) | more than 8 KB of upload unacknowledged (from 2 KB on, nothing more is sent) |

- **The game's normal** is its own first 10 s of running (built 2026-10-13, after the first harness run): nothing but the queue pauses during them (the start of a match has its hitch at GO), and every rule compares with them. A PC that always shows some late frames is not held to a faster PC's normal: on the shared test PC two Dolphins showed about 13% of frames late for a whole game, and a fixed limit kept the upload paused until the game ended.
- **Resume** after 10 s with no signal (a signal looks back 2 s, so about 12 s after the pause). After the third pause in a game, the primary stops uploading for that game and tells mm, which moves the stream to a witness.
- **Choosing the primary** (among players who stream, §3.1): each game reports its last game's pauses and its average rollbacks at the barrier. mm picks the player with fewer. A player who paused last game is never chosen while another streaming player can upload.
- When **every** streaming player is paused, spectators see "Buffering — the players' connections are busy" and wait. The game never pays for the stream.
- **Kill switches:** mm's `hello` reply carries `spectateUpload: off | witness | on` and the pacer rate. One server config change turns uploading off for everyone at once, without a launcher update, which matters because a launcher update is slow to reach everyone. A player can also turn it off for themselves ("Let others stream my games", §6.3).

### 4.4 Verification before shipping (pass criteria)

These tests are built before the feature (§8 P1). Each runs three ways: **uploader off**, **on**, and **forced**. Forced means backoff disabled, sending at full rate. Forced exists to prove a test can see harm at all. A test where forced looks the same as off is too weak and doesn't count.

| Test | Setup | Pass |
|---|---|---|
| **T1 Shared uplink** | Extend `netsim` so a player's P2P traffic **and** its traffic to the (mock) mm share one uplink queue, modelling a home connection. Uplinks of 24, 32, 48, 64 and 128 kbit/s, with `queue_ms` 250 (bufferbloat), plus the `bad_wifi` profile. Identical scripted sessions (`gprb_session.py`), 10 runs per setting. | "on" equals "off" within noise at every cap: rollbacks/s, max rollback, time-sync stalls, P2P RTT p50/p99, frame time p99, desyncs. "Forced" is measurably worse at the low caps. Also record the cap at which "off" itself fails: that's the cusp. |
| **T2 CPU starvation** | CPU burners as in `gprb_replay_stress.py --burn`, raised step by step until "off" drops below full speed. | "On" equals "off" at every step: emulation speed, frame time p99, rollbacks. Also measure how far the writer's queue grows, and when it truncates. |
| **T3 Misbehaving server** | A mock mm that never answers uploads, accepts but never acknowledges, answers at 100 B/s, disconnects mid-game, or sends malformed `spectate-*` messages. | Game metrics equal "off". An instrumented CPU thread never spends more than 50 µs in recorder or uploader calls in any frame. |
| **T4 Platforms** | T1 and T2 on Windows, the Mac test machine (arm64, QoS classes) and Linux/Deck (WSL repro). | Same as above |
| **T5 Staging** | `tools/staging/staging.py`, a real home network with a throttled uplink (a router cap or `clumsy`), a session of manual play | No felt difference; uploader logs show backoffs firing |
| **T6 Prod rollout** | `spectateUpload: off` by default. Then `witness` for everyone, then `on` for an allowlist. | Allowlisted players' per-game rollbacks, max rollback and desyncs (already in `ENDG` and the game report) don't rise against their own games before. Each game's report adds upload bytes, paused seconds, and pauses by reason. |

### 4.5 Results (2026-10-13)

`harness/tests/test_spectate_safety.py` (results in `run/artifacts/spectate-safety/`). The streamer's game packets and its upload to mm share one capped queue (netsim `SharedUplink`, 250 ms of buffer); each game packet records the upload bytes queued ahead of it (ENet channel 1 only: the online connection's own traffic is not blamed), so the upload's cost is exact whatever the game's own traffic does.

What the runs changed in the uploader:
- **The game's traffic is far more than assumed:** the host sends ~750 kbit/s (its 5.5 KB state message every 50 ms; the bandwidth session's local fix brings it to ~45), the joiner ~150. At 1.25x of it the game alone queues ~50-100 ms at its p99, and at about 1x it overflows by itself.
- **No upload during a game's first 10 s.** An upload starting with the match tipped a link 85% full into a queue that never drained.
- **LEDBAT-style yield:** nothing is sent while the round trip to mm (ENet's smoothed `roundTripTime`) is 25 ms over its lowest of the last 10 minutes, menus included. A lowest taken within the game misses a game that fills the link from its first frame; ENet's `lastRoundTripTime` is a 5 s window's lowest and lags.
- **One 256-byte packet in flight**, counted with what is still queued in ENet (`reliableDataInTransit` alone missed it): two in flight cost 1.7 packets' time at the p99 at 260 kbit/s.
- **Late frames are judged against the game's own first 10 s** (a fixed limit paused all game on a busy PC), and stalls are not a signal (routine pacing polls).
- mm: a new game's offer closes an unfinished one (a stopped upload blocked the rest of the set).

Final T1 run (2026-10-13 23:05, Null video backend; the streamer was joiner, 156 kbit/s; passed):

| Uplink | Shipping upload: cost to the game's packets (p99 / max), drops with upload ahead | Forced (no backoff, 16 KB/s) | Flood (16 KB/s junk) |
|---|---|---|---|
| 187 kbit/s (1.2x) | 0 / 12.6 ms, 0 (it held back nearly all the time) | 20 / 212 ms, 31 drops | 210 ms, 10,818 drops |
| 249 kbit/s (1.6x) | 9.4 / 16.9 ms, 0 (one 256 B packet is 9 ms here) | 40 / 157 ms | |
| 467 kbit/s (3x) | 3.6 / 9.0 ms, 0 | 7.7 / 21 ms | |

Earlier runs (two packets in flight) at 160-2,437 kbit/s: the shipping uploader never caused a drop, cost at most 15 ms at p99, and sent nothing at all on links the game itself overflowed. T3 (mm silent, 2 s late, trickling at 1 kbit/s, for a whole game): the game's packets stayed at their 15 ms base, no drops, the online connection dropped and came back.

T2 (2026-10-14 01:45, Null video; passed): two players and a burner on half of the PC's 16 threads, so both games ran late about half the time. The streamer's late frames per second were 31.8 and 40.2 with streaming off, and 35.3 and 30.4 with it on, so on stays within off's spread. Forced was 29.1, in one short game. The uploader read the starvation as its own signal and backed off: it paused once in each "on" game, and in the second one it sent 1 of its 65 chunks. A spectator of a starved game falls behind; the players don't. No checksum mismatch in any game. The first attempt failed before any game: a harness client could not connect to the second Dolphin ("another harness client is already connected"), a one-off harness race that the rerun did not hit.

## 5. Server capacity (sarahvps2 as it is)

Measured 2026-10-13:

- 6 vCPU (Haswell) and 11.6 GB RAM, 8.8 GB available.
- 17 GB disk free (83 % used).
- Load 0.07. mm is at 3 % CPU and 10 MB RSS. The box also runs a Source game server (`srcds`, 1.5 GB RSS), a TURN server and Docker.
- Outbound over the last 96 days: 201 GB, an average of ~0.2 Mbit/s.
- The NIC reports no link speed (virtual), so **the provider's bandwidth cap is the unknown that matters**. Check the plan before relying on the numbers below.

| Load | Per unit | 1,000 of them |
|---|---|---|
| Live game (ingest from the primary + witness digests) | ~5 kbit/s in; ≤ 0.5 MB RAM including 10 min after the set | 5 Mbit/s in, ~0.5 GB RAM, ~1,000 small ENet packets/s (one core does far more) |
| Live spectator | ~5 kbit/s out; one WebSocket (~50 KB RAM in nginx and mm) | 5 Mbit/s out, ~50 MB RAM |
| Spectator joining mid-game | a one-time burst of the game so far: ~140 KB at 4 min into a 1v1 | 140 MB if all 1,000 join at once (11 s at 100 Mbit/s) |
| Spectator chat at its cap (§6.4) | 5 lines/s per game × ~150 B to every spectator ≈ 6 kbit/s out per spectator | 6 Mbit/s out; at the cap, chat costs about as much as the game stream |

The ballpark: CPU, RAM and disk allow **thousands** of concurrent games, far beyond today's player base. The first limit is outbound bandwidth to spectators, about **5 Mbit/s per 1,000 watching**. A 100 Mbit/s cap is about 15,000 spectators minus what TURN and `srcds` use. The burst to watch for is many people joining one popular game at once. That costs bandwidth, not CPU, and the history chunks are immutable, so nginx could serve them from its cache if it ever matters. Disk only matters if streams are ever kept (Ranked upload); 17 GB holds about 60,000 games' inputs.

## 6. Spectator experience

### 6.1 Launcher: Home > Spectate

- `HOME_TABS` (`pages/home/home_routes.ts`) gets `spectate`; `home_page.tsx:64` gets `{ id: "spectate", label: Messages.spectate() }`.
- The page copies Public Rooms' structure (`public_rooms.tsx`, `public_rooms_view.tsx`). It has the same states (logged out, loading, ready, unreachable) and the same 15 s staleness rule. It polls `/v1/live` **every 3 s, only while the tab is open**. The server answers from its 1 s snapshot, so a new game shows within about 4 s.
- **Rows:**
  - who is playing: each player's stock icon, name and code, with teams shown as rooms show them;
  - **how long the game has been going**: the current game's time, ticking every second in the client from its start time, so it moves between polls;
  - a mode badge (Ranked, Unranked, Direct, Room), the stage, and "Game 2 · 1-0";
  - **how many are watching: an eye icon with the number next to it** (the user, 2026-10-13), from `/v1/live`'s `spectators`, refreshed with the list. The watch view shows the same eye and number, live from the stream's `spectators` events.
- **Ended streams** (the user, 2026-10-14) stay in the list while the server keeps them (10 minutes): a red ENDED replaces the game time, they sort below every live game, and Watch plays the set from its first game. The server ends a stream as soon as its set can't go on (a disconnect, a player leaving, an error, or a Ranked set won; a Ranked quit only loses that game), not 120 s later.
- **Sort:** newest first by default, by the set's start time. A set's next game doesn't move its row, so the list doesn't reshuffle every few minutes. A new row slides in at the top and pulses once. If the list is scrolled, the scroll position stays anchored to the rows already shown. A sort menu offers "Most watched".
- **Watch** opens the playback Dolphin. With one already open, it **switches** what that Dolphin plays (`commandId`). The old stream's WebSocket closes, and the new game catches up from its start (§6.2). In Dolphin the old game's match ends first: the session stops at the next loop top (also while paused, held at the end or buffering), the game leaves the match as after a quit of a 3-4 player match (LOCAL's `quitLeft`, no GAME!), and the new game is armed only once the game is out of the match scene (`PlaybackStopping`). Armed earlier, it took over the running match and never passed its barrier (the user, 2026-10-14). The row being watched is marked "Watching". This is a departure from Slippi, whose spectating is per-broadcaster, chosen by the user (2026-10-13).
- **Auto play** (a toggle above the list, off by default, remembered in the launcher's local storage): see §6.2 "When the stream ends".
- **Spectate files:** `Spectate/` in the playback User folder is cleared at launcher start. Only games the spectator saved to Replays are kept.

### 6.2 Playback Dolphin, `mirror` mode

- **Live edge:** the newest frame on disk minus a 3 s buffer. Live playback holds the buffer between 2 and 4 s by nudging speed between 0.98x and 1.02x, using the throttle factor the speed control already uses. If the buffer empties, the game holds its frame with "Buffering…" and resumes at 2 s, instead of stuttering.
- **Controls:** everything a replay has (`replays.md` §6.4: Space, Left/Right, comma/period, minus/equals, H, the seek bar). Additions:
  - A **LIVE** button and hotkey (End) that jumps to the live edge.
  - The seek bar shows the received part and the live edge moving.
  - "-12 s" beside LIVE while behind.
  - Fast-forward stops at the live edge and switches to LIVE.
  - (2026-10-14) A live game's seek bar ends at the live edge, so live is its very end; the 3 s buffer past it isn't drawn. A seek or jump reaching the live edge is Go Live. Every forward run of a live game (catching up, a seek, a jump) draws one frame in 15, so it never looks frozen.
- **A game watched from its start plays from its start** (the user, 2026-10-14, replacing "always catches up" for that case): the upload holds a game's first 10 s (§4.3) and then sends them at its usual pace, so a spectator who is there when a game starts (a set's next game, Auto play's next game, or a game clicked in its first 20 s) gets its first frames about 10 s late. Playback starts as soon as they come and plays at normal speed, about 10 s behind the players, so a tournament's viewers see the whole game, countdown included. Anyone who wants it live presses Go Live (End), drags the seek bar or fast-forwards. The upload is unchanged: still the 10 s hold, still 2 KB/s at most.
- **Joining a game well under way catches up to live** (the user, 2026-10-13):
  - Frames run as fast as the machine can, without drawing them (the seek's resimulation mode), with sound off.
  - **Progress is visible:** one frame in every 15 is drawn, so the game visibly fast-forwards. An overlay bar reads "Catching up to live · 2:14 / 4:05 · about 20 s". Its end is the moving live edge, and the time left comes from the measured catch-up rate. The seek bar's playhead runs toward the live edge.
  - At the live edge the overlay fades, sound comes on and normal live play starts.
  - Any control (pause, seek, step) stops the catch-up where it is, and LIVE starts it again.
  - The catch-up rate has to stay well above 1x, because the live edge keeps moving. That rate isn't measured yet (§8 P0). If a slow machine runs under about 1.5x, the overlay says so ("Your computer is catching up slowly") instead of looking stuck.
- **Sets** (Direct, Unranked, Ranked and room sets alike; in a room without a set, the room's next game with the same players): at the end of a game, a spectator at the live edge sees the last frame held with "Game 2 starts soon". The next game's `SETP` starts it through `queue` mode's next-game path. A spectator who is behind keeps watching, with the next game queued. Every earlier game of the set stays available and can be watched again.
- **When the stream ends** (the set is over, a room's players change, or every uploader left):
  - **Still behind:** playback goes on. Everything is on disk, so a finished stream on the server changes nothing for this spectator.
  - **At the end, with "Close Dolphin after a spectated game ends" on** (launcher Settings, default on): the last frame is held for 5 s with the result ("Drsmsh wins 2-1") and "Closing in 5 s · press any button to stay". Then the playback Dolphin closes, and the launcher comes to the front on the Spectate tab, ready for the next pick. The launcher ends Dolphin's process, so Dolphin saves its settings just before it asks (status `close`): a resolution set while watching, with the Settings window still open, is kept (the user, 2026-10-14). That Dolphin's settings are its own (Settings > Replays & Spectating), not the Netplay Dolphin's.
  - **Any input** cancels the countdown. Dolphin then holds at the end like a finished replay: seekable, and Play closes it.
  - **With the setting off:** Dolphin holds at the end like a finished replay from the start, with no countdown.
  - The setting travels in the comm file (`closeOnEnd`), so a change applies from the next game watched.
  - **With Auto play on** (the user, 2026-10-13), this takes the place of both the countdown and the Close setting:
    - When playback reaches the stream's end, the launcher **at once** switches the open playback Dolphin to the **most recently created** live game. That is the newest set by start time, leaving out the stream just watched and any game the spectator plays in.
    - It catches up to live as any switch does.
    - The launcher fetches `/v1/live` once at that moment, so this works even when the Spectate tab isn't showing.
    - A set's next game is not the end of the stream: Auto play stays for game 2, as without it.
    - Auto play **overrides** "Close Dolphin after a spectated game ends" (the user, 2026-10-13): with Auto play on, Dolphin never closes at a stream's end. With no other live game, it holds the last frame with "Waiting for the next live game…" and the launcher polls `/v1/live` every 3 s until one starts.
    - Turning Auto play off while watching takes effect at the next end.
  - **Why not close at once:** that cuts off the last KO and the result. **Why not black:** an empty window looks broken and gives the spectator nothing to do. Clicking another game from the end screen switches to it, as always.
- **Desync recovery** (`game-code.md` §11): the restarted game is just the next game in the stream.
- **A spectator checksum that differs:** "Lost sync with this game" and a jump to the next game. The spectator's launcher reports the frame to mm. That makes spectators a free desync canary for settings nothing pins yet.
- **Another build:** a spectator whose `SIM_VERSION` or build differs gets the replay check's message ("recorded with X, you have Y") and an update prompt. Spectating has no "Play anyway", because a live game would only lose sync.

### 6.3 Who can watch

- Watching requires being logged in, as joining rooms does. This allows per-account and per-IP rate limits on `/v1/live`.
- Every mode is listed: Ranked, Unranked, Direct and rooms (the user, 2026-10-13).
- **"Let others stream my games"** (launcher Settings, on by default; the user, 2026-10-13). Description: "Streams inputs to the server for others to spectate you. Very minimal network load. Your opponent's game may still stream the match." Behaviour:
  - Off means this player's game uploads nothing, not even digests.
  - The game is still streamed from any other player who has it on.
  - Only a game where every player has it off is not listed.
  - The setting travels in the matchmaking ticket, so a change applies from the next match.

### 6.4 Spectator chat

The user's decisions (2026-10-13): a chat window for everyone watching a game; **the players in that game never see it**; unencrypted is fine if end-to-end encryption doesn't scale, losing the lock icon.

**Why it isn't end-to-end encrypted.** Today's chat (`design/chat.md`, `chat-protocol.md`) is pairwise: each message is encrypted once per other member, at most 3 copies in a 4-member group, and a box is up to 512 B. That doesn't scale:
- With 1,000 spectators, one message is 999 boxes, about 500 KB. That's far over mm's 8 KiB packet and a lot of upload for whoever typed it.
- A group key (Signal's sender keys, MLS) would fix the size. But every join or leave would mean redistributing keys among up to 1,000 members, and spectators come and go all the time.
- It would also protect nothing. Anyone logged in can open the Spectate tab and join the chat, so a reader doesn't have to break anything; they just watch the game.

So spectator chat is **in the clear to mm, and encrypted on the wire by TLS** (it travels on the spectator's WebSocket, §3.2). The window has **no padlock**. Its title reads "Spectators · 214" instead of the players' codes.

**Players never see it:**
- Spectator chat is its own group kind, `spectate:<stream>`. Only spectator WebSockets join it, and mm never sends it on a game's online connection (ENet). A player's Dolphin has no way to receive it.
- mm refuses to let a player watch a stream they are playing in ("You're playing in this game."). The launcher shows no Watch button on the player's own rows, so they can't read it from the launcher either. Players' own chat stays end-to-end encrypted between them; spectators never get it.
- The rule can't stop a friend from relaying spectator chat to a player some other way. That's out of scope.

**Where it's drawn:** in the playback Dolphin, with the same `OnlineChatUI` window players know: the same look, hotkey, typing, Ignore/Report menu, and drag, resize and collapse. "Show during matches" doesn't apply, because spectating is the match.
- The playback Dolphin stays without credentials and without an internet connection (`replays.md` §6.2). The launcher owns the WebSocket and passes chat to Dolphin over a **loopback-only** channel: `127.0.0.1`, a random port, and a per-launch token on Dolphin's command line, so no other program can inject lines.
- Lines the spectator types go back the same way, and the launcher sends them.
- Ignoring uses the same shared `chat-hidden.json`. The launcher reads it from the netplay User folder, and Dolphin's Ignore goes back to the launcher, which writes it as today.

**Server (mm):**
- Text goes through the same cleaner as players' chat (`common::chat::clean_text`), with the same 100 code points and 300 bytes. mm cleans it before fan-out, and Dolphin cleans again before drawing.
- Names come from mm.
- A spectator who joins gets the last 20 lines.
- **Limits:**
  - Per account: the same 5 in 5 s and 30 a minute as players' chat.
  - Slow mode grows with the crowd: one line every 3 s per account above 50 spectators, every 10 s above 200.
  - At most 5 lines a second are delivered per game. Over that, the line is refused with "Chat is busy. Try again in a moment."
- **Reports:** mm keeps each stream's last 200 lines in memory. A report names the line, and mm stores that line and the 20 before it from its own copy in `chat_reports` as verified (mm saw them, so no signatures are needed). Admins see them in `admin chat-reports` as today.
- The chat ends with the stream, 10 min after the set. Nothing is written to disk except reports.

## 7. Aspect ratio in playback (checked 2026-10-13)

- **The launcher pins it:** `buildPlaybackDolphinArgs` builds on `buildNetplayDolphinArgs` (`launcher/src/dolphin/netplay_args.ts:89-92`), so the playback Dolphin also gets `-C SYSCONF.IPL.AR=True`. Spectating uses the same function.
- **Dolphin pins it:** `PinScreenSetting` runs at replay start (`GameplaySession.cpp:6237`), as at match start (`:6070`). It runs before the seeds and the match scene, so playback and `mirror` play with the screen setting at 16:9, whatever the spectator's Wii or Options > Screen say.
- **Gap 1, no test:** `test_screen_ko_aspect.py` covers live play only. Add a test that plays back that test's screen-KO game on a fresh Dolphin with `SYSCONF.IPL.AR=False` and the screen setting poked to `0x00`. Pass: every frame's checksum equals the file's.
- **Gap 2, old replays:** a replay recorded before 18e14c7a (2026-10-13) by players who **both** had 4:3 played consistently at 4:3. Playback now forces 16:9, so such a replay loses sync at its first screen KO. Playback reports that as a checksum difference rather than playing it wrong. The window is about a day, the case needs both players on 4:3, and `SIM_VERSION` was not bumped. Proposed: leave it, since a bump would refuse every older replay, including the fine ones.

## 8. Plan

| Phase | Work | Done when |
|---|---|---|
| **P0 Measure** | The game's real per-player traffic (`netsim_bench`); catch-up resimulation speed (seek to the end of an 8-min replay) on the Mac and a low-end Windows machine; the VPS's bandwidth cap | Numbers in this doc |
| **P1 Safety harness first** | `netsim` shared uplink; T1-T3 scaffolding with off/forced modes; writer thread priority (useful on its own) | "Forced" shows harm in T1/T2; "off" baselines recorded |
| **P2 Upload** | mm: channel, primary/witness, validation, live store, kill switch (default `off`). Dolphin: uploader on the `Rooms` thread, pacer, backoff, digests, telemetry | T1-T4 pass |
| **P3 Watch** | mm public listener + WebSocket, nginx location; launcher download to file; Dolphin `mirror`, LIVE, buffer, sets; the §7 playback aspect test | A harness spectator watches a Direct set and a 4-player room game with every checksum equal; joins mid-game; survives a takeover |
| **P4 Spectate tab + chat** | `/v1/live` list, the tab, settings ("Let others stream my games", "Close Dolphin after a spectated game ends"); spectator chat: mm group, limits and reports, the launcher's loopback channel, `OnlineChatUI` without the padlock | Storybook stories like Public Rooms; launcher tests; a harness spectator chats while a player's chat status shows no spectator line |
| **P5 Rollout** | Staging (T5), then `witness`, allowlist `on` (T6), then everyone | T6 shows no change for allowlisted players |

## 9. Open questions

Answered 2026-10-13:
- opt-out per uploader, with "Your opponent's game may still stream the match." in the description;
- Ranked listed with no extra delay, and Direct listed;
- joining always catches up to live, with visible progress;
- newest first;
- "Close Dolphin after a spectated game ends", on by default;
- spectator chat, unencrypted, never shown to players.

1. **Max spectators per game:** none at first. The counts above show the box can take it.
2. **Saving chat with a spectated game:** spectator chat isn't recorded anywhere. Should a spectated game saved to Replays keep its chat? Proposed: no.
