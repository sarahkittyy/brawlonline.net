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

Each player's lock-in carries a team, `0xFF` = no team (free-for-all), else Brawl's team colour (`gmPlayerInitData::m_teamNo`: 0 red, 1 blue, 2 green; Brawl has three, so 0-2).

| Where | Field |
|---|---|
| Dolphin | `Gprb::Session::LockIn::team`, `LobbyPlayer::team` (and so `Lobby::players[port].team`) |
| Wire | the `"lock"` object of the control messages: `"team": <0-2 or 255>`; the host's `"match_setup"` players carry it too. Anything else from a peer reads as 255. |
| LOCAL (game -> Dolphin) | offset **0x34** (the first byte of the old `_reserved[3]`), `u8 team`, written by the game with its lock-in (covered by the lock-in's `seq`). |
| SESSION (Dolphin -> game) | `players[port]` offset **0x03** (the old `_pad`), `u8 team`; 0xFF for an absent port. |

The barrier's setup check (`SetupKey`) also compares each port's `gmPlayerInitData` team byte (+0x0B), in team battles only (`gmMeleeInitData::m_isTeams`; in a free-for-all the byte holds whatever the character select left there).

`nplayer-setup` (merged into this branch at `a174637f`, see "Merging" below) has the game side and GameBridge's plumbing of both fields (PPOM version 4: LOCAL `lockTeam` at 0x34, SESSION `players[i].team` at +3, `gone[4]` at 0x20C, SESSION 0x220 bytes) and removes a gone player's fighter at the start of the frame whose flag reads 1; this branch carries the team in the session and writes the gone flags.

## Design

### Players and ports

A session has 2-4 players, each on an in-game port (P1-P4), gaps allowed. `ConnectOptions`:

| Field | Meaning |
|---|---|
| `local_slot` | This player's in-game port (0-3). -1 (the default): a 1v1 as before, the host P1 and the joiner P2, the joiner configured with `remote_host`/`remote_port`. |
| `peers` | Every other player: `{slot, host, port}`. An empty `host` means "learn the address": the first control message that claims that port (its `"slot"` field) confirms it. A known IP with port 0 means: only that IP may claim it (a NAT may change the port). |
| `host` | This player decides (sends the sync block, decides each game's setup). |
| `host_slot` | The decider's port as a joiner knows it; -1: learned from the first peer whose messages say `"host": true`. |

`GameplayOnlineBackend` fills these from a matchmaking `Match` with 3-4 players: every player on its server port (`players[i].port - 1`), every remote's address from `connected` (else `remotes`), in the order of `players` without the local one. A 1v1 from matchmaking is unchanged (host P1, joiner P2).

The host is the decider. If it leaves, the player with the lowest port still in the session decides from then on (every peer computes the same once it has seen the leave). A host who leaves at the barrier, before the match starts, ends the match start with an error.

### Network: every pair of players

One UDP socket per player, as before. Every pair of players exchanges packets directly (full mesh), with the same packet types: `C` control (JSON, now with `"slot"`), `P`/`Q` ping, `G` GekkoNet. A packet is the peer's when it comes from that peer's confirmed address; until then only a `C` packet claiming the peer's port (from a matching IP, if one is known) is accepted, and it confirms the address. Packets from anywhere else are counted (`foreign_packets`) and dropped. Nothing changed in the hole punch: Slippi's approach (both sides send from the start when they know the address) works per pair, and every player sends to every peer whose address it knows. There is no relay in this code base.

GekkoNet's addresses are the peers' ports (`"p0"`-`"p3"`); `GekkoSend` sends to that port's peer, `GekkoReceive` tags each packet with its sender's port. GekkoNet supports N players natively: each remote actor syncs, receives inputs and acks on its own.

State messages go to every peer every 50 ms (one JSON per peer), pings every 200 ms, the silence check (7.2 s at delay 2, outside GekkoNet) runs per peer.

### Match start with N players

- **Seeds and init block:** unchanged; the joiners take the host's (`ApplyMatchStart`).
- **Barrier (first simulation frame):** every player compares its setup hash with every other's; the match ports are the game's own human ports (`gmGlobalModeMelee` players with state 0), which every peer computes from the same setup; each must be this player or a peer of the session, and every peer still in the session must have one (else the match start fails with an error). The host sends its sync block, task order and match ports to everyone; each joiner applies them (and checks the host's ports equal its own). The host goes on when every joiner has applied them, a joiner RTT/2 after it has.
- **Countdown:** GekkoNet is created with the match's player count; handles follow the match ports in port order (a 1v1: the host handle 0, as before). Its handshake runs with every remote during the countdown.
- **Start barrier:** the host sends "go" once every player is at the start barrier and GekkoNet has started, and starts the slowest round trip's half later; each joiner starts when the host's "go" arrives.

### The lobby with N players

Each player's lock-in (now with the team) goes to every peer. The host decides a game's setup once every player still in the session has been heard and is locked in: players by port (`MatchSetup::players[port]`, `present`), the stage from the last game's loser's pick (`GameResult::loser`), else the first pick by port, else random. The setup's wire form: a 1v1 keeps `[kind, costume, pv]` per player; anything else (ports with gaps, teams) sends `[kind, costume, pv, team, port]`; the joiner accepts both and the setup branch's `[kind, costume, pv, team]`, checks every field (ports 0-3 and unique, 2-4 players, teams 0-2 or none; a team battle (`"teams": true`) must pass `GameSetup::DecideTeams` again) and ignores a setup without a player on its own port. The team and stage rules are the setup branch's (`GameSetup::DecideTeams`, `StagePickPort`, `DecideOutcome`, `docs/nplayer/setup.md`), called with the players by port.

Game results (`GameResult`, read from the state every peer ended on) cover 2-4 players: per port stocks and damage (`ReadFighterFields` looks fighters up by player number, the setup branch's fix: Brawl has no fighter entry for an empty port), the teams from the match's setup and each port's elimination order (SESSION `out`). A 1v1 keeps its rule (more stocks, then less damage; the loser picks the stage, both after a draw); 3-4 players use `GameSetup::DecideOutcome` (`GameResult::place`, `winner`, `pickers`). A player who left the match is not placed.

### A player leaves a 3-4 player match

1. **Detection.** A `"leave"` control message (the player stopped the session), the session's silence check outside GekkoNet, or GekkoNet's own timeout in a match (`GekkoPlayerDisconnected`). The leaver is marked `left`; in a match the CPU thread disconnects it in GekkoNet at the next loop top (`ApplyPeerDrops`).
2. **The agreed frame.** GekkoNet's disconnect claims: every remaining peer sends the inputs it holds for the dropped player; a peer that holds fewer catches up from the claim, and the agreed last frame is the highest one any peer holds (`disconnect_frame`). Until every remaining peer has agreed (or 2 s pass), GekkoNet does not confirm frames past it and does not run more than the prediction window ahead. From `disconnect_frame + 1` = F on, the player's input is GekkoNet's disconnected input, which this session sets to the **gone marker** (`gekko_set_disconnected_input`): a neutral pad with "GON" in gfPadStatus padding (+0x39-0x3B, which the game never reads; local input has those bytes cleared before it is sent).
3. **The flag.** At the loop top of every pass, before the frame's logic, `WriteGoneFlags` takes the marker out of each port's slot (the game sees a neutral controller) and writes `gone[4]` into SESSION. Because the flag is derived from GekkoNet's input, it follows the same rollbacks: if a late claim raises the agreed frame, GekkoNet overwrites the inputs from the old F on with the real ones; the marker and the real input always differ, so this is a misprediction and the frames from the old F are simulated again with the flag at 0.
4. **The end.** With fewer than two players left, the session ends as a 1v1 does today (`peer_left`, `disconnected`, the game's DISCONNECTED flow). A 1v1 never writes the flag. Between games the player stays `left`; the next game is set up without it.

A replay of a pass log (`replay_path`) keeps the marker in the slots but does not write the flags (diagnostics only).
