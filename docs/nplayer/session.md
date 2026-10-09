# 3-4 player gameplay sessions: the session and network layer

Branch `nplayer-session` (worktree `.claude/worktrees/nplayer-session`). Part of the 3-4 player work of `docs/design/rooms.md` §5: this document covers `Gprb::Session` (`dolphin/Source/Core/Core/Rollback/`) for 2, 3 and 4 players. The match setup and the game side are `docs/nplayer/setup.md` (branch `nplayer-setup`); the region set and determinism with 3-4 fighters are `docs/nplayer/determinism.md` (branch `nplayer-determinism`).

## Interfaces (for the setup and determinism work)

### The gone flag

A player who drops in a 3-4 player match leaves the match at an agreed frame; the others play on.

| | |
|---|---|
| Where | The PPOM SESSION block (`game-code/PPOnline/include/ppom.h` `Session`), offset **0x20C** (the old `u32 _reserved`): `u8 gone[4]`, indexed by in-game port (0 = P1). Guest address = GameBridge's SESSION address + 0x20C + port. |
| Values | 0: the port's player plays (or the port is empty). 1: the port's player has left the match. |
| Rolled back | Yes. SESSION is in the plugin's `.data`, which the region set covers (`rel_data`); only the mailbox and LOCAL are excluded. |
| Who writes | Dolphin, at the loop top of **every** pass of a 3-4 player network match (first runs and resimulations), before the frame's game logic runs: 1 for every pass that simulates session frame F or later, 0 before F. So a rollback to before F sees 0 again, and every resimulation of F and later sees 1. Outside matches GameBridge writes 0 (the whole SESSION block is rewritten after every match). Sessions of 2 players never write it: a 1v1 keeps today's DISCONNECTED flow. |
| F | The first session frame without the player's input, agreed by all remaining peers (GekkoNet's disconnect claims, below). Game frame = F + `start_frame` (240). |
| Input from F on | The gone port's pad slot is a neutral, connected GameCube controller (no buttons, sticks centred), the same on every machine. A fighter the game has not removed stands still. |
| What the game does | Reads `gone[port]` every frame of the match; on the first frame it reads 1 for a port whose fighter is still in, it removes that fighter. The removal is game state like any other and is rolled back with it. |
| Change it | `Gprb::Session::GONE_FLAG_OFFSET` in `GameplaySession.h` (offset into SESSION) and `ppom.h`. |

Dolphin's harness status (`gprb_status`) reports `gone_flag_addr`, and per peer `gone_frame` (F, or -1).

### The team byte

Each player's lock-in carries a team, `0xFF` = no team (free-for-all), else Brawl's team number (`gmPlayerInitData::m_teamNo`: 0 red, 1 blue, 2 green; 0-3 accepted).

| Where | Field |
|---|---|
| Dolphin | `Gprb::Session::LockIn::team`, `LobbyPlayer::team` (and so `Lobby::players[port].team`) |
| Wire | the `"lock"` object of the control messages: `"team": <0-3 or 255>`; the host's `"match_setup"` players carry it too. Anything else from a peer reads as 255. |
| LOCAL (game -> Dolphin) | offset **0x34** (the first byte of the old `_reserved[3]`), `u8 team`, written by the game with its lock-in (covered by the lock-in's `seq`). |
| SESSION (Dolphin -> game) | `players[port]` offset **0x03** (the old `_pad`), `u8 team`; 0xFF for an absent port. |

The barrier's setup check (`SetupKey`) also compares each port's `gmPlayerInitData` team byte (+0x0B).

## Design

(in progress)
