# Rollback fixes: status (2026-10-06)

Branch `rollback-fixes` in `dolphin/` (off `harness`). Not pushed. HEAD `8183cc10c5` builds. The harness regression tests are **not written yet**.

All numbers come from two instances on this PC, with another agent's Dolphins running at the same time, so FPS is noisy. Driver and results are in the session scratchpad (`rbtest.py`, `res/*.json`). They are not in the repo.

## Done (commits, oldest first)

| SHA | Fix | Evidence |
|---|---|---|
| eff14d6c32 | Memory leak: job closures were never destroyed. Each evicted snapshot (~5.5 MB/frame) leaked. Also fixes a stale `page_count`. | Peak private memory was 11-16 GB per instance after ~90 s (one orphan reached 15 GB). Now 1.1 GB. |
| 5a1cba82b7 | GekkoNet packets were sent with `enet_peer_send` from the CPU thread, racing the netplay thread's `enet_host_service`. They waited for the next received packet before going out. They are now queued and sent and flushed by the netplay thread. | |
| 581e716610 | `MMU::HostWrite`/`HostTryWrite` (all HLE hook writes) did not mark the dirty bitmap. Now they do. | |
| 6ac8dd0e15 | (#7) The save was taken before the `stw` of the frame counter at 0x80017504, so each rollback lost one count. Now the store happens first, so save and load share the loop-top boundary. The loop keeps no register state across iterations (checked in the disassembly). | |
| b74ba2a091 | IOS IPC request/reply queues and `m_last_reply_time` now roll back with the WiiIPC registers and CoreTiming events. | |
| 7ee1f9ab99 | Harness: `rollback_pad_history`, the pads the game used per GekkoNet frame. | |
| 744faec247 | (#5) Presented frames used the live local pad, while the peer used it 3-4 frames later. Now every pass injects GekkoNet's gfPadStatus for the player ports only. GekkoNet delay is 1, which gives 2 frames from pad read to use. | Single-poll presses: before, the joiner's X landed at 211/317/489 locally and 214/321/493 on the host (10 button and 424 CRC mismatches out of 1042 frames). After, the frames are identical on both sides and there are 0 mismatches. |
| f7bcf4a591 | (#1) The AX "audio buffer/voice block" exclusions were removed. They covered AX voice-list .bss. | |
| 72453dbfe4 | Harness: `cpu_state` (pc, msr, pending exceptions, PI/VI interrupt state). | Frozen side: pc 0x80201e40/74 (AX voice-list walk), msr 0x1032 (EE=0), exceptions 0x5. |
| 041f4cf2a5 | (#8) `DEFAULT_CPU_THREAD = true` restored, with P+'s description text. Note: the clone at `refs/pplus-Project-Plus-Dolphin` @93eae93 also has `false` ("input lag"), so upstream P+ may have changed its mind. | |
| 7afaab433a | **(#1) Root cause of the freezes:** JIT stores to compile-time-constant addresses (`WriteToConstRamAddress`, i.e. .bss/.sbss globals) were never marked dirty. Globals were not restored by loads, which corrupted AX voice lists into a cycle. | ROLLBACK_VALIDATE: 80 of 97 loads restored wrong RAM before, 0 of 201 after. to_match runs froze in 2/3 and 3/4 of runs before, 0/12 after (lan and typical). |
| 204a906682 | (#6) The 6 job helper threads per instance busy-spun forever. Now they block on an atomic epoch. | 3.5-3.7 cores per instance before, 0.1-0.2 after. This was the main cause of 36 FPS with two instances. |
| ef53aaeee7 | (#3, #2) Resimulation is unthrottled, and the throttle is re-anchored when the presented frame starts. `PauseForLocalAdvantage` sleeps were replaced by a Slippi-style speed nudge (-2%/+1%, every 15 frames). netplay_status has new counters: time_sync_speed, stall_polls, save/load µs. | CSS movement, lan: 51.5 FPS, 58-64 rollbacks, frames ahead +0.56/-0.20 → 59.9 FPS, 0-1 rollbacks, +0.04/+0.03. Typical: 47 → 58.5 FPS. Save is about 1.2 ms, load about 2 ms. |
| 6992f39aa2 | (#4) Synchronized start: the joiner sends Ready and the host sends Go, then starts after ping/2. This uses 0xB7 control packets on the GekkoNet channel. | Start offset not yet measured. The measurement run was interrupted (`bin/pacing` vs `bin/startsync`, logs in scratchpad `logs/`). |
| be0ddb58d7 | A stall waits on the host, not with a guest spin, so no emulated time passes between frames. | |
| 9026c5d1a3 | Resimulation no longer takes shortcuts: render, VI wait, forced completions and the sound-alloc skip are all run as normal (`kResimulationShortcuts=false`). | |
| 242af80c0e | (#9) The checksum now uses verified fields: frame counter, plus per-port active instance, damage, stocks, X/Y and status. It also records per-frame checksum parts and the gameProc step count in rollback_pad_history. | |
| 8183cc10c5 | ROLLBACK_VALIDATE reports the 64-byte granules that differ. | |

## Open problems / next steps

1. **Real desyncs remain in matches with rollbacks.** Inputs are identical (0 pad mismatches), but the per-frame fighters checksum diverges about 100 frames after match start, and the g_GameFrame persistent counter drifts by 1-8 frames over 25 s (typical preset, about 4 rollbacks/s). With 0 rollbacks, everything matches.
   - Not the cause: the gameProc step count (always 1 on both sides).
   - Next: per-frame 64 KiB chunk hashes, so the first diverging frame and region can be found. Code was drafted but not committed (env `PPR_ROLLBACK_CHUNK_HASHES`, harness `rollback_chunk_hashes`). Suspects: CPU registers/FPSCR/GQR/DEC not restored, IOS device state, dual-core/GPU timing.
   - A CSS→main-menu divergence (one side backs out) was also seen with identical inputs.
2. GekkoNet `desyncs_detected` stays high in matches. This is consistent with item 1, not with a checksum bug.
3. #8 dual core: not yet tested under rollback. The determinism agent found dual core diverges offline. Next: sync the GPU thread at the frame hook, then measure.
4. Not done: harness regression tests (`harness/tests/`, marked `dolphin`) for freeze, pad history, memory and FPS, plus doc updates. The doc updates needed are: `harness-protocol.md` for `cpu_state`, the new rollback fields, the extra pad-history columns and delay=1; and `harness-implementation.md`.
5. Not done: before/after with `python -m ppharness scorecard` (baseline in `docs/scorecard-baseline.md`). The binaries to compare are `bin/base` (original) and HEAD.
6. Audio is not muted during unthrottled resimulation, which matters for real (non-muted) play. Host-side render skip during resimulation (as Orca does) is a possible optimisation.
