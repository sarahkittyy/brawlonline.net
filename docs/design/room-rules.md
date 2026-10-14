# Room rules, published rulesets and sets (design; phases 1 and 3a built, §8)

_2026-10-11. The user's decisions, plus defaults chosen without asking (marked **default**); the user agreed with every default in §1 and §2 the same day. Builds on `rooms.md` (rooms, host, ready, start) and replaces its "Rules: P+ competitive" default for rooms. Direct, Unranked and Ranked keep today's fixed rules (`game-code.md` §6, Settings on the online CSS)._

---

## 1. Decisions

| # | Topic | Decision |
|---|---|---|
| 1 | Where | **Rooms only.** Direct, Unranked and Ranked stay fixed (Slippi). Ranked becomes a built-in ruleset in the same format (§5), not editable. |
| 2 | Which rules | Everything in P+'s rules menu: style (stock / time / coin), stocks, time, damage ratio and handicap (**default**: in the menu, so included), team attack, pause, items (frequency and item switch), stage switch (with hazards), auto L-cancel, input assist. |
| 3 | Who | **The host writes, everyone in the room reads**, live as the host changes them. |
| 4 | Button | **One RULES button**, not the rule bar: the rule bar stays the status line. ITEM and STAGE are not needed: P+'s rules menu (mode 0) reaches the item and stage switches. On the room CSS the ITEM / STAGE slots are already PUBLIC / PRIVATE and FFA / TEAMS, so **RULES is a third button** (2026-10-11, after the spike): the same button model next to them, its label rendered from the game's font (no new art). Non-hosts get the same screens read-only. |
| 5 | Change flow | Host edits → server validates (allowlist, ranges), bumps a revision, pushes `room-state` → every member's game shows the new value and the status line names the change for a few seconds. **Any change clears everyone's ready.** Changes apply from the next game, never mid-match. At the start the rules go into SESSION, `applyRules` applies them, and their hash joins the setup key: a mismatch refuses the match. |
| 6 | Auto L-cancel, input assist | **Global** rules-menu values (set rule +0xB / +0xC, found by the spike), one value for the match. P+'s per-player Code Menu lines stay at their defaults. |
| 7 | Rulesets | **Published snapshots with a code.** A code always means exactly the same rules; editing publishes a new version with a new code. The full hash travels with every match, result and replay. |
| 8 | Editor | A **Rulesets page in the launcher**: create, name, publish. **Anyone can publish.** |
| 9 | Links | **`brawlonline.net/r/<code>`** shows the ruleset and creates a room with it (through the launcher). |
| 10 | Launcher box | Next to **Create room**: a ruleset code box with the ruleset's name beside it. A link fills it, and it **persists**, so the next room uses it without going back to Discord. An X clears it (P+ default). |
| 11 | Remembered ruleset | **One per account, shared by the launcher box and in-game Create Room** (server-side). A new room, created either way, starts with it. The room always shows its name. |
| 12 | In-game choice | RULES screen has a **Ruleset** line: a list of **recent and own published rulesets**, plus **Enter code** on Brawl's keypad (letter mode). Picking one also makes it the remembered ruleset. "P+ default" is the first entry. |
| 13 | Editing a ruleset room | Allowed: the room becomes **"Custom"** and the code disappears, so it never claims to match the tournament. (It first read "Custom (based on NorCal Weekly v3)"; the user's, 2026-10-12: one change or many read the same, so the room list says only "Custom". mm still sends `basedOn` in `room-state` and `/v1/rooms`; nothing shows it.) |
| 14 | First striker | **Random**, from the session seed (the online stand-in for RPS). |
| 15 | FFA | **No striking, no sets.** An FFA ruleset is match rules plus a stage list. |
| 16 | Mid-set disconnect | ~~The host decides~~ **Replaced 2026-10-11 by A** (§1.1): the set waits for the player and goes on by itself when they are back. |
| 17 | No Bo1 | Sets are **Bo3, Bo5, Bo7**. Without a set (one game at a time, as rooms are now) the ruleset has a **stage choice**: **loser picks** (game 1 random, then the loser; today's rooms), **strike every game** (from the starters with the ruleset's pattern, the first striker random each game; this is a tournament's Bo1), or **random every game**. FFA: loser picks or random only. |
| 18 | Links | The launcher **registers a URL scheme** for `/r/<code>` (§6). |
| 19 | Handicap | **Left out for now** (2026-10-11): P+'s Stock Control needs per-player stock counts nobody can set online; off in every room and ruleset. Can be added later as a new field. |

### 1.1 Decisions of 2026-10-11 (before the first push; the user's, through the coordinating session)

Defaults the coordinating session chose where the user asked a question are marked **default**.

| # | Topic | Decision |
|---|---|---|
| A | Mid-set disconnect | The room **stays open but becomes private**. When the player reconnects (joins the room again), **the set continues as it was**: the server remembers the set's state (score, game, the strike / ban / pick step so far) and syncs it to everyone; no Resume / Restart. **Leaving mid-game counts as a loss of that game.** **Default:** a dropped connection mid-game counts the same as leaving. **Default:** the slot stays reserved for that account. **Default:** the room goes back to its old public / private setting when the set resumes. **Default:** if the host kicks the absent player (closes the reserved slot), that is a **forfeit of the set** (the others win it). |
| B | After a set | **Default:** everyone stays in the room on the CSS (nobody is sent to the launcher). The game shows the result ("alice wins the set 2-1") on the status line and the final score stays shown. The next all-ready starts a new set at game 1, and that is when the score resets (also reset by a ruleset or rules change, or a player change between sets). The user's idea for the score: Brawl's **stars on each player's panel**, one per game won (RE: what the CSS has; rooms already use one star for the host, `rooms.md` #8). |
| C | In-game Ruleset picker | Decision 12 is required. Built in P+'s RULES page (the STAGE CHOICE row becomes RULESET: P+ default, the player's rulesets, Enter code on Brawl's keypad), `room_play.cpp`. |
| D | 2v2 sets | Required. |
| E | Match history | Stores the ruleset (code, name, hash) and the history shows its name, linked to `/r/<code>`. Not for P+'s default rules, nor for matches from before this update. Replays: not required. |
| G | Rulesets and player counts (the user's, 2026-10-11) | **A ruleset has no player count**: the `players` field (1v1 / 2v2 / ffa) is gone, and any ruleset works in any room; the user: "silly to imagine someone playing a 2v2 in a 1v1 tourney; not a case we need to care about." The rules record applies to every count. A set (best of, strikes, bans, DSR), and striking every game, is played when the room makes two sides: two players, or two teams of two (Teams on). Elsewhere (3-4 players, uneven teams) its games are single games on a random stage of the ruleset's stages; "loser picks" and "random" play as in any room. The status line says so while nobody is ready ("Sets need 1v1 or 2v2", "Strikes need 1v1 or 2v2"); nothing waits for it. A set length can be set in any room (`room-best-of`). Older rulesets' `players` key is read and dropped (`rulesets-protocol.md` §1). |
| F | Quits and the salty runback (the user's, 2026-10-11) | **The pause screen's quit (L+R+A+START) in a set: the game did not happen.** Back on the CSS, no loss; both stay there until each presses START (characters may change; 2026-10-12, after staging), then the same set game is struck again from its first step (game 1 from the starters). Ranked's quit (a loss) waits for both STARTs on the CSS the same way before the winner's bans. Tournaments play warmup games and quit out of them. In a **2v2 set** one player's quit voids it for all four. **The salty runback (L+R+A+Y+START) in a set** plays the same game again at once, on the same stage and characters, not counted. Elsewhere: Unranked and Direct, a quit sends both to the CSS and a runback restarts the same match for both; 3-4 players (2v2 too) without a set, only the quitter leaves (as built), but **a runback restarts the same match for everyone** (the user's, 2026-10-13); **Ranked: any quit, the runback too, loses that game.** Every quit to the CSS says who quit ("rhea quit"). `docs/game-code.md` §11, the quit per mode. |

## 2. Defaults (not asked)

| Topic | Default |
|---|---|
| Ruleset code | **6 letters** from the room codes' 20 letters (`BCDFGHJKLMNPQRSTVWXZ`; 64 M codes). The keypad's letter mode is reused; never confused with a 4-letter room code. Server-assigned, random. |
| Changing or deleting | Published versions **can't be changed or deleted**. The owner can hide one from their list; the code keeps working, so a tournament link never breaks. An old version's page says "A newer version exists" and links it. |
| Names | Not unique. The **publisher's name is shown everywhere** with the ruleset: "NorCal Weekly v3 by sarah". |
| Drafts | Saved to the account, usable by nobody until published. A host who wants unpublished rules sets them by hand in the room (Custom). |
| Set lengths | No set, **Bo3, Bo5, Bo7** (decision 17). |
| Blind pick | Game 1 of a set: others' characters stay hidden until everyone is ready (rooms show them on ready today, `rooms.md` #15). Later games: the winner locks in first and the loser sees the pick (Ranked's order). |
| After a set | Score shown; the next all-ready starts a new set at game 1. |
| During a set | The rules and the ruleset are locked; slots and Teams too. Leaving the room is always possible. |
| Player count | A ruleset names 1v1, 2v2 or FFA. The room can't start until slots and teams match: "This ruleset needs 2 teams of 2". |
| 2v2 striking | Either teammate strikes or picks for the team; the same frame: the lower slot counts. |
| Timers | Ranked's step timers (strikes 30 s, last game-1 strike 10 s, bans and pick 30 s each, 3 s grace). Not part of the ruleset at first. |
| Code Menu modifiers | Not in rules or rulesets (hitstun, staling, Special Modes...): still forced to P+'s defaults. Alternate stages (P+'s held-button variants): **open question for later**, they change what a stage pick means. |

### 2.1 The paused set

Replaced by decision A (§1.1, 2026-10-11): no host decision, no Resume / Restart.

How A, B, D and E are built (`rooms-protocol.md` has the messages):

- **Paused and resumed.** mm (`room_set.rs`) keeps the set's state: score, game, each side's players, and the steps of the game being set up, which every player's Dolphin reports with `room-step` (`GameSetup::TakeStepReport`, sent by `Rooms` while the session runs). A set player leaving (`room-leave`, or the connection dropping) adds them to `away`: the room turns private (`public_before_pause`), their slot is held for their account, and their join takes it back; when nobody is away the set goes on and the room's old setting comes back. The next session's ticket carries the steps so far (`SetTicket.actions`, only for a game whose match has not started), and `GameSetup` starts game 1 of that session from them: a 1v1 player's own actions begin with them (in 2v2 the host's lists do), and each side's list is never shorter than the ticket's.
- **Leaving mid-game.** Once a player's game reported `matchStarted`, leaving loses that game for their side (counted at once; a player who already reported the game's result saw it end, so the others' reports count it). The host closing a held slot, or closing a present set player's slot, forfeits the set for that side.
- **Results that differ** stay a host decision: `room-set resume` plays the game again, `restart` ends the set.
- **End set** (the user's, 2026-10-11): during a set the host's RULES > SET row offers End set; `room-set restart` ends any set that is on with no result (no winner; the games played stay as played), and the SET row works as before (No set / Bo3 / Bo5 / Bo7). While a set is on, what waits for it says "End the set first (RULES > SET)."
- **After a set (B).** The finished set stays in the room (`finished`, `winner`) with "alice wins the set 2-1" on the status line until someone readies; the next all-ready starts a new set at game 1. A rules, ruleset or Teams change, or a set player leaving, clears it. The game shows the score as Brawl's win stars on each side's panels (RoomPlay `sideSlots`, `winner`).
- **2v2 (D).** A ruleset with a set plays it in a room of two teams of two (four players, Teams on; decision G): the sides are the two colours (two players each), side 0 the one with slot 1. **Either teammate** strikes, bans and picks for the side (the user's choice, 2026-10-11). The session host's machine is the single authority (`SetFormat.h` `TakePresses`): every player's game sends its presses in its gameplay-session extra (`gs.p`, each with the step it was made for), the host takes them in the order it sees them (on the same frame one per port per round, lower ports first), and a press counts only when its side is on that very step and the stage can still be chosen; otherwise it is dropped, so a late press never lands on a later step. The host sends its lists (`gs.h`) and how many presses of each port it took (`gs.n`); every machine plays the host's lists. A player's own presses the host has not answered show at once on top of them (`WithPending`); one the host dropped disappears again, with "Partner was first" on the line for 2 s. A step's timeout is completed by the host's machine (a random stage), for either side. The host's setup of the match carries the stage, as for every room game. Every player reports results, and after a game every player of the winning team, then of the losing one, chooses a character. 1v1 sets and Ranked keep each player's own list (`gs.a`).
- **The stage switch** (the user's, 2026-10-12). Without a set, "loser picks" becomes two choices: **loser picks any stage** (today's rooms and P+ default: the whole stage select and its alternate layouts; the stage list only decides random draws) and **loser picks from the list** (the stage select greys every stage off the list, START draws only from it, and alternate layouts are not taken). Alternate layouts only ever in an unlocked room; sets and strike-every-game always use the list. In the game, P+'s RANDOM STAGE SWITCH becomes the room's **STAGE SWITCH**, which the host edits: the room's stage list, its page laid out as the stage select's rows of 7, A cycling a stage Off / Counterpick / Starter, the top line (P+'s TOGGLE PAGE) the loser-picks choice and the bottom button (P+'s per-stage hazard switch) the global hazards on / off. The launcher's editor shows the stages as tiles (the stage select icon, Starter / Counterpick / Off): the 15 legal stages, then the other 46 of P+'s stage select under "Other stages", off by default. Built so far: the launcher's editor (2026-10-12); mm / Dolphin / plugin (2026-10-12: `loser-picks-list`, the room's stages in `room-state` and SESSION v7 `RoomStages`, `pickFromStages` on the ticket, the loser's stage select greying the rest with P+'s strike art and its random keeping to the list in both modes, alternate layouts only with any stage; `harness/tests/test_room_stage_list.py`). The game's STAGE SWITCH (2026-10-12, `game-code.md` §14 "The STAGE SWITCH"): starters show STR, the footer's second line says what ON / STR / OFF mean and the hazards (Z, one switch for every stage; P+'s hazard switch button is never drawn in this layout and could not be added to the page's scene), `room-stages` for the host's edits, and the page in the stage select's rows of 7 (a short last row centred) with the D-pad moving by them.
- **History (E).** mm records a published ruleset's code, name, version and hash with each room match (`migrations/0009_match_rulesets.sql`); `GET /v1/me/matches` returns `room.ruleset` with its page's `link`, and the launcher's history shows the name linked to it.

## 3. The rules record

One record, decided by the server, applied by the game from SESSION. Both machines write the same bytes before `sqVsMelee`'s setup reads them (`OnlineMenu::applyRules`, which today writes constants).

| Field | Values | Where it goes (game; found by the RE spike, `game-code.md` §14 "Room rules RE") |
|---|---|---|
| `style` | stock / time / coin | set rule (`GameGlobal+0x1C`) +2, low 3 bits: **0 time, 1 stock, 2 coin** (confirmed in the menu) |
| `stocks` | 1-99 | set rule +4 |
| `target` | 0 (infinite), 1-99 | set rule +3: P+'s "score to win" of a time or coin match (Brawl's old time limit byte). Not sent yet: always infinite |
| `timeMinutes` | 0 (none), 1-99 | set rule +8: P+'s time limit for **every** style (MORE RULES' TIME LIMIT); +3 is not the time |
| `damageRatio` | 0.5-2.0 | set rule +6, in tenths |
| `handicap` | not a rule yet (decision 19) | set rule +5 (P+'s "Stock Control"; on = 2), kept off. Per player stock counts in each player's `gmSelCharData` record, set on their own panel: see §7.2 |
| `teamAttack` | bool | set rule +9 |
| `pause` | normal / hold | set rule +0xA: 1 normal, **0 = P+'s hold START to pause** (there is no "off") |
| `items` | frequency 0-4, switch bitfield | menu record (`GameGlobal+0x24`) +0x810, +0x818 (8 bytes) |
| `stages` | starters, counterpicks (P+ stage ids) | the random stage switch's pages (`RSS_EXDATA` `0x8042C4E8`, 6 bytes a page, 32 stage bits) as the editing screen; online the stage list is the server's, so it only feeds the room's list |
| `hazards` | bool (or per stage) | **default off** (the user's, 2026-10-12: `MatchRules::default`, so new rulesets, rooms' P+ default, and Unranked / Ranked / Direct in `config/rulesets.json`). Stage select data +0x25 (1 = off), forced by `applyRules` at every online match setup and compared by the setup key; P+'s hazard switch (`RSS_EXDATA`+0x1E, a bit per stage) is the per-stage form |
| `autoLCancel` | bool | set rule +0xB (P+'s rules menu line; Brawl's score display). **Global**, not per player |
| `inputAssist` | bool | set rule +0xC (P+'s 3-frame buffer; Brawl's damage gauge). **Global** |

Not a room rule: set rule +7 (stage choice: the session picks the stage). The Code Menu's per player Input Buffer / Automatic L-Cancelling lines stay at their defaults (off), as `applyRules` keeps them. P+ copies +0xB / +0xC into `gmGlobalModeMelee+0xE5`; Dolphin's setup key compares the set rule's first 16 bytes (since phase 1), which covers them.

Wire: JSON (`MatchRules`, grown from `ModeRules` in `server/crates/mm/src/ruleset.rs`), with a `format` version. The server refuses a room or a join whose client is too old for a field in use: "Update to use this ruleset".

## 4. Rulesets

A ruleset = name + version + publisher + the rules record + the set:

| Field | Values |
|---|---|
| `bestOf` | 0 (no set), 3, 5, 7 |
| `stageChoice` | no set only: `loser-picks` / `strike` / `random` (decision 17; `strike` between two sides, else random) |
| `strikes` | the pattern over the starters (game 1 of a set, or every game with `strike`), e.g. `[1,2,1]` (5 starters), `[1,2,2,1]` (7); the server checks it leaves one stage. |
| `bans` | games 2+: the winner bans 0-3 |
| `dsr` | bool: the loser can't pick a stage they won on (Slippi's / Ranked's rule) |

Identity: the code (user-facing) and SHA-256 of the canonical JSON (machines: SESSION, setup key, results, replays).

## 5. Sets

Ranked already runs the whole flow (`Online/GameSetup.cpp`: 1-2-1 strikes, winner bans, loser picks, DSR, timers, the CSS order). Sets make it follow the ruleset instead of constants; Ranked becomes a built-in ruleset (Bo3, its starters, `[1,2,1]`, 2 bans, DSR) and `config/rulesets.json` moves to this format. The room shows the score in the top window ("Bo3 · 1-0").

## 6. Protocol (sketch; `rooms-protocol.md` gets the real one)

- `room-state` gains `rules`: `{revision, values, ruleset: {code, name, version, by} | null, basedOn: {...} | null}`, and `set`: `{bestOf, score, game, step, paused, waitingFor}`.
- Requests (host): `room-rules {values}` (between games, not during a set; clears ready), `room-ruleset {code | null}`, `room-set {action: "resume" | "restart"}`. `room-create` gains an optional `ruleset`.
- Accounts: `POST /v1/rulesets` (publish), `GET /v1/rulesets/<code>`, `GET /v1/me/rulesets` (own, drafts, hidden), recent; `PUT /v1/me/ruleset` (the remembered one).
- Launcher → game: `join-room.json` kind `create` gains `ruleset`.
- `brawlonline.net/r/<code>` → a launcher link. The launcher has no URL scheme today (`main.ts` `open-url` only opens replay files): register one (decision 18) (`brawlonline://room/create?ruleset=<code>`) on Windows, macOS and Linux.

## 7. Reverse engineering (game side): done 2026-10-11

Details, addresses and screenshots: `game-code.md` §14, "Room rules RE". Nothing is built yet.

1. **P+'s own rules menu opens over the room CSS and comes back to it**: no page of our own is needed. It is `muRuleTask` (`sora_menu_rule.rel`, loaded with every CSS), which the CSS itself opens over itself (`sel_char` text+0x105D8, `openRules(task, mode)`): mode 0 is the whole menu (RULES, MORE RULES, ITEM SWITCH, RANDOM STAGE SWITCH with the hazard switch); the Wi-Fi CSS's ITEM / STAGE buttons call it with modes 1 / 2 (item switch or stage switch alone). Opened live from the room CSS's PUBLIC button: same scene and sequence, still in the room, the status line updating under it; B closes it page by page and the CSS is fully usable after. The plugin can also close it (`muRuleTask+0x288 = 1`), for a game start or a host change. RULES button = in the room, that button goes to `openRules(task, 0)` instead of the plugin's room action. While it is open the plugin must stop its own room-CSS buttons: L/R still switched Public/Teams under it and START still locked the player in.
   - **The slot is taken**: on the room CSS the ITEM / STAGE buttons are already the room's PUBLIC / PRIVATE and FFA / TEAMS (`game-code.md` §14). Decided: a third button (decision 4).
2. **Where things live**: §3 (every value checked in the menu). Handicap is P+'s **Stock Control**: a mode in the set rule plus a stock count per player, set by each player on their own panel (a STOCKS box appears on every panel, drawn over the room's state boxes). The online match setup builds players from one template, so today nobody could set those counts. Decided: left out for now (decision 19). **Auto L-cancel and input assist are global rules-menu values** (set rule +0xB / +0xC), not Code Menu lines: decision 6 is simpler than planned (one value in the set rule; the per port Code Menu lines stay off).
3. **Read-only for non-hosts**: one input function feeds every page (`sora_menu_rule` text+0x12E0, its result at `0x806A50D8`); a hook there in the CSS lock's style keeps only cursor moves, B and the A that opens a sub-page for a non-host. The plugin writes the room's values into the set rule, menu record and stage switch before opening; a host change while someone has it open reopens it. The host's edits are read back when the menu closes (or at each page exit: the pages write back only then) and sent as `room-rules`.
4. **Hidden characters in game 1 of a set**: the other panels only ever show a ready member's `character` from `room-state`, and the match takes characters from the session's lock-in exchange, so blind pick is **the server sending `character` / `costume` as null to the others until everyone is ready**. Game side, one change: a ready member without a character must show "Ready" with no picture (today it shows "Choosing...").

## 8. Order of work

1. The rules record in SESSION, `applyRules` from it, its hash in the setup key. **Done 2026-10-11:** `get-ticket-resp.rules` (`common::rules::MatchRules`, per mode in `rulesets.json`), Dolphin `Online/MatchRules`, PPOM v6 SESSION `Rules`, `applyRules` (the set rule as the rules menu keeps it: style, stocks, damage, time limit at +8 for every style, team attack, pause normal / hold, auto L-cancel +0xB, input assist +0xC; items; hazards), and the setup key compares the applied set rule, hazard bit, item switch and Code Menu values (rather than a hash of the record). Not yet: `target`, handicap (decision 19).
2. Host-edited room rules: server room state, the RULES button and screens, the status line. **Done 2026-10-11 but the button:** `room-rules` (host, between games; takes back every ready), `room-state.rules`, room games' tickets carry the room's rules, Dolphin's `ROOM_RULES` op, P+'s rules menu over the room CSS with per-line input rules (viewers read-only; non-room-rule lines read-only for the host), write-back on close, "Host changed the rules" (`game-code.md` §14). The RULES button is a separate task (opened by a test flag until then). Hazards and the stage list stay the room default (the stage switch is read-only).
3. Rulesets: accounts storage, codes, the launcher page and box, the remembered ruleset, the in-game Ruleset line, the URL scheme and `/r/<code>`. **3a done 2026-10-11** (everything but the game and mm; format, API and links in `docs/rulesets-protocol.md`):
   - `common::rules::Ruleset` (§4's fields, serde camelCase like `MatchRules`), `normalize()` (clears what the set format leaves unused, so rulesets that play the same hash the same), `validate()`, `hash()` = SHA-256 of the canonical JSON (every field but `by`, keys sorted; pinned in a test), codes (6 letters, `parse_ruleset_code`).
   - Decided with the user the same day: **starters and counterpicks are separate lists** (games 2+ pick from both; no stage in both), the **recent list** is the rulesets made the remembered one (newest 10), **publishing needs an account that can play online** (verified email, a connect code) and is limited to 10 an hour / 30 a day, and `/r/<code>` is a **static website page** reading the API.
   - accounts: migration `0008_rulesets.sql` (published versions can't change or be deleted, a trigger enforces it; a deleted publisher's rulesets stay with their name), drafts, the remembered ruleset, the recent list, mm's grants; the API (§6's endpoints and drafts CRUD, hide). `common::db` has what mm needs (fetch by code, remembered get/set, recent).
   - Launcher: the Rulesets page (drafts, every field, publish with the code and link, hide, edit as a new version, import / export files), the code box next to Create room (the remembered ruleset; X = P+ default), `join-room.json` create's `ruleset`, the `brawlonline://room/create?ruleset=<code>` scheme on Windows, macOS and Linux.
   - Website: `/r/<code>` with "Create room with this ruleset" and the download link; nginx and Caddy route `/r/*` to it.
   - **Public rulesets (2026-10-12, the user's decision after staging: one account could not find another's ruleset):** every published ruleset is public. accounts `GET /v1/rulesets?q=&after=` (each ruleset's newest version that is not hidden, newest first, cursor pages, search on name / publisher / code; `0010_rulesets_browse.sql`); hiding now takes a ruleset off the public list. The launcher's Rulesets page lists drafts, the account's published rulesets and the public list with a search and infinite scrolling, previews any row and refreshes on open, focus and a button. Defaults chosen without asking are marked in `docs/rulesets-protocol.md` §3-4.
   - **3b (mm, Dolphin) done 2026-10-11:** rooms created with a ruleset (`room-create.ruleset`, the remembered ruleset for the game's own create, `room-ruleset`), "Custom (based on ...)", the ruleset in `room-state` and `/v1/rooms`, the Ruleset line's list (`room-rulesets`, `CMD_RULESETS`) and the room's ruleset and set in SESSION `RoomPlay`. Not yet: the game's Ruleset line and Enter code UI.
4. Sets: GameSetup driven by the ruleset, 1v1 then 2v2; the paused set. **1v1 done 2026-10-11:** Dolphin's `Online/SetFormat` (Ranked's numbers as a format: strike pattern, bans, DSR, best of, strike every game, a random first striker per game), GameSetup started from the room ticket's `set` (the score so far, the set's game, sides on in-game ports), one gameplay session per set (Rooms keeps the room game until GameSetup says the set is won), each game's `room-result`; mm's `room_set.rs` (the score both players agree on, paused on a player leaving with their slot kept, host resume / restart, slots / Teams / rules wait for a set, blind pick of game 1); the game runs Ranked's steps on the room CSS and stage select (`stepSession`). Rooms switch rulesets by code (`room-ruleset`, `room-create.ruleset`, the remembered ruleset for the game's own create). "Strike every game" and "random" stage choices without a set. Verified: `harness/tests/test_room_sets.py` (a Bo3 room from the launcher's box: blind game 1, strikes, bans, loser's pick, winner's character first, 2-0, the server's score, 0 mismatches). Not yet: 2v2 sets, the in-game Ruleset line (the picker UI), the score on the room CSS and the host's resume / restart controls in the game.
5. The ruleset code, hash and set score in results and replays.
