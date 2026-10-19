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
| 7 | Open slots | ~~Open slots must fill before a game starts.~~ **Replaced 2026-10-18** (§1.2): open slots wait for nobody. A game starts once every player in the room is ready or sitting out and at least two play; an open slot is only where someone can join. |
| 8 | Host | The creator. **Marked with a reused Brawl icon** next to their name plate (which icon: RE, §4). |
| 9 | Host leaves | **Host passes to the earliest-joined player**; the icon moves; the room keeps its code. The room closes when the last player leaves. |
| 10 | Kick | **Host closes an occupied slot = kick**, between games only, by **holding A** on the player's panel (48 frames; a tap only buzzes, so a press meant for the team flag kicks nobody). With only 2 slots open the kick leaves the slot open (a 1v1 room's player can be removed too; player feedback 2026-10-12). An empty slot opens or closes with a press. The kicked player goes idle with "Removed from the room." **Not a ban** (user decision, 2026-10-10): they may join again once a slot is open, so a misclick is undone by opening the slot. |
| 11 | Teams or FFA | **Host toggles Teams on/off** for the room. With Teams on, **each player picks their own color** on their own panel (Slippi Teams): A on their panel or its flag, as P+'s team battle. **Any costume on any team** (the user's, 2026-10-12): X / Y change the costume, as in FFA and in P+'s rainbow TEAM BATTLE (the user's, 2026-10-15, after bug #22; until then X / Y were the team and A the costume), and the game plays the costume picked, not the team's. |
| 12 | Splits | **Any split except everyone on one color** (2v2, 2v1, 3v1 allowed, as Brawl's Versus). All on one color: status "Pick different teams", no start. With 2 players the Teams switch has no effect: 1v1 (**default**). |
| 13 | Start | **Automatic once every player is ready or sits out, and two or more play** (§1.2; until 2026-10-18 every open slot had to be filled). START = lock in (ready); unlocking takes the ready back. As Direct. |
| 14 | Stage | Game 1 random from the legal list (Slippi). Then the **loser picks**: 1v1 the loser; Teams the losing team's lower slot; FFA last place (a tie: the lower slot, **default**). A draw: the lower slot picks (2026-10-09: everyone locks in first, then one player picks). |
| 15 | Other players' panels | **Username, "Ready" / "Choosing...", and their character and costume** as soon as their coin is put down (§1.2, 2026-10-18; until then only once they were ready). No live cursor or hover sync: a coin still in the hand shows nothing. |
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

### 1.2 Open slots, sitting out and the placed coin (the user's, 2026-10-18)

Asked for by players: start a 3-4 slot room with two people and let others join meanwhile; see the opponent's character before readying; rotations in a room of three (one sits out each game, by whatever rotation the players use: loser out, even turns).

| # | Topic | Decision |
|---|---|---|
| 21 | Open slots | **Always play with open slots.** All players fully readying up starts the game; nobody waits for an empty slot. A player who joins during a game waits on the CSS ("Waiting for the game to end") and plays the next one, which waits for them as for anyone. |
| 22 | Placed coin | **The character goes to the others when the coin is put down (A), not at START.** Others see it on that player's card with "Choosing...". START with the coin still in the hand readies nobody (it readied the player with the portrait under the hand; all online CSSs, Direct and Unranked too). Game 1 of a set stays a blind pick (the server withholds it). |
| 23 | SIT OUT | **A SIT OUT / PLAY button** under RULES (level with FFA), **the player's own name plate** (offline Brawl's player / CPU switch; its pencil still opens the name tags and their controls), or START toggle it. **START while sitting out plays again, it does not ready.** The host may toggle anyone (A on their plate); the others only themselves. Sitting out takes a ready back. |
| 24 | Look | A card sitting out is **a CPU's grey card**, its plate reading **SIT OUT** for everyone, no state box. |
| 25 | Lasts | **Sitting out stays until the player plays again** (START, the plate, the button); ready is still cleared after every game. |
| 26 | Joiners | **Join playing** (not sitting out). |
| 27 | Stage pick | Loser picks: **when the last game's loser sits out (or left), a random player of the next game picks** (mm's draw, one per game, the same on every screen). A player sitting out never picks. A draw's other picker still picks. |
| 28 | Sets | A set starts between the players who play then (a 1v1 set in a room of three: one sits out). **Everyone else sits out until the set is over** (joiners too) and can't play; the set's players can't sit out ("Wait for the set to end."). With more players than two sides, the note reads "Sets need 1v1 or 2v2: SIT OUT". |
| 29 | Too few | Only one player not sitting out: **"Need 2 players"**. |

