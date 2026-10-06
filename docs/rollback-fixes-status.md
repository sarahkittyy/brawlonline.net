# Rollback fixes: status

Branch `rollback-fixes` in `dolphin/` (off `harness`). Not pushed. All commits build. The regression tests are in `harness/tests/test_rollback.py` (`pytest -m dolphin tests/test_rollback.py`: 5 tests, about 4 minutes, all passing on HEAD).

Measurement caveat: all numbers come from two instances on one 8-core PC, with another agent's Dolphins running, single core, Null video, muted. FPS is noisy.

## Result

### Scorecard

`python -m ppharness scorecard --presets lan,typical,bad_wifi --sessions 1 --duration 60 --phase auto`. HEAD is compared with `docs/scorecard-baseline.md`.

| Preset | Baseline (`harness`) | HEAD (`4d972aaa29`) |
|---|---|---|
| lan | One side froze at 8.3 s (GekkoNet frame 146, CSS). The live side ran alone at 42 FPS. Setup never got past the character pick. | Reached the match. No freeze, no stall over 2 s. 9 rollbacks, desyncs 0. Frames ahead 0.14 / -0.03 (mean). Game FPS 59.4 / 59.4. State compare: match. |
| typical | One side froze at 7.2 s (frame 161). | Reached the match. No freeze. About 200 rollbacks per side, desyncs 0. Frames ahead 0.15 / 0.00. Game FPS 56.9 / 56.9. State compare: match. |
| bad_wifi | One side froze at 6.8 s (frame 122). | Reached the match. No freeze. About 240 rollbacks per side (591 / 634 frames resimulated), desyncs 0. Frames ahead 0.29 / 0.05. Game FPS 53.3 / 53.4. State compare: match. |

The full JSON is in `run/qa/scorecard-rollback-fixes.json` (local).

### Other checks (scratch driver, two instances, CSS → SSS → match with random input)

- **Desync.** Every per-frame checksum (frame counter, and per port damage, stocks, active-fighter X/Y and status) is identical on both peers on every confirmed frame. Pads are identical too.
  - typical: 3 of 3 runs, about 1,690 frames each, ~100 rollbacks each.
  - bad_wifi: 2 of 2 runs, 117-181 rollbacks each.
  - lan: 2 of 2 runs.
  - Before: fighters diverged about 100 frames into every match that had rollbacks, and the frame counter drifted 1-8 frames.
- **Sync test.** `PPR_SYNCTEST=3` in a Training match with random input, 30 s: 0 GekkoNet checksum mismatches.
- **Memory.** About 1.1 GB peak private bytes per instance. Before: 11-16 GB after 90 s.
- **CPU.** 0.1-0.2 cores per instance on the CSS. Before: 3.5-3.7 cores, from busy-spinning job threads.

## The nine items

1. **Host freeze at scene transitions — fixed.**
   - Root cause: JIT stores to constant addresses (globals) never set the dirty bitmap (`7afaab433a`). Loads left AX voice-list globals at their newer values, the lists formed a cycle, and AX's interrupt-time list walk (0x80201E40) spun forever with MSR[EE]=0.
   - Contributing fixes:
     - host writes are marked dirty (`581e716610`);
     - the AX exclusions are removed (`f7bcf4a591`);
     - the IOS IPC queues roll back (`b74ba2a091`);
     - packets go out on the netplay thread (`5a1cba82b7`).
   - Evidence:
     - ROLLBACK_VALIDATE: 80 of 97 loads restored wrong RAM before, 0 of 201 after.
     - The new `cpu_state` command (`72453dbfe4`) showed the spin.
