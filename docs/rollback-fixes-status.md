# Rollback fixes: status

Branch `rollback-fixes` in `dolphin/` (off `harness`). Not pushed. All commits build. The regression tests are in `harness/tests/test_rollback.py` (`pytest -m dolphin tests/test_rollback.py`: 14 tests, single and dual core, a real-backend presentation test and a long dual-core real-backend stress test; all 14 passed on `7b2227fd5e` in 16 minutes).

Measurement caveat: all numbers come from two instances on one 8-core PC, with another agent's Dolphins running most of the time, muted. Unless a section says otherwise: single core, Null video. FPS is noisy; before/after pairs were run back to back under the same conditions.

## Round 3: Linux merge, dual-core snapshots without a GPU sync, the stall desync

| SHA | Change |
|---|---|
| `2d6fd89d29` | Merge of `linux-build` (`f3e902488b`, `107af7a6f3`, `f166669140`, `4cecd8e97f`): GCC build fixes, the same snapshot code on every platform, the cached interpreter for rollback off x86-64, Docker tooling and CI. |
| `7ad875cd2c` | Instrumentation: per-snapshot timing samples (`rollback_timings`), GPU-sync and video-thread compile counters, GPU-thread writes to guest RAM. |
| `5defc02845` | **Dual-core snapshots no longer sync the GPU thread** (task 1). |
| `7b2227fd5e` | Fix for `5defc02845`: the restored FIFO is put in place on the video thread. A CPU-side pointer reset raced with the idle video thread; the dual-core sync test crashed 3 of 3, and passes 5 of 5 after the fix. |
| `05616daf01` | Rollback sessions always run the deterministic GPU thread (task 3). |
| `463c711a12` | GekkoNet: a local stall no longer drops a healthy peer; disconnect events raised by a network poll are no longer lost (task 2). |
| `1e04ab552a` | NetPlay: a two-player rollback game ends cleanly when GekkoNet drops the peer instead of desyncing silently; counters for the silent-desync paths; snapshot phase timings; raw pads in `rollback_pad_history` (task 2). |

New tool: `harness/tools/dc_rollback.py`. It runs two-instance sessions over netsim with any backend and reports:
- per-snapshot percentiles and game FPS;
- a comparison of every confirmed frame, with chunk hashes and raw pad bytes at the first mismatch.

`--freeze` suspends one or both Dolphins. Raw data: `run/qa/dc/` (local).

### 0. Linux merge (`2d6fd89d29`)

The four commits merged cleanly into `rollback-fixes` (merge commit, so their SHAs are kept). Windows: `test_rollback.py` 12/12 passed on the merge build (`run/bin/rf-2d6fd89d29`). On the first run two tests failed for harness reasons and passed on rerun:
- `synctest[4-dc]`: CSS navigation timed out under load;
- `presents_one_frame[sc]`: once too few rollbacks on the CSS, once the joiner never reported the game ready at session start.

The Linux rebuild was skipped. Docker Desktop's engine answers every request with "Internal Server Error" again (the hang described in `docs/linux-build.md`), and restarting Docker Desktop on the shared machine was not done. The new C++ in this round has not been compiled with GCC yet.

### 1. Dual-core snapshot stalls (`5defc02845`)

**Why the sync was there.** A rollback snapshot serializes the video state (`VideoCommon_DoState`). In dual core that ran on the GPU thread as a blocking request after `SyncGPUForDoState` drained it, because the state has to match the CPU thread's position in the command stream. In deterministic dual core (always the case in rollback netplay, see 3) the state is owned by two threads:

- CPU thread: the FIFO preprocessing position (`pp_read_ptr`, `write_ptr`), the CP registers and the PE registers. PE tokens and finish interrupts are raised by the CPU-side preprocessor, in emulated time. Bounding box and EFB access are disabled in this fork.
- GPU thread: BP/CP/XF memory, texture memory and TMEM, the pixel/vertex/geometry shader managers, the vertex manager, the presenter.
- Guest RAM written by the GPU thread: EFB copies to RAM. This fork copies EFB and XFB to RAM (`EFBToTextureEnable` and `XFBToTextureEnable` are off), deferred until the next draw-done/token. Measured in a match, about 5 per frame:
  - two 614 KB keep-frame-buffer copies, at 0x91329EE0 and 0x913BFF00;
  - a 256 KB copy at 0x933A99C0;
  - two 64-byte copies into .bss, at 0x804951C0 and 0x80495200;
  - the XFB, in the excluded framebuffer region.

  Each readback waits for the host GPU: 80-170 us on average, up to 50-210 ms when the GPU is contended.

