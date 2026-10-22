# Spectating: protocol

_2026-10-13. The design and the user's decisions are in `docs/design/spectating.md`. Server: `server/crates/common/src/spectate.rs` (types and chunk checks, shared with `mmclient`), `server/crates/mm/src/live.rs` (uploads, roles, the live store), `server/crates/accounts/src/live.rs` (the launcher's API and spectator chat)._

Three legs:

1. **Upload:** a player's game → mm, on the online connection (`docs/rooms-protocol.md` §2).
2. **Relay:** mm → accounts, on mm's loopback listener. Launchers never reach mm over HTTP.
3. **Watch:** accounts → the spectator's launcher (`/v1/live*`, session auth).

And for a room's own members (2026-10-19): mm → the game of a member who is not in the room's game, on that game's online connection (channel 2, `docs/rooms-protocol.md` §2 "Watching the room's game"): the same kept chunks, played in that member's own Dolphin from the room CSS (`docs/design/rooms.md` §1.3). It reads only what mm keeps for spectators; the players' games do nothing more for it.

Replay chunks travel exactly as `.brep` stores them (`ReplayFile.h`): a 20-byte header (FourCC, flags, stored length, raw length, CRC-32 of the stored bytes), then the stored bytes. Only `INFO`, `SETP`, `SYNC`, `FRMS`, `EVNT` and `ENDG` are sent, never `FTSP` or `FTRM`.

## 1. Upload (game ↔ mm)

JSON goes on channel 0, like every other online message (at most 8 KiB). Chunk data goes on **channel 1** (binary, reliable). An error is answered with `spectate-error` and the connection stays:

```json
{"type": "spectate-error", "op": "spectate-offer", "error": "…"}
```

### `spectate-offer` (game → mm)

Sent at the barrier of every game when the player's "Show my games on the Spectate tab" is on (Dolphin.ini `[Online] StreamGames`, default true; "Let others stream my games" before 2026-10-21). A player with it off sends nothing at all for spectating, except in Ranked: a Ranked set's games are always offered and streamed, as the server's record of the set (2026-10-21, §1.1).

```json
{"type": "spectate-offer", "matchId": "mode.direct-…", "gameUid": "<32 hex>", "game": 2,
 "paused": 0, "rollbacks": 1.4, "listed": true}
```

| Field | Meaning |
|---|---|
| `matchId` | the match mm made: a `get-ticket-resp`'s, or a room game's start |
| `gameUid` | the replay's `game_uid` (`replays.md` §4.4), the same on every player's game (builds up to 0.1.72 put their own count of a room's games into it, so a room game's players could send different ids; see below) |
| `game` | the game number the players see (INFO `gameIndex`), 1-99 |
| `paused` | how many times this player's uploader paused in its last game (0 for the first game) |
| `rollbacks` | its last game's average rollback depth in frames (0 for the first game) |
| `listed` | the player's `StreamGames`: whether spectators may see the stream. Left out by older builds (which offer only when it is on): true |

mm accepts an offer only:
- from an online connection whose account plays in that match;
- for a match mm made in the last 6 hours;
- for at most 64 games per match.

A second offer for the same game from the same account replaces the first. An offer whose `gameUid` mm doesn't know, from a player who hasn't offered for the match's open game, is for that game: the players of a match play its games together, one at a time. Each player's roles name the `gameUid` its own game sent. (Production 2026-10-14, room TLSZ: an older host's game offered its room game as game 4 and the guest's as game 1, under different ids; mm made two games with a primary each and no witness, and refused one upload.) An offer for a new game of the match closes the match's open game, if any (its upload stopped before ENDG): what was kept stays, and spectators get `bad` "The rest of this game wasn't streamed." for it.

### `spectate-role` (mm → game)

```json
{"type": "spectate-role", "gameUid": "<32 hex>", "role": "primary", "slot": 17, "from": null,
 "headers": true, "rate": 2048}
```

mm decides once every player of the match has offered, or 2 s after the first offer. The **primary** is the player with the fewest `paused`, then the lowest `rollbacks`, then the host. Every other offering player is a **witness**.

