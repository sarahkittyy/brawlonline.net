# Drop-in play: UI and user flows (design draft, nothing built)

_2026-10-08. A design for the user to decide on before any drop-in work starts. It covers screens and flows only, not the engine. Each concept ends with a short note on what it would need technically._

Sources: `docs/game-code.md` §6 and §11 (today's online screens and flow), `docs/backend-design.md` §1.5, §5.1 and §5.6, `docs/gameplay-rollback-status.md` (header), `research/04-slippi-reference-architecture.md` §4.3-4.7 (Slippi's flows), `refs/orca-netplay/ORCA.md` (Drop-in, Port values, Rooms, Going home, Online menu, Matchmaking and results, On-screen UI), `research/03-hbox-crunch-controversy.md` §3, `launcher/PPLUS_PORTING.md`.

---

## 0. Summary

| | Concept 1: **Winner stays** | Concept 2: **Everyone plays** | Concept 3: **Drop into the host's game** (Orca) |
|---|---|---|---|
| In one line | A Direct set that friends can join by code. Games stay 1v1; up to two extra players wait their turn and rotate in between games. | A code lobby of 2-4 where every seated player is in every game: 1v1, then a 3- or 4-player free-for-all, or 2v2 on TEAM BATTLE. New players take a seat between games. | The host plays anything. A friend's game is replaced by a copy of the host's machine and plugs in as the next port, wherever the host is. |
| How a friend joins | Enters a seated player's connect code on Brawl's keypad (WITH FRIENDS > BASIC VERSUS) | The same; TEAM BATTLE keeps Slippi's shared code | Enters the host's code; the friend's own game is thrown away |
| When they join | On the CSS between games. Mid-match, they wait on their own CSS. | The same | Anywhere except single-player modes |
| Fits gameplay-only rollback | Yes. Every match is still a 2-player session. | Yes, but it needs 3-4 player sessions (Teams needs them anyway) | No. It needs whole-machine state transfer and whole-machine rollback over the menus. |
| Data sent to join | Selections and port values, under 1 KB | The same | 26-30 MB keyframe (Orca), 5.7-7.2 s per join |
| Ranked and Unranked | Untouched (closed sessions) | Untouched | Orca needed special "leave the lobby for Ranked" logic |
| Cost | Small: a lobby on top of today's Direct session | Medium: N-player sessions, despawn on disconnect | Very large: a second session model |

**Recommendation (§8):** build Concept 1 first. It works on the 2-player session we have and reuses Direct's rules and screens. Build Concept 2's "refill a seat between games" together with Teams, on the same lobby code, once 4-player sessions exist. Reject Concept 3.

---

## 1. Where we start from

What a player does today (`game-code.md` §11):

1. Launcher: log in, Play. The launcher header shows the player's name and connect code (Slippi's `user_info`).
2. Game: main menu > PLAY ONLINE > ONLINE page > WITH FRIENDS > BASIC VERSUS (Direct) or TEAM BATTLE (Teams; the server refuses it for now).
3. Brawl's Wi-Fi CSS, P+ rules. The player sees only their own panel, as on Slippi. The rule line in the header is the single **status line**, using Slippi's strings.
4. START opens Brawl's name keypad in code mode (8 characters, `#`, recent codes in grey with L/R, Z accepts). START on the keypad searches: "Searching for ABCD#123" > "Connecting to ABCD#123" > "Playing: <name>".
5. Both are locked in, so the match starts. Back on the CSS after the game, still connected, the character still placed. "Press START to lock in". The loser reads "Press START to select stage" and picks on P+'s stage select.
6. Hold Z (48 frames) disconnects. Hold B leaves to the menus. If the opponent drops in a match, "DISCONNECTED" appears in red on the HUD, the game ends without "GAME!", and the player is back on the CSS, idle.