**Fix.** Snapshots are split along that line:
- The CPU part is serialized on the CPU thread in place of the video section. Of the FIFO only the bytes after `pp_read_ptr` are kept (a command not yet seen complete); before, the whole 2 MB buffer was copied.
- The GPU part is captured by the video thread itself (`Fifo::QueueVideoThreadCapture`) into the slot, once it has executed every command preprocessed before the snapshot.
- The CPU never waits, except when the video thread is a whole ring of snapshots behind. That never happened in the runs below.
- Loads still drain the GPU thread, then restore the CPU part and, in one blocking request on the video thread, the FIFO bytes and the slot's capture. The FIFO pointers must not be reset on the CPU thread: the idle video thread wakes up every 100 ms and runs the FIFO whenever `write_ptr > seen_ptr`. That race crashed the dual-core sync test (`7b2227fd5e`).

Guest-RAM writes by the GPU thread were already timing-dependent in dual core. With the async capture a copy can land just after the snapshot instead of before it. That changes which slot holds the bytes of the EFB copy buffers, never gameplay memory (see 3). Single core and `PPR_ROLLBACK_SYNC_VIDEO=1` keep the old path (used for the A/B below).

**Results.** Same binary, old path via `PPR_ROLLBACK_SYNC_VIDEO=1`, runs interleaved, dual core, 3x IR, 60 s of match per run. p50/p99 are the medians over runs and sides; max is over all runs:

| preset, backend | snapshot p50 / p99 / max, old | snapshot p50 / p99 / max, new | GPU-sync part of old (new: 0) | game FPS old / new |
|---|---|---|---|---|
| typical, D3D11 (final build `7b2227fd5e`, 2 runs each) | 1643 / 2699 / 4581 us | 791 / 1218 / 1890 us | 786 / 1700 / 3537 us | 59.85 / 60.04 |
| typical, D3D11 (earlier old-path runs) | 1523 / 3020 / 41641 us | (new-path runs failed in setup) | 813 / 2242 / 40970 us | 60.00 |
| typical, Vulkan | 1288 / 3059 / 130810 us | 649 / 1067 / 16992 us | 590 / 2248 / 129774 us | 59.83 / 60.32 |
| bad_wifi, D3D11 | 1460 / 2912 / 40062 us | 633 / 1020 / 9372 us | 787 / 2162 / 39418 us | 58.78 / 59.47 |
| bad_wifi, Vulkan | 1280 / 2921 / 15782 us | 623 / 987 / 11887 us | 592 / 2200 / 7783 us | 58.98 / 59.55 |
| awful, D3D11 (stress runs, 1 old / 3 new, heavier load) | 1578 / 4514 us | 691-802 / 1115-1438 us | 893 / 3460 us | 48.7 / 47.6-50.8 |

Snapshots now cost about 0.65-0.8 ms (p50) and 1.0-1.2 ms (p99) in dual core, about what they cost in single core, and the 40-130 ms spikes from the GPU sync are gone. On this PC the game runs at its 60 FPS cap on typical and bad_wifi either way. On awful (CPU-bound by 7-frame resimulations) FPS depends on resimulation and machine load.

**The 0.2-1 s spikes.**

- *Not shader compilation.* The fork defaults to `ShaderCompilationMode = AsynchronousSkipRendering`, so GX shaders never block the video thread. A session compiles only about 8 pipelines synchronously: the EFB-copy and texture-conversion utility pipelines, compiled from source on first use and never cached on disk, 2-15 ms each, all on boot or the CSS. Of the sync spikes over 50 ms, none overlapped a compile. Ubershaders would not change anything here. One caveat, from 3 below: skip-rendering makes the pixels a frame shows depend on compile timing.
- *Old path, up to about 130 ms.* The sync waited for whatever the video thread was blocked on: mostly the EFB-copy readbacks above (up to 210 ms each under contention), plus presents. Gone with the async capture.
- *Remaining outliers of 0.2-17 s in both paths.* These fall outside every snapshot phase: the phase breakdown (eviction wait, DoState, RAM copy) stayed at microseconds and about 1 ms. Both Dolphins' logs stop and resume at the same millisecond, so the cause is a host-wide I/O stall:
  - D: was 99 % full;
  - each test instance copies a 2 GB `sd.raw` onto it;
  - the harness logs at Info level, synchronously from the CPU thread (several lines per frame).

  These are test-environment artefacts, not rollback cost. They did expose the desync in 2.