| Field | Meaning |
|---|---|
| `role` | `primary`: send the chunks. `witness`: send digests. `none`: send nothing more for this game (spectating off on the server, or the offer came too late). |
| `slot` | mm's number for this upload, used in data packets and digests |
| `from` | `primary` only: the first `FRMS` frame to send (`null`: from the start) |
| `headers` | `primary` only: send `INFO`, `SETP` and `SYNC` first. They're false when mm already has them (a takeover). |
| `rate` | the pacer's rate in bytes per second |
| `startRate` | optional: the pacer's rate during the game's first 10 s of running (bytes per second, at most `rate`; 0: nothing then). Without it the game's own (1,536). |

A takeover is a new `spectate-role` to a witness, with `role: primary` and `from` set.

mm's `MM_SPECTATE` setting, the kill switch, decides what offers get:
- `off` (the default): every offer gets `none`.
- `witness`: everyone is a witness, so only digests flow and nothing is listed.
- `on`: the roles above.

### Data (game → mm, channel 1)

```
u32 slot | u32 seq | u32 offset | u32 total | bytes
```

These are little-endian. Dolphin (`Online/Spectate.cpp`) sends packets of at most 512 bytes, at the role's `rate` with a 1 s burst (`startRate` during the game's first 10 s of running; the headers usually go before, in the countdown), and nothing more while 2 KB to mm are unacknowledged. `seq` counts the slot's chunks from 0, and `total` is the chunk's whole size including its header (at most 64 KiB + 20). A chunk may come in several packets: `offset` is where these bytes start, the packets of one chunk come in order, and the next chunk starts after the last one (`offset + len = total`). Packets are at most 1,200 bytes.

mm checks every chunk. A failure ends the slot (`role: none`) and, when a witness exists, a takeover starts:
- **Header:** the type is one of the six; the flags are only deflate and critical; the stored length matches; the CRC-32 matches.
- **Size:** the raw length is within the type's limit (`ReplayFile.h` `MAX_*`).
- **Order:** `INFO`, `SETP` and `SYNC` come first, once each. `FRMS` are contiguous: the first one's `first_frame` is anything, then each starts at the previous `first_frame + count`. A takeover's first `FRMS` starts exactly at `from`. `ENDG` comes last.
- **`FRMS` content:** it inflates (zlib) to exactly its raw length, and that length fits its own `count` and ports.
- **`INFO`:** JSON whose `matchId` is the offer's and whose players' `uid`s are all players of the match.
- **Budget:** 2 MB per game. The rate is a token bucket of 4 KB/s with a 64 KB burst (the headers come at once).

### `spectate-digest` (witness → mm)

```json
{"type": "spectate-digest", "slot": 18, "frms": [[1080, "9c0f…(16 hex)"], [1140, "…"]]}
```

Each entry is an `FRMS` chunk's `first_frame` and the first 8 bytes of SHA-256 over its **raw** (inflated) bytes. There are at most 30 entries per message, sent every 5 s.

mm compares each entry with the primary's chunk of the same `first_frame`. An entry for a chunk mm doesn't have yet waits, up to 120 entries. If any differ, the game is marked bad: spectators get `bad`, and mm logs both accounts.

### `spectate-pause`, `spectate-resume`, `spectate-stop` (primary → mm)

```json
{"type": "spectate-pause", "slot": 17, "reason": "rtt"}
```

`reason` is `frame`, `timesync`, `rollback`, `rtt`, `loss` or `queue` (`design/spectating.md` §4.3).

- **Pause:** mm moves the upload to a witness at once if it has one. The paused player becomes a standby and sends nothing.
- **Resume:** a standby that resumes stays standby, unless the current primary stops and no witness is left. Then the standby takes over again.
- **Stop:** the player sends nothing more for this game. If nobody else can take the upload over (no witness, no standby that resumed), mm cuts the game at once: spectators get `bad` "The rest of this game wasn't streamed." (they play what came). The players are still playing it: the row stays playing and the wait between games starts only at the game's end (`spectate-end`). The same holds when the last uploader leaves or sends a bad chunk; a pause is not cut (it comes back within seconds).

