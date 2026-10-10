# Rooms: protocol, launcher endpoints and the launcher-game hand-off

_2026-10-09. What is built: the server side (`server/crates/mm/src/rooms.rs`, `server/crates/common/src/rooms.rs`, `GET /v1/rooms` in accounts), `mmclient room` / `mmclient online`, and the launcher's list on Home > Overview with its side of the hand-off. Dolphin's side is built on branch `rooms-dolphin` (`Online/Rooms.cpp`, `RoomMessages.cpp`, GameBridge; the game interface is `docs/rooms-game-interface.md`); the game's room CSS is not built yet. The decisions behind it are in `docs/design/rooms.md`._

## 1. Pieces

| Piece | Where | What it does |
|---|---|---|
| Rooms | mm, in memory (`rooms.rs`) | Codes, slots, host, Teams, public flag, readiness, the start. Nothing is stored: a room lives while someone is in it, and its code is free again when it empties. A restart of mm empties every room (the games see their online connection drop). |
| Online connection | mm, ENet UDP 43113, the same port as tickets | A running, logged-in game keeps one connection open: `hello`, then `room-*` requests, and mm pushes the room's state. It is also the heartbeat for the online count. |
| Room game ticket | mm, the existing ticket path | When a room starts, each member sends an ordinary `create-ticket` in mode 3 (Teams) with the room code; every member gets one `get-ticket-resp` that lists all of them. This replaces Slippi's Teams search for code-based multi-player. Direct, Unranked and Ranked are unchanged. |
| Status | mm, HTTP on loopback (`MM_STATUS_LISTEN`, default `127.0.0.1:43181`) | `GET /status`: the online count and the public rooms, refreshed twice a second. |
| Room list | accounts, `GET /v1/rooms` | What launchers read (session auth). accounts reads mm's status (`MM_STATUS_URL`, default `http://127.0.0.1:43181/status`) and serves one read for 1 s. |
| Hand-off | `<User>/Online/join-room.json` and `game-status.json` | A room clicked in the launcher reaches the game through a file next to `user.json`; the game reports busy or idle in another. |

Rooms are never ranked, never change a rating and are not recorded in `mm_matches` (so `report-game` refuses their match ids; Dolphin must not report room games).

## 2. The online connection

All messages are JSON in reliable ENet packets on channel 0, like tickets (`server/README.md`, "Matchmaking server"). The client connects with 3 channels.

### `hello` (client → server)

```json
{"type": "hello", "user": {"uid": "…", "playKey": "…"}, "appVersion": "0.1.0"}
```

The same checks as a ticket, in the same order: `appVersion` against `MM_MIN_APP_VERSION`, the uid and play key, the ban, a connect code. Then mm answers and keeps the connection:

```json
{"type": "hello-resp"}
```

or refuses and disconnects:

```json
{"type": "hello-resp", "error": "Update to 0.2.1 to play online.", "latestVersion": "0.2.1"}
```

- **One per account.** A second `hello` from the same account (another game) closes the older connection with `{"type": "error", "error": "Signed in from another game."}`; the older one leaves its room.
- **Heartbeat.** ENet's own keepalive: mm drops a peer it has not heard from for 10 s (`set_timeout(32, 5000, 10000)`, as for tickets). The game must keep servicing this host (also during matches; it is a separate ENet host from the P2P one). Nothing else is sent while idle.
- Online connections never hit the 10 s "no ticket" idle timeout.
- `hello` is limited to 10 per minute per account and shares the per-address ticket limit (30 per 10 s).
- A `create-ticket` on an online connection is answered `create-ticket-resp {error: "Invalid matchmaking request"}`; the connection stays. A room request before `hello` gets `{"type": "error", "error": "Log in again in the launcher."}` and a disconnect.

### Requests (client → server, after `hello`)

| Message | Who | When | Effect |
|---|---|---|---|
| `{"type": "room-create", "public": true}` | anyone | any time | A new room, the sender in slot 1 as host, slots 1-2 open, 3-4 closed. `public` is optional (default true). Leaves the sender's old room first. |
| `{"type": "room-join", "code": "KFQB"}` | anyone | any time | Takes the lowest open, empty slot. The code is matched case- and width-insensitively (`kfqb`, `ＫＦＱＢ`). Leaves the sender's old room first. Joining the room one is in answers its state. |
| `{"type": "room-leave"}` | member | any time | Leaves (answered with `room-left`). |
| `{"type": "room-slot", "slot": 3, "open": true}` | host | between games | Opens or closes slot 1-4. Closing an occupied slot removes its player ("Removed from the room."), who cannot join this room again. |
| `{"type": "room-teams", "on": true}` | host | between games | The Teams switch. Changing it clears everyone's ready. |
| `{"type": "room-public", "public": false}` | host | any time | Public or private (a private room is joinable by code only). |
| `{"type": "room-ready", "ready": true, "character": 12, "costume": 0}` | member | not during the start, not while still in the last game | START on the CSS (lock-in) or taking it back. `character` and `costume` are optional numbers 0-255 the server only passes on; they are shown to the others while ready. |
| `{"type": "room-team", "team": 1}` | member | not during the start, not while still in the last game | 0 red, 1 blue, 2 green. New members get red, blue, green, red by slot. |
| `{"type": "room-back"}` | member | after a game | This player is back on the room's CSS. |