### 2. The "dual-core" desync: a GekkoNet disconnect after a stall (`463c711a12`, `1e04ab552a`)

**Reproduction.** Stress campaign: dual core, D3D11 and Vulkan, bad_wifi and awful, about 2 minutes each, every confirmed frame compared. 10 sessions completed; 3 ended in setup errors (menu navigation or join under awful). Two runs desynced, one with the new snapshot path and one with the old. Both followed a host-wide stall of 8-14 s (above), and neither depended on dual core:

- The first symptom, on a confirmed frame where every checksum still matched, is a pad slot that differs:
  - on one peer, the remote player's `gfPadStatus` is all zeros;
  - the other peer has that player's real (neutral) input, which differs only in the game-owned bytes 0x39-0x3B (`CC CC CC`).

  The state checksums diverge 1-20 frames later.
- Freezing one Dolphin for 7 s with `NtSuspendProcess` (`dc_rollback.py --freeze 7 --freeze-who joiner`) reproduces it every time: 6 of 6 runs, single and dual core, Null and D3D11. The first pad mismatch came 5-30 s into the match, then 1900-2700 mismatching confirmed frames. GekkoNet's `desyncs_detected` stayed 0.

**Cause.**

1. GekkoNet drops a peer it has not heard from for 5 s (`DISCONNECT_TIMEOUT`). From an agreed frame on, it feeds that player its neutral "disconnected" input: zeros, hence the all-zero slot.
2. In a two-player game, both sides do this to each other at different frames, and each keeps its own player moving. The game cannot stay in agreement, and GekkoNet's checksum exchange stops for a dropped peer.
3. After a *local* stall, GekkoNet dropped a peer that was fine. The timeout compared the host clock with the last packet's arrival, while the peer's packets were waiting in our socket.
4. The disconnect event was lost. `gekko_network_poll` raised it, and the next `gekko_update_session` cleared it before `gekko_session_events` returned it, so the client never knew.
5. When the client did see it, `ProcessGekkoEvents` returned false and that update's game events were dropped. This left the client a frame behind GekkoNet's numbering for good, which matches the off-by-one frame counter in the first desync.

**Fix.**
- GekkoNet restarts its timeout clocks after a gap of over 1 s between two checks (our own stall), and keeps session events until they have been read.
- The client processes every event. On a peer disconnect in a two-player game it shows an OSD message and requests a game stop, which the server relays to both peers.

**Verified.**
- One peer suspended for 7 s (2 runs): the host logs `GekkoNet: Player 1 disconnected` and both games stop. No silent desync.
- Both suspended for 10 s: no disconnect, no mismatch on 3780 confirmed frames, and the match continues. A second attempt failed only because the harness's own 10 s `read_mem` timeout expired during the freeze.

**New counters.**
- `peer_disconnects`, `skipped_loads` and `dropped_advances` stayed 0 in all normal runs.
- `stall_fallbacks` counts stall waits that a pause request turned into a guest-side spin, which lets emulated time run between two frames on one peer. Every harness read during a stall does this: 1665 in 60 s under bad_wifi, with no mismatch on 4490 confirmed frames.
- Keeping the CPU on a host-side wait through such a lock was tried and dropped: it cost 8 FPS (51 vs 59), because harness reads then held the CPU thread.

**Not found: a dual-core-specific gameplay desync.** With the stall cases removed, all 27 dual-core sessions of this round compared clean: 66,944 confirmed frames in 11 runs compared end to end, plus the last 2048 frames of 16 earlier runs (32,593), over typical, bad_wifi and awful, D3D11 and Vulkan, old and new snapshot path. What differs between peers in dual core is render-side state only (3).

