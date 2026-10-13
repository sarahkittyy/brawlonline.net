# Rooms: the game <-> Dolphin interface (PPOM v5)

_2026-10-10. Owner: the Dolphin side (`Online/GameBridge.cpp`, `Online/Rooms.cpp`). What the game plugin sends and what Dolphin publishes for the room CSS of `docs/design/rooms.md`. The server side is `docs/rooms-protocol.md`; Dolphin translates between the two, so the game never sees JSON._

**Memory: zero new bytes.** Everything below lives in fields that were reserved or padding in PPOM v4: `sizeof(Block)`, `sizeof(Local)` (0x80), `sizeof(Session)` (0x220) and the mailbox (0x410) are unchanged, and so is every v4 offset. The plugin's `.data` does not grow. The only new code cost is the plugin's own (one new mailbox command, a few byte reads).

**Version.** `PPOM::VERSION` becomes **5** (the meaning of reserved bytes changed). Dolphin accepts a v4 or a v5 block: with a v4 plugin there are no rooms (it never sends `CMD_ROOM`, its `screen` byte reads 0 = unknown) and everything else works as before.

---

## 1. Summary for the game

| The game wants to... | How |
|---|---|
| create, join, leave, open/close a slot, Teams on/off, public/private, its own team colour | `CMD_ROOM` (0xD0) with a `RoomRequest` (one op per request). Dolphin sends it to mm. |
| show the status line | poll `CMD_ROOM` op `ROOM_POLL` (one outstanding at a time, like `GET_MATCH_STATE`); the answer is a `RoomStatus`: the line (server's text or Dolphin's), red or not. |
| draw the 4 panels, the code, the host icon, Public/Private, the Teams switch | read SESSION directly, any frame (no poll): `roomFlags`, `roomCode`, `roomHost`, `roomStatus`, `roomMode`, and per port `roomSlot` bits, `roomTeam`, `roomChar`, `roomCostume`, `name`, `code`. Redraw when `Session.seq` changes. |
| know its own slot | `Local.localPort` (0-3) while in a room. |
| ready / unready | the existing lock-in (`writeLockIn`, LOCAL `LockIn`) with `game` = `Session.game`. Dolphin turns it into `room-ready`. |
| start the match | as Direct: once `Session.state == SS_MATCH_READY && Session.game == the locked game`, leave the CSS (`tickConnected`'s path). The loser's stage pick as Direct (`picksStage`, `STAGE_PENDING`, `LOCAL.remoteReady`). |
| jump to the room from the menus (launcher click) | `Local.roomJoin` changes: go to the room CSS (Dolphin has already sent the join). |
| the launcher's Create room | `Local.roomCreate` changes: go to the room CSS as Create Room does, **without** posting `ROOM_CREATE` (Dolphin has left any room and sent the create). |
| the launcher's Direct invite | `Local.directJoin` changes: go to the Direct CSS with `directPrefix` + `#` + `directNumber` filled in (Dolphin has left any room); START searches that code without the keypad. |
| tell Dolphin whether it is busy | write `Local.screen` every frame (or on change). |

---

## 2. Layout changes (v4 -> v5)

### LOCAL (per machine, excluded from rollback)

| Offset | v4 | v5 | Who writes | Meaning |
|---|---|---|---|---|
| 0x05 | `localPort` | `localPort` (reused) | Dolphin | Also while in a room without a session: this player's slot as in-game port (0 = P1), 0xFF none. In a room game the session's port is the same slot. |
| 0x36 | `_reserved1[0]` | **`screen`** | game | `Screen`: where the game is (below). |
| 0x37 | `_reserved1[1]` | **`roomJoin`** | Dolphin | Counter, bumped when Dolphin accepted a launcher join (`join-room.json`) and sent the join: go to the room CSS. |
| 0x38 | `_reserved[0]` byte 0 | **`room`** | Dolphin | `RoomPhase`: 0 `RP_NONE`, 1 `RP_JOINING` (create/join pending), 2 `RP_IN` (in a room). |
| 0x39 | `_reserved[..]` | **`directJoin`** | Dolphin | Counter, bumped when Dolphin accepted a launcher Direct invite (`join-room.json` kind `direct`, `docs/rooms-protocol.md` §5) and left any room: go to the Direct CSS with the code below. |
| 0x3A | `_reserved[..]` | **`directPrefix[4]`** | Dolphin | The other player's connect code, its letters: 2-4 ASCII `A`-`Z`, NUL-padded. Written with `directJoin` (one LOCAL write) and kept until the next invite. |
| 0x3E | `_reserved[..]` | **`directNumber`** (u16) | Dolphin | Its number, 1-9999: the code is `directPrefix` + `#` + the number in decimal (`SARA#123`). A zero prefix: no code (a plain Direct CSS), or with the number 1 / 2 the launcher's **Play unranked / Play ranked** (`join-room.json` kind `queue`, 2026-10-12): the Unranked / Ranked CSS instead. |

| 0x7E | `_reserved2[0]` | **`roomCreate`** | Dolphin | Counter, bumped when Dolphin accepted a launcher create (`join-room.json` kind `create`), left any room and sent `room-create` (public): go to the room CSS as Create Room, without a `ROOM_CREATE`. |

`directJoin`, `directPrefix`, `directNumber` and `roomCreate` were added on 2026-10-10 without a version change: they were reserved, a plugin without them ignores them, and a plugin with them under an older v5 Dolphin just never sees `directJoin` move.

### SESSION (rolled back; Dolphin writes it only outside matches)

Room fields are written whenever this player is in a room and no match runs; during a match SESSION stays as it was at the start (room changes show after the match). The v4 fields keep their meaning: `present`, `charKind`, `costume`, `team` are still **the next game's setup** (filled at `SS_MATCH_READY`); the room view is in separate bytes so `online_match.cpp` is unaffected.

| Offset | v4 | v5 | Meaning |
|---|---|---|---|
| 0x06 `game` | the session's next game | in a room: **the room's next game number for this machine** (1, 2, ...), also while `state == SS_NONE` | Lock in with this number. It goes up by one after each room start (played, or failed), so a lock-in from before is stale and the player has to press START again ("ready cleared"). |
| 0x213 | `_reserved[0]` | **`roomFlags`** | `RF_IN` 0x01 (in a room; the room fields are valid), `RF_PUBLIC` 0x02, `RF_TEAMS` 0x04 (the host's Teams switch). |
| 0x214 | `_reserved[1..4]` | **`roomCode[4]`** | ASCII, upper case, no NUL (e.g. `KFQB`). |
| 0x218 | `_reserved[5]` | **`roomHost`** | the host's slot as in-game port 0-3; 0xFF none. |
| 0x219 | `_reserved[6]` | **`roomStatus`** | 0 waiting, 1 starting (everyone ready; tickets and P2P connect), 2 in game (until every player of the game is back). |
| 0x21A | `_reserved[7]` | **`roomMode`** | 0 1v1 (two open slots), 1 free-for-all, 2 teams (server's `mode`). |
| 0x21B-0x21C | | **`quitPorts`, `quitStays`** | 3-4 player matches: the pause screen's quit takes only the quitter out (`docs/nplayer/session.md`). |
| 0x21D-0x21E | | **`quitBy`, `quitRunback`** (still v6) | game, in the frame of a quit that ends the match for everyone (a 1v1, a 2v2 set): the quitter's port + 1, and 1 when L+R+Y were held (P+'s salty runback). Rolled back with the match, cleared at its setup; Dolphin decides what the quit means (`docs/game-code.md` §11, the quit per mode). |
| 0x21F | | `_reserved[1]` | free |
| player i +0x04 `name` | name in a session | also every **taken slot's** member name in a room | UTF-16, 15 units max + NUL. In a room's game, port i's is slot i's player (the ticket's ports), also with 2 players: Direct's decider-first order put the new host's name under P1 after the host left and came back to slot 1 (staging, 2026-10-10; `test_room_names_follow_the_slots_after_the_host_left`). |
| player i +0x24 `code` | code in a session | also every taken slot's connect code | |
| player i +0x36 `picksStage` | the session's pickers | in a room between games: this port picks the next room game's stage (the last room game's loser, carried by Dolphin) | as Direct |
| player i +0x38 | `_pad2[0]` | **`roomSlot`** | bits: `SLOT_OPEN` 0x01, `SLOT_TAKEN` 0x02 (a player is in it), `SLOT_READY` 0x04, `SLOT_HOST` 0x08, `SLOT_IN_GAME` 0x10 (still in the last game). Closed = not `SLOT_OPEN`; "Searching..." = `SLOT_OPEN` without `SLOT_TAKEN`. |
| player i +0x39 | `_pad2[1]` | **`roomTeam`** | 0 red, 1 blue, 2 green (the room's colour for this player, also with Teams off); 0xFF empty. |
| player i +0x3A | `_pad2[2]` | **`roomChar`** | gmCharacterKind of the player's lock-in while `SLOT_READY` (the server passes it on), else 0xFF. Panels that need a CSS id convert it (`exchangeGmCharacterKind2MuSelchkind`, 0x800AF708). |
| player i +0x3B | `_pad2[3]` | **`roomCostume`** | the costume of that lock-in, else 0. |
| player i +0x3C-0x3F | `_pad2[4..7]` | `_pad2[4]` | free |

Indices are in-game ports (slot 1 = index 0). Ports may have gaps (P1, P3). Every value comes from mm through Dolphin, which validates it first (codes of the 20 letters, slots 1-4, teams 0-2, names cut to 15 UTF-16 units, characters and costumes 0-255 passed through only while the player is ready).

### Mailbox: one new command

```cpp
const u8 CMD_ROOM = 0xD0;

enum RoomOp {
    ROOM_POLL = 0,     // nothing; answers the status line
    ROOM_CREATE = 1,   // arg: 1 public (the default), 0 private
    ROOM_JOIN = 2,     // code: the room code as typed (4 letters)
    ROOM_LEAVE = 3,    // leave the room (hold B: to the menus)
    ROOM_SLOT = 4,     // host: arg = in-game port 0-3, arg2 = 1 open / 0 close (close occupied = kick)
    ROOM_TEAMS = 5,    // host: arg = 1 Teams on, 0 off
    ROOM_PUBLIC = 6,   // host: arg = 1 public, 0 private
    ROOM_TEAM = 7,     // arg = this player's colour 0 red, 1 blue, 2 green
    ROOM_RULES = 8,    // v6, host: RoomRulesRequest {op, pad[3], Rules} (SESSION's layout);
                       // Dolphin checks every value (FromSessionBytes) and sends room-rules
    ROOM_SET = 9,      // v6, host: arg = 1 resume / 0 restart the room's paused set (room-set)
    ROOM_RULESET = 10, // v6, host: code = a ruleset code (6 letters) or empty for P+'s default
                       // (room-ruleset)
};

struct RoomRequest {   // CMD_ROOM request payload
    u8 op;             // RoomOp
    u8 arg;
    u8 arg2;
    u8 _pad;
    u16 code[CODE_LEN];   // ROOM_JOIN: ASCII or full-width UTF-16 (the keypad's), NUL-terminated
    u16 _pad2;
};                     // 0x18

enum RoomPhase { RP_NONE = 0, RP_JOINING = 1, RP_IN = 2 };

struct RoomStatus {    // CMD_ROOM response payload (cmd 0xD0, status 0), for every op
    u8 phase;          // RoomPhase
    u8 error;          // 1: `text` is an error (red; play the error sound when it appears)
    u8 localPort;      // as LOCAL.localPort
    u8 _pad;
    u32 serial;        // changes whenever `text`, `error` or the room changes
    u16 text[64];      // the status line, NUL-terminated ("" = the game's own line)
};                     // 0x88
```

Every `CMD_ROOM` gets one response (the mailbox's usual rules: in order, one per frame). Its `RoomStatus` is the state right after the request was taken, so a `ROOM_JOIN` is answered `phase 1, "Joining room KFQB"`; the outcome shows in later polls and in SESSION. The game routes it by `r->cmd == CMD_ROOM` like the other answers and may copy `text` into its existing status buffer (`s.error`, ASCII).

A request the server refuses changes nothing and turns the line red with the server's text (`docs/rooms-protocol.md` §2, Errors: "Room not found.", "This room is full.", "Only the host can do that.", "You can't close your own slot.", "At least 2 slots stay open.", "Wait for the game to end.", "Too many tries. Wait a moment.", ...). An error stays until the next request, or for 5 s while in a room (then the room's line comes back). Dolphin refuses some requests itself with the same texts (a code that is not 4 of the 20 letters: "Room not found.", without asking the server).

### `Screen` (LOCAL 0x36, game -> Dolphin)

| Value | Name | When | Launcher join |
|---|---|---|---|
| 0 | `SCREEN_UNKNOWN` | not written yet (boot), or a v4 plugin | kept until the game reports a screen |
| 1 | `SCREEN_MENUS` | the main menu's top page and the online pages (ONLINE, WITH FRIENDS, WITH ANYONE): the menu pages the game can jump to the room CSS from. Other pages of the main menu (VERSUS', SOLO's, OPTIONS ...) are `SCREEN_OTHER` | accepted |
| 2 | `SCREEN_ONLINE_CSS` | an online CSS (Direct/Unranked/Ranked), not searching | accepted |
| 3 | `SCREEN_ROOM` | the room CSS (in a room or not) | accepted (leaves the old room) |
| 4 | `SCREEN_ONLINE_BUSY` | an online CSS while searching, connecting, or connected to an opponent (a Direct/Ranked set) | refused, "Finish your current game first." |
| 5 | `SCREEN_MATCH` | any match, online or offline (scMelee) | refused |
| 6 | `SCREEN_OFFLINE` | single-player modes, offline Versus, anything offline that isn't a menu | refused |
| 7 | `SCREEN_OTHER` | anything else | refused, "Go back to the menus first." |

Dolphin also counts as busy whatever it knows itself (a search, a session, a room start), whatever `screen` says. `screen` also fills `game-status.json` for the launcher.

---

## 3. Flows

**Create.** WITH FRIENDS > Create Room: the game opens the room CSS and posts `ROOM_CREATE` (arg 1). `room` goes 1 then 2; SESSION shows the room (the player in slot 1 as host, slots 1-2 open, 3-4 closed); the line reads "Room KFQB: waiting for players".

**Join.** Join Room > the keypad in room-code mode > START posts `ROOM_JOIN` with the code. `room` 1 ("Joining room KFQB"; "Connecting to room KFQB" while the online connection is still coming up), then 2, or back to 0 with the server's error in red ("Room not found.", "This room is full.").

**Leave.** Hold B: `ROOM_LEAVE`, then the menus (a room has no hold Z since 2026-10-12: P+ picks AltZ characters with Z held). The game must send `ROOM_LEAVE` whenever it leaves the room CSS other than for the room's match. Safety net: if the game reports `SCREEN_MENUS`, `SCREEN_ONLINE_*` or `SCREEN_OFFLINE` for 5 s while in a waiting room, Dolphin leaves the room itself.

**Kicked / host gone.** "Removed from the room.": `room` 0, `roomFlags` 0, red line. Host hand-over: `roomHost` and `SLOT_HOST` move.

**Slots, Teams, public.** Host only, between games (`roomStatus == 0`; public at any time). The server's answer shows in SESSION. The game may hide controls the player can't use; Dolphin and mm refuse them anyway.

**What the room plays (v6).** SESSION `RoomPlay` (+0x240): the room's ruleset (`bestOf`, `stageChoice` 0 loser picks any stage / 1 strike / 2 random / 3 loser picks from the list (v7; a v6 plugin gets 0), `custom`, `code`, `name`) and its set (`setOn`, `setGame`, `score` by side, `sides` as in-game ports, `setFlags` 1 paused / 2 can resume / 4 finished), from `room-state` while in a room. `CMD_RULESETS` (0xD1): the Ruleset line's list (ppom.h `RulesetList`: state, count, serial, 15 × {code, name}); Dolphin asks mm (`room-rulesets`) again when its list is older than 5 s and answers with what it has.

**The room's stages (v7).** SESSION grows to 0x2C0: `RoomStages` (+0x280) has the ruleset's starters and counterpicks as two 32-byte bit masks over stage ids 0-255 (id `k`: byte `k / 8`, mask `1 << k % 8`), from `room-state`'s `ruleset.stages` while in a room (zero from an older mm). The loser's stage select uses them (`stage_legal.cpp`): with stage choice 3 only they can be picked (the rest drawn with P+'s strike art, START's random among them) and a pick is played on its normal layout; with 0 the stage select is P+'s whole one and only its random (START) draws from them.

**The host's STAGE SWITCH (v7).** `CMD_ROOM` op `ROOM_STAGES` (12), ppom.h `RoomStagesRequest`: `choice` (0 any stage, 3 from the list, `0xFF` as it is) and the starters' and counterpicks' masks as in `RoomStages` (a stage on both is a starter). Dolphin sends `room-stages` (`rooms-protocol.md`).

**Rulesets and sets (v6).** The game's own Create Room (`ROOM_CREATE`) asks mm for the account's remembered ruleset (`room-create.remembered`); the launcher's create (`join-room.json` kind `create`) may carry `"ruleset": "<code>"`, which Dolphin passes on as `room-create.ruleset` (an invalid code: the request is ignored). A room playing a set runs its games in one session, GameSetup's steps on the room CSS (`docs/game-code.md` §14, Room sets); Dolphin reports each game as `room-result`.

**Room rules (v6, `docs/design/room-rules.md`).** On the room CSS SESSION `Rules` (+0x220) holds the room's rules from `room-state` (in a room game, the game's ticket's). The game opens P+'s rules menu over the CSS with them (`room_rules.cpp`); the host's edits come back as `ROOM_RULES` when the menu closes. A change of SESSION's rules takes back the game's lock-in (the server took back everyone's ready).

**Team colour.** `ROOM_TEAM`; shown in `roomTeam` once mm confirms. Dolphin passes the room's colour to the session with the lock-in, so the game does not need to write `lockTeam` (harmless if it does). "Pick different teams" comes as the status line.

**Ready.** START on the CSS = `writeLockIn(true, css, kind, costume, stagePick, asl, Session.game, &pv, pad)`, as Direct. `stagePick` is `STAGE_PENDING` when SESSION says this port `picksStage`, else 0xFFFF. Dolphin sends `room-ready {ready, character: charKind, costume}` when the lock-in for `Session.game` changes. Unlocking (a lock-in with `ready` 0) takes it back. During the start (`roomStatus == 1`) the lock-in must not change (as Direct's "character locked while searching"); mm refuses it anyway ("The game is starting.").

**The start.** When everyone is ready, mm starts the room by itself: `roomStatus` 1, line "Starting the game". Dolphin sends the room's game ticket (mode 3, a new P2P port), connects to every member and starts the gameplay session with **ports = slots** (also for 2 players, so P1 is not always the host) and **the room host as the decider**. From here it is Direct's flow: `LOCAL.state` 1 connecting, 2 connected; the picker's stage select once `LOCAL.remoteReady`; `Session.state == SS_MATCH_READY` with `Session.game` = the locked game: leave for the match. A failed start (a ticket missing for 15 s, a member left, the P2P connect failed): `roomStatus` back to 0, red line ("ALIC#4 didn't connect in time.", "A player left the room.", "Could not connect to players: ..."), `Session.game` + 1 (everyone readies again).

**After the match.** The game comes back to the room CSS as after any online match. Dolphin ends the session (`LOCAL.state` 0, `Session.state` `SS_NONE`), tells mm `room-back`, carries the loser's pick (`picksStage`) to the next room game and bumps `Session.game`. The panels show the characters (ready cleared). Players still in the game show `SLOT_IN_GAME`; the line is "Waiting for the game to end" until they are back. DISCONNECTED and DESYNC DETECTED work as in Direct, except that in a room's set a desync plays the same set game again from the state every machine agreed on, after everyone readies up (`game-code.md` §11, desync recovery).

**Launcher click.** Dolphin reads `join-room.json`. Busy (`screen` 4-7, or a search/session/room start): refused, nothing changes in the game. Idle: Dolphin sends the join itself (leaving any old room), bumps `Local.roomJoin`, and the game goes to the room CSS from wherever it is (menus or an idle online CSS; already on the room CSS: stay). While the game boots (until `screen` first reads 1-3 after a boot), a request, also one already there when Dolphin starts, is answered `accepted` at once and kept (at most 3 minutes from its `createdAt`), and nothing the game reports meanwhile counts as busy; then Dolphin joins and bumps `roomJoin`. The game should remember the last `roomJoin` it acted on (start from the value it finds at boot: Dolphin only bumps it later).

**Launcher Direct invite.** Dolphin reads a `join-room.json` of kind `direct` (validated: `connectCode` as the server's codes) and applies the same rules (busy: refused; booting: accepted at once and held until `screen` first reads 1-3). Accepted: Dolphin leaves any room (as `ROOM_LEAVE`), writes the code into `directPrefix`/`directNumber` and bumps `directJoin`. The game (`online_menu.cpp` `launcherDirect`), remembering the last `directJoin` it acted on as for `roomJoin`:
- on a menu page it can jump from (the main menu's top page, ONLINE, WITH FRIENDS, WITH ANYONE): the menus' decision 0x1E (as the room jump), the CSS entered as Direct's (`MODE_DIRECT`, back to WITH FRIENDS on Direct 1v1);
- on an idle online CSS of another mode, or a room's CSS: the CSS built again as Direct's (a room's records 1-3 put back first, `RoomCss::releaseRecords`), the character kept;
- on an idle Direct CSS: only the code changes;
- with the keypad open it waits until the keypad closes; on an online CSS that is searching or connected (the game started a search in the moment between Dolphin's check and the frame) it is dropped.

Then the status line reads "Select character vs SARA#123" / "Press START to play SARA#123", and START (with a character picked) is `FIND_OPPONENT` mode 2 with that code, no keypad. The code is used once: after that search (cancelled with Z/B, failed, or the set over) and after leaving the CSS, START opens the keypad as usual. DEBUG `scratch[9]` bits 24-31 count the invites the game took (tests).

**Launcher Play unranked / Play ranked.** A `join-room.json` of kind `queue` takes the Direct invite's path with the same rules: Dolphin leaves any room and bumps `Local.directJoin` with a zero `directPrefix` and `directNumber` 1 (Unranked) or 2 (Ranked). The game goes to that mode's CSS (`MODE_UNRANKED` / `MODE_RANKED`, back to the ONLINE page as from WITH ANYONE) from the same places: a menu page it can jump from, an idle online CSS of another mode or a room's CSS (built again), and on an idle CSS of that mode it stays. Nothing is searched: the player picks a character and presses START as usual. A plugin without this reads it as a plain Direct CSS. The same `scratch[9]` count.

**Launcher create.** A `join-room.json` of kind `create`, same rules (busy: refused; booting: accepted and held). Accepted: Dolphin sends `room-leave` if the player is in a room, then `room-create` (public), and bumps `roomCreate`; a refusal from mm answers the request `failed` and shows red on the line as any create error. The game (`launcherCreate`), remembering the last `roomCreate` it acted on: from a menu page it can jump from, the menus' decision 0x1E and the room CSS entered as Create Room's (`ROOM_ENTRY_CREATE` with its create marked sent; back to WITH FRIENDS on Create Room); from an idle online CSS of another mode, that CSS built again as the room's; on a room's CSS, it stays (the panels follow the new room; a ready from the old room is taken back). With the keypad open it waits for it to close; on a searching or connected online CSS it is dropped. DEBUG `scratch[9]` bits 8-15 count the creates taken (tests).

**The create's ruleset** (room-rules phase 3a, `docs/rulesets-protocol.md` §4). The launcher's create can carry `"ruleset": "<code>"`: `{version: 1, id, kind: "create", ruleset?, createdAt}`, the 6-letter code in Home's ruleset box (from Create room, or from a `brawlonline://room/create?ruleset=<code>` link, which also makes it the account's remembered ruleset first). The launcher writes it uppercase and checks it (`/^[BCDFGHJKLMNPQRSTVWXZ]{6}$/`); absent means P+ default (the box was empty). Dolphin passes it on as `room-create.ruleset`; mm looks the code up (`common::db::fetch_ruleset`) and the room starts with that ruleset's rules and set. Reading it in Dolphin and mm is the room-rules session's part.

**Not logged in / no server.** The line says so in red ("Log in in the launcher.", "Can't reach the server.", or mm's own `hello` refusal such as "Close Dolphin and update to 0.2.1"); Dolphin keeps retrying the online connection with backoff and sends a pending create/join once it is up.

---

## 4. ppom.h (v5), as committed with this document

```cpp
const u16 VERSION = 5;
const u8 CMD_ROOM = 0xD0;          // in enum Cmd
// RoomOp, RoomRequest, RoomPhase, RoomStatus: section 2 above.
enum Screen { SCREEN_UNKNOWN = 0, SCREEN_MENUS = 1, SCREEN_ONLINE_CSS = 2, SCREEN_ROOM = 3,
              SCREEN_ONLINE_BUSY = 4, SCREEN_MATCH = 5, SCREEN_OFFLINE = 6, SCREEN_OTHER = 7 };

struct Local {                     // only 0x36-0x3F and 0x7E change (0x39-0x3F, 0x7E: 2026-10-10)
    ...
    u8 lockPad;                    // 0x35
    u8 screen;                     // 0x36 game: Screen
    u8 roomJoin;                   // 0x37 Dolphin: bumped on an accepted launcher join
    u8 room;                       // 0x38 Dolphin: RoomPhase
    u8 directJoin;                 // 0x39 Dolphin: bumped on an accepted launcher Direct invite
    char directPrefix[4];          // 0x3A Dolphin: its code's letters, NUL-padded
    u16 directNumber;              // 0x3E Dolphin: its code's number (1-9999)
    ...
    u8 desynced;                   // 0x7D
    u8 roomCreate;                 // 0x7E Dolphin: bumped on an accepted launcher create
    u8 _reserved2[1];              // 0x7F
    PortValues own;                // 0x40
    ...
};
struct SessionPlayer {             // only 0x38-0x3F change
    ...
    u8 out;                        // 0x37
    u8 roomSlot;                   // 0x38 Dolphin: SLOT_* bits
    u8 roomTeam;                   // 0x39
    u8 roomChar;                   // 0x3A
    u8 roomCostume;                // 0x3B
    u8 _pad2[4];                   // 0x3C
    PortValues pv;                 // 0x40
    u32 _pad3;
};
enum RoomSlotBits { SLOT_OPEN = 1, SLOT_TAKEN = 2, SLOT_READY = 4, SLOT_HOST = 8, SLOT_IN_GAME = 0x10 };
enum RoomFlags { RF_IN = 1, RF_PUBLIC = 2, RF_TEAMS = 4 };
struct Session {                   // only 0x213-0x21F change
    ...
    u8 outCount;                   // 0x212
    u8 roomFlags;                  // 0x213
    char roomCode[4];              // 0x214
    u8 roomHost;                   // 0x218
    u8 roomStatus;                 // 0x219
    u8 roomMode;                   // 0x21A
    u8 quitPorts;                  // 0x21B
    u8 quitStays;                  // 0x21C
    u8 quitBy;                     // 0x21D
    u8 quitRunback;                // 0x21E
    u8 _reserved[1];               // 0x21F
};
```

static_asserts added in `ppom.cpp`: `offsetof(Local, screen) == 0x36`, `roomJoin == 0x37`, `room == 0x38`, `directJoin == 0x39`, `directPrefix == 0x3A`, `directNumber == 0x3E`, `roomCreate == 0x7E`, `offsetof(Local, own) == 0x40`, `sizeof(Local) == 0x80`; `offsetof(SessionPlayer, roomSlot) == 0x38`, `roomTeam == 0x39`, `roomChar == 0x3A`, `roomCostume == 0x3B`, `offsetof(SessionPlayer, pv) == 0x40`, `sizeof(SessionPlayer) == 0x80`; `offsetof(Session, roomFlags) == 0x213`, `roomCode == 0x214`, `roomHost == 0x218`, `roomStatus == 0x219`, `roomMode == 0x21A`, `sizeof(Session) == 0x220`; `sizeof(RoomRequest) == 0x18`, `sizeof(RoomStatus) == 0x88`, both within the payload sizes. Dolphin mirrors every offset in `GameBridge.cpp` and `tools/gamecode/ppom.py` reads them.

---

## 5. Notes

- **Equal on every machine?** The room fields in SESSION come from mm's pushes; two members can write them a frame apart, so SESSION may differ slightly between machines at a match start. Nothing compares SESSION between machines (the desync check uses the gameplay checksums), and the game does not read the room fields during a match.
- **Sounds.** Join / leave sounds: the game compares `SLOT_TAKEN` bits with what it last saw (4 bits).
- **After a room game, other players' cleanups.** Each Dolphin ends its room game's session once its own game has left the match scene, so a slower player's session can see the others leave right after game set: `LOCAL.disconnected` may read 1 for a moment on the results screen or the room CSS. Outside a match the room CSS ignores it (the room's own state, in SESSION, is what counts); in a match it is the usual DISCONNECTED.
- **L+R+A+START in a 3-4 player game** takes only the quitter out (`docs/nplayer/session.md`): the quitter's game goes back to the room's CSS ("Waiting for the game to end": mm marks it out of the game with its `room-back`), the others play on, and the next game includes the quitter as usual. In a 1v1, and in a 2v2 set, it ends the game for everyone, as in Direct. In a set the game did not happen (`room-result` with `winnerSide` 253; with L+R+Y it is played again on the same stage at once), and the CSS says who quit (`docs/game-code.md` §11, the quit per mode).
- **Leaving during the start** (`ROOM_LEAVE` while `roomStatus` is 1 or the session is connecting) ends this player's room game at once; the others' sessions see the player leave (a 1v1: their room game ends, "Lost the connection to the other players."; 3-4 players: the others' sessions go on without them; if the host had already set up the game with them, the match start fails and the others' room game ends with that error; not tested yet).
- **Diagnostics.** The harness's `rooms_status` (docs/harness-protocol.md) shows everything Dolphin knows; dolphin.log has a `Rooms:` line for every request, answer and state change.
- **Room games are never ranked**: Dolphin never reports them to `/v1/ranked/report-game`.
- Not in this interface (game's own business): the keypad's room-code mode, the panel controls, the host icon texture, the labels.
