# Rollback scorecard: baseline

This is the starting point the C++ fixes should be measured against.

Measured 2026-10-06 18:11 on the frozen snapshot `run/bin/harness-100b8fd189` (Brawlback `rollback` plus our `harness` branch):

- Windows 11, 8 cores shared with another agent's Dolphin instances, so treat the FPS figures as noisy.
- Both instances single core (`CPUThread = False`), Null video, muted, P+ Netplay Launcher.
- GekkoNet input delay 2 (hard-coded by the fork).

```
cd harness
python -m ppharness scorecard --presets lan,typical,bad_wifi --sessions 1 --duration 60 \
    --phase auto --json ../run/qa/scorecard-baseline.json
```

The `auto` phase tries to get into a match as in the end-to-end test: each side picks its character (Fox / Falco), then the host picks the stage. If the setup fails it keeps driving the menus with seeded random input until 60 s have passed. Sampling runs four times a second throughout.

## Result

| Preset (RTT, loss) | Side | Rollbacks | Max depth | Frames resim. | Desyncs | Frames ahead mean / min / max | FPS VI / game / GekkoNet | Froze |
|---|---|---|---|---|---|---|---|---|
| lan (1 ms, 0 %) | host | 5 | 5 | 23 | 0 | 1.79 / -0.56 / 3.10 | 58.2 / 6.2 / 2.4 | **yes**, after 8.3 s, GekkoNet frame 146 (poll 384), CSS |
| | joiner | 7 | 5 | 39 | 0 | 0.09 / -0.17 / 2.69 | 42.2 / 42.3 / 38.8 | no (1 stall of 7.8 s at 47 s, recovered); ran alone to frame 2400 |
| typical (40 ms, 0.5 %) | host | 13 | 4 | 38 | 0 | -0.00 / -1.67 / 1.37 | 45.5 / 45.5 / 42.0 | no (1 stall of 9.5 s at 53 s, recovered); ran alone to frame 2682 |
| | joiner | 8 | 5 | 25 | 0 | 1.09 / -1.69 / 2.12 | 59.1 / 6.0 / 2.5 | **yes**, after 7.2 s, GekkoNet frame 161 (poll 400), CSS |
| bad_wifi (60 ms, 2 % bursty) | host | 5 | 5 | 17 | 0 | 0.04 / -0.33 / 3.38 | 50.7 / 50.8 / 47.3 | no (1 stall of 3.7 s at 44 s, recovered); ran alone to frame 2926 |
| | joiner | 4 | 5 | 13 | 0 | 0.88 / -1.60 / 2.17 | 59.2 / 5.8 / 2.0 | **yes**, after 6.8 s, GekkoNet frame 122 (poll 361), CSS |

Measured netsim conditions:

| Preset | One-way delay p50 / p99 (ms) | Loss up / down |
|---|---|---|
| lan | 0.6 / 1.4 | 0 / 0 |
| typical | 21 / 34 | 0.7 % / 1.1 % |
| bad_wifi | 34 / 72 | 1.3 % / 3.1 % |

**Peer state comparison:** skipped in all three sessions because a side froze. **Setup:** never got past the character pick (`css pick`: `wait_frame` timeout on the frozen side).

## What it shows

1. **One side always freezes on the character select within 7-8 s**, at GekkoNet frame 122-161, whatever the network.
   - It was the host on `lan` and the joiner on `typical` and `bad_wifi`, so it is not tied to the host role.
   - In the frozen process the VI keeps counting at about 60/s, but the game's per-frame pad reads stop (poll 361-400) and so does the GekkoNet frame. The game loop is blocked, not the emulator.
   - Its last log lines are a GekkoNet ADVANCE/SAVE pair. On `lan` they directly follow a rollback (`InjectPadsForIteration` 1 and 2 of a 4-frame resimulation, then `ADVANCE event at frame 146`).
   - The end-to-end test (`test_e2e_match.py::test_netplay_rollback_match`) records the same pattern: freeze right after a rollback during CSS cursor movement.
2. **The peer never waits.** After its partner froze, the live side kept simulating alone for about 50 s, up to 2400-2900 GekkoNet frames, with `frames_ahead` near 0. Rollback normally stalls a peer that gets too far ahead. Here it keeps predicting the frozen peer's input indefinitely, and nothing reports the disconnect.
3. **Speed.** With one side frozen, the live side ran at 42-51 game frames/s, not 60, in a two-instance session on this shared machine. Fixed-delay netplay on the same machine runs at 60.
4. **Rollbacks before the freeze were shallow.** 4-13 rollbacks, max depth 4-5 frames, no GekkoNet desyncs, no Dolphin desync reports.
5. **The live side also stalled** once per session for 3.7-9.5 s, alone on the CSS, and then recovered. The cause is unknown. It may be CSS file loading (portraits from SD), or GekkoNet throttling without a peer.

## What "fixed" should look like

- `froze=no` on both sides in every preset, and setup `reached=match`.
- The state comparison then runs. It compares at the same GekkoNet frame after 2 s of neutral input on both sides. The speculative-state caveat applies until the server offers confirmed-frame hashes (protocol doc, proposed change 4).
- `frames_ahead` stays within the rollback window, and FPS stays near 60 on both sides.
- Re-run with `--sessions 3` (and `--phase match`) to get spread. Also run `--cpu-thread on`: dual core is a requirement, and `docs/determinism-findings.md` shows it is less deterministic.

Full JSON (per-second `frames_ahead` series, stall list, key log lines of the frozen side): `run/qa/scorecard-baseline.json` (local, not in git).