**Stress test.** `test_dualcore_real_backend_long_session_no_desync[bad_wifi|awful]` (`slow`, `gpu`) runs dual core with a real backend, several matches back to back, about 150 s of match time (`PPR_TEST_LONG_SECONDS`). It asserts:
- pads and checksums equal on every confirmed frame, no GekkoNet desync, no freeze;
- the deterministic GPU thread is in use;
- at most 1 % of snapshots waited for the video thread, and snapshot p99 is under 10 ms.

Both variants passed on `7b2227fd5e`.

### 3. Offline dual-core reproducibility vs netplay (`05616daf01`)

**Why both observations hold.**

- In rollback netplay the deterministic GPU thread raises PE tokens and finish interrupts and runs the CP on the CPU thread, in emulated time. So the CPU never sees GPU-thread timing, except through guest RAM written by the GPU thread.
- Brawl reads back exactly one such thing, a screen-colour probe:
  - `fn_80025314` projects a 3D point to the screen and copies 4x4 pixels into the .bss buffer at 0x804951C0 (two 64-byte EFB copies);
  - `fn_80025520` averages them into a colour;
  - `gfSceneRoot`'s update (`fn_8000EB1C`) feeds that colour to NW4R G3D light objects (0x801A7xxx). That is the System-heap G3D object that diverged in `docs/determinism-findings.md` (cause 3).
- In dual core the CPU reads the probe before or after the video thread wrote this frame's copy, depending on host timing. On every backend the pixels also depend on rendering: the GPU, the driver, and with `AsynchronousSkipRendering` the shader compile timing. Offline with D3D11 even single core is not bit-reproducible (menu instances differ from field 420).
- So the offline whole-heap hashes see render-side differences. Netplay's per-frame checksum covers gameplay fields (frame counter and fighters), which never take these values.
- Lockstep and rollback do not mask anything: peers keep their own render-side state, and a rollback restores the local snapshot.
- The gameplay divergence once seen offline came from the harness pauses on the old build: a sync while paused dropped GPU commands (fixed in `a1f9ec2685`).

**Checked on the current build** (`determinism.py compare`, the same 528-input timeline, about 500 pauses per run):

| profile | runs | result |
|---|---|---|
| dual core, Null (`dc-rtc`) | 3 pairs | 2 bit-identical in whole MEM1/MEM2 at all 175 checkpoints, including the 1200-frame match. 1 differed in timestamps only (`__OSStartTime` by 6 ticks at boot, then the timestamp in gfPadSystem and once MenuInstance), with player state equal throughout. Before: 1 of 6 identical, 4 of 6 crashed, 1 gameplay divergence. |
| dual core, D3D11 (`dc-rtc-d3d11`, new) | 3 pairs | Players, damage, stocks and RNG equal at the end in all 3. 2 differed in the match instance heaps (G3D scene state) from field 2440-2450; 1 differed from boot (MenuInstance). |
| single core, D3D11 (`sc-rtc-d3d11`, new) | 1 pair | Also not bit-identical (MenuInstance from field 420); players equal. |

**Can dual core diverge gameplay in a netplay session?** Only if the deterministic GPU thread is off. `GPUDeterminismMode` was never part of the netplay settings. A user with "none" in their config therefore ran rollback dual core with the GPU thread owning the FIFO and PE interrupts: with two instances whose Dolphin.ini and game INIs say "none", `gpu_deterministic` was false. `05616daf01` makes the netplay layer force `fake-completion` for rollback sessions (checked true afterwards).

The render-side probe stays timing-dependent by design. It would matter only if a later region-set rollback put the G3D light objects into the compared or restored set, so leave them out.

## Round 2: presentation, dual-core tests, delay setting, rollback window

| SHA | Change |
|---|---|
| `2025577415` | Resimulated frames are no longer presented (item 1 below). |
| `0951d7afa1` | Per-depth load and resimulation timing (`netplay_status.rollback.by_depth`). |
| `15b94ca0ad` | Rollback input delay is a host setting, 1-9, default 2 (item 4). |
| `9e8c9bf6fd` | Tracy profiler client opt-in (`ENABLE_TRACY`, off): `--version` took 17-19 s. |
| `64bfbdb681` | Inherited P+ updater removed; stub for our own update channel. |
| `8408ee9506` | Harness: a replaced `pad_script` keeps the port overridden until the new one starts (proposed change 12). |
| `0e49c345e2` | Offline custom RTC advances with emulated time (proposed change 14). |
| `996a9dea34` | Rollback window 5 -> 7 frames (item 5). |
| `a1f9ec2685` | Dual-core `SyncGPU` no longer drops unexecuted GPU commands while paused: the dual-core pause/frame_advance crash (proposed change 13). |