mm also starts a takeover when the primary has sent no chunk for 5 s while a witness's digests show the game ahead of what mm has. A primary with no `FRMS` yet is given the uploader's start hold too: the witness must be past frame 900 (the game's first 10 s, when nothing was sent before 2026-10-19 and now only a trickle that a queue on the uplink holds, and 5 s more). The stalled player gets a new `witness` role, so it stays a backup that a later takeover can hand the game back to. With nobody to take over, the primary keeps the game, and only after 30 s without a chunk is the game cut. (Production 2026-10-14: the headers' arrival didn't count as progress, so 435 of 572 games were taken from their primary 12-14 s in, as its headers landed, and the stalled player was dropped as a backup; a pause of the new primary then left the stream with no data for 10 s or more, and a stall cut the game.)

### `spectate-end` (every player's game → mm)

```json
{"type": "spectate-end", "matchId": "mode.ranked-…", "gameUid": "<32 hex>", "endMethod": "game",
 "winnerPort": 2, "isTeams": false, "players": [{"port": 1, "uid": "…"}, {"port": 2, "uid": "…", "teamId": 0}],
 "setGame": 2}
```

`perf` (2026-10-22; left out by older builds and by games not played over the network): this PC's speed over the game, from its `ENDG`: `{"frames": 7020, "fps": 59.94, "ownLate": 0.01, "followed": 0, "peerFps": 44.5, "slow": false, "peerSlow": false}` (`peerFps` only when `followed` is above 0). `ownLate` is the share of frames late without a rollback before them (this PC's own speed), `followed` the share run at the speed of a peer that could not keep up, `slow` / `peerSlow` whether the game showed "can't keep up" for this PC / a peer. mm records it for the match's players (`speed_reports`, `docs/design/speed-check.md`); one that does not read as valid is dropped and the report counts without it.

`setGame` (Ranked only, 1-99; left out by older builds and in other modes): the set's game as the player's result report numbers it (`gameIndex`: a desynced game played again keeps its number, unlike `game`). With it, mm records the game's result as this player's game says it (§1.1).

A `none` role after a `primary` or `witness` one (a paused primary that mm handed on as a standby) does not stop it: that game's end is reported too (2026-10-22: before, a Ranked game whose upload paused for the PC's speed lost that player's result in `ranked_game_results`). Only a game mm answered `none` from the start (spectating off) sends none.

How a game the player offered ended, from its own `ENDG` (`winnerPort` and `quitterPort` 1-4, left out for a draw or none; `players` from its `INFO`). Every player's game sends it once per game, whatever its role and however its upload went (not when mm answered `none` from the start, below): an upload cut before its `ENDG` used to leave the game out of the list's score (production 2026-10-15, a Ranked set listed as "Game 3 · 1-0"). mm counts each game's result once: from the upload's `ENDG` when that comes; a report that arrives while the upload still goes on waits for it (the score doesn't run ahead of what spectators have seen), and counts when the upload is cut or the match's next game is offered. The winner's uid comes from the kept `INFO` if there is one, else from the report, players of the match only. Reports from strangers are refused; reports for a game mm doesn't know (spectating off) are ignored.

### 1.1 Ranked: streamed for the record (2026-10-21)

A Ranked set's games are always streamed, whatever `MM_SPECTATE` and the players' `StreamGames` say: both games offer every game (`listed` says whether the player lets others watch), mm gives one the primary and the other the witness as usual, and every player's game sends `spectate-end` with `setGame`. `MM_RANKED_STREAM=off` puts Ranked back under `MM_SPECTATE`. Such a stream is listed and served to spectators only under `MM_SPECTATE=on` and once a player of it offered with `listed` true; otherwise mm keeps it (the witness's digests are still checked against it) but `/status`'s `live` and `GET /live/{stream}` leave it out.

It is what accounts checks a game only one player reported against (`common::ranked::resolve`). mm writes to the database:
- `ranked_presence (match_id, uid, online, changed_at)`: whether each player's game has an online connection to mm, when the set is made and whenever that player's first connection opens or last one closes, for 3 hours after the set was made. A newer `changed_at` wins.
- `ranked_game_results (match_id, game_index, uid, winner, end_method)`: each player's `spectate-end` for a Ranked game with `setGame`, ended by `game` or `quit` (a Ranked quit loses the game for the quitter); a desync, a disconnect and the like are no result. `winner` is null for a draw.

