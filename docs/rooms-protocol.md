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

Rooms are never ranked and never change a rating. Each room game is recorded in `mm_matches` for the match history (mode 3, with the room's code, its mode and each player's slot and team: section 3), and every player's game reports it like a 1v1 (`POST /v1/ranked/report-game`, with every player's place; `server/README.md`).

## 2. The online connection

All messages are JSON in reliable ENet packets on channel 0, like tickets (`server/README.md`, "Matchmaking server"). The client connects with 3 channels.

### `hello` (client → server)

```json
{"type": "hello", "user": {"uid": "…", "playKey": "…"}, "appVersion": "0.1.0", "platform": "win", "chatKey": "3d40…"}
```

`chatKey` (optional): the install's chat identity key, 64 hex digits; chat on this connection is `docs/chat-protocol.md`.

The same checks as a ticket, in the same order: the build (below), the uid and play key, the ban, a connect code. Then mm answers and keeps the connection:

```json
{"type": "hello-resp"}
```

or refuses and disconnects:

```json
{"type": "hello-resp", "error": "Close Dolphin and update to 0.2.1", "latestVersion": "0.2.1"}
```

- **The build.** `appVersion` (CI stamps the release version into Dolphin) and `platform` (`win`, `mac`, `linux`) are checked against the newest build of that platform in the launcher's update feed (`MM_UPDATE_FEED_DIR`: `latest.yml`, `latest-mac.yml`, `latest-linux.yml`, read again every 10 s), and against `MM_MIN_APP_VERSION` if set. Each platform has its own: the macOS build goes live later (Apple's notarization), and its players keep playing on the previous one meanwhile. A build that sends no `platform` (older than the field) is held to the oldest feed among the platforms it can be from: builds are published only through the feeds, so a 0.1.41 cannot be a macOS build while the macOS feed is at 0.1.28. Tickets (`create-ticket` also carries `platform`) are checked the same way, and so is a room's `room-ready` with `ready: true`, which a game left running across a release is refused: `room-error {op: "room-ready", error: "Close Dolphin and update to 0.2.1"}`. The game shows the text in red on the CSS; Dolphin keeps its build until it is closed, and the launcher updates then.

- **One per account.** A second `hello` from the same account (another game) closes the older connection with `{"type": "error", "error": "Signed in from another game."}`; the older one leaves its room.
- **Heartbeat.** ENet's own keepalive: mm drops a peer it has not heard from for 10 s (`set_timeout(32, 5000, 10000)`, as for tickets). The game must keep servicing this host (also during matches; it is a separate ENet host from the P2P one). Nothing else is sent while idle.
- Online connections never hit the 10 s "no ticket" idle timeout.
- `hello` is limited to 10 per minute per account and shares the per-address ticket limit (30 per 10 s).
- A `create-ticket` on an online connection is answered `create-ticket-resp {error: "Invalid matchmaking request"}`; the connection stays. A room request before `hello` gets `{"type": "error", "error": "Log in again in the launcher."}` and a disconnect.

### Requests (client → server, after `hello`)