### 1. Resimulated frames on screen (`2025577415`)

Resimulated frames were presented. A rollback of N frames sent N+1 frames to the presenter within one real frame: the screen briefly showed the re-run frames (a rewind-and-catch-up flash), and the presents also cost time. Null video hid it.

- This fork presents with **Immediate XFB** by default (`GFX_HACK_IMMEDIATE_XFB = true`, also synced by netplay), i.e. when the GPU executes the XFB copy, not at the VI field. Skipping only the VI path did nothing: presents stayed equal to XFB copies.
- Fix: the guest still runs and renders every resimulated frame (skipping guest rendering changes game memory), but their output does not reach the presenter. For Immediate XFB the decision is made where the CPU thread processes the copy command: in single core when it runs, in deterministic dual core in the FIFO preprocessor, with the GPU thread taking the decisions in command order. For VI presentation the field is not sent. Both use the CoreTiming flag that already drops resimulated audio.
- `BrawlbackSkipResimRenderHook` is still patched at 0x80017404, but its skip is off (it is a resimulation shortcut that changes game memory). It replicates `lbz r0,0xed(r23); rlwinm. r0,r0,28,31,31; beq 0x80017464` correctly: both targets start with a call, so leaving r0/cr0 unset is safe (checked against the code at 0x80017404-0x8001746c).
- `PPR_ROLLBACK_PRESENT_RESIM=1` restores the old behaviour for A/B runs; `status.presentation` has the counters.

Frames sent to the presenter per displayed frame, D3D11 (3x), bad_wifi, in a match, 20 s, host / joiner (same binary, switch on/off):

| | single core before | single core after | dual core before | dual core after |
|---|---|---|---|---|
| right after a rollback (mean) | 4.18 / 3.31 | 1.00 / 1.00 | 4.55 / 3.82 | 1.00 / 1.00 |
| right after a rollback, host (frames sent: count) | 2:20 3:21 4:20 5:39 6:22 | 1:83 | 2:14 3:8 4:20 5:39 6:29 | 1:108 |
| presents per displayed frame, all frames | 1.31 / 1.14 | 0.999 / 0.999 | 1.32 / 1.10 | 0.999 / 0.999 |
| game FPS | 42.5 | 50.4 | 33.1 | 52.0 |

Cross-check with Dolphin's frame dumping (a PNG per presented frame, 1x, bad_wifi on the CSS for 15 s, both instances paused before counting), old binary `15de378723` against the new one, host / joiner:

| | dumped frames | VI fields | displayed + resimulated frames |
|---|---|---|---|
| old, single core | 1156 / 1134 | 1156 / 1134 | 907+248 / 912+222 |
| new, single core | 911 / 910 | 1015 / 1123 | 910+105 / 910+213 |
| old, dual core | 1219 / 1012 | 1218 / 1012 | 910+308 / 906+106 |
| new, dual core | 913 / 910 | 1362 / 946 | 914+445 / 910+35 |

Before, every resimulated frame was dumped (that is, presented); after, exactly the displayed frames are. The JSON files are in `run/qa/present/` (local).

Remaining stutter is the CPU cost of the rollback itself, not presentation: the displayed frame after a rollback of N frames comes about N x 5-6 ms late (see item 5). In dual core with a real backend, each snapshot also syncs the GPU thread (`SyncGPUForDoState`), so snapshot time rises from about 1 ms to 1.5-3.5 ms mean, with rare spikes of 0.2-1 s (likely GPU-thread shader compilation; not seen in single core). Worth a follow-up: capture the video register state in command order without draining the GPU.

### 2. Dual-core regression tests

