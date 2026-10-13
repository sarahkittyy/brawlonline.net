# Spectating

Status: proposal (2026-10-13). Nothing here is built.

## 0. What the user asked for (2026-10-13)

- A **Spectate** tab on Home, next to Overview. It lists every live game the way Public Rooms lists rooms. A click watches one.
- Watching uses the playback Dolphin and has replay controls: pause, play, frame steps, ±5 s, seek, slow and fast. A **Go live** button jumps back to the live game.
- Frames a spectator already received stay cached on their computer. Scrubbing back and forth never asks the server again.
- **Players' games must not be affected in any way.** Under bad performance the replay and upload work is the first thing to throttle or stop. This has to be tested and verified before it ships.
- The playback Dolphin plays at the pinned 16:9, like the players' games, so a screen KO can't desync a replay or a spectated game (§7).

## 1. Summary

Spectating is a live replay. The players' games already record every game's confirmed inputs and checksums (`FRMS` chunks, `replays.md` §4.2). Playback rebuilds the match from those inputs and checks every frame against the recorded checksum. For spectating:

1. One player's game sends those chunks to mm over the online connection it already keeps open. The other players send small digests of the same chunks, so mm can check the stream and switch uploaders if one stops.
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
- **Who:** one **primary** per game, chosen by mm at the barrier. It is the player with the most headroom (§4.3), the host on a tie. Every other player is a **witness**. A witness sends a 12-byte digest per chunk (`first_frame` plus a hash of the uncompressed pads and checksums), batched every 5 s, about 0.1 kbit/s. It keeps its own copy of the chunks in memory (≤ 0.5 MB per game, kept even with `SaveReplays` off).
- **Takeover:** mm sends `spectate-upload {from_frame}` to a witness when the primary goes quiet for 5 s or says it paused (§4.3). The witness sends from that frame through the same pacer. Spectators show "Buffering…" while the gap fills.
- **Validation:** if a digest differs from the primary's chunk, mm ends the stream ("This game can't be shown") and logs it. One modified client can't show spectators a fake game. Peers' confirmed inputs are byte-identical; the files' `inputsSha256` already matches.
- **Cost on the primary:** a 500-650 B chunk per second plus ENet/UDP headers ≈ **4.5-5.5 kbit/s up**. That is roughly 25-50 % on top of the game's own traffic (estimated at 10-20 kbit/s per player in `backend-design.md` §2.7; to be measured, §8 P0). §4 explains why that still can't hurt the game.

### 3.2 mm (relay)

- **Live store:** each live game holds its chunks in memory, plus the match's earlier games, so a set can be scrubbed. A game stays listed while it plays and for 10 min after its set ends. Nothing is written to disk.
- **Limits** (uploads are untrusted): a chunk ≤ 64 KB; a game ≤ 2 MB; an uploader ≤ 4 KB/s; frames contiguous. Headers, CRCs and sizes are checked as `ReplayFile` reads them (`replays.md` §8.1).
- **Server delay:** mm never relays a chunk until it is `D` seconds old. Live delay is decided per mode (open question 2). Because mm enforces it, a client can't bypass it.
- **Fan-out** runs in a tokio task, off mm's ENet thread. The ENet thread only hands each validated chunk to a `tokio::sync::broadcast`.
- **List:** `GET /v1/live`, served from a 1 s snapshot like `/v1/rooms`. It lists mode, ruleset, stage, players (name, code, character, costume), set score, game number, elapsed time and spectator count. A player who opted out hides the game (§6.3).
- **Watching:** `GET /v1/live/{stream}/ws` (WebSocket). The client sends `{from: <game>, <frame>}`. mm sends everything from there, then each new chunk as it passes the delay. On reconnect the client asks from its newest frame, so nothing is sent twice. Each spectator downloads each chunk exactly once.
- **nginx:** `location ^~ /v1/live` proxies to a new public axum listener in mm, with `Upgrade` headers for the WebSocket. The nginx site is installed by hand on sarahvps2 (memory: push is a deploy), so it has to go in before the matching server code ships.

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
- **Small packets, sent between game packets:** each chunk goes out as packets of at most 256 B. They are spread over the second and sent just after the session sends its own input packet for a frame, so an upload packet is never queued just ahead of a game packet. At 256 kbit/s up, a 256 B packet holds the link for 8 ms.
- **DSCP CS1** ("background") on the upload's packets where the OS allows it (Linux, macOS). Home routers mostly ignore it, so nothing relies on it.

### 4.3 Backoff (the uploader stops first)

The primary's uploader reads signals the session already has, every frame, on the `Rooms` thread. **Any** of these pauses it at once:

| Signal | Pause when |
|---|---|
| Emulation speed | a frame missed its deadline in the last 2 s (the throttle fell behind) |
| Time sync | the session stalled waiting for a peer's input in the last 2 s |
| Rollbacks | the 2 s average rollback depth is over the game's own average for its first 10 s + 1 frame |
| P2P latency | the RTT to any peer is 20 ms over its baseline from the game's first 10 s (the uplink is queuing) |
| P2P loss | any loss on the P2P link in the last 2 s |
| Upload queue | more than 2 KB of upload is unacknowledged |

