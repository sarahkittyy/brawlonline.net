# Rooms: Direct 1v1, Create Room, Join Room (design, nothing built)

_2026-10-09. The user's decisions from a question round, plus the defaults chosen without asking (marked **default**: say if one is wrong). This replaces TEAM BATTLE (Slippi's shared-code Teams) and settles most open questions of `drop-in-ux.md` §9: a room is Concept 2 ("everyone plays") with its own code, host and slots._

---

## 1. Decisions

| # | Topic | Decision |
|---|---|---|
| 1 | Order of work | **3-4 player rollback first.** Rooms ship together with 2v2 and free-for-all, not as a 1v1-only lobby. |
| 2 | Menu | WITH FRIENDS page: **Direct 1v1 / Create Room / Join Room**, three buttons. If the third button needs serious menu RE, the RE is done anyway (no fallback to two buttons). Labels are textures rendered from the game's own font (as Orca's `make-labels.py` does). No AI art. |
| 3 | Direct 1v1 | Today's Direct, unchanged (**default**). |
| 4 | Joining | Join Room opens the room CSS; START opens Brawl's keypad in a **room-code mode**; START on the keypad joins. Same screens as Direct. |
| 5 | Room code | **4 letters, no vowels** (20 letters: `BCDFGHJKLMNPQRSTVWXZ`, Y left out too; 160,000 codes), e.g. `KFQB`. Letters page only, no '#', no page switching. Unique among live rooms; freed when the room empties (**default**). Shown at the top of the CSS, in the window that shows the connect code in Direct (**default**). |
| 6 | Slots | 4 panels. A new room has slots 1-2 open, 3-4 **closed**. Open and empty = "Searching...". **Only the host** opens and closes slots, between games only. |
| 7 | Open slots | **Open slots must fill before a game starts.** The open slots are the player count; to play 3, the host closes a slot. |
| 8 | Host | The creator. **Marked with a reused Brawl icon** next to their name plate (which icon: RE, §4). |
| 9 | Host leaves | **Host passes to the earliest-joined player**; the icon moves; the room keeps its code. The room closes when the last player leaves. |
| 10 | Kick | **Host closes an occupied slot = kick**, between games only. The kicked player goes idle with "Removed from the room." |
| 11 | Teams or FFA | **Host toggles Teams on/off** for the room. With Teams on, **each player picks their own color** on their own panel (Slippi Teams). |
| 12 | Splits | **Any split except everyone on one color** (2v2, 2v1, 3v1 allowed, as Brawl's Versus). All on one color: status "Pick different teams", no start. With 2 players the Teams switch has no effect: 1v1 (**default**). |
| 13 | Start | **Automatic once every open slot is filled and every player is ready.** START = lock in (ready); unlocking takes the ready back. As Direct. |
| 14 | Stage | Game 1 random from the legal list (Slippi). Then the **loser picks**: 1v1 the loser; Teams the losing team's lower slot; FFA last place (a tie: the lower slot, **default**). A draw: both pick, as Direct (**default**). |
| 15 | Other players' panels | **Username, "Ready" / "Choosing...", and their character and costume once they are ready** (carried by the lock-in that already exists). No live cursor or hover sync. |
| 16 | Sounds | **Join: an existing Brawl menu sound. Leave: the back sound** used today when an opponent leaves. |

### 1.1 Launcher: players online and public rooms

The two empty columns of Home > Overview (where Slippi shows news and tournaments) show the number of players online and the list of public rooms.

| # | Topic | Decision |
|---|---|---|
| 17 | Public / private | A room is **public by default**. The host toggles it with **a controller button on the room CSS** (e.g. Y, at any time). The top window shows "KFQB Public" / "KFQB Private". A private room is joinable by code only. |
| 18 | Room row | **Host name, players (3/4), mode (1v1 / FFA / Teams), status (Waiting / In game), and every player's name.** |
| 19 | Click, game not running | **Launch and go straight in**: Play starts, the game skips the menus and lands on that room's CSS, joining. |
| 20 | Click, game running | From the menus or an idle online CSS, the game jumps to the room. **Busy** (in a match, searching Unranked or Ranked, in a single-player mode): **refused with a launcher message**, "Finish your current game first"; nothing changes in the game. |

Defaults: "online" counts players whose game is running and logged in (Dolphin keeps a heartbeat with the server), not launchers left open. The list updates live (server push or a few-second poll). Full rooms are shown but can't be clicked; "In game" rooms can be clicked only if a slot is open and empty (the joiner waits on the CSS). The launcher hands the room code to Dolphin through a file next to `user.json`, which Dolphin already watches; the game reads it from the mailbox.

## 2. Defaults (not asked)

| Topic | Default |
|---|---|
| Who may join | Anyone with the room code. The random code is the consent; no friends list. |
| Errors (status line, red, server text) | "Room not found.", "This room is full.", "Removed from the room." Version mismatch: the server refuses with its text, as Direct. |
| Joining during a game | Only into an open, empty slot (one freed by someone leaving). The joiner waits on their own CSS: "Waiting for the game to end". |
| Ports | Slot = in-game port, stable for the life of the room. Port gaps allowed (P1, P3). |
| Rules | P+ competitive (4 stocks, 8 minutes, items off), pause on, as Direct. |
| After a game | Everyone back on the room CSS, characters kept, ready cleared. |
| Disconnect in a match | 1v1: today's DISCONNECTED flow. 3-4 players: the dropped player is despawned at an agreed frame and the match goes on (Slippi's rule); their slot stays open and empty. |
| Leaving | Hold Z: leave the room, idle on the CSS. Hold B: leave to the menus (WITH FRIENDS, the room's button highlighted). As Direct. |
| Host limits | The host can't close their own slot. At least 2 slots stay open. |
| Ranked / Unranked | Rooms are never ranked and never change a rating. Matchmade sessions stay closed. |
| Recent codes | None for rooms (codes are throwaway). |

## 3. Status lines

| Situation | Text |
|---|---|
| Room created, waiting | "Room KFQB: waiting for players" (the code is also in the top window) |
| Open slots still empty | "Waiting for players (2/4)" |
| Everyone present, some not ready | "Waiting on: BO, CY" |
| Teams all one color | "Pick different teams" |
| Your stage pick | "Press START to select stage" (Direct's) |
| Joined during a game | "Waiting for the game to end" |
| Joining | "Joining room KFQB" > "Connecting to room KFQB" |

## 4. Reverse engineering needed (game side)

1. **A third button on the WITH FRIENDS page** (Brawl's WITH ANYONE page, `muProcWifiAnybody`, two image buttons), and font-rendered label textures for all three.
2. **The CSS with 4 panels.** Brawl's Wi-Fi CSS (`sqNetAnyOkiraku`) shows only the player's own panel today, as on Slippi. Find which CSS state draws 4 panels (`sqNetAnyTeamMelee`'s?) and how to print a name and "Searching..." / "Ready" / "Choosing..." on each, and how to show a remote player's character and costume in a panel.
3. **Slot toggle:** the panel control the host presses to open or close a slot (the Versus panel's HMN/CPU/NA button, if the Wi-Fi CSS has it).
4. **Team color** on each player's own panel, and the room's Teams/FFA switch (Brawl's CSS rule toggle, if the Wi-Fi CSS has one).
5. **The host icon:** a Brawl texture that fits next to the name plate, and how to draw it on another player's panel.
6. **Keypad room-code mode:** 4 letters from the 20-letter set, no '#' key, the other letters' keys refused with the error sound.

## 5. Order of work

1. **3-4 player gameplay rollback.** The session runs 2 players today (`gameplay-rollback-status.md`; SESSION is sized for 4). Needed: N-player sessions over GekkoNet, connections between every pair (or the relay), the match setup for N players and port gaps, despawn at an agreed frame, determinism checks with 3-4 fighters on the stage (region set, sync tests, network sessions under `netsim`).
2. **Server rooms:** create (code generation), join, leave, roster, slots, host and host hand-over, Teams switch, public flag, readiness, kick; pushed to every member. Replaces the Teams ticket path. Plus the online count (heartbeat) and the public room list for the launcher.
2b. **Launcher:** the online count and the room list on Home > Overview; click to join (launch if needed, or hand the code to the running game; refuse when busy).
3. **Dolphin:** room state between the server and the game's mailbox; connections to every member; the lock-in (character, costume, team, port values) shared with the room; start the session when all are ready.
4. **Game plugin:** the menu page, the keypad mode, the 4-panel CSS (names, ready, characters, host icon, slot and team controls), status lines, sounds.
5. **Tests:** harness tests with 2, 3 and 4 instances: create, join by code, slots, kick, host leaves, Teams split rules, loser's pick, a dropped player despawned.