Facts that shape this design:
- **Gameplay-only rollback.** Each player boots alone and uses their own menus. The session lives on the CSS: players exchange lock-ins (character, costume, stage pick, port values), the host's Dolphin decides the setup, and each game builds the same match. Only the match is rolled back. So **the CSS between games is the natural join point**: a newcomer brings a lock-in, and their own game is never touched.
- SESSION already has `players[4]` by in-game port and `numPlayers`. The rollback session itself runs 2 players today.
- Slippi has **no drop-in, no lobby, no friends list and no invites** (research 04 §4.4). Its "recent opponents" feature is the code history on the keypad. Its only group feature is **Teams**: 4 players enter the same code and wait until all 4 are there. In 3-4 player games a disconnected player is despawned and the match goes on (research 04 §4.7).
- Every in-game join action has to work with a controller only (Steam Deck). The keypad already does.

---

## 2. What "drop-in" could mean

| # | Meaning | Example | Worth it? | Why |
|---|---|---|---|---|
| M1 | **A 3rd or 4th friend joins an existing set between games** | Anna and Bo play Direct. Cy enters Anna's code and is in from the next game. | **Yes** | The core request. It fits the CSS session model with no state transfer. |
| M2 | **Joining while a game is running** (you get in at the next game) | Cy enters the code mid-match, waits on his own CSS and is in from the next game | **Yes**, as part of M1 | The join itself happens on the joiner's CSS, so nobody has to wait for a transfer |
| M3 | Joining into the *middle* of a running match | Cy appears as P3 at 2 stocks in | **No** | Needs gameplay state transfer and 3-player rollback mid-match. Nobody asked for it, and it is unfair to the players already in the match. |
| M4 | Joining a host who is anywhere (single-player mode, menus, training) | Anna is in Classic; Cy joins her game | **No** | This is Orca's model (Concept 3). It needs whole-machine transfer and shared menus, which the user's gameplay-only choice ruled out. Even Orca holds joins while the host is in a single-player mode. |
| M5 | A waiting player watches the current match, then plays | Cy watches Anna vs Bo, then rotates in | **Later** | Spectating is dropped for the first release (`backend-design.md` §1.9). Concept 1 is built so spectating can slot in later. |
| M6 | 3-4 player free-for-all lobbies | Anna, Bo, Cy and Dee in one FFA | **Maybe** | Brawl's own With Friends was a 2-4 player brawl. Slippi has FFA only as matchmade Party mode, not by code. It needs N-player rollback. Open question 1. |
| M7 | Teams seats refilled between games | Dee leaves a 2v2; Eve enters the code and takes the seat | **Yes, with Teams** | The cheapest kind of drop-in once Teams exists |
| M8 | Rejoining after a disconnect | Bo's Wi-Fi drops. He enters Anna's code again and is back next game. | **Yes**, it comes free with M1 | Same flow as M1 |
| M9 | A friend joins your Unranked or Ranked search or set | Cy tags along into Anna's Ranked set | **Never** | It would undermine Ranked's integrity. Matchmade sessions stay closed (§3.6). |
| M10 | A second controller on the same machine joins online (couch + online) | Anna's roommate plays P2 on her PC | **No** | Slippi allows one human per machine online. It would also change the input format. Not requested. |

---

## 3. Shared parts (Concepts 1 and 2)

### 3.1 Finding and inviting a friend

| Option | Slippi has it? | Use it? |
|---|---|---|
| **Connect code on Brawl's keypad**, as Direct does today | Yes | **Yes**: the only way in |
| **Recent codes** (grey completion text, L/R, Z) | Yes ("recent opponents") | **Yes**: a friend who played the lobby before rejoins with a few presses, controller only |
| The code in the launcher header | Yes | **Yes**, unchanged. That is where a host reads their code to tell a friend. |
| Launcher friends list / online presence / "invite" button | No | **No**: it would be invented, and needs a presence service. Open question 7. |
| Separate room codes (Orca: 6-12 of `a-z0-9`) | Partly: Teams lets you type "any code" as a lobby key | **No** for Direct. TEAM BATTLE keeps Slippi's shared code. |