| Message | Who | When | Effect |
|---|---|---|---|
| `{"type": "room-create", "public": true, "ruleset": "BCDFGH", "remembered": true}` | anyone | any time | A new room, the sender in slot 1 as host, slots 1-2 open, 3-4 closed. `public` is optional (default true). Leaves the sender's old room first. `ruleset` (the launcher's box, a published ruleset's code) or `remembered: true` (the game's own Create Room: the account's remembered ruleset, `docs/design/room-rules.md` decision 11) is looked up after the room is made; the room plays P+'s default until then, and an unknown code answers `room-error` `Ruleset not found.`. |
| `{"type": "room-ruleset", "code": "BCDFGH"}` | host | between games, no set on | The room plays that published ruleset (`code: null`: P+'s default); it also becomes the host's remembered ruleset. Takes back every ready; ends a finished set's score. |
| `{"type": "room-result", "game": 2, "winnerSide": 0, "stage": 33}` | a set's player | in its session | A game of the room's set as this player's game saw it end (`winnerSide` 0 / 1, 254 a draw, 253 quit with the pause screen's L+R+A+START: the game did not happen; side 0 is the one with the lower slot). Once everyone agrees on 253 the set plays that game again from its first step, its "match started" cleared (leaving before the replay does not lose it). It counts once every player of the set who is in the room reported the same; different reports pause the set. Sent by Dolphin. |
| `{"type": "room-step", "game": 2, "actions": [[2, 3], [33]], "matchStarted": false}` | a set's player | in its session | The set's game being set up as this player's game sees it: each side's struck / banned / picked stages in order (at most 64 each) and whether its match has started. The server keeps the longest lists, so a session started again after a player left goes on from the same step (the ticket's `set.actions`); a player who leaves once the match started loses that game. Sent by Dolphin whenever they change. No answer. |
| `{"type": "room-best-of", "bestOf": 3}` | host | between games, no set on | The room plays sets of 3, 5 or 7 games, or none (0); any room takes it, and a set is played when the room makes two sides (decision G). Like `room-rules` it makes the ruleset custom (`basedOn` the published one) and takes back every ready; a set turned on gets Ranked's format where the ruleset has none (strikes [1, 2, 1] over five starters, 2 bans, Dave's rule). |
| `{"type": "room-set", "action": "resume"}` | host | between games | `resume`: a set paused by reports that differed (`canResume`) plays that game again. `restart` (the game's RULES > SET > End set): any set that is on ends with no result, also between a running set's games (its session on the CSS or stage select; refused once that game's match started, `room-step`; the players' Dolphins end the session when the set is gone from `room-state`) (nobody wins it; the games played stay as played; a player away no longer holds a slot, and the room's old public / private comes back); the next all-ready starts a new one. A player who left needs no host action. |
| `{"type": "room-rulesets"}` | anyone | any time | The rulesets to offer in the game's Ruleset line: answered with `{"type": "ruleset-list", "rulesets": [{"code", "name", "version", "by", "bestOf"}]}`, the player's recent rulesets (newest first), then their own published ones that are not hidden, each once, at most 15 (`room-error` `Rulesets unavailable. Try later.` when the database fails). |
| `{"type": "room-join", "code": "KFQB"}` | anyone | any time | Takes the lowest open, empty slot. The code is matched case- and width-insensitively (`kfqb`, `ＫＦＱＢ`). Leaves the sender's old room first. Joining the room one is in answers its state. |
| `{"type": "room-leave"}` | member | any time | Leaves (answered with `room-left`). |
| `{"type": "room-slot", "slot": 3, "open": true}` | host | between games | Opens or closes slot 1-4. Closing an occupied slot removes its player ("Removed from the room."), who may join again once a slot is open. |
| `{"type": "room-teams", "on": true}` | host | between games | The Teams switch. Changing it clears everyone's ready. |
| `{"type": "room-public", "public": false}` | host | any time | Public or private (a private room is joinable by code only). |
| `{"type": "room-ready", "ready": true, "character": 12, "costume": 0}` | member | not during the start, not while still in the last game | START on the CSS (lock-in) or taking it back. `character` and `costume` are optional numbers 0-255 the server only passes on; they are shown to the others while ready. |
| `{"type": "room-team", "team": 1}` | member | not during the start, not while still in the last game | 0 red, 1 blue, 2 green. New members get red, blue, green, red by slot. |
| `{"type": "room-back"}` | member | after a game | This player is back on the room's CSS. |
| `{"type": "room-rules", "rules": {...}}` | host | between games | The room's match rules (`common::rules::MatchRules`, `docs/design/room-rules.md` §3; missing fields are P+'s defaults, unknown ones and bad values are refused). A change takes back everyone's ready; the same rules again change nothing. A new room has Direct's rules. |

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
  "statusText": "Waiting on: bob",
  "rules": {"style": "stock", "stocks": 4, "timeMinutes": 8, "damageRatio": 1.0, "teamAttack": true,
            "pause": "normal", "itemFrequency": 0, "itemSwitch": [0, 6, 0, 0, 0, 0, 0, 0],
            "hazards": true, "autoLCancel": false, "inputAssist": false}
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

`room-state` also carries what the room plays and its set (`docs/design/room-rules.md`):

```json
  "ruleset": {"code": "BCDFGH", "name": "NorCal Weekly", "version": 3, "by": "sarah", "basedOn": null,
              "bestOf": 3, "stageChoice": "loser-picks"},
  "set": {"bestOf": 3, "game": 2, "score": [1, 0], "sides": [1, 2], "members": [[1], [2]], "paused": false,
          "waitingFor": null, "canResume": false, "finished": false}
```

- `ruleset.code` null: P+'s default, or custom rules (`name` "Custom", `basedOn` "NorCal Weekly v3" when they were made from a published one: the host's `room-rules` turns a published ruleset into custom rules).
- Any ruleset plays in any room (`docs/design/room-rules.md` decision G): a set, or striking every game, needs the room to make two sides (two players, or two teams of two with Teams on); elsewhere its games are single games on a random stage of the ruleset's, and while nobody is ready the status line says "Sets need 1v1 or 2v2" / "Strikes need 1v1 or 2v2". Nothing waits for it.
- `set` while the ruleset plays one (`bestOf` 3/5/7) and the room makes two sides (two players, or four on two teams of two by colour, Teams on): made at the first start, kept across the room's games until someone has `bestOf / 2 + 1` wins (`finished`, `winner` the side; the score and "alice wins the set 2-1" (a team: "alice & bo wins the set 2-1"; too wide: "Set over: 2-1") stay until someone readies for the next set) or the host ends it (RULES > SET > End set: no result). A rules, ruleset or Teams change, or a set player leaving, after a finished set clears it (the next set starts 0-0). `sides` are each side's first (lowest) slot, `members` each side's slots; in 2v2 either teammate strikes, bans and picks (the session host's game decides between their presses, `docs/design/room-rules.md`). While it is on, slots, Teams, rules, the ruleset and the set's length wait (`End the set first (RULES > SET).`).
- A set player who leaves (or whose connection drops) pauses it (`waitingFor`, "Set paused: bob left"): the room turns private, their slot is kept for their account, and when they join again the set goes on by itself and the room is public again if it was (the host's PUBLIC / PRIVATE while it waits is what it goes back to). Leaving once the game's match started (`room-step`) loses that game for their side. The host closing a held slot forfeits the set for that side (a 1v1 room keeps the slot open); closing a set player's slot (a kick) forfeits too. Reports that differ pause it until the host resumes (the game is played again) or ends it ("Results differ: host decides"). In game 1 the others' `character` is withheld (a blind pick).

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
3. Once every member's ticket is in (a room's set: its one session plays every game of the set, from `set.game`; the ticket's `set` carries the format, the score so far, each side's `members` (ports) and the steps of `set.game` made so far (`actions`, from `room-step`), `common::proto::SetTicket`; the ticket's `rules` and `stages` are the room's ruleset's), mm records the game in `mm_matches` (mode 3; `room_code`, `room_mode` `1v1`/`ffa`/`teams`, per player `ports` and, in a team battle, `teams`, and a published ruleset's `ruleset_code`, `ruleset_name`, `ruleset_version` and `ruleset_hash`, `migrations/0009_match_rulesets.sql`) and each member gets one `get-ticket-resp` in Slippi's shape: `matchId` (`mode.room-<code>-…`), `isHost` true for the room's host (the session's decider), `players` with every member (`port` = slot, `isLocalPlayer`, `ipAddress` as mm saw the ticket, so the NAT mapping of the P2P port, `ipAddressLan`), and Direct's `stages`, `items` and `rules` (the match rules, `server/crates/common/src/rules.rs`; rooms play like Direct: game 1 random from the list, then the loser picks; host-set room rules are `docs/design/room-rules.md`). The ticket connections are closed after 1 s, as after any match. `status` becomes `in-game`, every player's `inGame` is true and `ready` false.
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
  "queues": {"unranked": 6, "ranked": 3},
  "playing": {"unranked": 14, "ranked": 8},
  "updatedAt": "2026-10-09T15:00:00.000Z"
}
```

- `queues` (2026-10-12): the Unranked and Ranked tickets waiting in mm, for the "X in queue" under Home's Play unranked / Play ranked. Absent from an older mm (accounts then sends zeros).
- `playing` (2026-10-12): the players in an Unranked and a Ranked match, the "X in game" beside "X in queue". mm counts the members of the matches' chat groups (`docs/chat-protocol.md` §2) that still have two or more: a player is in from the pairing until their game leaves the online CSS (between games and runbacks too), searches again or closes, and a player whose opponent left is out. Absent from an older mm (zeros).
- `online`: **players whose game is running and logged in**: distinct accounts with an online connection (`hello`). Launchers left open without the game do not count; neither do games that are not logged in.
- Each room also has `ruleset` (`room-state`'s: `code`, `name`, `version`, `by`, `basedOn`, `bestOf`, `stageChoice`), so the list can show what it plays.
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

or, for a **Direct invite** (the player list's "Invite to game" when the inviter is in no room; both launchers write one once the invite is accepted, each naming the *other* player's connect code):

```json
{"version": 1, "id": "5b0f3f0e-…", "kind": "direct", "connectCode": "SARA#123", "createdAt": "2026-10-10T15:00:00.000Z"}
```

| Field | |
|---|---|
| `kind` | Absent or `"room"`: the room request above (`code` required). `"direct"`: a Direct invite (`connectCode` required, `code` ignored). `"create"`: a new room (below; `code` and `connectCode` ignored). `"queue"`: Play unranked / Play ranked (below; `queue` required). Any other value (also another case, `"Create"`): the file is ignored. |
| `connectCode` | The other player's connect code, checked as the server checks codes (`server/crates/common/src/codes.rs`, Dolphin `NormalizeConnectCode`): 2-4 ASCII letters, `#`, 1-4 digits with a value of 1-9999, at most 8 characters as written back; any case and leading zeros are taken (`sara#0123` is `SARA#123`). Anything else (spaces, full-width or non-ASCII characters, a second `#`, not a string) and the file is ignored with a log line. |

