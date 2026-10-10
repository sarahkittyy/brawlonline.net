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

## Wire changes (both peers run the same build)

| Packet | Change |
|---|---|
| `C` state message | new `"slot"` (the sender's port; how a learned address is claimed); `"host"` now says who decides (the decider can change when the host leaves); the host's message carries `"ports"` (the match's ports) with its sync block. |
| `C` leave | `{"t":"leave","v":1,"slot":N}`, sent three times to every peer. |
| `"lock"` | new `"team"` (0-2, 255 none). |
| `"match_setup"` | 1v1: unchanged (`[kind, costume, pv]` per player); otherwise `[kind, costume, pv, team, port]`, and `"teams": true` for a team battle. |
| `G` GekkoNet | unchanged; one GekkoNet session per match with 2-4 handles. A dropped player's input is the gone marker from F on. |
| GekkoNet API | `gekko_set_disconnected_input(session, input)` (the input a disconnected player gets after the agreed frame). |

A 1v1 session (`local_slot` -1) sends the same messages as before plus `"slot"` and `"team"`; its setup message is unchanged.

## Harness

`gprb_connect` takes `slot` (this player's port, 0-3), `peers` (`[{slot, host, port}, ...]`, `host` "" to learn the address) and `host_slot`; without `slot` it is the 1v1 as before. `gprb_status` adds `local_slot`, `host_slot`, `players`, `match_ports`, `num_players`, `gone_flag_addr`, `gone_flags` (the last pass's), and `peers` (per peer: address, `left`, `left_reason`, `handle`, `gekko_dropped`, `gone_frame`, lock-in, round trip); `peer` stays the first peer's.

`harness/tools/gprb_session.py` with `--ports` runs one instance per player (2-4, gaps allowed), each booted alone and driving every port of the match on its own character select: the first goes straight to the match (`history_a`), the others take `history_b`'s detour with different idle times (both now drive 2-4 ports; `gameplay_rollback.make_instance(controllers=...)` as on `nplayer-determinism`). Every pair of players goes through a `netsim` proxy: for ports a < b, b sends to a's proxy and a learns b's address. The lowest port hosts. Each instance plays its own port closed-loop (random macros, its own pad port); at the end every pair still in the match is compared: confirmed checksums (16-frame margin), per-frame traces, and the pads every frame consumed.

| Option | |
|---|---|
| `--ports 1,2,3,4` / `--chars fox,falco,mario,marth` | ports and one character each |
| `--teams 0,1,0,1` | a team battle (tests only: `PPR_GPRB_TEST_TEAMS` makes every instance's match a team battle with these teams before it loads; the online character select's teams are the setup branch's) |
| `--drop 4:40:stop,2:60:kill` | players who leave: port, seconds after the match started, `stop` (leaves the session: the `"leave"` message) or `kill` (its Dolphin is killed: GekkoNet's timeout) |
| `--plugin PPOnline.rel` | our game plugin on every SD card: P+ boots to the ONLINE page, the tool goes to the Versus character select, sets the plugin's `CFG_TEST_GONE` (its removal also runs in a local Versus match) and turns GameBridge's servicing off (it still finds the block, so the session writes the gone flags into SESSION; the plugin's `CLEANUP_CONNECTION` would otherwise stop the session) |
| `--pass-log`, `--save-countdown` | as for 1v1: every peer's passes and countdown savestate, for replays |

```
python harness/tools/gprb_session.py --ports 1,2,3,4 --chars fox,falco,mario,marth --preset typical --cpu sc --minutes 2 \
    --region-set gp-v20 --log-dir run/qa/nplayer/x --json run/qa/nplayer/x/r.json
python harness/tools/gprb_session.py --ports 1,2,3,4 --teams 0,0,1,1 --chars marth,ike,pikachu,falco --stage final_destination ...
python harness/tools/gprb_session.py --ports 1,2,3,4 --drop 4:40:stop --plugin game-code/PPOnline/PPOnline.rel ...
python harness/tools/gprb_session.py --ports 1,2,4 --chars fox,falco,mario --stage smashville ...      # a gap
```

Sim-start fixtures with 3-4 fighters: `gameplay_rollback.py prep --chars a,b,c,d` then `simstart` (both instances drive every port; the controllers follow the character count). The determinism branch made its own 4-fighter fixtures for its sync tests (`docs/nplayer/determinism.md`).

## Results (2026-10-09)

One 8-core PC shared with the other two agents' instances, all players on localhost, netsim `typical` (40 +- 8 ms round trip, 0.5 % loss) between every pair, P+'s rules shortened to 2 minutes (4 stocks), Null video, muted. Builds: `run/bin/nps-a5104f8e` and, after the merge with `nplayer-setup`, `run/bin/nps-71344706`. Raw: `run/qa/nplayer/<run>/` (`r.json`, every instance's log).

**Region set: gp-v20** (`nplayer-determinism`, `b828ca20`; copied to `run/qa/nplayer/sets/gp-v20-det-1557.json` and passed with `--region-set`). With gp-v19 every 3-4 player session diverged (below). The 1v1 regression runs use the default gp-v19.

| Run | Ports | Characters | Teams | CPU | Drop | Agreed F | Frames, end | Rollbacks (max depth) | Confirmed checksums, mismatches | Traces identical through game set | Save / load avg | Rollback cost avg |
|---|---|---|---|---|---|---|---|---|---|---|---|---|
| s3ffa1 | 1,2,3 | Fox, Falco, Mario | - | sc | - | - | 7,193, game set | 76/200/149 (5) | 7,178, **0** | yes | 826 / 1,131 us | 7.8 ms |
| s3ffa2dc | 1,2,3 | Marth, Ike, Pikachu | - | dc | - | - | 7,193, game set | 416/427/434 (3) | 7,178, **0** | yes | 843 / 1,158 us | 6.9 ms |
| s3ffa3 | 1,2,3 | Fox, Falco, Mario | - | sc | - | - | 6,987, game set | 179/240/17 (3) | 6,972, **0** | yes | 660 / 1,103 us | 7.6 ms |
| s3gap1 | 1,2,4 | Fox, Falco, Mario | - | sc | - | - | 7,193, game set | 75/275/102 (2) | 7,178, **0** | yes | 880 / 1,189 us | 8.6 ms |
| s3team1 | 1,2,3 | Fox, Falco, Mario | 0,0,1 (2v1) | sc | - | - | 7,193, game set | 27/177/146 (2) | 7,178, **0** | yes | 499 / 896 us | 5.5 ms |
| s4ffa2 | 1-4 | Fox, Falco, Mario, Marth | - | sc | - | - | 7,193, game set | 343/301/458/373 (7) | 7,178, **0** | yes | 885 / 1,197 us | 17.4 ms |
| s4ffa3dc | 1-4 | Fox, Falco, Mario, Marth | - | dc | - | - | 7,193, game set | 607/565/632/629 (6) | 7,178, **0** | yes | 884 / 1,232 us | 7.9 ms |
| s4team1 | 1-4 | Fox, Falco, Mario, Marth | 0,1,0,1 | sc | - | - | 7,193, game set | 95/336/368/178 (6) | 7,178, **0** | yes | 903 / 1,242 us | 9.3 ms |
| s4team3 | 1-4 | Marth, Ike, Pikachu, Falco | 0,0,1,1 | sc | - | - | 7,193, game set | 49/185/221/431 (2) | 7,178, **0** | yes | 537 / 946 us | 6.9 ms |
| s4team4dc | 1-4 | Fox, Falco, Mario, Marth | 0,1,1,0 | dc | - | - | 7,193, game set | 582/549/536/597 (3) | 7,178, **0** | yes | 609 / 1,080 us | 6.2 ms |
| s4drop1 | 1-4 | Fox, Falco, Mario, Marth | - | sc | P4 leaves at 2,420 | 2,490 on all three | 7,193, game set | 103/253/235 (4) | 7,178, **0** | yes | 874 / 1,235 us | 8.4 ms |
| s4drop3hostb | 1-4 | Fox, Falco, Mario, Marth | - | sc | P1 (the host) leaves at 2,416 | 2,470 | 7,193, game set | 245/46/391 (7) | 7,178, **0** | yes | 583 / 1,040 us | 21.3 ms |
| s4dropplug2 | 1-4, plugin | Fox, Falco, Mario, Marth | - | sc | P4 leaves at 2,436 | 2,453 | 6,123, game set (by stocks: P4's fighter removed) | 92/219/103 (2) | 6,108, **0** | yes | 579 / 1,059 us | 7.2 ms |
| s4killplug2 | 1-4, plugin | Fox, Falco, Mario, Marth | - | sc | P2's Dolphin killed at 2,422 | 2,469 | 7,192, game set | 244/231/30 (6) | 7,177, **0** | yes | 532 / 967 us | 7.0 ms |
| s3drop2 | 1,2,3 | Fox, Falco, Mario | - | sc | P3 leaves at 1,833, then P2 at 3,707 | P3: 1,858 | P1 alone: session ended at 3,708 (`peer left`, `disconnected`) | 46 (3) | (one player left) | - | 794 / 1,077 us | 7.8 ms |
| s2gap | 1,3 | Fox, Falco | - | sc | - | - | 7,193, game set | 34/104 (7) | 7,178, **0** | yes | 727 / 995 us | 7.2 ms |
| s2reg1 | 1v1 (as before) | Fox, Falco | - | sc | - | - | 5,268, game set | 12/63 (2) | 5,253, **0** | yes | 711 / 983 us | 6.2 ms |
| s2reg2dc | 1v1 | Fox, Falco | - | dc | - | - | 6,305, game set | 158/174 (1) | 6,290, **0** | yes | 768 / 1,012 us | 5.4 ms |
| s2reg3 (merged build) | 1v1 | Fox, Falco | - | sc | - | - | 6,743, game set | 25/62 (2) | 6,728, **0** | yes | 468 / 843 us | 5.0 ms |

"Frames" are session frames (game frame = session frame + 240); the drop's frame is the session frame the harness saw when it stopped or killed the player; "Agreed F" is `gone_frame` on every remaining peer (always equal). Pads compared: 0 mismatches in every run listed.

What the drop runs show:
- **Agreement.** Every remaining peer reported the same F and `gone_flags` (status and, with the plugin, the bytes read from SESSION + 0x20C at `0x817C4960`) with only the gone port set, and the three ran on to game set with 0 confirmed-checksum mismatches and identical traces.
- **Leave** (`stop`): F 17-70 frames after the harness's stop (the leave message, then GekkoNet's claims); no stall.
- **Window closed** (`emu_stop`, 2026-10-10): emulation stops and the process stays, as when a player closes the game window in Qt. Until then the leave went out only at process exit (`~MainWindow`), so the others stalled up to the silence timeout (testers: "froze for 5 seconds, then the game continued"; the harness: 7.05 s, `peer timed out`). Now the gameplay backend leaves as emulation stops (`Gprb::RegisterOnlineBackend`'s Stopping hook): `peer left` 0.03-0.08 s after the close, the longest freeze 0.03-0.08 s, F equal on both (`test_online_leave.py::test_closing_the_window_in_a_three_player_room`).
- **Kill:** the remaining peers stalled 7.05 s (GekkoNet's silence timeout, `waits for the peer 6,281 (7,047 ms)`), then went on; F is the frame after the last input any of them had received (2,469, with the kill seen at 2,422-2,455).
- **The host leaving** does not matter in a match; the next host would be P2 (lowest port).
- **With the plugin** (`nplayer-setup`'s removal) P4's fighter was removed on every machine: the game ended by stocks at 6,123 instead of at the time limit, the same on all three, P3 first, P1 last (stage picker, `DecideOutcome` with the elimination order).
- **Down to one player:** the last player's session ended at once with `peer left` and `disconnected` (the 1v1 DISCONNECTED flow), without writing a flag for the second leaver.

**Cost with 4 fighters.** Save 0.53-0.91 ms and load 0.95-1.25 ms on average (3 players 0.50-0.88 / 0.90-1.19, 1v1 0.47-0.77 / 0.84-1.01), with four instances on one PC. The region set (gp-v20) is 45.5 MB in 109 ranges; dirty tracking keeps the cost close to the 1v1's. A rollback burst (load, re-runs, saves) averaged 6-9 ms (5-6 ms in 1v1); 17 and 21 ms in two runs while the machine also ran the other agents' instances. Rollbacks on 0.3-9 % of frames, deepest 7 (the prediction window); with four players each peer waits on the slowest of three links.

### Earlier runs: the region set with 3-4 fighters (gp-v19)

Before gp-v20, every 3-4 player session diverged, always in fighter 3 or 4 (s1: game frame 3,815, P3's status 274 against 69; s2: 979; s4ffa1 (4 players): 721, after which the peers reached game set on different frames). Evidence from s2 (`run/qa/nplayer/s2`: every peer's pass log and countdown savestate):
- Each peer's log flattened (`gprb_passlog.py flat`: every frame once with its confirmed input) and replayed from its own countdown savestate: the three replays are identical through game set (4,437 frames). So the start (seeds, sync block, task order) and the confirmed inputs agree between the peers.
- Each peer's log replayed with its rollbacks diverges from that: P2 at game frame 979 (P3's fighter, x 45.75 instead of 45.81), P3 at 980, the host at 3,079 (P3's status kind 0 instead of 67).
- Giving P2's mispredicted pass at session frame 736 its final input does not help; removing the rollback at update 737 moves the divergence to the rollback before it. So any rollback over those frames leaks state of fighter 3: memory outside the set.
- The determinism branch found it at the same time: P+'s per-port records above the heaps (0x935F0000, stride 0x880) were covered for P1 and P2 only (0x1000); gp-v20 covers all four (0x2000). With gp-v20 every 3-4 player session above ran clean.

### Open: one 4-player run with different inputs from frame 150

s4drop3host (gp-v20, 4 players, `--drop 1:40:stop`): P4 consumed different pads than P2 and P3 from game frame 390 (session frame 150) on, so its state differed (P2 and P3 agreed with each other through game set, also after the host left). At that time P4 had waited 11.7 s for the others in the first 2 s of the session (its round trip to one peer averaged 300 ms, jitter 266 ms: the PC was saturated); no warning was logged (no "cannot roll back", no lost GekkoNet inputs). It did not happen again in the 8 later 3-4 player runs, including the same scenario (s4drop3hostb). Not reproduced and not explained; pass logs were not recorded for that run. To find it: run the 4-player sessions with `--pass-log` under load and compare which port's input a peer's pass log confirms differently.

## Merging

- **`nplayer-setup`** is merged here (`a174637f`). Its `GameplaySession` changes are folded into this version: `ConnectOptions::teams`; teams 0-2 (`PeerData::ValidTeam`, `LockIn::team`); `DecideTeams` on the host's setup (players by port) and again on the joiner (`SetupFromJson`); `StagePickPort` with the last game's pickers; results by port with the elimination order and `DecideOutcome` for 3-4 players (`GameResult::pickers`/`place` replace this branch's earlier `loser`); `Lobby::teams/stage_pickers/setup_error`; its `ReadFighterFields` by player number. GameBridge: its v4 layout and `SessionAddress()` (this branch's copy was dropped); this branch keeps the names by server port for 3-4 players and rewrites SESSION in full after every match.
- **`nplayer-determinism`** merges without conflicts (`git merge-tree`): both branches made the same `make_instance(controllers=...)` change in `gameplay_rollback.py`. Its gp-v20 is what these sessions need; this branch still defaults to gp-v19.
- **`main`** has moved on (`11992449`, the pause-screen quit). `GameplaySession.cpp` merges without conflicts, but `game-code/PPOnline/source/online_match.cpp` conflicts between that commit and `nplayer-setup`'s removal (game code; not resolved here).

## Not done

- Rooms (server, the 4-panel CSS, teams on the CSS) are the next steps of `rooms.md`; the session takes the ports and peers from `ConnectOptions` (a room would fill them like `GameplayOnlineBackend` does from a 3-4 player `Match`).
- The online path with 3-4 players (matchmaking, the lobby and SESSION through GameBridge on the online CSS) was not run end to end; the harness drives `gprb_connect` and each game's Versus CSS directly, as the 1v1 netsim tests do.
- A joiner learns the host from the first peer that says it hosts (matchmaking does not tell the joiners who decides). A malicious member could claim it first; the worst it can do is make the match start fail (setups differ), as leaving would.
- A host who leaves at the barrier (between the first simulation frame and the countdown) makes the match start fail for everyone.
- The early-input divergence above.