`harness/tests/test_rollback.py` now runs every test in single core (`sc`) and dual core (`dc`), plus `test_rollback_presents_one_frame_per_displayed_frame[sc|dc]`. That test is marked `gpu`: it needs D3D11 or Vulkan, which is probed once with a screenshot; `--no-gpu` skips it and `--video` picks the backend. It asserts that every displayed frame sends exactly one frame to the presenter, also right after rollbacks, with no dual-core decision misses. On `996a9dea34`: 12 passed in 598 s.

### 3. Protocol docs

`docs/harness-protocol.md` has a new "Rollback" section: every `netplay_status.rollback` field (including `by_depth`, `input_delay`, `announced_delay`, `max_rollback_window`), `status.presentation`, `rollback_pad_history`, `rollback_chunk_hashes`, `cpu_state`, the delay setting and the environment switches (`PPR_SYNCTEST`, `PPR_ROLLBACK_CHUNK_HASHES`, `PPR_ROLLBACK_PRESENT_RESIM`). The command table lists the extra commands, and proposed changes 1 and 12-14 are marked resolved.

### 4. Input delay setting (`15b94ca0ad`)

`NetPlay.RollbackDelay` (1-9, default 2) is the number of frames from pad read to use, like Slippi's delay frames; GekkoNet's delay is that minus one (the local sample already lags a frame). The host's value is announced to clients (new `MessageID::RollbackDelay`, also on join), shown in the Qt netplay dialog ("Rollback Delay", host only) and sent with the start-game settings; `InitGekkoSession` uses it. Harness: `netplay_host` `delay` in rollback mode (1-9), `rollback.input_delay` and `announced_delay`.

Measured on lan: a press scheduled at a known pad read lands 2 frames later with delay 4 than with 2, and 7 frames later with 9, on the same GekkoNet frame on both peers. The default behaves like 2, the previous feel.

### 5. Rollback window (`996a9dea34`)

awful preset (150 +- 50 ms RTT, 5 % loss) to force deep rollbacks, in a match, D3D11 at 3x unless noted. Per rollback depth (`by_depth`):

| | load | resimulation per frame | 5-frame rollback | 7-frame rollback | snapshot (mean) |
|---|---|---|---|---|---|
| single core | 1.4-2.2 ms | 5-6 ms (8-11 ms under heavy load) | 29-30 ms | 58-62 ms (busy machine) | 0.85-1.6 ms |
| dual core | 1.4-2.2 ms | 5.3-5.6 ms | 26-30 ms | 34-39 ms, max about 55 ms | 1.5-3.5 ms |
| dual core, Null video | 2.0 ms | 4.9-5.2 ms | 24 ms | 34-37 ms | 1.8 ms |