- **Resume** after 10 s with no signal. After the third pause in a game, the primary stops uploading for that game and tells mm, which moves the stream to a witness.
- **Choosing the primary:** each game reports its last game's pauses and its average rollbacks at the barrier. mm picks the player with fewer. A player who paused last game is never chosen while another player can upload.
- When **every** player is paused, spectators see "Buffering — the players' connections are busy" and wait. The game never pays for the stream.
- **Kill switches:** mm's `hello` reply carries `spectateUpload: off | witness | on` and the pacer rate. One server config change turns uploading off for everyone at once, without a launcher update, which matters because a launcher update is slow to reach everyone. A player can also turn it off for themselves (§6.3).

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

The ballpark: CPU, RAM and disk allow **thousands** of concurrent games, far beyond today's player base. The first limit is outbound bandwidth to spectators, about **5 Mbit/s per 1,000 watching**. A 100 Mbit/s cap is about 15,000 spectators minus what TURN and `srcds` use. The burst to watch for is many people joining one popular game at once. That costs bandwidth, not CPU, and the history chunks are immutable, so nginx could serve them from its cache if it ever matters. Disk only matters if streams are ever kept (Ranked upload); 17 GB holds about 60,000 games' inputs.

## 6. Spectator experience

### 6.1 Launcher: Home > Spectate

- `HOME_TABS` (`pages/home/home_routes.ts`) gets `spectate`; `home_page.tsx:64` gets `{ id: "spectate", label: Messages.spectate() }`.
- The page copies Public Rooms' structure (`public_rooms.tsx`, `public_rooms_view.tsx`). It has the same states (logged out, loading, ready, unreachable), the same 4 s poll, and the same 15 s staleness rule. It only polls while the tab is open.
- **Rows:** a mode badge (Ranked, Unranked, Direct, Room), each player's stock icon, name and code, the stage, "Game 2 · 1-0", elapsed time, and the spectator count. Teams are shown as rooms show them. The default sort is most watched, then newest. A filter lets people narrow by mode.
- **Watch** opens the playback Dolphin. With one playback Dolphin already open, it switches what that Dolphin plays (`commandId`). This is a departure from Slippi, whose spectating is per-broadcaster, chosen by the user (2026-10-13).

### 6.2 Playback Dolphin, `mirror` mode

- **Live edge:** the newest frame on disk minus a 3 s buffer. Live playback holds the buffer between 2 and 4 s by nudging speed between 0.98x and 1.02x, using the throttle factor the speed control already uses. If the buffer empties, the game holds its frame with "Buffering…" and resumes at 2 s, instead of stuttering.
- **Controls:** everything a replay has (`replays.md` §6.4: Space, Left/Right, comma/period, minus/equals, H, the seek bar). Additions:
  - A **LIVE** button and hotkey (End) that jumps to the live edge.
  - The seek bar shows the received part and the live edge moving.
  - "-12 s" beside LIVE while behind.
  - Fast-forward stops at the live edge and switches to LIVE.
- **Joining mid-game:** the game is shown from frame 1 at once, so there is no wait. LIVE catches up by running frames without drawing them (the seek's resimulation mode), with "Catching up… 62 %". How fast that runs on a weak machine is not measured (§8 P0). If it's too slow for long games, joining starts at the beginning and LIVE stays optional.
- **Sets:** at the end of a game, a spectator at the live edge sees "Game 2 starts soon". The next game's `SETP` starts it through `queue` mode's next-game path. A spectator who is behind keeps watching, with the next game queued. Every earlier game of the set stays in the list and can be watched again.
- **Desync recovery** (`game-code.md` §11): the restarted game is just the next game in the stream.
- **A spectator checksum that differs:** "Lost sync with this game" and a jump to the next game. The spectator's launcher reports the frame to mm. That makes spectators a free desync canary for settings nothing pins yet.
- **Another build:** a spectator whose `SIM_VERSION` or build differs gets the replay check's message ("recorded with X, you have Y") and an update prompt. Spectating has no "Play anyway", because a live game would only lose sync.

### 6.3 Who can watch

- Watching requires being logged in, as joining rooms does. This allows per-account and per-IP rate limits on `/v1/live`.
- **"Let others watch my games"** (Settings > Online, on by default; open question 1): if any player in a game has it off, the game isn't listed and nothing is uploaded. **"Upload my games for spectators"** (Advanced, on by default): off makes this player a witness only, never the primary.

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
| **P4 Spectate tab** | `/v1/live` list, the tab, settings | Storybook stories like Public Rooms; launcher tests |
| **P5 Rollout** | Staging (T5), then `witness`, allowlist `on` (T6), then everyone | T6 shows no change for allowlisted players |

## 9. Open questions

1. **Opt-out or opt-in?** The proposal is on by default with a setting to turn it off. That matches showing every live game, but it's a privacy call.
2. **Delay per mode.** Proposed: 3 s for Direct, Unranked and rooms. Ranked at 20 s, enforced by mm, so nobody can coach a player live. Or should Ranked not be listed at all?
3. **Direct games:** listed like the rest, or only rooms and matchmade games? Direct is two friends who found each other by code.
4. **Max spectators per game:** none at first. The counts above show the box can take it.