A Direct invite follows the room request's rules: the same freshness (3 minutes from `createdAt`), `refused` with "Finish your current game first." while busy ("Go back to the menus first." on other screens), `accepted` at once and held while the game boots until it shows its menus, answered by `id` in `lastRequest`. Also `refused` "That's your own code." for the player's own code (`failed` if it was already accepted during the boot). Accepted while the player is in a room (or joining one), Dolphin leaves the room first. The game then goes to the Direct 1v1 character select (WITH FRIENDS > Direct 1v1) from the menus or any idle online CSS, a room's included, with the code filled in: the status line reads "Select character vs SARA#123", then "Press START to play SARA#123" once a character is picked; START locks in and searches that code with no keypad. Both players search each other's code, so mm pairs them as any Direct match. The code is used for that one search: cancelled, failed or after the set, and after leaving the CSS, Direct works as usual (the code is then in the recent codes). There is no `failed` answer for the search itself; it shows on the CSS as any Direct search does. The game side: `docs/rooms-game-interface.md` (LOCAL `directJoin`).

**Create room** (the launcher's button next to the public rooms list, 2026-10-10):

```json
{"version": 1, "id": "5b0f3f0e-…", "kind": "create", "createdAt": "2026-10-10T15:00:00.000Z"}
```

It does what WITH FRIENDS > Create Room does: a **public** room (the in-game default) with the player as host in slot 1 (slots 1-2 open, 3-4 closed), and the game lands on the new room's character select. The same rules as the other requests: 3 minutes from `createdAt`; `refused` "Finish your current game first." while busy, "Go back to the menus first." on other screens; `accepted` at once and held while the game boots until it shows its menus. Accepted, Dolphin leaves any room the player is in (`room-leave`, sent right before the create) and sends `room-create` itself; the game goes to the room CSS from the menus or any idle online CSS (built again as the room's), or stays on a room's CSS, and sends no create of its own. `lastRequest` reads `accepted` as soon as the request is taken; if mm then refuses the create, the same `id` is answered again `failed` with mm's text: "Too many rooms. Wait a moment." (5 creates a minute, 30 an hour per account), "No rooms free. Try later.", or "Can't reach the server." / "Log in in the launcher." after 20 s without an answer. The game then shows the same text in red on the room CSS, out of any room (START there enters a room code, as after a failed Create Room in game).

