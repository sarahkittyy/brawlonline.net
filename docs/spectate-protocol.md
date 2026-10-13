# Spectating: protocol

_2026-10-13. The design and the user's decisions are in `docs/design/spectating.md`. Server: `server/crates/common/src/spectate.rs` (types and chunk checks, shared with `mmclient`), `server/crates/mm/src/live.rs` (uploads, roles, the live store), `server/crates/accounts/src/live.rs` (the launcher's API and spectator chat)._

Three legs:

1. **Upload:** a player's game → mm, on the online connection (`docs/rooms-protocol.md` §2).
2. **Relay:** mm → accounts, on mm's loopback listener. Launchers never reach mm over HTTP.
3. **Watch:** accounts → the spectator's launcher (`/v1/live*`, session auth).

Replay chunks travel exactly as `.brep` stores them (`ReplayFile.h`): a 20-byte header (FourCC, flags, stored length, raw length, CRC-32 of the stored bytes), then the stored bytes. Only `INFO`, `SETP`, `SYNC`, `FRMS`, `EVNT` and `ENDG` are sent, never `FTSP` or `FTRM`.

## 1. Upload (game ↔ mm)

JSON goes on channel 0, like every other online message (at most 8 KiB). Chunk data goes on **channel 1** (binary, reliable). An error is answered with `spectate-error` and the connection stays:

```json
{"type": "spectate-error", "op": "spectate-offer", "error": "…"}
```

### `spectate-offer` (game → mm)

Sent at the barrier of every game when the player's "Let others stream my games" is on (Dolphin.ini `[Online] StreamGames`, default true). A player with it off sends nothing at all for spectating.

```json
{"type": "spectate-offer", "matchId": "mode.direct-…", "gameUid": "<32 hex>", "game": 2,
 "paused": 0, "rollbacks": 1.4}
```

| Field | Meaning |
|---|---|
| `matchId` | the match mm made: a `get-ticket-resp`'s, or a room game's start |
| `gameUid` | the replay's `game_uid` (`replays.md` §4.4), the same on every player's game |
| `game` | the game number the players see (INFO `gameIndex`), 1-99 |
| `paused` | how many times this player's uploader paused in its last game (0 for the first game) |
| `rollbacks` | its last game's average rollback depth in frames (0 for the first game) |

mm accepts an offer only:
- from an online connection whose account plays in that match;
- for a match mm made in the last 6 hours;
- for at most 64 games per match.

A second offer for the same game from the same account replaces the first.

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

A takeover is a new `spectate-role` to a witness, with `role: primary` and `from` set.

mm's `MM_SPECTATE` setting, the kill switch, decides what offers get:
- `off` (the default): every offer gets `none`.
- `witness`: everyone is a witness, so only digests flow and nothing is listed.
- `on`: the roles above.

### Data (game → mm, channel 1)

```
u32 slot | u32 seq | u32 offset | u32 total | bytes
```

These are little-endian. `seq` counts the slot's chunks from 0, and `total` is the chunk's whole size including its header (at most 64 KiB + 20). A chunk may come in several packets: `offset` is where these bytes start, the packets of one chunk come in order, and the next chunk starts after the last one (`offset + len = total`). Packets are at most 1,200 bytes.

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
- **Stop:** the player sends nothing more for this game.

mm also starts a takeover when the primary has sent no `FRMS` for 5 s while a witness's digests show the game going on.

## 2. Relay (mm → accounts, loopback)

On mm's status listener (`MM_STATUS_LISTEN`):

- `GET /status`: as before, plus `live`, the list of streams (the `/v1/live` entries without `spectators`).
- `GET /live/{stream}?game=<n>&chunk=<i>`: newline-delimited JSON, from game `n`'s chunk `i` on (default: everything). Each line is one of:

| Line | Meaning |
|---|---|
| `{"t":"game","game":2,"number":2,"uid":"<32 hex>"}` | the stream's 2nd game starts (`number`: the game number the players saw; a desync restart repeats it); its chunks follow. `game` in every line is this place in the stream. |
| `{"t":"chunk","game":2,"i":5,"b":"<base64 chunk>"}` | the chunk as uploaded, header included |
| `{"t":"end","game":2}` | the game's `ENDG` arrived |
| `{"t":"bad","game":2,"error":"…"}` | the game can't be shown; nothing more of it comes |
| `{"t":"over"}` | the stream is over; the body ends |

An empty line is sent every 15 s, so idle connections stay open.

A **stream** is a match: a Direct, Unranked or Ranked set, or a room's games while the same players play in the same room. Its id is 12 lowercase hex characters. A stream is over:
- 120 s after its last game ended with no new offer;
- when no game of it has been uploaded for 10 minutes.

It stays readable for 10 minutes after that.

## 3. Watch (launcher ↔ accounts)

Every endpoint needs a session (`Authorization: Bearer`).

### `GET /v1/live`

```json
{"games": [{
  "id": "3fa9c0d1e2b4", "mode": "ranked", "room": null, "isTeams": false,
  "startedAt": "2026-10-13T18:03:12.345Z", "spectators": 4,
  "game": {"number": 2, "startedAt": "…", "stageId": 3, "state": "playing"},
  "players": [{"uid": "…", "displayName": "Drsmsh", "connectCode": "DRS#1",
               "characterId": 17, "costume": 0, "teamId": null, "wins": 1}]
}]}
```

- The order is newest first (`startedAt`).
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
