# Gameplay rollback: coverage sweep

A sweep of the gameplay-only rollback mode over every Project+ v3.2 character, the legal stages, items, Final Smashes, and two-instance sessions. It ran twice:

- **gp-v12** (`run/bin/gprb-b79fe057cf`, `dolphin-gprb` `b79fe057cf`; `run/qa/sweep2`): 101 of 136 passed. Its failures were the work list of Phase 8 of `docs/gameplay-rollback-status.md`; their record is kept below ([gp-v12 failures](#failures-of-the-gp-v12-sweep-by-family)).
- **gp-v19** (`run/bin/gprb-e642b3ce98`, `dolphin-gprb` `e642b3ce98`; `run/qa/sweep3`), after the fixes: **143 of 143** passed, the same 136 runs plus 7 with forced Final Smashes.

Both: tool `harness/tools/gprb_sweep.py` (raw results `<out>/runs/<id>.json`, logs of failed runs in `<out>/logs/`, screenshots in `<out>/shots/`, replay files of failed runs in `<out>/work/`); 2026-10-07, one 16-thread Windows PC shared with other agents' Dolphin instances, at most 3 of the sweep's instances at a time. The first pass on `gprb-fe35002ead` (gp-v9) is compared at the end ([gp-v9 against gp-v12](#gp-v9-against-gp-v12)).

## Summary

| Mode | gp-v12 | gp-v19 |
|---|---|---|
| Sync test, each character vs Fox (Battlefield) | 38 / 41 | **41 / 41** |
| Sync test, mirror matches (Battlefield) | 29 / 42 | **42 / 42** |
| Sync test, legal stages (Fox vs Falco) | 10 / 10 | **10 / 10** |
| Sync test, all items, highest frequency | 1 / 6 | **6 / 6** |
| Sync test, Smash Ball only | 0 / 7 | **7 / 7** |
| Sync test, Smash Ball only, Final Smash forced (new, `ffs`) | - | **7 / 7** |
| Sessions through netsim `typical`, single core | 13 / 15 | **15 / 15** |
| Sessions through netsim `typical`, dual core | 10 / 15 | **15 / 15** |
| **All** | **101 / 136 (74 %)** | **143 / 143 (100 %)** |

Every gp-v19 sync test also matched its no-rollback ground truth through game set; every session ended in game set on both peers with 0 confirmed-checksum mismatches and identical traces.

**Retries on gp-v19.** `m-yoshi` (in the driver) and `m-ness` (in its ground-truth replay) first failed to launch an instance ("another harness client is already connected", the harness-port collision of open issue 8 in the status doc). Run again together with `--retry-errors`, both **hung** (`m-yoshi` at session frame 4,270, `m-ness` at 3,865, both in the effect manager's list walk `fn_8005C2B0`; JSON kept in `run/qa/sweep3-hangs/`), and a third run (`--rerun`) passed both. Their exact replays mostly run to the end, but `m-yoshi`'s crashed in 7 of 25 replays in effect/render code: a nondeterministic fault, open issue 9 in the status doc. The table counts the third runs.

### Before and after, per failure family

| Family (gp-v12) | gp-v12 failures | Cause | Fix | Exact replays of the kept failures on gp-v19 | gp-v19 sweep |
|---|---|---|---|---|---|
| D. Items and Smash Balls | 9 of 13 (7 drifts, 2 hangs; plus `i-rob-game_and_watch`, Family B) | stale list heads when items appear or vanish (camera subjects, archives), mid-match file loads completing on emulated time | gp-v14, gp-v15 + the file IO wait, gp-v16 | `f-wario-bowser`, `f-samus-zero_suit_samus`, `i-mario-marth`: identical through game set; `i-fox-falco`, `f-ice_climbers-peach`, `f-olimar-lucario`: no hang, identical | 13 / 13 |
| B. Early hang in mirrors | 8 (7 mirrors + 1 item run) | nw4r::ef draw strategies: function-local statics partly in the set, guards outside | gp-v13 | `m-yoshi`, `m-ice_climbers`: past the hang point, identical | all pass |
| A. Zelda/Sheik | 7 (2 drifts, 3 hangs, 2 session desyncs) | the transformation's file loads, the archive list, the scene's per-model records | gp-v15/IO wait, gp-v16, gp-v17 | `v-zelda`, `v-sheik`, `m-zelda`, `m-sheik`, `f-zelda-sheik`: identical | all pass |
| C. Meta Knight | 2 hangs | the scene's per-model records | gp-v17 | `v-meta_knight`, `m-meta_knight`: past the hang point, identical | both pass |
| E. Late mirror drifts | 3 (DK, Mewtwo, Wolf) | Mewtwo, Wolf: covered by gp-v13 to gp-v17; DK: the controller configuration's stick state | gp-v18 | all three identical through game set | all pass |
| F. Session desyncs | 7 (5 dual core, 2 single core) | the families above, hit more often by dual core's 2-4 times as many rollbacks | - | (sessions keep no replay) | 30 / 30 |
| Final Smashes | never seen executing | the EffectSystem partly in the set (a forced Wario-Man hung on turning back) | gp-v19, `PPR_GPRB_FORCE_FINAL` | forced FS replays of 6 kept logs: identical | `ffs` 7 / 7 |

### Final Smashes

The `ffs` group gives both ports their Final Smash at the last countdown frame (`PPR_GPRB_FORCE_FINAL=3`: the session calls `ftManager::setFinal`, as a broken Smash Ball does, in the sync test and in its ground truth alike); the random fighters use it at their first neutral B. One pair per kind: Wario/Bowser (transformations), Samus/ZSS (beam, suit), Olimar/Lucario (cutscene, beam), Marth/Ike (cutscenes), Mario/Pikachu (projectile, Volt Tackle), Captain Falcon/Ganondorf (cutscene, transformation), Zelda/Sheik (arrows). All 7 passed against ground truth through game set. Evidence that they ran: the forms seen include Wario-Man and Giga Bowser, the logs show the fighter changes (Wario/Bowser at game frames 333 and 1,339; Zelda/Sheik's), and in the replays of kept logs (status doc, Phase 8) the forced Final Smash changed the match from the first neutral B on (Olimar's status 274 → 278/279; the matches ended 1,000-3,500 frames earlier) while the rolled-back run stayed identical to its ground truth. Without forcing, Wario-Man also appeared in the gp-v19 Smash Ball run `f-wario-bowser`, which passed.

## Matrices (gp-v19, with gp-v12 for comparison)

"gf N" is the first game frame that differs; "@N" the session frame at which a hang stopped. Mirror results on gp-v12, for the runs that failed there: Yoshi, Dedede, G&W, R.O.B., Ice Climbers, Knuckles, Lucas: early hang (Family B); Zelda, Sheik, Meta Knight: hang; Donkey Kong: desync; Mewtwo, Wolf: drift.

| Character | vs Fox | vs Fox, gp-v12 | Mirror | Forms seen |
|---|---|---|---|---|
| mario | pass | pass | pass | mario |
| luigi | pass | pass | pass | luigi |
| peach | pass | pass | pass | peach |
| wario | pass | pass | pass | wario |
| yoshi | pass | pass | pass | yoshi |
| bowser | pass | pass | pass | bowser |
| donkey_kong | pass | pass | pass | donkey_kong |
| diddy_kong | pass | pass | pass | diddy_kong |
| captain_falcon | pass | pass | pass | captain_falcon |
| fox | pass | pass | pass | fox |
| falco | pass | pass | pass | falco |
| wolf | pass | pass | pass | wolf |
| link | pass | pass | pass | link |
| toon_link | pass | pass | pass | toon_link |
| zelda | pass | **DRIFT** gf 929 | pass | sheik, zelda |
| sheik | pass | **DRIFT** gf 788 | pass | sheik, zelda |
| ganondorf | pass | pass | pass | ganondorf |
| pikachu | pass | pass | pass | pikachu |
| jigglypuff | pass | pass | pass | jigglypuff |
| mewtwo | pass | pass | pass | mewtwo |
| squirtle_solo | pass | pass | pass | squirtle |
| ivysaur_solo | pass | pass | pass | ivysaur |
| charizard_solo | pass | pass | pass | charizard |
| lucario | pass | pass | pass | lucario |
| samus | pass | pass | pass | samus |
| zero_suit_samus | pass | pass | pass | zero_suit_samus |
| ness | pass | pass | pass | ness |
| lucas | pass | pass | pass | lucas |
| kirby | pass | pass | pass | kirby |
| meta_knight | pass | **HANG** @921 | pass | meta_knight |
| king_dedede | pass | pass | pass | king_dedede |
| marth | pass | pass | pass | marth |
| roy | pass | pass | pass | roy |
| ike | pass | pass | pass | ike |
| game_and_watch | pass | pass | pass | game_and_watch |
| rob | pass | pass | pass | rob |
| ice_climbers | pass | pass | pass | popo |
| pit | pass | pass | pass | pit |
| olimar | pass | pass | pass | olimar |
| snake | pass | pass | pass | snake |
| sonic | pass | pass | pass | sonic |
| knuckles | pass | pass | pass | knuckles |

| Stage | kind | Fox vs Falco | gp-v12 |
|---|---|---|---|
| battlefield | 0x01 | pass | pass |
| final_destination | 0x02 | pass | pass |
| pokemon_stadium_2 | 0x2E | pass | pass |
| smashville | 0x21 | pass | pass |
| luigis_mansion | 0x04 | pass | pass |
| temple_of_time | 0x09 | pass | pass |
| green_hill_zone | 0x23 | pass | pass |
| bowsers_castle | 0x06 | pass | pass |
| frigate_husk | 0x0C | pass | pass |
| dream_land | 0x2D | pass | pass |

| Items run | items | result | gp-v12 | forms seen |
|---|---|---|---|---|
| charizard_solo vs squirtle_solo, smashville | smashball | pass | **DRIFT** gf 1534 | charizard, squirtle |
| ice_climbers vs peach, final_destination | smashball | pass | **HANG** @1971 | peach, popo |
| ivysaur_solo vs pikachu, battlefield | smashball | pass | **DRIFT** gf 1728 | ivysaur, pikachu |
| olimar vs lucario, battlefield | smashball | pass | **HANG** @4492 | lucario, olimar |
| samus vs zero_suit_samus, battlefield | smashball | pass | **DRIFT** gf 1662 | samus, zero_suit_samus |
| wario vs bowser, final_destination | smashball | pass | **DRIFT** gf 1706 | bowser, wario, warioman |
| zelda vs sheik, battlefield | smashball | pass | **HANG** @2154 | sheik, zelda |
| captain_falcon vs ganondorf, battlefield | smashball, Final Smash forced | pass | - | captain_falcon, ganondorf |
| mario vs pikachu, final_destination | smashball, Final Smash forced | pass | - | mario, pikachu |
| marth vs ike, battlefield | smashball, Final Smash forced | pass | - | ike, marth |
| olimar vs lucario, battlefield | smashball, Final Smash forced | pass | - | lucario, olimar |
| samus vs zero_suit_samus, battlefield | smashball, Final Smash forced | pass | - | samus, zero_suit_samus |
| wario vs bowser, final_destination | smashball, Final Smash forced | pass | - | bowser, giga_bowser, wario, warioman |
| zelda vs sheik, battlefield | smashball, Final Smash forced | pass | - | sheik, zelda |
| fox vs falco, battlefield | all | pass | **HANG** @6104 | falco, fox |
| mario vs marth, final_destination | all | pass | **DRIFT** gf 4097 | mario, marth |
| peach vs diddy_kong, smashville | all | pass | **DRIFT** gf 3866 | diddy_kong, peach |
| pikachu vs olimar, dream_land | all | pass | **DRIFT** gf 3382 | olimar, pikachu |
| rob vs game_and_watch, battlefield | all | pass | **HANG** @40 | game_and_watch, rob |
| snake vs ice_climbers, pokemon_stadium_2 | all | pass | pass | popo, snake |

| Session | stage | single core | dual core |
|---|---|---|---|
| fox vs falco | battlefield | pass | pass |
| mario vs marth | final_destination | pass | pass |
| peach vs game_and_watch | pokemon_stadium_2 | pass | pass |
| ice_climbers vs olimar | smashville | pass | pass |
| zelda vs sheik | battlefield | pass | pass |
| squirtle_solo vs charizard_solo | dream_land | pass | pass |
| samus vs zero_suit_samus | frigate_husk | pass | pass |
| snake vs rob | luigis_mansion | pass | pass |
| pikachu vs jigglypuff | green_hill_zone | pass | pass |
| diddy_kong vs king_dedede | temple_of_time | pass | pass |
| link vs toon_link | bowsers_castle | pass | pass |
| marth vs roy | smashville | pass | pass |
| lucario vs mewtwo | pokemon_stadium_2 | pass | pass |
| sonic vs knuckles | battlefield | pass | pass |
| wolf vs captain_falcon | final_destination | pass | pass |

## How it was run

### Runs

Command: `python harness/tools/gprb_sweep.py run --out run/qa/sweep2 --jobs 3 --retry-errors` (136 runs; `list` prints them). The gp-v19 sweep: `python harness/tools/gprb_sweep.py run --out run/qa/sweep3 --jobs 3 --retry-errors` (143 runs, the build and region set are the tool's defaults now), then `--retry-errors` and `--rerun --only "^m-(yoshi|ness)$"` (see Summary).

| Group | Runs | P1 vs P2 | Stage | Items |
|---|---|---|---|---|
| `vsfox` (`v-<char>`) | 41 | every P+ character vs Fox (Fox vs Fox is `m-fox`) | Battlefield | off |
| `mirror` (`m-<char>`) | 42 | every character vs itself | Battlefield | off |
| `stage` (`s-<stage>`) | 10 | Fox vs Falco | each legal stage | off |
| `items` (`i-…`) | 6 | Fox/Falco, Mario/Marth, Peach/Diddy, Snake/Ice Climbers, Pikachu/Olimar, R.O.B./G&W | BF, FD, SV, PS2, DL, BF | all items, frequency 4 (the highest) |
| `fs` (`f-…`) | 7 | Samus/ZSS, Wario/Bowser, Zelda/Sheik, Charizard/Squirtle, Ivysaur/Pikachu, Ice Climbers/Peach, Olimar/Lucario | BF, FD, SV | Smash Ball only, frequency 4 |
| `session-sc`, `session-dc` (`n-sc-…`, `n-dc-…`) | 15 + 15 | the 15 pairs in the session table below | various | off |

**Characters.** P+ v3.2's CSS has 42 characters (`brawl.PPLUS_CSS_ROSTER` without Random). There is no Pokémon Trainer: Squirtle, Ivysaur and Charizard are separate characters, and all three were run. Zelda and Sheik are separate CSS slots, but **down-B still transforms**: the sweep saw both forms in every Zelda and Sheik run (the "Forms seen" column). This contradicts the Phase 3 coverage caveat in `gameplay-rollback-status.md`. Samus and Zero Suit Samus only change form through the Final Smash.

**Stages.** The legal list is P+'s 2024 Proposed ruleset (the 9 stages Orca's ranked mode uses: Battlefield, Pokémon Stadium 2, Smashville, Luigi's Mansion, Temple of Time, Green Hill Zone, Bowser's Castle, Frigate Husk, Dream Land) plus Final Destination. `--extended-stages` adds the other 11 stages of SSS page 0; they were not run on gp-v12.

**Match setup.** As in `gprb_synctest.py`: Fox and Falco are picked on the CSS, the wanted characters are written into `gmSelCharData` on the stage select, and the stage is picked closed loop. Rules: 4 stocks, a 2-minute limit (`--minutes 2`), so every match ends in game set, by stocks or at 7,193 session frames. Items: the record's item frequency (`gmGlobalRecord+0x810`) and item switch (`+0x818`, 64 bits in `gmItSwitch` order; bit 0 of the second word is the Smash Ball).

**Mirror matches** give both players the same costume, which the real CSS does not allow (P1's costume 0 from Fox, P2's from Falco). The early hang of Family B needs rollback, though: the Ice Climbers mirror without rollback ran 2 of 2 matches to the end, and with the sync test it stuck in 1 of 2.

**Input.** Both ports play seeded random macros, closed loop, from `sweep_macro`:
- movement and jumps; jab, tilts, C-stick smashes, aerials;
- all four specials, with neutral B sometimes held 30 frames (it also starts a Final Smash);
- shield, rolls, spot dodge, grab followed by a throw in one of four directions.

A stage-aware `decide` keeps them near the centre, and double-jumps and up-Bs back from off stage. Without it, Fox self-destructed every few seconds and matches ended before anything was tested. `--macro-exclude` drops categories; it was used to bisect on gp-v9.

### Pass criteria

**Sync test** (`gprb_synctest`, single core, unthrottled, D3D11 headless, `distance` 7: every frame the state from 7 frames back is restored and 6 frames are run again, the whole prediction window). A run passes only if all three hold:
1. **The sync test's own check:** each frame's checksum after a resimulation equals its first run (frame counter, RNGs, per port the active instance, damage, stocks, x, y, status).
2. **No crash and no hang** (no frame progress for 45 s).
3. **Ground truth** (Phase 6): the run's pass log (`PPR_GPRB_PASS_LOG`) is flattened with `gprb_passlog` `flat`: every frame once, with its final input, no rollback. It is replayed from the run's countdown savestate in a second instance, and the two per-frame traces (`frame_trace`: RNGs, fighters, game set) must be identical up to the end of the shorter one. A run that passes 1 and 2 but fails 3 is a **drift**.

The ground truth catches what check 1 cannot: on gp-v10, the Mario mirror ran to the end with 0 sync-test mismatches, yet drifted from no-rollback play at game frame 479 (`run/qa/sweep2-control-gpv10`). That is the fault gp-v11 fixed, found again by this method.

**Session** (`gprb_connect`, A hosts, B joins through `ppharness.netsim` `typical`: 40 ± 8 ms RTT, 0.5 % loss; delay 2; independent menu paths as in `gprb_session.py`; each instance plays only its own port). A session passes if both instances reach game set with no confirmed-checksum mismatch and identical per-frame traces up to the confirmed frame. The criterion is that both peers agree. No session was compared with a no-rollback replay.

**Results per run** (JSON): result, symptom, first mismatching frame, ground-truth comparison (`ground_truth.diverged_at`, the differing fields and values), sync-test desync log, the main thread's backtrace on a hang, notable OSReport and log lines with counts, the log tail, the forms seen per port, a timeline every ~10 s (ftKind, stocks, damage, status per port), and the screenshots. A hung game presents no new frame, so a screenshot taken at the hang never completes. The sweep therefore takes one every 40 s and keeps the latest for a failed run (`<id>-last?.png`).

### Reproducing a failure

- **Run it again** (same scenario and seeds; inputs are closed loop, so frames differ from run to run): the `cmd` field of the run's JSON, e.g. `python harness/tools/gprb_sweep.py run --out run/qa/sweep2 --only "^v-zelda$" --rerun`.
- **Replay it exactly** (failed sync tests marked "replay kept" below): the countdown savestate and pass log are kept in `run/qa/sweep2/work/<id>/`.
  ```
  GPRB_NAME_PREFIX=sweep PPHARNESS_DOLPHIN_DIR=run/bin/gprb-b79fe057cf \
  python harness/tools/gprb_mispredict.py run --cpu sc \
      --state run/qa/sweep2/work/<id>/<id>.sav --inputs run/qa/sweep2/work/empty-inputs.json \
      --modes replay=run/qa/sweep2/work/<id>/<id>.synctest.m0 --save-traces <dir>
  ```
  `--inputs` is required by the tool and unused by a replay; `empty-inputs.json` is `{"pads": []}`. Checked for `v-zelda`: the replay ends at the same 2,915 frames, with the same broken Sheik values at game frames 928-930 as the run. Replay files of a failure family's duplicates were deleted to save disk (about 45 MB each); their JSON names the kept one.
- **Sessions** keep no pass log. `gprb_session.py --p1 … --p2 … --stage … --cpu dc --preset typical --pass-log DIR --save-countdown DIR` records one for `gprb_mispredict.py replay=`.

## Matrices of the gp-v12 sweep

"gf N" is the first game frame that differs: for a drift, from the no-rollback replay; for a desync, also from the sync test's own check. "@N" is the session frame at which a hang stopped. The game frame is the session frame plus 240.

### Characters

| Character | vs Fox | Mirror | Forms seen |
|---|---|---|---|
| mario | pass | pass | mario |
| luigi | pass | pass | luigi |
| peach | pass | pass | peach |
| wario | pass | pass | wario |
| yoshi | pass | **HANG** @97 | yoshi |
| bowser | pass | pass | bowser |
| donkey_kong | pass | **DESYNC** gf 5644 | donkey_kong |
| diddy_kong | pass | pass | diddy_kong |
| captain_falcon | pass | pass | captain_falcon |
| fox | pass (= mirror) | pass | fox |
| falco | pass | pass | falco |
| wolf | pass | **DRIFT** gf 4088 | wolf |
| link | pass | pass | link |
| toon_link | pass | pass | toon_link |
| zelda | **DRIFT** gf 929 | **HANG** @1697 | sheik, zelda |
| sheik | **DRIFT** gf 788 | **HANG** @1875 | sheik, zelda |
| ganondorf | pass | pass | ganondorf |
| pikachu | pass | pass | pikachu |
| jigglypuff | pass | pass | jigglypuff |
| mewtwo | pass | **DRIFT** gf 5529 | mewtwo |
| squirtle (solo) | pass | pass | squirtle |
| ivysaur (solo) | pass | pass | ivysaur |
| charizard (solo) | pass | pass | charizard |
| lucario | pass | pass | lucario |
| samus | pass | pass | samus |
| zero_suit_samus | pass | pass | zero_suit_samus |
| ness | pass | pass | ness |
| lucas | pass | **HANG** @114 | lucas |
| kirby | pass | pass | kirby |
| meta_knight | **HANG** @921 | **HANG** @725 | meta_knight |
| king_dedede | pass | **HANG** @46 | king_dedede |
| marth | pass | pass | marth |
| roy | pass | pass | roy |
| ike | pass | pass | ike |
| game_and_watch | pass | **HANG** @123 | game_and_watch |
| rob | pass | **HANG** @103 | rob |
| ice_climbers | pass | **HANG** @26 | popo |
| pit | pass | pass | pit |
| olimar | pass | pass | olimar |
| snake | pass | pass | snake |
| sonic | pass | pass | sonic |
| knuckles | pass | **HANG** @69 | knuckles |

### Stages (Fox vs Falco, items off)

| Stage | Kind | Result |
|---|---|---|
| Battlefield | 0x01 | pass |
| Final Destination | 0x02 | pass |
| Pokémon Stadium 2 | 0x2E | pass |
| Smashville | 0x21 | pass |
| Luigi's Mansion | 0x04 | pass |
| Temple of Time | 0x09 | pass |
| Green Hill Zone | 0x23 | pass |
| Bowser's Castle | 0x06 | pass |
| Frigate Husk | 0x0C | pass |
| Dream Land | 0x2D | pass |

### Items and Final Smashes

| Run | Items | Result |
|---|---|---|
| Fox vs Falco, Battlefield | all | **HANG** @6104 |
| Mario vs Marth, Final Destination | all | **DRIFT** gf 4097 |
| Peach vs Diddy Kong, Smashville | all | **DRIFT** gf 3866 |
| Snake vs Ice Climbers, Pokémon Stadium 2 | all | pass |
| Pikachu vs Olimar, Dream Land | all | **DRIFT** gf 3382 |
| R.O.B. vs Game & Watch, Battlefield | all | **HANG** @40 (Family B) |
| Samus vs ZSS, Battlefield | Smash Ball | **DRIFT** gf 1662 |
| Wario vs Bowser, Final Destination | Smash Ball | **DRIFT** gf 1706 |
| Zelda vs Sheik, Battlefield | Smash Ball | **HANG** @2154 (Family A) |
| Charizard vs Squirtle, Smashville | Smash Ball | **DRIFT** gf 1534 |
| Ivysaur vs Pikachu, Battlefield | Smash Ball | **DRIFT** gf 1728 |
| Ice Climbers vs Peach, Final Destination | Smash Ball | **HANG** @1971 |
| Olimar vs Lucario, Battlefield | Smash Ball | **HANG** @4492 |

**Final Smashes were not verified.** Smash Balls do spawn with this item switch: in a check without rollback, Falco broke one within 30 s and used his Landmaster. But the sweep's 3-second samples never saw a Final Smash form (Wario-Man, Giga Bowser, Samus becoming ZSS), and the random fighters' chance of breaking a ball and then pressing B is unknown. The FS group therefore tests "items: Smash Ball only" for sure, and Final Smashes only possibly.

### Sessions (`typical`, delay 2)

| Pair | Stage | Single core | Dual core |
|---|---|---|---|
| Fox vs Falco | Battlefield | pass | **DESYNC** gf 4438 |
| Mario vs Marth | Final Destination | pass | pass |
| Peach vs Game & Watch | Pokémon Stadium 2 | pass | pass |
| Ice Climbers vs Olimar | Smashville | **DESYNC** gf 4404 | pass |
| Zelda vs Sheik | Battlefield | **DESYNC** gf 1091 | **DESYNC** gf 755 |
| Squirtle vs Charizard | Dream Land | pass | pass |
| Samus vs Zero Suit Samus | Frigate Husk | pass | **DESYNC** gf 6007 |
| Snake vs R.O.B. | Luigi's Mansion | pass | pass |
| Pikachu vs Jigglypuff | Green Hill Zone | pass | pass |
| Diddy Kong vs King Dedede | Temple of Time | pass | pass |
| Link vs Toon Link | Bowser's Castle | pass | pass |
| Marth vs Roy | Smashville | pass | pass |
| Lucario vs Mewtwo | Pokémon Stadium 2 | pass | **DESYNC** gf 420 |
| Sonic vs Knuckles | Battlefield | pass | **DESYNC** gf 1635 |
| Wolf vs Captain Falcon | Final Destination | pass | pass |

Rollbacks per peer: 13-95 in single core (deepest 1-7), 81-352 in dual core (deepest 3-7). Passing sessions ran to game set: 7,193 frames at the time limit, or earlier by stocks.

## Failures of the gp-v12 sweep, by family

All fixed on gp-v19; the causes and fixes are in `docs/gameplay-rollback-status.md`, Phase 8 (family letters as here).

### A. Zelda/Sheik transformation

| Run | Result | Where | Log |
|---|---|---|---|
| `v-zelda` | drift | gf 929 | (replay kept) |
| `v-sheik` | drift | gf 788 | (replay kept) |
| `m-zelda` | hang | stuck at gf 1937 | (replay kept) |
| `m-sheik` | hang | stuck at gf 2115 | `存在しないArticleへのchangeStatus命令です kind:2` (changeStatus on a non-existent article), repeated; replay kept |
| `f-zelda-sheik` | hang | stuck at gf 2394 | the same message; replay kept |
| `n-sc-zelda-sheik` | desync | gf 1091 (confirmed frame 851) | the same message, then `#fighter change begin [gameframe:1084]` |
| `n-dc-zelda-sheik` | desync | gf 755 (confirmed frame 515) | the same |

**Symptom.** At the transformation, the incoming form's per-frame fields in the rollback run are garbage. In `v-zelda` at gf 929, P1 is instance 1 (Sheik) with x = 0x80789790 and y = 0x80B85104 (pointers, read as floats) and status 0x01000000. The no-rollback replay has x 42.07 and status 286 (the transformation). After that, Sheik never takes damage again (damage frozen at 7 % for 2,000 frames while Fox loses his stocks). Without rollback the transformation works both ways: a plain Zelda vs Fox match (no rollback, random inputs, on the gp-v9 build) transformed three times with normal status values and damage, while the same setup under the sync test broke at its first transformation.

**The sync test's own check does not see it.** Both the first run and the resimulation read the same broken fighter. The ground truth and sessions do see it.

**The mirror and FS hangs:** the main thread loops in `fn_8009EFCC` ← `fn_8009EC5C` ← `fn_8009CA88` ← `gfTaskScheduler::process`, after the article message.

**Lead:** the second fighter instance (the form not active at the match start) or its articles are created and initialised outside the region set, or are torn down by a rolled-back run.

### B. Early hang: null virtual call from a layout-pane loop

**Cause found in Phase 8:** not a layout pane but nw4r::ef: `fn_801638D0` walks an emitter's particle managers, and the call through the zero vtable is a draw strategy's (`fn_8016DE08`), a function-local static that `dolw-g1` restored without its guard. Fixed in gp-v13.

| Run | Stuck at |
|---|---|
| `m-ice_climbers` | gf 266 (replay kept) |
| `m-king_dedede` | gf 286 |
| `m-yoshi` | gf 337 (replay kept) |
| `m-rob` | gf 343 |
| `m-knuckles` | gf 309 |
| `m-lucas` | gf 354 |
| `m-game_and_watch` | gf 363 |
| `i-rob-game_and_watch` | gf 280 |

**Signature.** All the same: main thread `srr0 = 0x00000020`, `lr = 0x80163918`.
- 0x80163918 is the return address of the `bctrl` in `fn_801638D0`. That function walks a list at `r4+0x90` (`fn_8015C1B0` = next) and calls each element's virtual function at vtable +0x1C, i.e. a layout (nw4r lyt) pane's child list.
- An element with a zeroed vtable sends the thread to 0x20.

**When.** Always 26-123 frames after rollback starts (start frame 240): the window in which the "GO!" sign and the HUD's start-of-match layouts change.

**Rate.** 7 of 42 mirror matches, none of the 41 vs-Fox runs, and one non-mirror item run. So likely, but not only, mirrors: same character twice, and the same costume (see the caveat under Runs).

**Rollback is required.** The Ice Climbers mirror without rollback: 0 of 2 stuck. With the sync test: 1 of 2, at gf 256.

**Lead:** a pane list (or the pane objects) of a match-start layout whose nodes live in the set, and whose head or owner does not (compare the ground-collision list heads of gp-v11).

### C. Meta Knight

| Run | Result | Main thread |
|---|---|---|
| `v-meta_knight` | hang at gf 1161 | `srr0 = 0x20`, `lr = 0x8019D990` (in the nw4r g3d object code near `G3dObj::Destroy`); replay kept |
| `m-meta_knight` | hang at gf 965 | `srr0 = 0x80025858` (`gfKeepFrameBuffer` area), `lr = 0x80712E28` (sora_menu/melee REL code); replay kept |

The only character that fails against Fox outside Zelda/Sheik. Same signatures on gp-v9 (`v-meta_knight` hung at session frame 469 with `lr = 0x8019D990`).

### D. Items and Smash Balls: drift and hangs

| Run | Result | First difference against no-rollback play |
|---|---|---|
| `i-mario-marth` | drift | gameplay RNG at gf 4097, fighters at 4112 (P2 damage 98 vs 96); replay kept |
| `i-peach-diddy_kong` | drift | RNG at gf 3866, fighters at 3867 |
| `i-pikachu-olimar` | drift | RNG at gf 3382, fighters at 3782 |
| `f-samus-zero_suit_samus` | drift | RNG at gf 1662, fighters at 2986; replay kept |
| `f-wario-bowser` | drift | RNG at gf 1706, fighters at 2142; replay kept |
| `f-charizard_solo-squirtle_solo` | drift | RNG at gf 1534, fighters at 1823 |
| `f-ivysaur_solo-pikachu` | drift | RNG at gf 1728, fighters at 2023 |
| `i-fox-falco` | hang at gf 6344 | `srr0 0x801A0B7C`, `lr 0x801A0AA0` (nw4r g3d); replay kept |
| `f-ice_climbers-peach` | hang at gf 2211 | `GXWaitDrawDone` chain (`fn_801E0A80` ← `fn_801E1790` ← `VIWaitForRetrace` ← `fn_80023AE4`); replay kept |
| `f-olimar-lucario` | hang at gf 4732 | the same chain, after `Non-recoverable Exception 7`, `SRR0 = 0x0000040c`; replay kept |

**The drift always starts in `g_mtRand`** (the gameplay RNG), with the fighters following 1-400 frames later. Every Smash-Ball-only run drifts within about 1,300-1,500 frames of GO (gf 1534-1728). That is about when the first Smash Ball drops, which suggests item spawning state outside the set. Candidates:
- `itManager`'s Smash Ball drop timer and frames (`m_smashBallDropTimer`, `m_smashBallDropFrame1/2`);
- the item lot rates;
- the next-assist and next-Pokémon info.

Whether `itManager` itself is in the set was not checked.

The one passing item run is Snake vs Ice Climbers on Pokémon Stadium 2. The two `GXWaitDrawDone` hangs are the only ones left of that kind on gp-v12, both in Smash Ball runs.

### E. Late drift in mirror matches

| Run | Result | First difference |
|---|---|---|
| `m-donkey_kong` | desync (the sync test's own check) and drift | gf 5644: P1 x −16.24 against −16.28; replay kept |
| `m-mewtwo` | drift | gf 5529: P1 damage 97.67 against 96.67 (the RNG did not differ); replay kept |
| `m-wolf` | drift | gf 4088: P2 damage 1.0 against 0.0. The log has `存在しないArticleへのchangeStatus命令です kind:4` and `kind:1` (Wolf's articles); replay kept |

Small, late, fighter-only differences without an RNG difference first. Each appears once in 7,000 frames. All three have exact replays for a `--trace-frame` investigation.

### F. Sessions

| Run | First trace difference | Confirmed checksums that differ |
|---|---|---|
| `n-dc-fox-falco` | gf 4438 | 994 / 5192 |
| `n-dc-lucario-mewtwo` | gf 420 | 6998 / 7178 |
| `n-dc-samus-zero_suit_samus` | gf 6007 | 1411 / 7178 |
| `n-dc-sonic-knuckles` | gf 1635 | 5741 / 7136 |
| `n-dc-zelda-sheik` | gf 755 (Family A) | 3312 / 3827 |
| `n-sc-ice_climbers-olimar` | gf 4404 | 3014 / 7178 |
| `n-sc-zelda-sheik` | gf 1091 (Family A) | 518 / 1621 |

**Dual core fails 5 of 15**, single core 2 of 15. Dual-core sessions also roll back 2-4 times as often (81-352 rollbacks per peer). Every character in these pairs passes its single-instance sync test against Fox with ground truth (Fox, Falco, Lucario, Mewtwo, Samus, ZSS, Sonic, Knuckles), and all 10 Fox/Falco stage runs pass.

So these are rollbacks with corrected input, which the sync test (same input on resimulation) cannot produce. The misprediction test (`gprb_mispredict.py mp`) is the single-instance tool for them. Fox/Falco on Battlefield is the simplest case: it passed every session in Phases 4-6.

**To reproduce:** rerun with `gprb_session.py --cpu dc --preset typical --pass-log … --save-countdown …` and replay the peers' logs.

## All failures of the gp-v12 sweep

"Replay kept": `run/qa/sweep2/work/<id>/` holds the savestate and the pass log.

| Run | Result | First divergent frame / hang | Main thread (srr0 / lr) or trace fields | Log | Replay kept |
|---|---|---|---|---|---|
| `f-charizard_solo-squirtle_solo` | drift | gf 1534 | | | (as `f-samus-…`) |
| `f-ice_climbers-peach` | hang | stuck at gf 2211 | GX draw-done wait | | yes |
| `f-ivysaur_solo-pikachu` | drift | gf 1728 | | | (as `f-wario-bowser`) |
| `f-olimar-lucario` | hang | stuck at gf 4732 | GX draw-done wait | `Non-recoverable Exception 7`, `SRR0 = 0x0000040c` | yes |
| `f-samus-zero_suit_samus` | drift | gf 1662 | | | yes |
| `f-wario-bowser` | drift | gf 1706 | | | yes |
| `f-zelda-sheik` | hang | stuck at gf 2394 | 8009f0d8 / 8009eca4 | `存在しないArticleへのchangeStatus命令です kind:2` | yes |
| `i-fox-falco` | hang | stuck at gf 6344 | 801a0b7c / 801a0aa0 | | yes |
| `i-mario-marth` | drift | gf 4097 | | | yes |
| `i-peach-diddy_kong` | drift | gf 3866 | | | (as `i-mario-marth`) |
| `i-pikachu-olimar` | drift | gf 3382 | | | (as `i-mario-marth`) |
| `i-rob-game_and_watch` | hang | stuck at gf 280 | 00000020 / 80163918 | | (as `m-yoshi`) |
| `m-donkey_kong` | desync + drift | gf 5644 | | | yes |
| `m-game_and_watch` | hang | stuck at gf 363 | 00000020 / 80163918 | | (as `m-ice_climbers`) |
| `m-ice_climbers` | hang | stuck at gf 266 | 00000020 / 80163918 | | yes |
| `m-king_dedede` | hang | stuck at gf 286 | 00000020 / 80163918 | | (as `m-ice_climbers`) |
| `m-knuckles` | hang | stuck at gf 309 | 00000020 / 80163918 | | (as `m-ice_climbers`) |
| `m-lucas` | hang | stuck at gf 354 | 00000020 / 80163918 | | (as `m-ice_climbers`) |
| `m-meta_knight` | hang | stuck at gf 965 | 80025858 / 80712e28 | | yes |
| `m-mewtwo` | drift | gf 5529 | | | yes |
| `m-rob` | hang | stuck at gf 343 | 00000020 / 80163918 | | (as `m-yoshi`) |
| `m-sheik` | hang | stuck at gf 2115 | 8009f0d8 / 8009eca4 | `存在しないArticleへのchangeStatus命令です kind:2` | yes |
| `m-wolf` | drift | gf 4088 | | `…changeStatus命令です kind:4`, `kind:1` | yes |
| `m-yoshi` | hang | stuck at gf 337 | 00000020 / 80163918 | | yes |
| `m-zelda` | hang | stuck at gf 1937 | 8009f0d8 / 8009eca4 | | yes |
| `n-dc-fox-falco` | desync | gf 4438 | fighters, rng, game set | | |
| `n-dc-lucario-mewtwo` | desync | gf 420 | fighters, rng | | |
| `n-dc-samus-zero_suit_samus` | desync | gf 6007 | fighters, rng | | |
| `n-dc-sonic-knuckles` | desync | gf 1635 | fighters, rng, game set | | |
| `n-dc-zelda-sheik` | desync | gf 755 | fighters, rng, game set | `…changeStatus命令です kind:2`, `#fighter change begin` | |
| `n-sc-ice_climbers-olimar` | desync | gf 4404 | fighters, rng | | |
| `n-sc-zelda-sheik` | desync | gf 1091 | fighters, rng | `…changeStatus命令です kind:2`, `#fighter change begin` | |
| `v-meta_knight` | hang | stuck at gf 1161 | 00000020 / 8019d990 | | yes |
| `v-sheik` | drift | gf 788 | | | yes |
| `v-zelda` | drift | gf 929 | | | yes |

Screenshots: `run/qa/sweep2/shots/`. For the early hangs it is the last frame before the hang. For each failed session, both peers at the end.

## gp-v9 against gp-v12

The same driver, inputs and run IDs ran first on `gprb-fe35002ead` (gp-v9) in `run/qa/sweep` (raw JSON and logs kept; sessions only partly, the extended stages not at all). gp-v9 had no ground-truth check, so "pass" there means only check 1 and no crash or hang.

| | gp-v9 | gp-v12 |
|---|---|---|
| Runs with a verdict (the same IDs) | 110 | 110 |
| Pass | 69 | 81 |
| Dolphin crashed (host exception) | **11** | **0** |
| Hang | 27 | 16 |
| Desync (sync-test check, or between session peers) | 3 | 3 |
| Drift from no-rollback play (new criterion) | not measured | 10 |

**Gone on gp-v12** (23 runs that failed on gp-v9 and pass now, ground truth included):
- **All 11 Dolphin crashes.** Three kinds:
  - `read from address 0x0` in JIT code: Bowser, DK, Ice Climbers, Mewtwo and Pit vs Fox, Bowser's Castle;
  - `memmove` writing past a buffer: Falco vs Fox, Link mirror;
  - `read from address 0x4c20`: Lucario vs Fox, Pit mirror;
  - and the Samus/ZSS Smash Ball run, which crashed on gp-v9 and drifts on gp-v12.
  
  This fits the GX FIFO tail fix (gp-v10): stale command blocks fed garbage to the GPU and its FIFO copy.
- **The `GXWaitDrawDone` hangs** of Mario, Marth and ZSS vs Fox and the Jigglypuff and Lucario mirrors (`fn_801E0A80` ← `fn_801E1790` ← `fn_801F0A30` ← `fn_80023AE4`, the draw-done wait of Phase 6). Only the two Smash Ball hangs above remain of that kind.
- **Other hangs:** Wolf vs Fox (main loop in `fn_8005C2B0`), King Dedede vs Fox, the Samus and Ganondorf mirrors, the FD and Frigate Husk stage runs.
- **The Link desync** at gf 247 (RNG and fighters, the first frames after rollback starts), and the Link/Toon Link single-core session desync.

**Still failing:** Family B's early hang (same signature, the same 7 mirror characters on both builds); Zelda/Sheik (mirror and FS hangs, session desync); Meta Knight; the item and Smash Ball runs (hangs on gp-v9, drifts or hangs now).

**C-stick smashes on gp-v9.** A bisect of the input categories on gp-v9 (Null video, throttled, `distance` 2, `run/qa/bisect`, `run/qa/sweep-ctl-null-thr-d2`):
- With C-stick smashes, Falco, DK, Bowser, Lucario and Mewtwo vs Fox desynced or crashed in 6 of 6 runs.
- Without them they all passed (6 of 6), and smashes plus movement alone were enough to desync Falco.

Smash attacks are what makes a camera quake, so this is most likely the quake controller that gp-v12 restores. On gp-v12 every vs-Fox run with C-stick smashes passes, except Zelda, Sheik and Meta Knight. On gp-v9, a no-C-stick pass at `distance` 2 (`run/qa/sweep-nocstick-d2`, 44 runs) still hung or crashed in 9: Peach, Captain Falcon, Ice Climbers, Pit and Zelda among them. So the C-stick was not the only trigger; their signatures (draw-done waits, null jumps, game exceptions) match the faults gp-v10 fixed.

## Caveats

- **Closed-loop input** is not repeatable run to run: the driver reacts to the state it reads, at wall-clock times. A rerun of a failing ID may pass, or fail at another frame. The replay files reproduce a run exactly.
- **Load.** Up to 10 other Dolphin instances ran on the machine at times. Four runs failed to launch or lost their harness connection and passed on the retry. No result above is a harness error.
- **`distance` 7** resimulates 6 frames every frame. Phase 6's sync tests mostly used `distance` 2. A failure here that does not reproduce at `distance` 2 still happens in sessions only when a rollback is that deep.
- **Mirrors** use the same costume twice (see Runs).
- **Final Smashes** were not verified in the gp-v12 sweep (see Items and Final Smashes); the gp-v19 sweep forces them (`ffs`).
- **No session** was compared with a no-rollback replay; their criterion is agreement between the peers.

## Files

- `harness/tools/gprb_sweep.py`: the driver.
  - `run` (resumable, `--jobs`, `--retry-errors`, `--only`, `--groups` (now also `ffs`), `--macro-exclude`, `--no-ground-truth`, `--video`, `--throttled`, `--distance`, `--minutes`);
  - `list`;
  - `report` (these matrices from a result directory; `--compare DIR` adds a column from a second pass).
  
  Touch `<out>/dump-stacks` to get the driver's thread stacks in `<out>/stacks.txt`.
- `run/qa/sweep2/`: gp-v12 results (`runs/`, `logs/`, `shots/`, `work/`).
- `run/qa/sweep3/`: gp-v19 results; `run/qa/sweep3-hangs/`: the two hung retry runs; `run/qa/gfx-sess1/`: the failing session pairs rerun on the fixed build first.
- `run/qa/sweep/`, `run/qa/sweep-nocstick-d2/`, `run/qa/bisect/`, `run/qa/factorial/`, `run/qa/sweep-ctl-*`: gp-v9 results and controls.
- `run/qa/sweep2-control-gpv10/`: the gp-v10 drift control.