A game only one player reported counts at once when the other player's game told mm the same winner, and voids the set as `ERROR` when it told mm another. Without that, it counts after the grace period (2 minutes) only when the other player's game is not connected to mm (or mm recorded nothing, as for sets from before this); while it is connected, the report waits, and the set is `ORPHANED` after 30 minutes. An `opponent_left` about a player whose game is still connected blames nobody.

## 2. Relay (mm → accounts, loopback)

On mm's status listener (`MM_STATUS_LISTEN`):

- `GET /status`: as before, plus `live`, the list of streams (the `/v1/live` entries without `spectators`).
- `GET /live/{stream}?game=<n>&chunk=<i>`: newline-delimited JSON, from game `n`'s chunk `i` on (default: everything). Each line is one of:

| Line | Meaning |
|---|---|
| `{"t":"game","game":2,"number":2,"uid":"<32 hex>"}` | the stream's 2nd game starts (`number`: the game number the players saw; a desync restart repeats it. A room's games are numbered by mm, 1, 2, 3… in the stream, since each player's game counts a room's games its own way); its chunks follow. `game` in every line is this place in the stream. |
| `{"t":"chunk","game":2,"i":5,"b":"<base64 chunk>"}` | the chunk as uploaded, header included |
| `{"t":"end","game":2}` | the game's `ENDG` arrived |
| `{"t":"bad","game":2,"error":"…"}` | the game can't be shown; nothing more of it comes |
| `{"t":"over"}` | the stream is over; the body ends |

An empty line is sent every 15 s, so idle connections stay open.

