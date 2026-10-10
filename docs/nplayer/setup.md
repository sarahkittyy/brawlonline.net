# 3-4 player matches: the setup and the game side

Branch `nplayer-setup` (worktree `.claude/worktrees/nplayer-setup`). Part of the 3-4 player work of `docs/design/rooms.md` §5: the match setup for 2 to 4 players (free-for-all and teams, ports with gaps), who picks the next stage, and the game side of a player who drops out of a running match. The network session is `docs/nplayer/session.md` (branch `nplayer-session`), the region set with 3-4 fighters `docs/nplayer/determinism.md` (branch `nplayer-determinism`).

## Interfaces

PPOM is now **version 4** (`game-code/PPOnline/include/ppom.h`, `Online/GameBridge.cpp`, `tools/gamecode/ppom.py`). The layout before SESSION is unchanged; SESSION grows from 0x210 to **0x220** bytes, and the DEBUG block that follows it moves by 0x10. Dolphin and the plugin must both be v4 (GameBridge refuses another version).

| Where | Offset | Who writes | Rolled back | What |
|---|---|---|---|---|
| LOCAL | 0x34 | game, with the lock-in (its `seq` covers it) | no | `lockTeam`: the player's team colour, 0 red, 1 blue, 2 green; 0xFF none. The plugin writes 0xFF until the CSS has a team control (rooms work). |
| SESSION player i | +0x03 | Dolphin | yes (constant in a match) | `team`: 0-2 in a team battle, 0xFF in a free-for-all and for an empty port. |
| SESSION player i | +0x36 | Dolphin | yes (constant in a match) | `picksStage`: 1 = this player picks the next game's stage (the loser's pick, below). Replaces the game's own `lastWinner != me` test. |
| SESSION player i | +0x37 | game, every frame of a match | yes | `out`: the order in which the port ran out of stocks, 1 = first; ports out on the same frame share it; 0 = still in. Dolphin reads it at the end of a game for the placings. |
| SESSION | 0x20C | Dolphin's session, in a match | yes | `gone[4]` by in-game port (as `docs/nplayer/session.md`, "The gone flag": the same offset). |
| SESSION | 0x210 | Dolphin | yes (constant in a match) | `teams`: 1 = the next game is a team battle. |
| SESSION | 0x211 | Dolphin | yes | `setupError`: why the next game is not set up (`GameSetup::SetupError`): 1 "Pick different teams", 2 "Pick a team" (3 "Waiting for players" is Dolphin's only). For the CSS's status line later. |
| SESSION | 0x212 | game, in a match | yes | `outCount`: the highest `out` given so far. |

The session agent's `docs/nplayer/session.md` uses the same offsets for the gone flags (0x20C) and the team bytes (LOCAL 0x34, SESSION player +0x03). One difference: it accepts team 0-3 from a peer; here a team is 0-2 (`GameSetup::NUM_TEAMS`). Brawl's team battle has three colours (the team-to-colour table at 0x805A21F0 is `00 01 03`, and a fourth byte that is no team), so 3 is refused as "Pick a team" by `DecideTeams` and by the plugin.

**The gone flag, game side.** Dolphin writes 1 from the agreed frame on, at the start of every pass (first runs and resimulations), and never clears it during the match. At the start of each game frame (`gfPadSystem::updateSystem`, before the frame's game code) the plugin removes every port whose flag is 1 and whose fighter still has stocks (below). Once removed, the port has no stocks and the flag does nothing more.

**Dolphin side (`Online/GameSetup.h`, pure functions, unit-tested):**
- `DecideTeams(teams_on, seats)`: free-for-all or a team battle from the room's Teams switch and each port's lock-in team. Two players are always a 1v1. A team battle needs every player on one of the three colours and two colours at least (2v2, 2v1, 3v1, 2v1v1, 1v1v1); everyone on one colour is `SetupError::SameTeam` ("Pick different teams"), a player without a colour (or with a byte that is none) `NoTeam`.
- `DecideOutcome(teams, ports)`: the placings, the winner and who picks the next stage, from each port's end state (present, team, stocks, damage, `out`).
- `StagePickPort(pickers, picks)`: whose stage pick the next game is played on.
- `SetupErrorText(error)`.
- `GameBridge::SessionAddress()` and `SESSION_GONE`, `SESSION_PLAYERS_OFF`, `SESSION_PLAYER_SIZE`, `SESSION_PLAYER_OUT` (`Online/GameBridge.h`) for reading `out` and writing `gone`.

**What the session has to call** (done here for the 2-player session in `GameplaySession.cpp`, kept small because `nplayer-session` rewrites that file; see "Merging"):
- `MaybeDecideSetup` (host): `DecideTeams(ConnectOptions::teams, seats)`; on an error keep `setup_error` and do not set up the game; else put `teams` and each player's team in `MatchSetup`. The stage: `StagePickPort(last_pickers, picks)`, else the random draw.
- The setup message: `"teams": true` and a team as each player's fourth element; the joiner checks it with `DecideTeams` again (`SetupFromJson`).
- At the end of a game (`EndRunning`): 2 players keep today's winner rule, and the pickers are the loser (both after a draw), so 1v1 behaves as before; 3-4 players use `DecideOutcome` with the fighters' stocks and damage (`ReadFighterFields`) and each port's `out` from SESSION.
- `Lobby`: `teams`, `players[i].team`, `stage_pickers`, `setup_error`; GameBridge writes them into SESSION.

## The match setup (`game-code/PPOnline/source/online_match.cpp`)

The online CSS leaves for a match exactly as before; `setupMatch` builds it from SESSION:
- **Ports.** `gmSelCharData` players 0-3 by in-game port, empty ports as "none" (state 3), present ones human with their controller number = port + 1. Brawl's own Versus setup (`sqVsMelee`'s, `sora_scene` text+0x21940) builds the match from that, gaps included: P1 + P3 + P4 is a 3-player match with P2's panel absent.
- **Team battle.** `gmSelCharData+0x33` is the CSS's team switch: the Versus setup copies it into `gmMeleeInitData.m_isTeams` (`lbz r0,0x33(r30); stb r0,0x13(r31)` at text+0x219A4), and each player's `+0x0B` into the match's team number. The plugin sets both from SESSION and puts the CSS's switch back after the match with the rest of the saved records. Team attack is P+'s set rule default (on), written by `applyRules`; it only matters in a team battle.
- **Team costumes.** The CSS gives a player in team mode a costume of the team's colour through `muMenu::findCharTeamColorNo` (0x800AF520), which P+ v3.2 replaces (Legacy TE `UnboundedTeamEngine.asm`) with a walk over the character's costume list in its CSS slot table (0x80585B00 + slot * 0x10, +8: 2-byte entries, the first byte the costume's colour, ending with 0x0C). The plugin does the same walk: the player's own costume if it already has the team's colour (their chosen shade of it), else the character's first costume of that colour. The slot comes from `exchangeGmCharacterKind2MuSelchkind` (0x800AF708), which P+ also points at its table.
- **Shades.** A free-for-all keeps the colour-clash shades (`docs/game-code.md` "Colour clash"), now counted over the final costumes. A team battle is left to the game: when the match starts it shades a second one of a character on a team itself (seen: the second red Mario got shade 1 whatever the plugin had written), and players on different teams wear different costumes.
- **Validation.** Every present player's character and costume as before, and in a team battle a team below 3; anything else is no match and back to the CSS (`lastError` 0x5E72), as for a bad character. Dolphin refuses a one-colour team battle before it reaches SESSION.

## Removing a dropped player

**What Brawl does.** A fighter that falls off on its last stock goes to status 0xBD (dead, down), whose code calls `ftManager::setDead` (sora_melee text+0x10B604: dead count, stock count, the entry's dead event) and then sits in status **0x10B**: not drawn, not on the stage, out of the camera, its HUD panel broken. Brawl ends the game by itself once one player or team has stocks left. (Checked with a harness experiment: P3 put on its last stock and moved below the stage: 0xBD, then 0x10B with 0 stocks.)

**The removal** (`tickPlayers`, every frame of an online match, first runs and resimulations alike, from rolled-back state only):
1. the port's fighter is put on its last stock (`ftOwner` data +0x34 = 1);
2. `ftManager::setDead(mgr, entryId, 5, -1)`: reason 5 and no killer, as Fighter's own code calls it at text+0x131220 (an earlier plugin ended 1v1 games this way). No KO is credited to anyone, nobody else's stocks or damage change;
3. the fighter's status becomes 0x10B through the small Fighter function at text+0x12D188 (0x80837B9C: the status module's change to 0x10B). `setDead` alone left the fighter standing on the stage, moved to a respawn point, still able to act; the dead status itself (0xBD) would also play the KO blast and credit the last attacker.
4. Ice Climbers: Nana is the entry's second fighter (`ftEntry+0x3C`, as `ftManager::getSubFighter` finds her when the entry's character is 0x10) and gets status 0x10B too. Without that she stayed on the stage and hit Marth.

The HUD panel of the removed player breaks as on any elimination. The match goes on, and ends with Brawl's own game set when one player or team is left (then state 10, back to the CSS, as any online match).

**The fighter table.** `ftEntryManager` at 0x80624780 has 9 entries in a match; the used ones come first in port order and an empty port has none (P1 + P3 + P4: entries 0, 1, 2 with player numbers 0, 2, 3 at +0x58; the rest have no player number, 0xCCCCCCCC). So an entry is found by its player number, never by `port * 0x244`. `Rollback::ReadFighterFields` (the session's per-port stocks, damage and position, and its fighter checksum) indexed by port; it now maps entries by player number. With two players nothing changes.

**The elimination order** (`out`): each frame, a port seen with stocks earlier in this match that now has none gets `outCount + 1` (all ports out on that frame the same number). "Seen earlier" keeps the previous match's fighter table, which is still in memory while the next match loads, from counting as out.

**Inputs of a gone port** are the session's business (`session.md`: a neutral controller from F on). A removed fighter does not read them anyway.

## Stage rules

`GameSetup::DecideOutcome`. A side is a player (free-for-all) or a team. Sides still in at the end rank first, by stocks (more first) and then damage (less first), P+'s time-out rule; eliminated sides come after, the later out the better (a team is out when its last player is). Places are shared by ties.
- **Winner** (SESSION `lastWinner`): the first side's lowest port; 0xFE when several sides share first place.
- **Pickers** (SESSION `picksStage`): the last place's lowest port: 1v1 the loser, teams the losing team's lower port, free-for-all last place, a tie for last place the lower port among them. When every side ties (a draw), each side's lowest port picks: a 1v1 draw makes both pickers, and the game lets the lower port pick (its stage select comes after every player has locked in, so only one player picks).
- **The stage of the next game** (`StagePickPort`): the first picker (port order) who picked a stage, else the first player who did, else the random draw (`DrawRandomStage`, Slippi's stage pool). Game 1 is always random.
- The CSS (`online_menu.cpp picksStage`) gives the stage select to the lowest port with `picksStage` in Direct and Teams (rooms) mode, after every player has locked in (the picker's lock-in carries `STAGE_PENDING` and the host waits for its pick); Unranked never, Ranked has its own steps. A gone player is no longer in the room, so their pick is never asked for; if no picker is left, the stage is random.

1v1 compatibility: with two players the session keeps its old winner rule and sets the pickers from it (loser, or both after a draw), so Direct's loser's pick works exactly as before; only the channel changed (SESSION `picksStage` instead of the game comparing `lastWinner` with its port).

## How to test

```sh
cd game-code && ./build.sh                       # plugin
cd dolphin && cmake --build --preset ninja-build-release-x64 --target tests
dolphin/build/release/x64/Binaries/Tests/tests.exe --gtest_filter=NPlayerSetup*
python harness/tools/nplayer_setup.py --scenario all              # setup and removal, screenshots
python harness/tools/nplayer_setup.py --scenario ffa-gap,teams-2v2 --synctest --distance 2
```

`harness/tools/nplayer_setup.py` boots one instance with the plugin and plays Dolphin's part of the mailbox itself (GameBridge off: logged in, a Direct search answered "connected"), then writes a SESSION as Dolphin would. The online CSS leaves for that match and the plugin builds it; all four ports are harness pads. Removal: the gone flag is written into SESSION while the match runs, as the session does. With `--synctest` the sync test (`gprb_synctest`, the ports of the match) is armed on the CSS, and the removal comes from the plugin's test hook instead (DEBUG `cfg` `CFG_TEST_GONE`, `testGoneFrame` / `testGonePorts`: ports set gone from a game frame, a second set some frames later), since nothing may write the rolled-back state from outside while a sync test runs. Video is Null for sync tests (D3D11 writes EFB copies into game memory, 0x933A99C0, which the region hash then reports). Scenarios: `ffa-gap` (P1 Fox, P3 Marth, P4 Fox in the same costume; P3 gone, then P4), `ffa-4`, `ffa-ics` (Ice Climbers removed), `teams-2v2` (red Mario + Mario, blue Fox + Falco; P2 gone, then P4), `teams-2v1-gap` (red P1 + P3, green P4), `ffa-2` (a 1v1 through the same path), `bad-team-byte` (a team byte 7: refused). Results: `run/artifacts/nplayer/<scenario>[-synctest]/result.json` and screenshots.

## Results (2026-10-09)

**Unit tests:** `NPlayerSetup*` 8/8 (splits, refusals, 1v1/FFA/teams outcomes, ties, draws, hostile end states, stage pick), `PeerData*` 10/10.

**Setup and removal, one instance (D3D11, dual core):**

| Scenario | Setup checked | Removal |
|---|---|---|
| `ffa-gap` | 3 players, P2 empty, teams off, costumes as locked in, P4's Fox (same costume as P1's) shade 3 | P3 at once: status 0x10B, 0 stocks, `out` 1, the others unchanged, no game set; P4 later: game set, P1 left, `out` [0,0,1,2]; back on the CSS with the team switch off |
| `teams-2v2` | team battle, team attack on, red Marios in Mario's red costume (the second shaded by the game), Fox and Falco in blue costumes | P2 out, blue plays on with P4; P4 out: game set, red (P1, P3) left |
| `teams-2v1-gap` | P1 + P3 red, P4 green, P2 empty | P1 out (red plays on with P3), then P4: game set, P3 left |
| `ffa-4` | 4 players | P2, then P3 and P4 on the same frame (`out` [0,1,2,2]) |
| `ffa-ics` | | Popo and Nana both gone (before the Nana fix she stayed and fought) |
| `bad-team-byte` | refused: no match, `lastError` 0x5E72 | |

Screenshots: `run/artifacts/nplayer/<scenario>/` (02 the match, 03 right after the first removal, 04 the match going on, 05 game set, 06 back on the CSS).

**Sync test** (single instance, `gp-v19`, Null video, removals at session frames 20 and 120, run to game set):

| Run | Distance | Frames | Rollbacks | Checksum desyncs | Region mismatches |
|---|---|---|---|---|---|
| `ffa-2` (1v1, baseline) | 2 | 140 | 138 | 0 | every frame: 0x805B4E80, 0x80B89DC0, 0x805A0340, 0x805A7400 |
| `ffa-gap` (3 players, 2 removals) | 2 | 140 | 138 | 0 | the same chunks, none new at the removals |
| `teams-2v2` (4 players, 2 removals) | 2 | 140 | 138 | 0 | the same, plus 0x81541B60 from frame 2 |
| `ffa-4` (4 players, 3 removals) | 7 | 140 | 133 | 0 | the same, plus 0x8153EB60 and 0x81530B60 from frame 2 |

The gameplay checksums (fighters, the three RNGs, the frame counter) agree in every frame through both removals and the game set: the removal is deterministic under rollback. The region-hash mismatches are not from this work: the same four chunks show up in a 1v1 sync test through the online CSS with **main's** Dolphin and plugin (a copy of the main checkout's build of 2026-10-09 15:47 and a plugin built from `40d0a57c`; 10,426 frames, the same four chunks). The 4-player-only chunks (0x8153xxxx-0x8154xxxx) appear from frame 2, long before any removal, so they belong to the region set with 3-4 fighters (`docs/nplayer/determinism.md`). No chunk appears for the first time at a removal frame.

**2-player regression tests** (this branch's Dolphin and plugin, two instances, the local accounts and mm servers): `test_online_game.py` `test_direct_set_under_the_gameplay_session` (two games, the loser's stage pick, now through `picksStage`), `test_opponent_leaves_in_the_middle_of_a_game`, `test_each_player_keeps_their_tag_controls`: 3 passed. `test_online_unranked.py`: 3 passed (including `test_direct_loser_picks_off_the_server_list`). `test_online_ranked.py`: passed on the second run. The first run timed out waiting for both games on the stage select: the host's log shows game 1 set up with a random stage ("random from 15 stages") in the same millisecond as, but before, "GameSetup: ranked set, player 1", so the setup was decided before Ranked's stage decider was registered and there were no strikes. That ordering (the session can decide a setup as soon as both lock-ins are in, while `GameSetup::Begin` runs when the online session starts) is not touched by this branch; worth a look by whoever owns the session start.

**Plugin size.** `.text` 0x9038 against 0x8BBC on `main` (+0x47C), `.data` +0x18, `.bss` +8. It loads (`Loaded plugin (PPOnline` in every run above); the heap margin was about 0x5F0 before `main`'s own growth since 2026-10-08 (`docs/game-code.md` §2), so little is left for the rooms CSS. Everything is `-Oz` already.

## Merging

- `nplayer-session` rewrites `GameplaySession.cpp/.h` for 2-4 players. The changes here to those files are small and meant to be taken over into that version rather than merged line by line: `ConnectOptions::teams`; `LockIn::team` (validated 0-2 here) and `LobbyPlayer::team` (both branches add them, same names); `Lobby::teams/stage_pickers/setup_error`; `MatchSetup::teams` with the setup message's `"teams"` and fourth player element; `MaybeDecideSetup` calling `DecideTeams` and `StagePickPort`; `EndRunning` calling `DecideOutcome` for 3-4 players. `nplayer-session` computes a `GameResult::loser` from stocks and damage alone; `DecideOutcome` should replace it, since it also uses the elimination order (`out`), without which the first player out and the second cannot be told apart (both have 0 stocks and 0 damage at the end).
- `GameBridge.cpp` (both branches): v4 sizes and the new SESSION fields here; the lock team read from LOCAL 0x34 is the same in both.
- `Rollback::ReadFighterFields` now maps entries by player number; any code that indexes `ftEntry` by port has the same problem with gaps.

## Not done

- The CSS controls: a team colour per player, the room's Teams switch, and the `setupError` text in the status line ("Pick different teams") are the rooms CSS work (`rooms.md` §4). Until then the lock-in's team is always 0xFF.
- Pause: P+'s pause is on in Direct. A player who drops while the game is paused, or while someone else holds it paused, is the session's concern (`session.md`).
- Removal while a fighter is in the middle of something that involves another fighter (a grab, a Final Smash, Kirby's inhale) was not tested; Brawl's status change is the same one its own last-stock death ends in, so the other fighter's status code should see the usual release.
- Pokemon Trainer (if a P+ build has a multi-Pokemon entry): only the active fighter and Nana are handled.