What the launcher shows (suggested): `accepted`: "Creating a room in your game." (the room's code appears in `game-status.json` `room` once it exists); a later `failed` for the same `id`: its `message`; `refused`: its `message`; no answer in 5 s: "Your game didn't respond. Create the room in game: WITH FRIENDS > Create Room."

**Play unranked / Play ranked** (the launcher's buttons beside Home's Overview tab, 2026-10-12):

```json
{"version": 1, "id": "5b0f3f0e-…", "kind": "queue", "queue": "ranked", "createdAt": "2026-10-12T15:00:00.000Z"}
```

`queue` is `"unranked"` or `"ranked"` (anything else, or absent: the file is ignored). The game goes to that mode's character select, as ONLINE > WITH ANYONE > Unranked / Ranked does; it does not search (the player picks a character and presses START). The rules are the Direct invite's: 3 minutes from `createdAt`, `refused` while busy or on other screens, `accepted` at once and held while the game boots until it shows its menus; Dolphin leaves any room first. The game side: `docs/rooms-game-interface.md` (LOCAL `directJoin`, `directNumber` 1 / 2). The launcher says "Your game is going to Ranked."; no answer in 5 s: "Your game didn't respond. In game: Online > With Anyone > Ranked."

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
| `lastRequest` | The answer to the last `join-room.json` (a room request, a Direct invite or a create): `accepted`, `refused` (with `message`), or `failed` (with `message`, e.g. the server's "Room not found."). |

### What Dolphin and the game must do (next phase)

1. **Watch for `join-room.json`.** Keep polling the online folder (every 500 ms, as `User.cpp`'s watcher does for `user.json`, but without stopping once logged in). On a new file: read it, delete it, ignore it if `version` is not 1, the code is not 4 of the 20 letters, or `createdAt` is more than 3 minutes old (a request from a start that never got that far).
2. **At start-up**, read a request that is already there (Play wrote it before Dolphin started), answer it `accepted` and keep it until the game reaches the main menu (the boot is never busy; `game-status.json` says idle meanwhile); then go straight to the room's CSS, skipping the menus, and join (`room-join`). The 3-minute window covers the SD card copy and the boot.
3. **While running:** busy → answer `refused` and change nothing in the game (`docs/design/rooms.md` #20). Idle → answer `accepted` and go to the room: from the menus or an idle online CSS straight to the room CSS; from another room, leave it first (`room-leave`). If the join then fails, show the server's text on the room CSS's status line, as for a code typed on the keypad.
4. **Write `game-status.json`** on every state change and every 5 s, atomically, with `lastRequest` set to the answer for the request's `id`.
5. **The online connection**: open it (`hello`) when `user.json` is read and the game is logged in, keep it while the game runs (reconnect with backoff if it drops), close it on logout or exit. It carries the room's requests and pushed state; it is what counts the player as online.
6. **The room's game ticket** (section 3): on `room-start`, the mode-3 ticket from the P2P port while the online connection stays open; `room-back` after the game.
7. Report each room game as it ends to `/v1/ranked/report-game` (match history): `winner` the winner's uid (a team battle: the winning team's lowest slot), and in `players` every player of the game with `port` and `place` (1 = first, shared by a tie and by a team; a 1v1's winner 1 and loser 2, both 1 after a draw) and `team` in a team battle, or `{uid, port, left: true}` for one who left (`Online/Ranked.cpp`).

## 6. mmclient

```
mmclient online --user-json a.json [--hold-secs 60]
mmclient room --user-json a.json --create [--private] [--open 3,4] [--close 2] [--teams] [--team N]
              [--ready [--character N --costume N]] [--play [--back-after-secs S | --quit-after-match]] [--hold-secs S]
mmclient room --user-json b.json --join KFQB [the same options]
```

Both keep an online connection open like a running game and print every message from mm as one JSON line. `--play` sends the room's game ticket from a new port when the room starts and prints `{"type": "mmclient-ticket", "result": …}` (the same result as `mmclient search`). Without `--back-after-secs` the player stays "in game", which keeps the room in game in the list; `--quit-after-match` drops the connection right after the match is made, like a game that closes mid-match (its slot becomes open and empty). Exit codes: 0 done or left, 2 `hello` refused, 3 create or join refused, 1 other.