A **stream** is a match: a Direct, Unranked or Ranked set, or a room's games while the same players play in the same room. Its id is 12 lowercase hex characters. A stream is over:
- at once when its set can't go on: a game ended by a disconnect, a player leaving the match or an error (ENDG `endMethod` `disconnect`, `stopped`, `error`), or a Ranked player's second win (a Ranked quit is a lost game, the quitter's opponent's win, not the set's end);
- when one of its players leaves the match or the room, once no game of it is going on: mm sees them leave the match's (or room's) chat group, which Dolphin does when the game leaves the online CSS, a new search, another room or a dropped connection (`docs/chat-protocol.md` §2). Between games on the CSS the players stay in it;
- otherwise 120 s after its last game ended (its `ENDG` or a player's `spectate-end`) with no new offer (players without in-game chat never join a group; Unranked, Direct and rooms have no set length). A game going on holds the stream, whether anything of it was uploaded or not;
- when nothing of it (an upload, an offer, a report) has come for 10 minutes.

A match's game after its stream ended starts a new stream that keeps the match's score and game count.

It stays readable for 10 minutes after that.

## 3. Watch (launcher ↔ accounts)

Every endpoint needs a session (`Authorization: Bearer`).

### `GET /v1/live`

```json
{"games": [{
  "id": "3fa9c0d1e2b4", "mode": "ranked", "room": null, "isTeams": false,
  "startedAt": "2026-10-13T18:03:12.345Z", "spectators": 4,
  "game": {"number": 2, "place": 2, "startedAt": "…", "stageId": 3, "state": "playing"},
  "players": [{"uid": "…", "displayName": "Drsmsh", "connectCode": "DRS#1",
               "characterId": 17, "costume": 0, "teamId": null, "wins": 1}]
}]}
```

- The order is newest first (`startedAt`).
- `place` is the current game's place in the stream (the `game` of §2's lines). A launcher joining a set mid-way watches from `?game=<place>` (`place + 1` between games), so it starts with the game being played.
- `state` is `playing`, `between` (waiting for the set's next game) or `over`.
- A game marked bad is listed with `state: "over"`.
- The answer comes from a snapshot about a second old (mm refreshes it every 0.5 s and accounts keeps it for 1 s). It's limited like `/v1/rooms`.

### `GET /v1/live/{id}/watch?game=<n>&chunk=<i>`

The answer is server-sent events, one JSON object per `data:` line, with a `: ping` every 15 s and `X-Accel-Buffering: no`. A launcher that hears nothing for 45 s reconnects, from the chunk after its newest.

- **403** `playing` "You're playing in this game.": the account plays in the stream.
- **404** `not_found` "This game has ended.": the stream is unknown or gone.

| Event | Fields |
|---|---|
| `welcome` | `stream` (its `/v1/live` entry), `you` (uid), `chat` (the last 20 lines) |
| `game`, `chunk`, `end`, `bad`, `over` | as in §2 |
| `chat` | `line`: `{id, uid, displayName, connectCode, text, at}` |
| `spectators` | `count` |

### `POST /v1/live/{id}/chat`

The body is `{"text": "…"}`, and the answer is 204. The sender must have a watch stream of that stream open.

| Status | `error` | When |
|---|---|---|
| 400 | `invalid` | the text is empty after cleaning, or the body is malformed |
| 403 | `not_watching` | no watch stream open |
| 429 | `too_fast` "You're sending messages too fast." | 5 in 5 s or 30 a minute per account, or slow mode (one line per 3 s above 50 spectators, per 10 s above 200) |
| 429 | `busy` "Chat is busy. Try again in a moment." | the stream already delivered 5 lines in the last second |

The text goes through `common::chat::clean_text` (100 code points, 300 bytes). Names come from the account.

### `POST /v1/live/{id}/report`

The body is `{"line": <id>, "reason": "…"}`, and the answer is 204. accounts stores the reported line and up to 20 lines before it from its own copy in `chat_reports`, with group `spectate-<id>`, `verified` = the number of lines, and `sig` empty. The limit is 5 an hour per account.

Spectator chat lives only in accounts' memory: the last 200 lines per stream, dropped with the stream. **It is never sent to a game's online connection**, so players never see it.

## 4. Playback (launcher ↔ the playback Dolphin)

The launcher (`launcher/src/spectate/spectate_client.ts`) writes each game of the watched stream to `<playback User folder>/Spectate/<stream>/<place>.brep` as it comes: the 32-byte `.brep` header with the game's `uid`, every chunk exactly as relayed, and at `end` the trailer, which makes it a whole replay (`live_game_file.ts`). `Spectate/` is cleared at every launcher start.

The playback Dolphin follows those files through Slippi's comm file (`-i`, `Rollback/ReplayPlayback.h`) in `mirror` mode:

```json
{"mode": "mirror", "commandId": "spectate-3fa9c0d1e2b4-…",
 "queue": [{"path": "…/1.brep", "cut": true}, {"path": "…/2.brep"}],
 "streamOver": false, "closeOnEnd": true, "banner": "Waiting for the next live game..."}
```

| Field | Meaning |
|---|---|
| `queue` | the stream's games so far. Under the same `commandId` games are only added at the end (a set's next game); a new `commandId` is another stream. |
| `cut` | the stream said `bad` for the game, or ended before it did: the file won't grow. Playback plays what is there ("The rest of this game wasn't streamed.") and goes on. |
| `streamOver` | `over` came: no more games. |
| `closeOnEnd` | "Close Dolphin after a spectated game ends" and not Auto play: at the end, a 5 s countdown, then status `close`, and the launcher closes Dolphin. Any replay key cancels it. |
| `banner` | shown at the end of a stream that is over (Auto play: "Waiting for the next live game..."). |

Dolphin re-reads a game's file when its size changes (every 200 ms) and adds the new frames to the playing match (`Gprb::Session::ExtendPlayback`). It arms a game once its setup is there: the match loads, and the barrier before its countdown waits for the first frames. A game it is there for from about its start (the request's first game with at most 20 s received at that barrier, and every next game of the stream) plays from its first frame at normal speed, as far behind the players as the stream comes (about 4-5 s: the frames come about a second after the players' GO, and the countdown runs while more come). A game joined well under way first catches up to the live edge (the newest frame less 3 s) unpresented but for one frame in 15; so does Go Live at any time. At the live edge it keeps 2-4 s of frames by nudging its speed 2 %. It holds ("Buffering…") when the frames run out. While a game has no frames yet it shows "Game found, waiting for data...", and after 15 s that the stream pauses while the players' connection is busy. A game cut before any frames came is passed over: the next game when it comes ("This game wasn't streamed. Waiting for the next game..." meanwhile, state `between`), or, with the stream over, the end as after a game that played (state `ended`, then `close` 5 s later with `closeOnEnd`). A finished game is followed by the next one in the queue; with none yet, it holds its last frame ("The next game starts soon"). Its status file adds `mirror`, `at_end`, `live_edge`, `buffering`, `catching_up`, `catch_rate`, `complete` and `closeIn`, and the states `between` and `close`.
