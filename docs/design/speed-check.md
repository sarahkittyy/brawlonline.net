# Speed check: PCs that can't run the game at full speed

Status (2026-10-22):
- **Built, committed locally, not pushed:** both measurements, mm's records and `pp-admin speed`.
- **Log only.** Nothing is refused. The rule below is computed and shown, never applied. Turning it on is a later decision, after looking at the numbers.

## 0. What the user asked for

2026-10-21: "would we be able to restrict people with terrible PCs from playing the game somehow?? network is one thing, it seems stupid to allow people getting 45fps to join ranked."

After the proposal: "Build it: the waiting-room speed check before Ranked queues, plus slow reports to mm with a Ranked lock after two slow games in a row. Log only for now, no blocking, until we've seen the numbers."

## 1. Why it matters

Under rollback the slower PC sets the speed: a peer that can't keep 60 frames a second makes the other one follow it (GameplaySession's `UpdatePeerFollow`, "frames at the peer's speed"). A 45 fps player puts both players at 45. The game already notices: `UpdateSlowNotice` shows "Your PC can't keep up. Expect slowdowns." and "<name>'s PC can't keep up." after two 5-second checks in a row under 58 frames a second with more than half the frames late for this PC's own reasons. Before this, mm never heard about it.

## 2. The two measurements

**Every online game** (`spectate-end`'s `perf`, `docs/spectate-protocol.md`). At the end of each network game, the session's totals (`NetStats`, the same numbers as the `gprb perf:` log line) go into the replay's `ENDG` as `perf`. The spectate uploader copies them into `spectate-end`, which every player's game already sends for every game (Ranked always, since games are streamed for the record). mm keeps them in `speed_reports` for the match's players only, one row per game and player. Since 2026-10-22 `perf` also has the game's input delay and what it cost on that screen (`delay`, `delayProfile`, `delayAsked`, `delayFixed`, `rttStart`, `ping`, `jitter`, `rollbacks`, `depth`, `rollbackMs`, `lateAfterRollback`, `hitches`, `waits`, `waitMs`, `resimMs`; `common::speed::GameDelay`, migration 0022), for tuning the delay profiles (docs/gameplay-rollback-status.md, "Delay profiles"); `pp-admin speed show` prints them under each game.

**The waiting room** (`speed-check`, `docs/rooms-protocol.md` §2). Unranked and Ranked searches wait in Brawl's Online Training room, where no session runs. `Online/SpeedCheck.cpp` counts the game's frames at the loop top while the search's mode is Unranked or Ranked and the scene is the room (`IsWaitingRoom`). It skips the room's first 120 frames (loads and shader compiles stutter on any PC), then times windows of 300 frames, at most four. A gap of over 1 s between two frames (Dolphin paused, the PC asleep) starts the window again and is counted as a gap. After four windows, or when the room ends with at least one, one `speed-check` goes to mm on the online connection. mm keeps it in `speed_checks` (at most 3 a minute per account).

Neither delays anything: the search is sent when START is pressed, as before.

## 3. The rule (`server/crates/common/src/speed.rs`)

- **A slow game:** at least 600 frames, under 58 frames a second, and more than half its frames late for this PC's own reasons (`ownLate > 0.5`). A game slowed by the other player's PC is not this PC's slow game: its frames are late because it waited (`followed`), not on its own.
- **A check:** fewer than two windows is *unsure* (the search matched too soon). Two windows in a row under 58 is *slow*. Anything else *passes*: one slow window alone may be a shader compiling.
- **The lock:** two slow Ranked games in a row lock the player out of Ranked until a waiting-room check passes. A Ranked game at full speed or a passing check ends a run. Slow or unsure checks change nothing. Unranked and room games are recorded but don't count. Elo is never changed and no set is voided over speed: otherwise a player could slow their own PC on purpose to void a loss.

## 4. Looking at the numbers

On the server (`pp-admin` is the admin CLI):

- `pp-admin speed summary [--days 7]`: games per mode, how many were slow and by how many players; checks per platform and video backend (pass / slow / unsure); how many players the rule would lock now.
- `pp-admin speed locked [--days 30]`: who the rule would lock now, with their last Ranked game.
- `pp-admin speed show IDENT [--days 30]`: one player's games and checks in order, and what the rule says.

mm also logs `slow game: this player's PC could not keep full speed` for each slow game and `speed check` for each check.

## 5. Turning it on later (not built)

What enforcing would need: mm reads a player's recent rows when a Ranked ticket comes (the same `common::speed::locked`), and refuses it with a message ("Your PC couldn't run your last two Ranked games at full speed. Play Unranked or wait in Ranked's search for a speed check."). The waiting-room check would then have to run *before* the ticket for a locked player: the search would wait in the room for the check's windows first. Steam Deck and older Macs should pass the check before this is turned on (they are supported platforms).

## 6. Found on the way: ranked_game_results was never written in production

Migration 0016 created `ranked_presence` and `ranked_game_results` and granted mm's role nothing on them. In production every write failed with "permission denied" (4,834 errors in the 24 h to 2026-10-22 00:00 UTC, both tables empty), so accounts saw no word from a set's other player, and a lone report counted after the grace period as it did before 0016. Migration 0018 grants them. The new tables' grants are in 0019.

Also: a paused primary that mm hands on as a standby gets `role: none`, and Dolphin took that as "spectating is off" and sent no `spectate-end` for the game. A Ranked game whose upload paused for the PC's speed (exactly the slow games) lost that player's result and speed. Dolphin now reports a game's end after any `none` that follows a real role (`Uploader::Game::had_role`).

## 7. Tests

- `common::speed` unit tests (the rule, the message checks, a broken `perf` read as none).
- mm `engine::tests::speed_reports_come_from_the_matchs_players`, `speed_checks_are_recorded_and_limited`.
- `admin` `tests/speed.rs`: rows written through mm's own inserts, then `speed summary/locked/show` against a fresh database.
- Dolphin `Spectate.EveryPlayerReportsTheGamesEndWhateverItsRole` (the standby `none` case).
- Harness `test_waiting_room.py::test_waiting_room_speed_check` (four windows near 60 fps recorded; one window on a cancel; none on an immediate cancel) and `test_online_ranked.py::test_ranked_set_is_rated` (every game's speed recorded for both players, none slow).