The invite happens outside the game (Discord, text, voice), exactly as for Slippi Direct today. This is the main "invent nothing" choice.

### 3.2 The join rule

- **Direct (BASIC VERSUS):** the first two players pair as today: each enters the other's code. **New:** a third or fourth player enters the code of **any player already in the lobby**, and the server adds them to it. If nobody with that code is in a lobby, the ticket waits, "Searching for ABCD#123", exactly as Direct waits today. *(Invented: Slippi Direct only ever pairs two tickets that name each other.)*
- **Teams (TEAM BATTLE):** Slippi's rule, unchanged: everyone enters the same code. **New:** a seat that empties can be refilled between games by anyone who enters that code.
- **Consent.** Slippi Teams lets anyone who knows the code in. For Direct, the same rule means anyone who knows a seated player's code could join a friendly. The options are in open question 2. The default proposed here: **only someone the named player has played Direct with before** (their code is in that player's recent Direct codes, `direct-codes.json`). Dolphin checks this silently, so no new UI is needed. A first-time friend first plays a normal mutual-code Direct with one seated player, or that player enters their code once.
- **A full lobby** (4 seats) refuses the ticket. The joiner sees the server's text in red on the status line: "This lobby is full." *(new server string)*

### 3.3 Status line strings

The online CSS has one status line (the rule-line window). Slippi's strings stay where they apply. The new strings are marked.

| Situation | Text | Source |
|---|---|---|
| Idle | "Select your character" / "Press START to enter code" | Slippi |
| Searching / connecting | "Searching for ABCD#123" / "Connecting to ABCD#123" | Slippi |
| In the lobby, about to play | "Playing: BO" (FFA or teams: "Playing: BO/CY/DEE", Slippi joins team names with "/") | Slippi |
| Locked in, others not | "Waiting on opponent" | Slippi |
| Between games | "Press START to lock in" / "Press START to select stage" | Slippi (§11) |
| **Waiting for a turn or for the running game to end** | "Waiting: ANNA vs BO" | **New** |
| **Someone else is waiting (Concept 1, shown to the two playing)** | "Playing: CY, next: DEE" | **New** |
| Connect failure to one member | "Could not connect to players: DEE" | Slippi (Teams) |
| Full, wrong version, refused | the server's text in red, e.g. "This lobby is full." | Slippi's error style, new server text |

Nothing else is drawn: no panels for other players (Slippi shows none), no new art, no overlay. Sounds: the existing ones only (back sound when someone leaves, error sound on an error). Whether a join should make a sound is open question 6.

```
 Brawl's Wi-Fi CSS, as a waiting player sees it (Concept 1):
 +--------------------------------------------------------------+
 | [rule line] Waiting: ANNA vs BO                               |
 |                                                              |
 |              P+ character grid (unchanged)                    |
 |                                                              |
 |  +------------+                                   [ LEAVE ]  |
 |  | your panel |  your character, costume and name tag;       |
 |  |            |  free to change while you wait               |
 |  +------------+                                              |
 +--------------------------------------------------------------+
```

### 3.4 Name tags and controls

Unchanged from §11 "Each player's own controls": every player picks their tag on their own CSS with Brawl's name button, and the tag's layout travels as port values with every lock-in. A player who changes their tag between games (A on the name plate while not locked in) plays the next game with the new controls. Nobody ever plays on another player's save. This is the opposite of Orca's original behaviour, where the name button was masked and players complained about it (research 03 §3, row 4).

### 3.5 Leaving

