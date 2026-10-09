# 3-4 player gameplay rollback: determinism

Branch `nplayer-determinism` (worktree `.claude/worktrees/nplayer-determinism`). Part of the 3-4 player work of `docs/design/rooms.md` §5: this document covers the region set and determinism of gameplay rollback with 3 and 4 fighters (free-for-all and team battles). The session and network layer are `docs/nplayer/session.md` (branch `nplayer-session`); the match setup and the game side are `docs/nplayer/setup.md` (branch `nplayer-setup`). Background: `docs/gameplay-rollback-status.md` (2 players, gp-v19), `docs/gprb-coverage.md`.

**Default region set: gp-v21** (gp-v19 with P+'s per-port records for all four ports, gp-v20, and the Tmp heap, gp-v21). Two holes were found: the first only with 3-4 players, the second in 2-player matches too.

**Measurement caveat**, as in the status doc: one 8-core Windows PC shared with two other agents (one of them running 4-instance sessions during most of these runs). Single core = `CPUThread = False`; dual core = `CPUThread = True` with the deterministic GPU thread. Video `Null`, muted. Timings are noisy.

## Summary

| | 2 players | 3 players | 4 players |
|---|---|---|---|
| Sweep sync tests (distance 7, 2-minute matches) against their no-rollback ground truth, gp-v20 | (the 2-player sweep: status doc) | **5 / 5** (4 free-for-all, 1 team battle 2 vs 1) | **24 / 24** (free-for-all, team battles, all items, forced Final Smashes, 4-player mirrors) |
| Sync test with recorded input against ground truth, gp-v19 | identical | identical (14,910 frames) | **drift** from game frame 535 (free-for-all) |
| the same, gp-v20 | identical (3 fixtures, 14,907-14,916 frames) | identical | identical through game set (6,546 and 6,706 frames) |
| Misprediction test (every frame's first run with another frame's input), single core, gp-v20 | - | identical (14,910 frames) | identical through game set |
| Misprediction test, dual core, distance 4, gp-v20 | identical (2 fixtures, 14,909-14,916 frames) | identical (14,910 frames) | identical through game set (free-for-all and team battle) |

The holes:
- **P+'s per-port records above the heaps** were in the set for P1 and P2 only (gp-v20). 4-player matches drifted within seconds.
- **The Tmp heap** was outside the set (gp-v21). A rollback across one of its long-lived allocations leaked the block; with deep rollbacks the heap fragmented until a per-frame allocation failed and the game skipped work. Found in the cost series' 2-player match (Zelda vs Ice Climbers, distance 7, game frame 1637); gp-v19 has it too.

The other state the 3rd and 4th fighters add (their instance and resource heaps, effects, articles, camera subjects, ground collisions, team battle rules) was already in the set: gp-v19 covered `Fighter1Instance` to `Fighter4Instance` and their resource heaps by name.

## Fixtures and inputs

**Scenarios** (`harness/tools/gprb_synctest.py`, `NP_SCENARIOS`): P+ competitive rules (4 stocks, 8 minutes, items off; the sweep shortens matches to 2 minutes); team battles with team attack on (P+'s default rule, checked in the match setup).

| Name | Players (P1..P4) | Stage | Teams |
|---|---|---|---|
| `f4-bf-zelda-ics-olimar-peach` | Zelda, Ice Climbers, Olimar, Peach | Battlefield | free-for-all |
| `f4-ps2-gw-snake-rob-wario` | Game & Watch, Snake, R.O.B., Wario | Pokémon Stadium 2 | free-for-all |
| `f4-sv-bowser-sheik-ivysaur-squirtle` | Bowser, Sheik, Ivysaur, Squirtle | Smashville | free-for-all |
| `f4-fd-charizard-link-diddy-pit` | Charizard, Link, Diddy Kong, Pit | Final Destination | free-for-all |
| `f4-dl-tlink-samus-falco-dedede` | Toon Link, Samus, Falco, King Dedede | Dream Land | free-for-all |
| `f4-fh-ics-ics-olimar-olimar` | Ice Climbers ×2, Olimar ×2 | Frigate Husk | free-for-all |
| `f3-fd-peach-gw-snake` | Peach, Game & Watch, Snake | Final Destination | free-for-all |
| `f3-gh-rob-zelda-wario` | R.O.B., Zelda, Wario | Green Hill Zone | free-for-all |
| `f3-tt-olimar-bowser-ics` | Olimar, Bowser, Ice Climbers | Temple of Time | free-for-all |
| `t4-bf-ics-olimar-vs-zelda-sheik` | Ice Climbers, Olimar / Zelda, Sheik | Battlefield | red / blue |
| `t4-sv-rob-wario-vs-bowser-peach` | R.O.B., Wario / Bowser, Peach | Smashville | red / blue |
| `t4-ps2-snake-gw-vs-charizard-squirtle` | Snake, Game & Watch / Charizard, Squirtle | Pokémon Stadium 2 | red / blue |
| `t4-lm-peach-peach-vs-snake-snake` | Peach ×2 / Snake ×2 | Luigi's Mansion | red / blue |
| `t3-dl-gw-ics-vs-rob` | Game & Watch, Ice Climbers / R.O.B. | Dream Land | red / blue |
| `f2-bf-zelda-ics`, `f3-bf-zelda-ics-olimar` | the first 2 and 3 players of `f4-bf-...` | Battlefield | free-for-all (the cost series) |

P+ v3.2 has no Pokémon Trainer (Charizard, Squirtle and Ivysaur are standalone characters) and no Zelda/Sheik down-B swap in the CSS sense: Zelda and Sheik are separate characters that transform into each other in the match (the forms seen below include both).

**How a match is set up** (`setup_match_n`), as the 2-player tools do: every port gets a standard controller (`make_instance(controllers=...)`; the 2-player instances keep 2), Fox/Falco tokens are placed on every port's panel with the CSS recipe, a team battle is switched on with port 1's hand on the BRAWL tab (top left, hand button 0x03; the CSS task's byte +0x5C8 reads 1 afterwards), Start, then on the stage select the wanted characters and teams are written into `gmSelCharData` (players +0x00 character, +0x0B `m_teamNo`), and the stage is picked. `check_setup` verifies the match: player count, characters, stocks, `m_isTeams` (gmGlobalModeMelee+0x13), team attack (init byte 2 bit 0), and each fighter's team as the match plays it (`ftEntry::m_pointTeam`, +0x60): every port its own in a free-for-all, the scenario's partition in a team battle. Notes for the setup work:
- the CSS's own team default after the BRAWL tab depends on when it is pressed (pressed before the tokens are placed every port read team 0; after, 0 1 1 0), so the setup writes `m_teamNo` itself;
- `gmPlayerInitData` +0x05/+0x06 are the costume bytes: in a team battle the Versus setup replaces them with the team's costume; they are not the team.

**Fixtures** (`run/qa/nplayer/fixtures/`): `gprb_mispredict.py prep` saves a countdown savestate (game frame 100-103) and writes the scenario next to it (`<state>.json`: players, port mask, teams); `record` plays the match closed-loop (seeded random macros, `gentle_macro`, every port; in a team battle each fighter goes for the other team) and records every port's `gfPadStatus` slot of every game frame (`game_pads`, anchored on all ports) for 15,000 frames. The other commands read the port count from `<state>.json` (older 2-player fixtures without one: 2 players).

| Fixture | Players | Recorded frames | Without rollback (ground truth) |
|---|---|---|---|
| `f4-bf-zelda-ics-olimar-peach` | 4 | 14,862 | game set at session frame 6,546 (three fighters out of stocks) |
| `t4-bf-ics-olimar-vs-zelda-sheik` | 4, teams | 14,851 | game set at 6,706 |
| `t4-sv-rob-wario-vs-bowser-peach` | 4, teams | 13,697 | |
| `f3-fd-peach-gw-snake` | 3 | 14,865 | 14,918 frames, no game set |
| `f2-bf-zelda-ics`, `f3-bf-zelda-ics-olimar` | 2, 3 | 14,862, 14,869 | |
| `fd-mario-marth`, `ps2-ics-olimar`, `sv-peach-gw` | 2 | 14,863-14,865 | no game set |

(The recording itself ran without the session's pacing, so the replays through the session do not play the same match as the recording; every comparison is between runs of the same injected input.)

## Commands

```sh
# a fixture
python harness/tools/gprb_mispredict.py prep --scenario f4-bf-zelda-ics-olimar-peach --cpu sc --out run/qa/nplayer/fixtures/f4-bf-zelda-ics-olimar-peach.sav
python harness/tools/gprb_mispredict.py record --state run/qa/nplayer/fixtures/f4-bf-zelda-ics-olimar-peach.sav --frames 15000 --out run/qa/nplayer/fixtures/f4-bf-zelda-ics-olimar-peach.in.json
# ground truth, sync test, misprediction test (every port mispredicted by default)
python harness/tools/gprb_mispredict.py run --state <sav> --inputs <in.json> --cpu sc --modes ref --no-rollback --save-traces <dir>/gt
python harness/tools/gprb_mispredict.py run --state <sav> --inputs <in.json> --cpu sc --modes ref --distance 2 --save-traces <dir>/ref
python harness/tools/gprb_mispredict.py run --state <sav> --inputs <in.json> --cpu dc --modes mp --distance 4 --save-traces <dir>/mp
# closed-loop sync tests of the scenarios (groups np, f4, f3, t4, t3, f2)
python harness/tools/gprb_synctest.py --scenario f4,t4 --distance 2 --unthrottled --no-hash
# the sweep's 3-4 player groups (sync test + ground truth)
python harness/tools/gprb_sweep.py run --out run/qa/nplayer/sweep1 --groups np --jobs 3 --build dolphin/build/release/x64/Binaries --region-set gp-v20 --video Null
```

`--save-traces` dirs were compared with `gprb_ab.compare_traces` (RNGs, the fighters' checksum and per-port fields of all four ports, game set), dropping the last 8 frames.

## The hole: P+'s per-port records (gp-v20)

**Symptom.** The first closed-loop 4-player sync tests on gp-v19 (`f4-bf-zelda-ics-olimar-peach` and `t4-bf-ics-olimar-vs-zelda-sheik`, distance 2) logged a checksum mismatch every 150-200 frames from the first seconds (session frame 64: `g_mtRand` and the fighters). With the recorded input of the `f4-bf` fixture:

| Run (gp-v19) | Sync-test mismatches | Against ground truth |
|---|---|---|
| sync test, distance 2 | 22 (first at session frame 431) | P4's action state differs from game frame 535 (69 against 51); game set at 4,273 instead of 6,546 |
| misprediction test, distance 2 | (by design) | P4's action state differs from game frame 417 |

**Finding it.** Region-set dumps of the ground truth and the sync test (`PPR_GPRB_DUMP_FRAMES`, `scratch rdiff`) differed only in the known noise (tick-derived values in `.sbss` 0x805A036A and `rel27.bss` 0x80B8A2xx, dead stack, and 23 bytes of floats in P1's fighter heaps that differ the same way in a 2-player Mario/Marth run, where the traces stay identical) up to session frame 14. At frame 15 one byte of P1's instance heap (0x812BE264) was 0xC2 after the first run (as in the ground truth) and 0x80 after the resimulation; by frame 20 dozens of spans differed. An interpreter trace of game frame 255's first run and its resimulation (`gprb_mispredict.py --interpreter --trace-frame 255`) has the same control flow (3,741,044 records each); the store to 0x812BE264 (sora_melee 0x80781FA8) wrote different values, and the first differing load that is not a tick is P+ code at 0x8056F3BC reading 0x935F1238: 0x35 in the first run, 0x36 in the resimulation. That address is outside the set.

**Cause.** P+ keeps one record per port in high MEM2 above the last heap (P1 0x935F0000, P2 0x935F0880, P3 0x935F1100, P4 0x935F1980; stride 0x880, about 0x5C0 bytes of each written during a match: input buffers and counters of its codes). gp-v7 added 0x935F0000+0x1000, which covers P1 and P2 only. A census of the granules written outside the set during a match (`PPR_GPRB_CENSUS=1`) shows 0x935F1100+0x300 and 0x935F1980+0x300 (team battle: +0x5C0 each) written in 4-player matches and never in 2-player ones. A resimulated frame read P3's and P4's newer values.

**Fix (gp-v20).** gp-v19 with 0x935F0000+0x2000. The same recorded input: sync test and misprediction test identical to the ground truth through game set (6,546 frames), 0 sync-test mismatches.

**Not rolled back, as before:** the census lists more granules written only in 4-player matches, all of them of the kinds the 2-player work already classified as not gameplay: sound and AX buffers (MEM2 0x9025xxxx, 0x90ECxxxx), the main thread's dead stack (0x805AF340-0x805B1D80), GX display lists and the Tmp heap (0x8105xxxx, 0x8178xxxx), and nw4r render statics (`lbl_804A9A80`, `lbl_804C2340`). None of them changed a sync test or a misprediction test against its ground truth.

## The hole: the Tmp heap (gp-v21)

**Symptom.** The cost series' 2-player match (`f2-bf-zelda-ics`, Zelda vs Ice Climbers on Battlefield, recorded input) with gp-v20: at `distance` 7 (6 frames resimulated every frame), single and dual core, `g_mtRand` differs from the ground truth at game frame 1637 (`1747906815` against `1753075742`); the fighters follow. No sync-test mismatch at all: every run of every frame agreed with the others. `distance` 2 and 4 stay identical. gp-v19 diverges at the same frame. Restoring the task-id counter (`gUnk8059c66c`, left out on purpose since Phase 6) changes nothing.

**Finding it.**
- `PPR_GPRB_RNG_LOG` on both runs (`gprb_rngcmp.py`): at session frame 1397 the ground truth draws one random number from sora_melee `fn_27_200538+0x200` (a call of `fn_27_1EFE3C`, through `fn_27_1FF328` ← `fn_27_1F38E4`), the rolled-back run does not.
- Interpreter traces of game frame 1637 in both runs (`PPR_GPRB_INTERP_FROM`, `--trace-frame`): `fn_27_200538` takes another branch on a word at 0x80615798 (System heap): 0 in the ground truth, 0x80615760 in the rolled-back run. Earlier in the frame the ground truth stores 0 there at 0x808F72D0, in a function the rolled-back run leaves early.
- It leaves early because its scratch allocation fails: `gfHeapManager::alloc` of 0x1760 bytes from heap 12 (**Tmp**, 0x81049E60-0x81060F60, 94 KB) at 0x808F70D4. In the ground truth the allocator finds a free block of 0x10CC0 bytes; in the rolled-back run the free list holds only 0x1300, 0x60, 0x40, 0x40, 0x40 and 0x80 bytes, and the function gets null.
- Tmp was outside the set. `PPR_GPRB_PROBE` on the allocator's returns and `free` (ground truth, 618 frames): every Tmp block is freed in the frame it was allocated in, except four allocated at session frames 147-148 that stay for the rest of the match (sora_melee 0x808F4AF4 and 0x808F77D8, DOL 0x80116EE0 (0x3000 bytes) and 0x80013D70). A rollback across such an allocation restores the state that refers to the block, but not the heap: the block stays allocated and every resimulation allocates another one. With 6 resimulations of each frame the 94 KB heap is gone in seconds.

**Fix (gp-v21).** gp-v20 plus the Tmp heap. `f2-bf-zelda-ics`, distance 7: sync test (single core) and misprediction test (dual core) identical to the ground truth for 3,755 frames, past game frame 1637. Cost: about 220 more granules per save (9,228 against 9,011 on average).
- An all-threads interpreter trace of the 10 frames around the long-lived allocations (game frames 384-393): every store into Tmp comes from the main thread (121,467 stores; the pad thread 0x805BA108, three other threads and the idle loop store nothing there).
- In network sessions the hole needs a rollback across one of the rare long-lived Tmp allocations; each one leaks a block on the peer that rolled back, so the peers' heaps differ and a later allocation can fail on one peer only. It is a 2-player hole as much as a 3-4 player one.

## Results on gp-v20

### Sync and misprediction tests with recorded input

Against each fixture's ground truth (`--no-rollback`, single core), raw `run/qa/nplayer/r2`, `dc1`, `reg2p`:

| Fixture | Run | Frames | Result |
|---|---|---|---|
| `f4-bf-zelda-ics-olimar-peach` | sync test, sc, distance 2 | 6,546 | identical through game set, 0 mismatches |
| | misprediction, sc, distance 2 | 6,546 | identical through game set |
| | misprediction, dc, distance 4 | 6,546 | identical through game set |
| `t4-bf-ics-olimar-vs-zelda-sheik` | sync test, sc, distance 2 | 6,706 | identical through game set, 0 mismatches |
| | misprediction, sc, distance 2 | 6,706 | identical through game set |
| | misprediction, dc, distance 4 | 6,706 | identical through game set |
| `f3-fd-peach-gw-snake` | sync test, sc, distance 2 | 14,985 | identical (14,910 frames compared) |
| | misprediction, sc, distance 2 | 14,952 | identical |
| | misprediction, dc, distance 4 | 14,923 | identical |
| `fd-mario-marth` (2 players) | misprediction, dc, distance 4 | 14,924 | identical |
| `ps2-ics-olimar` (2 players) | misprediction, dc, distance 4 | 14,948 | identical |
| `sv-peach-gw` (2 players) | sync test, sc, distance 7 | 14,915 | identical, 0 mismatches |

"Misprediction": every frame's first run gives every port the recorded input of 7 frames earlier, the resimulations the true input (`mispredict_ports` = all ports, `mispredict_offset` -7).

### The sweep's 3-4 player groups

`gprb_sweep.py run --groups np` (raw `run/qa/nplayer/sweep1`), gp-v20, this branch's build, single core, `distance` 7 (6 frames resimulated every frame), 2-minute matches, every run's trace compared with its pass log replayed without rollback. **29 of 29 passed**: every sync test 0 mismatches and identical to its ground truth for all 7,208 compared frames (every match ran to the time limit, game set at session frame 7,193). 208,597 frames, 1.25 million resimulated frames.

| Group | Runs | Result |
|---|---|---|
| `np-f4` free-for-all, 4 players | 6 | 6 / 6 |
| `np-f3` free-for-all, 3 players | 3 | 3 / 3 |
| `np-t4` team battles 2 vs 2 | 4 | 4 / 4 |
| `np-t3` team battle 2 vs 1 | 1 | 1 / 1 |
| `np-items` all items, highest frequency (one team battle) | 3 | 3 / 3 |
| `np-ffs` Smash Ball only, every port's Final Smash forced (`PPR_GPRB_FORCE_FINAL=15`; one team battle) | 4 | 4 / 4 |
| `np-mirror` 4 of one character | 8 | 8 / 8 |

Forms seen: Zelda and Sheik transforming (in free-for-all, team battle and the Zelda mirror), Wario-Man (forced Final Smash), Popo and Nana. The full matrix: `gprb_sweep.py report --out run/qa/nplayer/sweep1`.

## Cost per frame

(in progress: the cost series on gp-v21)

## Harness changes

All in `harness/tools/` (commit `b828ca20` and later):
- `gameplay_rollback.py`: `make_instance(controllers=...)` (default the old 2), `instances` sets every configured port neutral, `load_fixture` every port that has a controller.
- `gprb_synctest.py`: `Scenario` and `NP_SCENARIOS`, `scenario(name)` for both tables, `setup_match_n` (any number of ports, team battles), `write_selections(..., teams)`, `css_team_battle`, `check_setup`, `fighter_teams`, `TeamFighter`; the sync test passes its `ports` mask; `--scenario` takes the groups `np`, `f4`, `f3`, `t4`, `t3`, `f2`.
- `gprb_mispredict.py`: fixtures carry `<state>.json`; `record` drives and anchors every port; `run` passes `ports`, mispredicts every port by default (`--mispredict-ports`), retries a launch that lost its harness port, and reports the cost of the running phase (`cost:` line).
- `gprb_sweep.py`: runs with `chars` and `teams`; groups `np-*`; the forced Final Smash mask covers every port; the report has a 3-4 player table.
- `gprb_replay_stress.py`: `--players` (default from `<state>.json`).
- `gprb_memdiff.py diff --only-inset`.
- Dolphin: `RollbackManager` counts the granules each save copies and each load restores (`gprb_status` `save_granules_total/max`, `load_granules_total/max`). `GameplaySession.cpp`: only those four status fields.

The C++ side needed nothing else for 3-4 players in a single instance: the sync test already took a port mask (`ports`), the frame trace and the checksum already covered all four ports (24 fighter words), and the pass log keeps all four pad slots.

Not generalised: `gprb_ab.py` (the Phase 1 two-instance A/B tool), `gprb_resimtrace.py` (its own fixed 2-port script; `gprb_mispredict.py --trace-frame` does the same for any fixture), and the two-instance session tools (`gprb_session.py`, `gprb_online.py`; multi-instance sessions are the session work's).

## Open issues

(in progress)