A request that is refused changes nothing and is answered with `room-error`. Malformed requests (wrong types, slot 0 or 5, team 3, unknown `room-*` types) get `room-error {"op": "invalid", "error": "Invalid matchmaking request"}`; the connection stays.

### Messages (server → client)

**`room-state`**, to every member after every change, personalised by `you` and `statusText`:

```json
{
  "type": "room-state", "code": "KFQB", "public": true, "teams": false,
  "mode": "1v1", "status": "waiting", "you": 2, "host": 1,
  "slots": [
    {"slot": 1, "open": true, "host": true, "player": {"displayName": "alice", "connectCode": "ALIC#4", "ready": true, "team": 0, "character": 12, "costume": 0, "inGame": false}},
    {"slot": 2, "open": true, "host": false, "player": {"displayName": "bob", "connectCode": "BO#77", "ready": false, "team": 1, "character": null, "costume": null, "inGame": false}},
    {"slot": 3, "open": false, "host": false, "player": null},
    {"slot": 4, "open": false, "host": false, "player": null}
  ],
  "statusText": "Waiting on: bob"
}
```

- `mode`: `1v1` with two open slots (the Teams switch has no effect then), `teams` with Teams on and three or four open slots, else `ffa`.
- `status`: `waiting`, `starting` (tickets coming in), `in-game` (until every player of the game is back).
- `slot` is the in-game port and stays the same for the life of the room; gaps are allowed.
- `statusText` is the CSS status line (`docs/design/rooms.md` §3), short enough for the game's line (unit-tested like the other messages):

| Situation | `statusText` |
|---|---|
| Only the receiver is in the room | `Waiting for players` |
| Open slots are still empty | `Waiting for players (2/4)` |
| Everyone is there, some are not ready | `Waiting on: bob, carol` (names that do not fit become `+N`) |
| Teams, three or more players, all one colour | `Pick different teams` |
| The start is under way | `Starting the game` |
| The room plays, the receiver is in the game | `In game` |
| The room plays, the receiver is not (joined during the game, or came back early) | `Waiting for the game to end` |

**`room-error`**, to the sender: `{"type": "room-error", "op": "room-join", "error": "This room is full."}`. `op` is the request's type (`room-start` when a start failed, `invalid` for malformed requests).

**`room-left`**, to a member who is no longer in the room: `{"type": "room-left", "code": "KFQB"}` after `room-leave`, or with `"reason": "Removed from the room."` after the host closed their slot. Members whose connection dropped are not told (they are gone).

**`room-start`**, to every member: `{"type": "room-start", "code": "KFQB", "matchId": "mode.room-KFQB-2026-10-09T15:00:00.000Z-1", "timeoutSecs": 15}`. Send the room game ticket now (section 3).

**`error`**: `{"type": "error", "error": "…"}`, fatal, followed by a disconnect.

### Errors

| Situation | Text |
|---|---|
| A code that is no live room (or is not 4 letters of the 20) | `Room not found.` |
| No open, empty slot | `This room is full.` |
| The host removed this player | `Removed from the room.` |
| A host-only request from someone else | `Only the host can do that.` |
| Slots or Teams during a start or a game | `Wait for the game to end.` |
| The host closes their own slot | `You can't close your own slot.` |
| Closing would leave fewer than 2 open | `At least 2 slots stay open.` |
| A room request from someone in no room | `You are not in a room.` |
| Ready or team during the start | `The game is starting.` |
| More than 5 rooms a minute or 30 an hour (per account) | `Too many rooms. Wait a moment.` |
| More than 10 joins a minute or 60 an hour (per account, counted before the code is looked up) | `Too many tries. Wait a moment.` |
| More than 40 room requests in 10 s (per connection) | `Too many requests. Wait a moment.` |
| The server holds 10,000 rooms | `No rooms free. Try later.` |
| A start that waited 15 s for a ticket (`MM_ROOM_START_TIMEOUT_SECS`) | `<code> didn't connect in time.` (the first missing player's connect code) |
| A member left during the start | `A player left the room.` |
| Version, login, ban, no code | the ticket texts (`server/README.md`) |

## 3. Starting a game

The room starts by itself once **every open slot is taken, every player is ready, nobody is still in the last game** and, with Teams on and three or more players, not everyone is on one colour. Then:

1. mm sends `room-start` to every member; `status` is `starting`; no request can change the room except leaving and the public flag.
2. Each member's game opens a **new** ENet host on its P2P port (Slippi's `41000 + rand % 10000`, or the forced port) and sends the ordinary ticket from it, keeping the online connection open:

   ```json
   {"type": "create-ticket", "user": {"uid": "…", "playKey": "…", "connectCode": "…", "displayName": "…"},
    "search": {"mode": 3, "connectCode": [130, 106, 130, 101, 130, 112, 130, 97]},
    "appVersion": "0.1.0", "ipAddressLan": "192.168.1.20:41234"}
   ```

   `search.connectCode` is the room code as the keypad's Shift-JIS bytes (full width, as Direct sends a connect code) or plain ASCII. mm answers `create-ticket-resp {}`, or an error (`Room not found.`, `You are not in this room.`, `The room is not starting a game.`).