Window 5 against 7, dual core, same conditions, about 32 s each: 7,000-10,600 stalled GekkoNet polls (waiting for the peer because the prediction window ran out) against 2,500-4,200; game speed 44-47 against 42-49.5 FPS. A 7-frame rollback costs about 2.3 real frames of CPU; with a 5-frame window the game freezes instead while it waits on such links. Chosen: **7** (Slippi's value). On lan and typical, rollbacks rarely exceed 3 frames, so nothing changes there. Two more snapshot slots cost about 12 MB.

One dual-core D3D11 run with the 7-frame build desynced: GekkoNet reported 863 / 868 checksum mismatches from some point on, and the run also had 0.4-0.8 s resimulation spikes. It did not reproduce in 3 repeats (each compared about 2,030 confirmed frames with identical pads and checksums) or with the Null backend, and no 5-frame or single-core run desynced. Open: it may be the rare dual-core divergence from `docs/determinism-findings.md` (cause 3), which does not depend on the window. Two runs (one per window) also failed to leave the CSS under awful in the scripted navigation; both instances kept running. Raw data: `run/qa/window/` (local).

### Harness fixes (protocol proposed changes 12-14)

- **12, `pad_script` gap (`8408ee9506`):** checked offline. X was held by a long script, then replaced by a 3-poll Y press starting 20 polls later. Before: the game read 0 for those 20 polls (the port was back on the unbound real controller). After: X, then Y for exactly 3 polls, then neutral.
- **13, dual-core `pause`/`frame_advance` crash (`a1f9ec2685`):** reproduced with `tools/determinism.py compare --profile dc-rtc` (about 500 pauses per instance): DolphinNoGUI died in 2 of 5 runs, on both the old harness build and `996a9dea34`; a stress loop of 3,000 plain pause/frame_advance cycles in a match never crashed. A small debugger (`DebugActiveProcess` + dbghelp) attached to every instance caught it: an access violation on the GPU thread in `memmove` in `VertexShaderManager::SetConstants`, after `LoadIndexedXF`. Cause: in deterministic dual core, `Fifo::SyncGPU` moved the video buffer from the preprocessor's position, assuming the GPU thread had executed everything up to it. While the emulator is paused the GPU loop does nothing, so a sync then (`PauseAndLock` from a `CPUThreadGuard` during a pause) dropped the unexecuted commands and the GPU thread resumed mid-stream. Now the unexecuted part is kept and run on resume; the normal case is unchanged. After the fix: 0 crashes in 18 runs; with VIDEO logging the case occurs 600-800 times per run (about 150 bytes each), each of which used to drop commands. This may also explain part of the dual-core divergence under pauses in `docs/determinism-findings.md` (cause 3): every pause dropped GPU commands. Netplay without harness pauses is not affected (the GPU thread runs while syncing). The dual-core rollback tests (6) pass with the fix.
- **14, custom RTC (`0e49c345e2`):** the RTC is the custom value plus emulated seconds. The old divergence did not reproduce here (six pre-fix boots, two at 25 % speed, had identical RNG seeds at field 400), so this is a fix by construction.

### Launcher-related (coordinator items)

- **`Dolphin.exe --version` took 17-19 s (`9e8c9bf6fd`).** The version printed at once; the exit then waited for Tracy's profiler thread, which was initialising symbol lookup over the working directory (slow from the workspace root). The Tracy client was always compiled in (it also listens on the network). It is now opt-in. `--version` from the workspace root: 16.7-19.3 s before, 0.09-0.13 s after.
- **P+ auto-updater (`64bfbdb681`).** The P+ GitHub update check, its dialogs and its downloader are removed; Dolphin's own auto-updater is not started; the toolbar Update button is hidden. `MainWindow::CheckForUpdatesAuto` and `ShowUpdateDialog` are a marked stub for our update channel (config `AutoUpdate.CheckForUpdates`, default off, no network call). `Dolphin.exe -u <dir> -e "Project+ Netplay Launcher.dol"` without the harness reached the character select with no dialog and no remote TCP connection; with the harness, a screenshot shows the character select (`run/qa/updater/qt-netplay-launcher.png`).

## Result

### Scorecard

`python -m ppharness scorecard --presets lan,typical,bad_wifi --sessions 1 --duration 60 --phase auto`. HEAD is compared with `docs/scorecard-baseline.md`.

| Preset | Baseline (`harness`) | HEAD (`4d972aaa29`) |
|---|---|---|
| lan | One side froze at 8.3 s (GekkoNet frame 146, CSS). The live side ran alone at 42 FPS. Setup never got past the character pick. | Reached the match. No freeze, no stall over 2 s. 9 rollbacks, desyncs 0. Frames ahead 0.14 / -0.03 (mean). Game FPS 59.4 / 59.4. State compare: match. |
| typical | One side froze at 7.2 s (frame 161). | Reached the match. No freeze. About 200 rollbacks per side, desyncs 0. Frames ahead 0.15 / 0.00. Game FPS 56.9 / 56.9. State compare: match. |
| bad_wifi | One side froze at 6.8 s (frame 122). | Reached the match. No freeze. About 240 rollbacks per side (591 / 634 frames resimulated), desyncs 0. Frames ahead 0.29 / 0.05. Game FPS 53.3 / 53.4. State compare: match. |

The full JSON is in `run/qa/scorecard-rollback-fixes.json` (local).

### Scorecard, dual core

Same command with `--cpu-thread on`, HEAD `7ff324a940`. The JSON is in `run/qa/scorecard-rollback-fixes-dualcore.json` (local).

| Preset | Result |
|---|---|
| lan | Reached the match. No freeze, no stall. 44 / 47 rollbacks, desyncs 0. Game FPS 59.0 / 57.9. State compare: hashes differ, `checksum_fields_match: true` (see below). |
| typical | Reached the match. No freeze. 199 / 223 rollbacks, desyncs 0. Game FPS 57.8 / 57.0. State compare: match. |
| bad_wifi | Reached the match. No freeze. 292 / 321 rollbacks (645 / 679 frames resimulated), desyncs 0. Game FPS 58.9 / 58.6. State compare: match. |

Before the two dual-core fixes, the first rollback in dual core either crashed (`memmove` with a negative size in `Fifo::SyncGPU`) or each save took about 100 ms, giving 5.8-7.7 FPS. After: save 2.4-4.3 ms, load 2.0-2.4 ms, about 58 FPS.

The lan state-compare mismatch is in the heap-range hash only; every checksum field matches.

### Where the peers still differ

Measured with the scratch driver on a frame both peers have confirmed (typical preset, in a match). It compares MEM1/MEM2 hashes in 4 KiB chunks (`PPR_ROLLBACK_CHUNK_HASHES=4096`), then reads the differing chunks from both peers and keeps the words that are stable on each side but differ between them. An earlier version of this section listed 0x9432xxxx-0x9439xxxx. Those addresses were wrong: `rollback_chunk_hashes` reported the 64 KiB default chunk size whatever the env value was (fixed in `2332460493`).

8 chunks and 36 words differ, none of them in a gameplay heap:

| Address | Area | What it looks like |
|---|---|---|
| 0x804C9FA8 | .bss | a saved return address, i.e. an OS thread stack in .bss |
| 0x804DE4B8, 0x80584008 | .bss | PAD/SI status words (0x40808080 vs 0xC0808080), the known sync-test residue |
| 0x804F67C4 | .bss | PAD/SI library counter, known residue |
| 0x805B2E14-0x805B37C8 | between the DOL's .bss end (0x805A5154) and the System FW heap (0x805B5160) | thread stacks: return addresses (0x801B85xx, 0x801E1Axx), OSThread pointers (0x804DD558) |
| 0x805B9A40, 0x805B9F84-0x805B9F8C | System FW heap | a hash-like word and two pointers (one to gfPadSystem 0x805BACC0) |

The fighter checksums matched on all compared frames. These ranges are candidates to leave out of the gameplay region set; they are not desyncs.

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
8. **Dual core — default restored and working under rollback.** The default is true again (`041f4cf2a5`). Two fixes were needed (single core is never forced):
   - `d8359cde64`: the GPU thread is synced (`Fifo::SyncGPU`) before every rollback save and load, so the video DoState doesn't run against a moving FIFO. Before: a crash on the first rollback.
   - `7ff324a940`: `AsyncRequests::QueueEvent` wakes the GPU thread. In deterministic dual core the GPU loop slept until the next timeout, so each save waited about 100 ms. Before: 5.8-7.7 FPS; after: about 58 FPS.
   - Result: the dual-core scorecard above, with desyncs 0 on all presets.
9. **Checksum — fixed (`242af80c0e`).** GekkoNet now checksums verified fields: the frame counter, plus per port the active instance, damage, stocks, X/Y and status kind. The parts are recorded per frame for the harness.

## Audio during resimulation (`15de378723`)

Samples produced while resimulating are dropped in `AudioCommon::SendAIBuffer`; those frames were already heard when they first ran. Measured with the DSP audio dump (typical preset, both peers moving on the CSS, about 2,330 presented frames):

| | Samples per presented frame |
|---|---|
| Before | 592.6 / 604.3: 533 for each presented frame, plus each resimulated frame again |
| After | 533.7 / 533.5, which is 32,000 Hz / 60 |

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

1. Done: resimulated frames are not presented (round 2, item 1). Follow-up: dual-core snapshots drain the GPU thread (`SyncGPUForDoState`), which costs FPS with a real backend.
2. Gameplay-only rollback (user decision): restrict save/restore to a region set. Inputs to that work:
   - memory that legitimately differs between peers: PAD/SI library buffers 0x804DE3B0-0x804DE4B0, 0x804F67B0, 0x80584000;
   - OS thread contexts and timestamps;
   - the CPU and timing state that must still be restored: registers, downcount, exceptions, CoreTiming;
   - the ranges that differ between peers outside the gameplay heaps (table above).
3. Done (round 3): the dual-core desync was a GekkoNet disconnect after a multi-second stall; fixed. Open: decide the UX for a dropped peer (now: the game ends on both sides with an OSD message), and compile round 3 with GCC once Docker works again.