2. **Asymmetric pacing — fixed (`ef53aaeee7`).** The sleeps were repaid by the absolute-timeline throttle. They are replaced by a speed nudge (-2% to +1%, Slippi-style). Frames ahead was about 1.9 / 1.4 on the original build; it is now about 0.0-0.3 (scorecard above).
3. **Throttled resimulation — fixed (`ef53aaeee7`).** Resimulated iterations run unthrottled, and the throttle is re-anchored when the presented frame starts.
4. **Synchronized start — fixed (`6992f39aa2`).** The joiner sends Ready, and the host sends Go and starts half a ping later.
   - Time at which each side simulated GekkoNet frame 1, joiner minus host, typical preset: -19 / -180 / -171 ms before; +6 / +12 / -1 ms after.
5. **Input timing — confirmed and fixed (`744faec247`).**
   - Before, a single-frame press landed 3-4 frames later on the peer than locally: 10 button and 424 CRC mismatches over 1,042 frames.
   - After, it lands on the same frame on both peers with 0 mismatches. Delay is 1, which gives 2 frames from pad read to use.
   - This uses the harness command `rollback_pad_history` (`7ee1f9ab99`).
6. **Running at 36 FPS — fixed.** There were three causes:
   - busy-spinning job workers, about 3.5 cores per instance (`204a906682`);
   - a ~300 MB/s snapshot leak (`eff14d6c32`);
   - the sleeps and throttled resimulation of items 2 and 3.
   - Cost now: a save is about 1.2 ms per frame, a load about 2 ms.
7. **Save and load at different points — real, fixed.**
   - The save happened before the frame-counter store (`6ac8dd0e15`).
   - The save now happens at the loop top, the same instruction boundary as loads, so the CoreTiming position matches (`1a46ce8b61`).
8. **Dual core.** The default is restored to true (`041f4cf2a5`). Dual core is not yet tested under rollback (next).
9. **Checksum — fixed (`242af80c0e`).** GekkoNet now checksums verified fields: the frame counter, plus per port the active instance, damage, stocks, X/Y and status kind. The parts are recorded per frame for the harness.

## How the remaining desync was found and fixed

| SHA | Fix |
|---|---|
| `92a44d4f68` | Sync test (`PPR_SYNCTEST=N`, a GekkoNet stress session) plus per-frame MEM1/MEM2 chunk hashes with word-level diffs. |
| `c0a734f8db` | **The main bug: the wrong snapshot slot was restored on every rollback.** The code loaded one frame too old, so each rollback dropped a frame of game progress. A distance-1 sync test replayed the same frame forever. |
| `dfe25b4651` | The CPU's slice position (downcount) and pending exceptions are restored. Zeroing them shifted resimulated time. |
| `1a46ce8b61` | Saves happen at the loop top. |
| `17157c72ad` | The main thread's stack is restored. The live-stack exclusion kept post-snapshot locals. |
| `de21413848` | GPRs, FPRs, CR, XER, FPSCR and run-time SPRs are saved and restored. |
| `4d972aaa29` | Pads are read on resimulated frames too. Skipping the read left PAD/SI library state and CPU time different from the first run. |
| `be0ddb58d7` | Stalls wait on the host rather than in a guest spin, so emulated time doesn't advance between frames. |
| `9026c5d1a3` | Resimulation shortcuts (render skip, VI-wait bypass, forced completions) are off. |

Sync-test residue (non-gameplay, whole-machine only):
- PAD/SI buffers, because the local port's SI data is the live controller in resimulated frames too;
- a few timestamps one tick apart.

None of them reach the gameplay checksum.

## Next steps

1. Dual core under rollback: scorecard with `--cpu-thread on`. If it diverges, sync the GPU thread at the frame hook.
2. Audio during resimulation. AI/DSP output plays the resimulated frames at unthrottled speed. Mute it while resimulating. XFB presentation of resimulated fields can flash too (Null video hides this).
3. Gameplay-only rollback (user decision): restrict save/restore to a region set. Inputs to that work:
   - memory that legitimately differs between peers: PAD/SI library buffers 0x804DE3B0-0x804DE4B0, 0x804F67B0, 0x80584000;
   - OS thread contexts and timestamps;
   - the CPU and timing state that must still be restored: registers, downcount, exceptions, CoreTiming.