3. Once every member's ticket is in, each gets one `get-ticket-resp` in Slippi's shape: `matchId` (`mode.room-<code>-…`), `isHost` true for the room's host (the session's decider), `players` with every member (`port` = slot, `isLocalPlayer`, `ipAddress` as mm saw the ticket, so the NAT mapping of the P2P port, `ipAddressLan`), and Direct's `stages` and `items` (rooms play like Direct: game 1 random from the list, then the loser picks). The ticket connections are closed after 1 s, as after any match. `status` becomes `in-game`, every player's `inGame` is true and `ready` false.
4. The game does what `Matchmaking.cpp` already does for Teams: disconnect from mm, connect to every remote from the P2P port within 8 s (Slippi's LAN rule per peer), and on failure report "Could not connect to players: …" (no requeue in mode 3). Then the N-player session.
5. After the game (or after a failed connect), each game sends `room-back` and is on the room's CSS again, characters kept, ready cleared. When nobody is in the game any more, `status` is `waiting`. A game that never says `room-back` is taken to be over after 30 minutes.

If a ticket is missing after 15 s, or a member leaves during the start, the tickets that did come in get `get-ticket-resp {error}`, every member gets `room-error {op: "room-start"}`, the room is `waiting` and everyone has to ready up again.

**Joining during a game.** A player whose game closes during a match leaves the room (their online connection drops), so their slot is open and empty. Someone can join into it while the others play; they wait with `Waiting for the game to end`, and the room starts again only when the players are back and the open slots are full and ready.

**Host hand-over.** When the host leaves, the earliest-joined player becomes host (`host` and the slot's `host` flag move). The code stays. The room closes when its last player leaves.

## 4. Launcher endpoints

### `GET /v1/rooms` (accounts)

Session auth (`Authorization: Bearer <sessionToken>`; 401 without it, since the list names players). 60 per minute per account (429 with `Retry-After`). The launcher polls it every 4 s while Home > Overview is open.

```json
{
  "online": 128,
  "rooms": [
    {"code": "KFQB", "host": "alice", "players": 1, "openSlots": 2, "mode": "1v1", "status": "waiting", "names": ["alice"], "joinable": true},
    {"code": "HBPL", "host": "mango", "players": 4, "openSlots": 4, "mode": "teams", "status": "in-game", "names": ["mango", "zain", "cody", "hbox"], "joinable": false}
  ],
  "updatedAt": "2026-10-09T15:00:00.000Z"
}
```

- `online`: **players whose game is running and logged in**: distinct accounts with an online connection (`hello`). Launchers left open without the game do not count; neither do games that are not logged in.
- `rooms`: public rooms only, at most 200: joinable first, then waiting before in game, then fuller, then older. `players` / `openSlots` is what the launcher shows as "players n/N" (N is the number of open slots, the most the room takes). `status` is `waiting` or `in-game` (a starting room is listed as in game). `joinable`: an open slot is empty (also during a game).
- 503 `{"error": {"code": "mm_unavailable", "message": "The matchmaking server is not answering. Try again later."}}` when mm does not answer within 2 s.

### `GET /status` (mm, loopback only)

The same JSON. Only accounts reads it. `MM_STATUS_LISTEN=off` turns it off (then `/v1/rooms` answers 503). Production needs no new proxy rule (nginx already sends `/v1/` to accounts); mm and accounts run on the same machine, and both defaults (`127.0.0.1:43181`) match. If 43181 is taken there, set `MM_STATUS_LISTEN` and `MM_STATUS_URL` together.

## 5. Launcher and game

The launcher's list (`launcher/src/renderer/pages/home/overview/public_rooms/`) shows host, players n/N, mode, status and every player's name. A full room (no open, empty slot, in game or not) is greyed and cannot be clicked; a room in game with an empty open slot can. Clicking a room:

1. **The game is not running** (no netplay Dolphin started by this launcher): the launcher writes `join-room.json` (below), then does exactly what Play does (login, play key on disk, disc image check, launch). If Play stops at a check, the request is removed again. When Dolphin closes, the launcher removes any request the game did not pick up.
2. **The game is running**: the launcher reads `game-status.json`.
   - Fresh (`updatedAt` within 15 s) and `state: "busy"`: nothing is written; the launcher shows **"Finish your current game first."**
   - Otherwise it writes `join-room.json` and waits up to 5 s for `game-status.json` to answer the request's `id` in `lastRequest`: `accepted` ("Joining room KFQB in your game."), `refused` (busy: "Finish your current game first."; other reasons: the game's `message`).
   - No answer within 5 s (an older Dolphin that writes no status, or a game stuck loading): the launcher removes the request and says "Your game didn't respond. Join in game with the code KFQB."

### Files (in `<Dolphin User folder>/Online/`, next to `user.json`)

**`join-room.json`**, launcher → game, written atomically (a temporary file renamed over it):

```json
{"version": 1, "id": "5b0f3f0e-…", "code": "KFQB", "createdAt": "2026-10-09T15:00:00.000Z"}
```

**`game-status.json`**, game → launcher, written atomically by Dolphin on every change and at least every 5 s:

```json
{
  "version": 1,
  "state": "idle",
  "screen": "menus",
  "room": null,
  "updatedAt": "2026-10-09T15:00:01.000Z",
  "pid": 12345,
  "lastRequest": {"id": "5b0f3f0e-…", "result": "accepted"}
}
```

| Field | Values |
|---|---|
| `state` | `idle`: the main menus, the online menus, an online CSS that is not searching, or a room's CSS. `busy`: a match (any mode, online or not), a search or connect (Direct, Unranked, Ranked, a room's start), a single-player mode. |
| `screen` | `menus`, `online-css`, `room`, `match`, `searching`, `single-player`, `other` (for logs and later UI). |
| `room` | The room the player is in, or null. |
| `lastRequest` | The answer to the last `join-room.json`: `accepted`, `refused` (with `message`), or `failed` (with `message`, e.g. the server's "Room not found."). |

### What Dolphin and the game must do (next phase)

1. **Watch for `join-room.json`.** Keep polling the online folder (every 500 ms, as `User.cpp`'s watcher does for `user.json`, but without stopping once logged in). On a new file: read it, delete it, ignore it if `version` is not 1, the code is not 4 of the 20 letters, or `createdAt` is more than 3 minutes old (a request from a start that never got that far).
2. **At start-up**, read a request that is already there (Play wrote it before Dolphin started) and keep it until the game reaches the main menu; then go straight to the room's CSS, skipping the menus, and join (`room-join`). The 3-minute window covers the SD card copy and the boot.
3. **While running:** busy → answer `refused` and change nothing in the game (`docs/design/rooms.md` #20). Idle → answer `accepted` and go to the room: from the menus or an idle online CSS straight to the room CSS; from another room, leave it first (`room-leave`). If the join then fails, show the server's text on the room CSS's status line, as for a code typed on the keypad.
4. **Write `game-status.json`** on every state change and every 5 s, atomically, with `lastRequest` set to the answer for the request's `id`.
5. **The online connection**: open it (`hello`) when `user.json` is read and the game is logged in, keep it while the game runs (reconnect with backoff if it drops), close it on logout or exit. It carries the room's requests and pushed state; it is what counts the player as online.
6. **The room's game ticket** (section 3): on `room-start`, the mode-3 ticket from the P2P port while the online connection stays open; `room-back` after the game.
7. Do not report room games to `/v1/ranked/report-game`.

## 6. mmclient

```
mmclient online --user-json a.json [--hold-secs 60]
mmclient room --user-json a.json --create [--private] [--open 3,4] [--close 2] [--teams] [--team N]
              [--ready [--character N --costume N]] [--play [--back-after-secs S | --quit-after-match]] [--hold-secs S]
mmclient room --user-json b.json --join KFQB [the same options]
```

Both keep an online connection open like a running game and print every message from mm as one JSON line. `--play` sends the room's game ticket from a new port when the room starts and prints `{"type": "mmclient-ticket", "result": …}` (the same result as `mmclient search`). Without `--back-after-secs` the player stays "in game", which keeps the room in game in the list; `--quit-after-match` drops the connection right after the match is made, like a game that closes mid-match (its slot becomes open and empty). Exit codes: 0 done or left, 2 `hello` refused, 3 create or join refused, 1 other.