| Action | Where | Effect on the leaver | Effect on the others |
|---|---|---|---|
| Hold Z (48 frames) | CSS | Leaves the lobby, back to idle on their CSS (Slippi) | The lobby goes on without them; their seat frees; the back sound plays |
| Hold B / LEAVE | CSS | Leaves the lobby and goes to the menus (Slippi) | Same |
| Closes Dolphin, loses the connection | Anywhere | — | In a match: today's DISCONNECTED flow for a 1v1; despawn for 3-4 players (Concept 2). On the CSS: they just leave the lobby. |
| The lobby's first seat (the "host") leaves | Anywhere | — | **Nothing visible.** The next seat takes over the host's work (deciding setups and the order). With gameplay-only rollback the host holds no game state, so there is nothing to hand over. *(Invented: Slippi Teams has no host migration. It is needed because players never see who the host is.)* |
| The last but one leaves | CSS | — | The last player goes idle with the back sound, as after a Slippi Direct disconnect |

### 3.6 Ranked and Unranked stay closed

- The server never adds a ticket to a session that matchmaking made (Unranked, Ranked). Drop-in exists only under WITH FRIENDS.
- A friend who enters the code of someone who is searching or playing Unranked or Ranked just keeps "Searching for ABCD#123" until that player is in a WITH FRIENDS lobby. They never pull the player out, and they learn nothing about the ranked player's state.
- A player in a lobby who wants to play Ranked leaves it (hold B), goes to WITH ANYONE > Ranked, and the lobby continues without them. This is Slippi's path, and it avoids Orca's "kept pick" logic ("Casual or Ranked with friends in the game").
- Lobby games are never reported as ranked and never change a rating.

---

## 4. Concept 1: Winner stays (Direct with a rotation)

Every game is 1v1 under Direct's rules. Up to two more players wait in the lobby. After each game, the winner stays and the loser goes to the back of the line. This is how friends usually share one setup, and it keeps P+'s 1v1 game.

### 4.1 Host flow (Anna)