Defaults chosen without asking: the room's host who sits out stays the host; the game's decider is then the earliest-joined player of the game (mm's `room-start.host`). A player who sits out can toggle back during a game they are not in. Their coin still shows (grey card). Watching the game in progress while sitting out (the user's, 2026-10-18: "i think yes") is §1.3.

### 1.3 Watching the room's game (the user's, 2026-10-19)

A member who is not in the room's game (sitting out, or joined during it) watches it live in their own game, from the room CSS, and is back on the room CSS for the next game.

| # | Topic | Decision |
|---|---|---|
| 30 | Start | **Automatic.** Everyone in the room who is not in the game is taken into watching it from the room CSS once its setup comes, in the players' countdown: the watcher's stage loads while the first frames come (about a second after the players' GO; the upload trickles from the game's start, `spectating.md` §4.3 "The start"), and its countdown starts with them, so it watches about 4-5 s behind (2026-10-19; about 13 s or more before). No WATCH button. A player who joined well into a game catches up to the live edge first, as a spectator does. |
| 31 | Leave early | **START or B** ends the watch at once: back on the room CSS, and nothing more of that game is watched. The room's next game is watched again. |
| 32 | The next game | **Finish, never block.** The watcher sees the game's end (normally about 4-5 s behind), then goes back to the room CSS. The room never waits for a watcher: while watching, a member is not waited for (a joiner who would play next too), so the next game can start without them, and they watch that one. |
| 33 | The end line | Once the players' game is over and it still plays on the watcher's screen: **"Game ended, press B to return to CSS."**, a line top left (Dolphin's on-screen messages). |
| 34 | Launcher | **Not busy.** A launcher click (another room, an invite, Play unranked/ranked) while watching ends the watch and goes ahead once the game is back on the room CSS. |
| 35 | Others' cards | A watcher's card is the grey SIT OUT card with **WATCHING** on its plate (default, not asked). |

How it works (`docs/rooms-protocol.md` §2 `room-watch`, `docs/rooms-game-interface.md` "Watching the room's game"): the players' games upload the game exactly as for spectators (nothing changes on their side, so their games are unaffected); mm sends the kept chunks to the room's watchers on the online connection's watch channel (ENet channel 2), and the watcher's Dolphin plays them as a live replay (`Online/RoomWatch.cpp`, the playback of `spectating.md` §6.2 in the netplay Dolphin). A game nobody streams (`MM_SPECTATE` off, or every player's "Let others stream my games" off) is not watched: the watcher stays on the CSS with "Waiting for the game to end", as before.

## 2. Defaults (not asked)

| Topic | Default |
|---|---|
| Who may join | Anyone with the room code. The random code is the consent; no friends list. |
| Errors (status line, red, server text) | "Room not found.", "This room is full.", "Removed from the room." Version mismatch: the server refuses with its text, as Direct. |
| Joining during a game | Into an open, empty slot. The joiner waits on their own CSS: "Waiting for the game to end", and plays the next game (§1.2). |
| Ports | Slot = in-game port, stable for the life of the room. Port gaps allowed (P1, P3). |
| Rules | P+ competitive (4 stocks, 8 minutes, items off), pause on, as Direct. |
| After a game | Everyone back on the room CSS, characters kept, ready cleared. |
| Disconnect in a match | 1v1: today's DISCONNECTED flow. 3-4 players: the dropped player is despawned at an agreed frame and the match goes on (Slippi's rule); their slot stays open and empty. |
| Leaving | Hold B: leave to the menus (WITH FRIENDS, the room's button highlighted). No hold Z (the user's, 2026-10-12): it was Direct's, carried over, and P+ picks AltZ characters with Z held, so picking one left the room. |
| Host limits | The host can't close their own slot. At least 2 slots stay open (a kick of 2 open leaves its slot open). |
| Ranked / Unranked | Rooms are never ranked and never change a rating. Matchmade sessions stay closed. |
| Recent codes | None for rooms (codes are throwaway). |

## 3. Status lines

| Situation | Text |
|---|---|
| Room created, waiting | "Waiting for players" (the code is in the top window; 2026-10-10: the line was shortened so its text stays large next to the room's buttons) |
| Open slots still empty | "Waiting for players (2/4)" |
| Everyone present, some not ready | "Waiting on: BO, CY" |
| Teams all one color | "Pick different teams" |
| Your stage pick | "Press START to select stage" (Direct's) |
| Another player on the stage select (the room's game started, everyone locked in) | "P2 choosing stage..." (the picker's slot; also in Direct) |
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
