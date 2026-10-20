# Highlights (v1, 2026-10-20)

A replay's stats page lists that game's highlights (strings the launcher finds in the recorded
fighter memory) and L+R+Z clips, and plays them in the playback Dolphin. The decisions and the
measurements behind them are in the prototype's DESIGN.md (D:\code\brawl-highlights, local only);
this is what was built. Not yet: background detection over all replays, the sidebar's ★ Highlights
folder, `.bhl` files, a savestate cache, video export (single clips, the combo movie).

## 1. Detection (launcher main process)

| Where | What |
|---|---|
| `launcher/src/replays/highlights/replay_frames.ts` | Decodes only what the detector needs from a `.brep` (CRC-checked, bounded): per player and frame damage, stocks, status kind, animation frame, motion kind, the attacker's ftEntry id, the player's own id (28 of the 2,248 fighter bytes, XOR-accumulated byte by byte within each `FTRM` chunk) and 10 pad bytes from `FRMS`. |
| `detect_highlights.ts` | The prototype's `timeline.py` + `highlights.py`: conversions (the victim never escapes; 45 free frames end one, or a KO), features, tags (`combo`, `zero-to-death` / `low-to-death`, `fast`, `clean`) and the score. A highlight is a conversion tagged other than `clean`. |
| `lrz_clips.ts` | L+R+Z (gfPadStatus +0x0: L 0x40, R 0x20, Z 0x10) held 30 frames marks a clip: 20 s before the hold, 2 s after it, the next one 5 s later at the earliest (Project Clippi's defaults). Only the recording player's presses count (`CLIP_PLAYERS` in `highlight_service.ts`). |
| `analyze_replay.ts` | The list's rows: a highlight plays from 1.5 s before its first hit to 1 s after the string (a KO: after the stock is lost). |
| `highlights.worker.ts`, `highlight_service.ts` | On demand when a stats page opens, one replay at a time in a `worker_threads` worker (gone after 2 minutes idle), cached in replays.sqlite `highlight_cache` (path, size, mtime, detector version, what was looked for). |

Score (private: the list shows only who on whom): moves, damage / 15, a KO bonus (more from lower
percents), speed, one point a move for a clean string, all scaled by the share of attacks that hit.
Detector version 2 (the user, 2026-10-20) rewards variety: a move whose kind (the attacker's motion
at the move's start) the string already used counts half, and every kind past the second adds 0.75.
Chain grabs and other repeated strings stay, ranked below varied ones.

Checked against the Python prototype with version 1's score (`noVariety`): the 56 replays in
Documents\Brawl Online\2026-10 give the same 1,528 conversions (94 highlights) with every feature
and score equal. The TS decoder takes 1.5 s for all 56 (about 27 ms a replay; 120 ms in the worker
for a 1.5-minute game with the file read). L+R+Z: over those 768 k player-frames the three were
never down together.

## 2. The stats page

`replay_file_stats/highlights/highlight_list.tsx`: rows of the attacker's character icon and name
vs the victim's (teams: the attacker's team vs the victim's), best first, then "Clips (L+R+Z)" with
who clipped each. A click plays it; ↑/↓ or Tab/Shift+Tab plays the next one at once; Enter plays the
selected one; **Play all** plays the list in order. A banner says what the playback Dolphin does:
starting, loading the replay, **fast-forwarding to the highlight (frame N / target, a progress
bar)**, playing, done, or why it can't (a refused replay; a replay that plays differently here and
ends before the highlight). A double-click on a replay's row in the Replays tab opens the page.

Settings > Clips: "L+R+Z to clip" (on), "Disable automatic highlight detection" (off; "Disables
the local highlight detection for replays."), and the video output folder (default
`<Root Replay Folder>/Highlights`, `SettingsManager.getClipsFolder`; for the render work).

## 3. Playing (comm file `highlights`)

The launcher writes `{"mode": "highlights", "commandId", "queue": [{"path", "startFrame",
"endFrame"}, ...]}` (no 1 s wait between writes: our Dolphin reads the whole file every 200 ms).
`ReplayPlayback.cpp` arms each replay holding at its end and gives the session the clip
(`Session::SetPlaybackClip`): a seek to its start (forward: on from where it is; back: the newest
keyframe at or before it is put back), then play, paused once its last frame has run. The next item
of the same replay, or a new request whose first item is the replay already playing, goes on in
the running match; another replay ends the match and arms the next. During a clip's fast-forward the
game is muted and one frame in 30 is presented, so the screen moves and the OSD line ("Fast-forwarding
to the highlight... 45%", "(2 of 5)") shows. The status file adds `highlights`, `clipStart`,
`clipEnd`, `seekFrom` and the state `seeking`; the launcher shows it from `PLAYBACK_STATUS` events
for its own `commandId`.

Measured here (Qt Dolphin, D3D11, other sessions' harness Dolphins running): a cold start to the
first highlight's fast-forward ~7 s; the fast-forward 330-680 frames a second (8,160 frames in 12-25
s); the next highlight of the same replay at once (back: under 0.2 s; forward: from where it is).

Harness: `test_replay.py::test_launcher_comm_file_plays_highlights` (a recorded Direct game; three
clips back and forth in one match, a new request in the same match, a clip past the end holding at
the last frame, no checksum differing, one match armed).