1. **Launcher:** log in, Play. Her code ANNA#123 is in the header; she tells Bo and Cy on Discord.
2. **Game:** main menu > PLAY ONLINE > WITH FRIENDS > BASIC VERSUS. Brawl's Wi-Fi CSS: "Select your character".
3. She picks Marth, presses START: Brawl's keypad. She enters BO#456 (or picks it from recent codes with L/R, Z), presses START. "Searching for BO#456".
4. Bo enters ANNA#123: "Connecting to BO#456" > "Playing: BO". Game 1 starts as today (random stage from the server's list).
5. **Cy joins during game 1.** Anna sees nothing in the match. Nothing is shown in the HUD.
6. Back on the CSS after the game (Anna won): "Playing: CY". She presses START: "Waiting on opponent".
7. Game 2: Anna vs Cy, on the stage Cy picked. If Anna loses, her line reads "Waiting: CY vs BO" and she waits on her own CSS. She can change character, costume or tag meanwhile.
8. **Leaving:** hold Z on the CSS (leaves the lobby, stays on the CSS) or hold B (back to WITH FRIENDS' page, BASIC VERSUS highlighted). The others go on.

### 4.2 Joiner flow (Cy)

1. **Launcher:** log in, Play.
2. **Game:** PLAY ONLINE > WITH FRIENDS > BASIC VERSUS > picks a character > START > keypad: ANNA#123 > START.
3. "Searching for ANNA#123" > "Connecting to ANNA#123" (the usual hole punch, to Anna and to Bo).
4. Game 1 is running: "Waiting: ANNA vs BO". Cy stays on his CSS. His character is **not locked**, so he can still change it.
5. Game 1 ends; Anna won, so Bo goes to the back. Cy reads "Press START to select stage". *(Reuses Direct's loser's pick: the challenger picks, on P+'s whole stage select.)* He picks, comes back locked in, "Waiting on opponent" until Anna locks in.
6. Game 2: Anna vs Cy.
7. Leaving: as for Anna.

### 4.3 What each screen shows

| Screen (reused) | Player in the next game | Waiting player |
|---|---|---|
| Launcher Home | Unchanged | Unchanged |
| Main menu > ONLINE > WITH FRIENDS page | Unchanged | Unchanged |
| Brawl's keypad (code mode) | Unchanged | Unchanged; the code is that of any seated player |
| Brawl's Wi-Fi CSS | "Playing: CY" or "Playing: CY, next: DEE"; Slippi's lock-in strings | "Waiting: ANNA vs BO". Character free to change. |
| P+'s stage select | The challenger, or the loser when there are only two, picks as in Direct today | — (stays on the CSS) |
| Load screen, match (`scMelee`) | Unchanged | — (stays on the CSS) |
| Back on the CSS | Today's flow | Their turn: "Press START to select stage" |

### 4.4 Rotation and ports

```
 Lobby: ANNA (seat 1), BO (seat 2), CY (seat 3)        line: CY
 Game 1  ANNA vs BO   (random stage)    ANNA wins  ->  line: CY, BO
 Game 2  ANNA vs CY   (CY picks)        CY wins    ->  line: BO, ANNA
 Game 3  CY vs BO     (BO picks)        ...
```

- **Winner stays, loser to the back.** With only two in the lobby, it is plain Direct (the loser picks).
- **A draw:** nobody rotates. The same pair plays again, with Direct's draw rule (both may pick).
- **A no contest** (the pause screen's quit in Direct): the player who quit counts as the loser. A **disconnect**: that player leaves the lobby.
- **In-game ports:** the earlier seat is P1, the other P2, in every game. This keeps today's 1v1 match setup unchanged. Seats are the order of arrival; a seat frees when its player leaves.
- **Who decides the setup:** the player in the earlier seat among the two playing, as the "host" does today. The lobby's order (who is next) is kept by the lowest seat and sent to everyone.
- Open: a cap on winning streaks (for example, the winner also rotates out after 3 wins in a row), question 4.

### 4.5 Edge cases

| Case | What happens |
|---|---|
| Host in a single-player mode (Classic, Training...) | There is no lobby. The joiner's ticket waits, "Searching for ANNA#123", as Direct does today; Z cancels. No change. |
| Host in a match | The joiner connects at once and waits on their own CSS: "Waiting: ANNA vs BO". They play from the next free turn. |
| Host on the stage select (loser's pick) | Same as in a match: the pick belongs to the game being set up. The newcomer goes to the back of the line. |
| Host on the keypad or still searching for Bo | Not a lobby yet. Cy's ticket waits; once Anna and Bo connect, Cy is let in. |
| 3rd and 4th player | Both join the line in order of arrival. A 5th gets "This lobby is full." |
| Host leaves | Nothing visible. Seat 2 takes over the host's work. If one player is left alone, they go idle with the back sound. |
| Joiner leaves while waiting | Their seat frees and they leave the line. The players see the "next:" name change. |
| A player in the match drops | Today's DISCONNECTED flow for the other player. The dropped player leaves the lobby, and the next in line plays the remaining player. |
| A desync (when detection exists) | The game ends as a no contest for both, as on Slippi. Nobody rotates; the same pair plays again. |
| Joiner is searching Unranked or Ranked | One search at a time, as on Slippi: Z cancels it, then WITH FRIENDS. |
| Someone names a player who is in Ranked | §3.6: the ticket waits; the ranked player is never disturbed. |
| Controller unplugged or changed mid-session | Dolphin's own mapping, local to that machine, as today. An unplugged pad sends neutral input. Nothing in the lobby changes. On Deck, Steam Input. |
| Name tag changed while waiting | Allowed. The new controls travel with the next lock-in. |
| Next player is idle (AFK) when their turn comes | Slippi waits forever ("Waiting on opponent"). Proposed: same, and the others can leave. A skip timer is open question 5. |
| P+ version or build mismatch | The server compares the build hash with the lobby's and refuses the ticket. The joiner sees the server's text in red; the lobby sees nothing. Brawl Rev 1 and Rev 2 discs may mix (one word differs, no gameplay effect). Vanilla Brawl and P+ never meet. |
| Slow join | Nothing big is transferred: a ticket, a hole punch to each member (Slippi's 8 s window; the relay fallback when it exists), and a lock-in under 1 KB. The joiner waits on their own CSS, and the players in the match never wait. |
| A pair that cannot connect (NAT) | The joiner sees "Could not connect to players: BO" and is not admitted. Admitting a player who can reach only some members would break the rotation. |

### 4.6 Technical cost

Small. The match is the 2-player gameplay session we already run, so there is no new rollback work. What is new: (a) the mm server keeps track of open Direct lobbies and admits a ticket that names a seated player, with the consent and build checks, and gives the newcomer every member's address; (b) Dolphin keeps a lobby of up to 4 members over control messages (members, order, the last winner), connects the newcomer to every member, and starts each game's 2-player session between the two who play while the others stay connected but idle; host hand-over is a deterministic rule over the members, with no state; (c) the game side adds the two new status strings and the waiting state on the CSS, and gives the challenger the loser's stage-select path that already exists. Each machine's menus stay its own.

---

## 5. Concept 2: Everyone plays (code lobby of 2-4)

Every seated player is in every game. This is closest to Brawl's own With Friends (a 2-4 player brawl) and to Slippi Teams. New players take a seat between games.

| Players seated | BASIC VERSUS | TEAM BATTLE |
|---|---|---|
| 2 | 1v1, Direct rules (loser picks) | Waits for players, as Slippi Teams ("Searching for ABCD#123") |
| 3 | Free-for-all | 2v1 (Slippi's 3-player teams) or waits; open question 3 |
| 4 | Free-for-all | 2v2 |

Stage for 3-4 players: Slippi Teams' rule: game 1 random from the server's list, then P1 picks. Rules: P+'s competitive set (4 stocks, 8 minutes, items off).

### 5.1 Host flow (Anna)

1-4. As in Concept 1 (Direct with Bo). For TEAM BATTLE: everyone enters the same code (usually Anna's), as on Slippi.
5. Cy joins during a game. Back on the CSS: "Playing: BO/CY". Everyone presses START; the next game is a 3-player free-for-all.
6. In a match, a player who drops is despawned and the match goes on (Slippi's rule for 3-4 players). DISCONNECTED and the end come only when every other player is gone.
7. Leaving: §3.5.

### 5.2 Joiner flow (Cy)

1. Launcher: Play. Game: WITH FRIENDS > BASIC VERSUS (or TEAM BATTLE) > character > START > keypad: a seated player's code (Teams: the shared code) > START.
2. "Searching for ..." > "Connecting to ..." > during a game: "Waiting: ANNA vs BO" (or "Waiting: ANNA/BO/DEE").
3. Back on everyone's CSS: Cy reads "Playing: ANNA/BO" and "Press START to lock in". From now on he plays every game.

### 5.3 Ports and teams

- **Stable ports:** each player keeps their in-game port for the whole lobby; a newcomer takes the lowest free one. Port gaps (P1, P3) are allowed, as in Brawl's Versus. SESSION already marks each port `present`. This keeps each player's colour and spawn the same from game to game.
- **Teams:** each player picks a team on their own CSS, as Slippi's team toggle does. Which control Brawl's Wi-Fi CSS offers for it needs reverse engineering. Until then, teams by port (P1+P2 vs P3+P4).
- A newcomer who arrives after the next game's setup has already been decided waits for the game after that ("Waiting: ..."). The setup is never redone.

### 5.4 Edge cases (where they differ from §4.5)

| Case | What happens |
|---|---|
| Host in a match | As Concept 1: the newcomer waits and is seated from the next game |
| 3rd and 4th player | Each takes the lowest free port at the next game. The match type follows the head count (table above). |
| A player drops mid-match (3-4 players) | Despawned on every machine at the same frame; the match goes on (Slippi Teams). On the CSS, their port frees. |
| The head count changes between games | The next game is set up for the new count. A Teams lobby that drops below its count waits for a refill: this is the M7 drop-in. |
| Host leaves | As Concept 1: nothing visible |
| Mixed connection quality | One slow link slows everyone (rollback waits on the worst peer). Slippi Teams has the same issue. Out of scope for the UI. |
| Ranked, version, controllers, slow joins | As §3.6 and §4.5 |

### 5.5 Technical cost

Medium, and mostly work Teams needs anyway. The session has to run 3 and 4 players (GekkoNet supports N; our gameplay session and its tests run 2; the region set and determinism must be checked again with 3-4 fighters on the stage). It needs full-mesh connections or the relay, a match setup for N players and port gaps, despawning a dropped player at an agreed frame, and team selection on the online CSS. The lobby, join rule and status lines are Concept 1's. No state transfer: a newcomer still brings only a lock-in.

---

## 6. Concept 3: Drop into the host's game (Orca's model)

Shown for comparison. It is what Orca does.

### 6.1 Flows

**Host (Anna):**
1. Launcher: Play. Game: she plays anything, offline menus included. Her Dolphin has to keep a log of her pads and be ready to capture her machine.
2. Cy enters ANNA#123. Anna's game captures its whole machine at the next frame (an 8-10 ms hitch on Orca), compresses it and uploads 26-30 MB. Anna keeps playing meanwhile.
3. Cy's controller "plugs in" as P2 wherever Anna is. If she is in the menus, both games move to a Versus CSS together (Orca's `FriendsMove`). If she is in Classic, Training or the Subspace Emissary, the join is held until she leaves it.
4. From then on, both machines run one shared game: either player's B on the CSS takes both back to the menus, and the stage select and menus are shared.

**Joiner (Cy):**
1. Launcher: Play. Game: WITH FRIENDS > keypad > ANNA#123.
2. His own game stops. Progress has to be shown somewhere: Orca prints `joining <percent>` to its app, so we would need a Dolphin OSD line or a launcher view. *(Invented UI.)*
3. He appears as P2 on Anna's screen, in Anna's game, with Anna's save: her name tags, her stage builder stages, her records. His own tags and controls arrive as port values.
4. When he or Anna leaves, his game is a copy of Anna's machine. Orca then "goes home": it moves him to port 1 of that copy. In our launcher model, getting his own game back means a reboot.

### 6.2 Edge cases (the hard ones)

| Case | What happens |
|---|---|
| Host in a single-player mode | The join is held; the joiner waits with nothing to do (Orca: `friend-waiting`) |
| Host in a match | The joiner plugs in at frame S, mid-match, unless that is blocked |
| Host leaves | Every joiner is left on a copy of the host's machine; Orca's "Going home" rules |
| Slow join | 26-30 MB per join. 5.7-7.2 s on Orca over the internet; about 25 s on a 10 Mbit/s uplink. The upload competes with the host's own rollback traffic. |
| Version mismatch | The whole machine must match: Dolphin settings, the P+ SD card, and **every Gecko code** (our launcher keeps Slippi's Gecko code manager, so users may differ). |
| 3rd and 4th player | Orca tested only two. With a third, a catching-up joiner stalls the live players. |

### 6.3 Technical cost

Very large. A keyframe needs the full machine state, including IOS/SD state and the NAND (our snapshots skip IOS today, `backend-design.md` §5.1 B). After the join, menus and the CSS must be rolled back too, which is the whole-machine model the user turned down for gameplay-only rollback, so we would be maintaining two session models. Dual-core determinism over menus is unproven (§8 of the backend design notes render-side state diverging), and Orca forced single core, which players disliked. The joiner loses their own menus and save for the session. This would be months of work for the least Slippi-like flow.

---

## 7. Comparison

| | Concept 1 | Concept 2 | Concept 3 |
|---|---|---|---|
| Slippi equivalent | Direct, extended | Teams, extended | none |
| New strings | 2 (+ server errors) | 2 (+ server errors) | progress UI, join and leave notices |
| New screens | none | none (team toggle: RE) | a joining/progress view |
| Each player keeps own menus, save and tags | yes | yes | no |
| Joining mid-match | waits for the next game | waits for the next game | plugs in mid-match |
| Needs N-player rollback | no | yes | yes |
| State sent to join | < 1 KB | < 1 KB | 26-30 MB |
| Steam Deck, controller only | yes (keypad) | yes | yes for the join; the progress view must be readable in game mode |
| Risk to Ranked | none | none | needs special rules |
| Effort | small | medium (shared with Teams) | very large |

---

## 8. Recommendation

1. **Build Concept 1 (winner stays) for BASIC VERSUS.** It is Direct with a line of waiting friends. Matches stay 1v1 on the 2-player session we have; the challenger's stage pick reuses Direct's loser's pick; the only new UI is two status strings. The join rule is "enter a seated player's code", on the screens players already know.
2. **When Teams is built, use the same lobby for TEAM BATTLE** with Slippi's shared code, and let empty seats be refilled between games (M7). That is Concept 2's mechanism, paid for by Teams.
3. **Decide later whether BASIC VERSUS should seat 3-4 for a free-for-all** (Concept 2) instead of rotating. Both can't be the default without a new switch. Proposed: rotation, because P+ friendlies are 1v1 and Slippi has no code-based FFA.
4. **Reject Concept 3.** It contradicts the gameplay-only session model, replaces the joiner's own game, and costs the most.
5. Spectating (M5) can come later on top of Concept 1: a waiting player could watch the running match instead of sitting on the CSS.

### What is invented (needs the user's OK)

| Invention | Where | Why it can't be copied from Slippi |
|---|---|---|
| A 3rd/4th player joins a Direct lobby by entering a seated player's code | mm server, Dolphin | Slippi Direct pairs exactly two tickets |
| Rotation: winner stays, loser to the back, the challenger picks the stage | Session rules | Slippi has no rotation |
| "Waiting: ANNA vs BO" and "Playing: CY, next: DEE" | CSS status line | Slippi has no waiting player |
| Invisible host hand-over | Dolphin | Slippi has no lobby that outlives a player |
| "Played Direct before" consent check (proposed default) | Dolphin, using `direct-codes.json` | Slippi Teams has no consent check |
| "This lobby is full." | Server text | — |

---

## 9. Open questions for the user

1. **3-4 players in BASIC VERSUS:** rotation (Concept 1, recommended) or everyone plays a free-for-all (Concept 2)? Or both, with a new switch somewhere (which would be invented UI)?
2. **Who may join a Direct lobby?**
   (a) Anyone who knows a seated player's code (Slippi Teams' rule; simplest; codes are visible to past opponents and on leaderboards).
   (b) Only players a seated player has played Direct with before (the proposed default; invisible; a first-time friend must play one normal Direct first).
   (c) Only by mutual codes, so a seated player must also enter the newcomer's code. Safest, but there is no screen for that between games, so it would need a new one.
3. **TEAM BATTLE with 3 players:** play 2v1 (Slippi's `%3` codes) or wait for the 4th?
4. **Rotation details:** winner stays or loser stays? A cap on winning streaks? Does a no contest count as a loss for the player who quit?
5. **An idle challenger:** wait forever, as Slippi waits for a lock-in, or skip them after a time limit (for example 60 s), which would be invented?
6. **A sound when someone joins or leaves the lobby?** The back sound exists for leaving. Using a Brawl menu sound for joining would be a small invention.
7. **Launcher friends list or presence** (seeing who is online, an "invite" button): not in Slippi, so not proposed. Do you want it later?
8. **Waiting place:** wait on the CSS (Slippi, and the earlier decision in `backend-design.md` §5.4 row 2), or in Brawl's own online waiting room (the training room with the Sandbag, which Brawl uses while it finds players)? The CSS lets a waiting player change character; the training room gives them something to do.
9. **Spectating while waiting (M5):** worth planning for in the first lobby design, or leave it for later with broadcast?
