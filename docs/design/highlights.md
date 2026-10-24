# Highlights (v1, 2026-10-20)

A replay's stats page lists that game's highlights (strings the launcher finds in the recorded
fighter memory) and L+R+Z clips, and plays them in the playback Dolphin. The decisions and the
measurements behind them are in the prototype's DESIGN.md (D:\code\brawl-highlights, local only);
this is what was built. Video export (single clips and the combo movie, §4-5) and the Replays >
Highlights tab of every replay's highlights (§6) came 2026-10-21; manual clips, made on the
seek bar of a replay or a spectated game (§9), 2026-10-24. Not yet: `.bhl` files, a savestate
cache.

## 1. Detection (launcher main process)

| Where | What |
|---|---|
| `launcher/src/replays/highlights/replay_frames.ts` | Decodes only what the detector needs from a `.brep` (CRC-checked, bounded): per player and frame damage, stocks, status kind, animation frame, motion kind, the attacker's ftEntry id, the player's own id (28 of the 2,248 fighter bytes, XOR-accumulated byte by byte within each `FTRM` chunk) and 10 pad bytes from `FRMS`. |
| `detect_highlights.ts` | The prototype's `timeline.py` + `highlights.py`: conversions (the victim never escapes; 45 free frames end one, or a KO), features, tags (`combo`, `zero-to-death` / `low-to-death`, `fast`, `clean`) and the score. A highlight is a conversion tagged other than `clean`. |
| `lrz_clips.ts` | L+R+Z (gfPadStatus +0x0: L 0x40, R 0x20, Z 0x10) pressed together, for a frame or more, marks a clip: 20 s before the hold, 2 s after it, the next one 5 s later at the earliest (Project Clippi's defaults). Only the recording player's presses count (`CLIP_PLAYERS` in `highlight_service.ts`). |
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
never down together. The threshold was 30 frames (half a second) until 2026-10-24: the user's
clips never showed, as a press meant as a clip is a tap. In 122 replays (2.6 M player-frames) their
7 presses were down 6 to 16 frames, an opponent's 28 and 131, and nothing else 3 frames or more,
so 5 frames it was; then any press (the user: "i can't imagine an actual game situation where you
press all 3 and dont intend to clip"). The clips come from the replay's pads, so a lower threshold
finds the presses of every replay already recorded (a DETECTOR_VERSION bump has the cached ones
looked at again).

## 2. The stats page

`replay_file_stats/highlights/highlight_list.tsx`: rows of the attacker's character icon and name
vs the victim's (teams: the attacker's team vs the victim's), best first, then "Clips (L+R+Z)" with
who clipped each. A click plays it and selects it; ↑/↓ or Tab/Shift+Tab plays the next one at once; Enter plays the
selected one (Play all went 2026-10-24: the playback Dolphin's list buttons, §3, took its place).
A row starts with its favorite star and ends with a Play button (as the Replays page's), which plays
it without touching the selection (2026-10-24, the user).
The row playing is shown selected, and follows the playback Dolphin's buttons and stars. A banner
says what the playback Dolphin does:
starting, loading the replay, **fast-forwarding to the highlight (frame N / target, a progress
bar)**, playing, done, or why it can't (a refused replay; a replay that plays differently here and
ends before the highlight). A click on a replay's row in the Replays tab opens the page (rows can't
be selected: multi-select and its Delete / Select All / Play All bar went 2026-10-24).

Settings > Clips: "L+R+Z to clip" (on), "Disable automatic highlight detection" (off; "Disables
the local highlight detection for replays."), and the video output folder (default
`<Root Replay Folder>/Highlights`, `SettingsManager.getClipsFolder`), where renders go (§5).

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

The playback Dolphin's overlay is the replay one (the same seek bar, buttons and hotkeys): a
highlight is its replay with a seek to the clip's start and a pause at its end. The end is a line
(2026-10-24, the user: "whenever the end of a highlight is hit ... it should pause"): playing
pauses each time it gets there, also after a seek back to before it, and the bar shows it as a
white line while it is ahead; a seek past it, or Play once paused there, plays on (the replay,
until another highlight). Each highlight of the replay playing is marked on the bar with a gold
line and a star above it (2026-10-24, the user's request); hovering a star shows where its
highlight ends; a click on the star or the line plays it from its start (`Session::
SetPlaybackMarks`, `PlaybackCommand::PlayHighlight`). The render Dolphin has no marks.

**The list's buttons** (2026-10-24, the user's request). A list's row asks for that one highlight
and adds the list: `"marks": [{"startFrame", "endFrame", "position"}]` (the list's highlights of
that replay, the stars) and `"nav": {"position", "total", "hasPrevReplay", "hasNextReplay"}`.
The overlay's title row then ends with "3/40" and **<< < > >>** ("Previous replay", "Previous
clip", "Next clip", "Next replay"). Their order (`highlight_nav.ts`): the list's replays in the
order they first come in it, each one's highlights by their start, so with Best first the count
and the row playing jump about the list (with Newest first it is the list's own order). < and >
go to the highlight before / after the frame shown in this replay, Dolphin itself from the stars
(a < more than 1 s into a highlight plays it again from its start, as media players' Previous),
and past this replay's first / last one to the replay before's last / the next replay's first;
<< and >> to the replay before / after, its first highlight. What needs another replay is asked
of the launcher in the status, `"navRequest": {"seq", "action", "frame"}`, which plays the one it
picks as a new request; a star or < > played here is told, `"highlightPick": {"seq",
"startFrame", "endFrame"}`, and the list's row follows (both count from 1 for each commandId).
The list is the one played from last: the Highlights tab's section or a replay's stats page.
Another section opened doesn't change what plays, but the buttons go by it from then on; the same
commandId is written again with its stars and count (at most every 400 ms), and Dolphin updates
them without touching the clip (a highlight not in it shows "-/40"; from it the buttons go to the
list's first or last). The launcher answers with any page open (`useHighlightNavigation`, mounted
for the whole window). A replay of another Project+ release asks as a row's click does, in the
launcher's window. Spectating has no list buttons.

**The Replays tab's replays** (2026-10-24, the user: "replays should get those buttons too, just
two of them, for next and previous replay, and count of all replays"). A replay played from the
Replays list (a row's play button, or the play button of its stats page) is a `queue` request with
`nav` and `"clips": false`: the title row ends with "12/340  << >>", the count of the folder's
replays as the browser lists them (its filters and sort, every one, not only the rows loaded:
`getAllFilePaths`, as Play selected with Select all), and << >> play the replay before / after it
(`play_from_list.ts`, the same `navRequest`). Its row in the list is shown selected and follows.
Another folder, filter or sort while it plays is the buttons' list from then on (the count shows
it at the next replay). A replay not in the list (opened from a file) plays without buttons.

Measured here (Qt Dolphin, D3D11, other sessions' harness Dolphins running): a cold start to the
first highlight's fast-forward ~7 s; the fast-forward 330-680 frames a second (8,160 frames in 12-25
s); the next highlight of the same replay at once (back: under 0.2 s; forward: from where it is).

Harness: `test_replay.py::test_launcher_comm_file_plays_highlights` (a recorded Direct game; three
clips back and forth in one match, a seek away and a click on the second clip's mark, then on to
the third, a new request in the same match, a clip past the end holding at the last frame, no
checksum differing, one match armed); `test_launcher_comm_file_saves_manual_clips` (end lines,
the list's stars, count, picks, nav requests and updates). Launcher: `highlight_nav.test.ts`.

## 4. Rendering, the launcher (2026-10-21)

Above the rows, one line that stays as the rows scroll (2026-10-24, the user: no "Highlights"
title, no Render as Video tick box): the Highlights tab's search and sort, then **One combo movie**,
**Render (w/ Music)** and **Render (No Music)**, enabled once a row is selected. A click selects
a row (and plays it), shown with a brighter outline; Ctrl+click adds or removes one, Shift+click
selects a range from the last one clicked (Ctrl+Shift adds it), neither playing; Ctrl+A selects
all. The render buttons render the selected rows, a video each, or with One combo movie one video,
in the list's order; queued, the selection clears. No Dolphin window opens: under the buttons each job
shows its name, w/ Music or No Music, "Waiting", "Starting...", "Rendering 45%" (a combo: "Clip 2 of
5 · 45%") with a bar and **Cancel**, then "Saved" with **Show in folder**, or "Couldn't render it:
<why>". Jobs belong to the launcher run (main keeps them; the page shows this replay's, a cancelled
one not at all).

Names (DESIGN.md §5, `render_names.ts`): the game's date and time from its replay's name, then who
did what with their characters: `2026-10-20 1635 sarahkitty (Mr. Game & Watch) vs t3o7 (Lucas).mp4`
(a highlight: attacker vs victim), `... Clip by sarahkitty (Mr. Game & Watch).mp4` (L+R+Z),
`2026-10-15 2153 sarahkitty (Mr. Game & Watch) vs Smear (Snake) - combo movie (5).mp4`. Made safe
for every file system, and ` (2)`, ` (3)`... when taken.

Project+ versions (pplus-versions.md): a render request is decided as a play request is
(`DolphinManager.decideRenderReleases`), so a v3.2.0 replay asks first ("This clip is from Project+
v3.2.0", "Download and render"), with no "anyway": a clip renders on its own release or not at all
(`withRenderVersion`). A combo is always one movie (the user, 2026-10-21: "I don't want two video
files"). When its replays are on different releases (the Highlights tab's), each run of clips of
one release in the list's order is a piece rendered in that release's Dolphin (a Dolphin plays one
release's card) to `<name>.mp4.pieceN.mp4`, and the pieces are joined into `<name>.mp4` without
re-encoding (`mp4_concat.ts`), then deleted.

**Joining** (`mp4_concat.ts`): each track's samples one piece after another, copied byte for byte
into one `mdat` after a rebuilt `moov` (one sample a chunk, 64-bit offsets, the tracks
interleaved by time; ftyp+moov+mdat, so it plays as it loads). The three encoders' files differ:
Media Foundation's have no edit list and no B-frames; AVFoundation's and x264's have edit lists for
the encoder's delay (video, B-frames) and the AAC priming and padding, and end their last video
sample's decode duration short of its frame. So: a piece's video lasts from its first frame shown
to its last one's end (its last decode duration made up) and is shown from where the first piece's
first frame is (composition offsets shifted); a later piece's audio drops the priming samples its
edit list skipped, and every piece's audio ends where its edit list ends (last sample shortened);
the first piece's edit list stays, covering the whole. A piece whose sample description differs
adds an `stsd` entry. Measured on real renders joined with themselves: Windows, macOS and Linux
2 and 3 pieces, every frame decoded (966 = 2 x 483, 1449 = 3 x 483), no decode error; audio
within 3 ms of the video after 3 pieces on Windows and macOS; on Linux the video runs one frame
(17 ms) longer per piece than its audio (Dolphin's video has a frame more than its segment).

`render_queue.ts` (main) runs the jobs one at a time in the **render Dolphin**: the playback
Dolphin's command line plus `-b --render` (no window), `[Core] EmulationSpeed` 0 (as fast as the PC
goes: frames are never dropped, the video is timed by the game), `[DSP] Volume` 0 (the video gets
the samples before the volume), not fullscreen, the internal resolution the video's height needs
(`Graphics InternalResolution`: 480p 1x, 720p 2x, 1080p and 1440p 3x: the game's 528 lines scaled
to at least the video's), and the job's `[Online] Music` (`buildRenderDolphinArgs`). It has its own User folder, `<userData>/render/User`
(its own NAND, Brawl save seeded as the playback one's), so it never shares one with a playback
Dolphin open at the same time; the SD card is shared read-only as for playback. The Dolphin stays
open between jobs with the same release, music and resolution (a next one of the same replay goes
on in its match) and closes when the queue is empty. Progress is read from the comm status every 250 ms: per
clip, its fast-forward weighted at a third of a rendered frame (they run ~600 against ~200 a second
here) and the clip itself. A job fails when Dolphin says so, closes, finishes without a file, or
says nothing new for 3 minutes; Cancel (and a failure) closes the render Dolphin and deletes the
file.

## 5. Rendering, Dolphin (comm file `render`)

`{"mode": "render", "commandId", "output": "<path>.mp4", "queue": [{"path", "startFrame",
"endFrame"}, ...]}`: the highlights flow of §3, each clip recorded, all of them one after another
in one file. A next request for the replay already armed (once the last file is written) goes on in
its match. The status adds `render`, `output`, `rendered` (clips written) and `framesWritten`;
`ended` once the file is complete, `error` with the file deleted (an encoder error, a replay that
can't be played, the match ending before a clip); a new request or Dolphin's shutdown gives up a
render still going, deleting its file.

`Core/Rollback/ClipRecorder.h` (`Gprb::Clip`): the file is H.264 at 60000/1001, 16:9 at the height
the comm file's `height` says (Settings > Clips > Video Resolution: 854x480 at 5 Mbit/s, 1280x720 at
12, the default, 1920x1080 at 20, 2560x1440 at 32), and AAC 48 kHz stereo (192 kbit/s) in MP4, written as `<name>.mp4.part` and named `<name>.mp4` once
complete (a render cut short, e.g. the launcher closed, leaves no half-written video under the
real name; Cancel deletes both). The session (`SetPlaybackClip`'s `record`) opens a
segment when the clip's first frame runs after its fast-forward and marks its end, in emulated
ticks, at the clip's pause (or the replay's last frame). The frame dumper reads back every field the
game presents (repeated ones too), at the XFB's aspect-corrected size fitted into 1280x720, and the
recorder pads it to 1280x720; the DSP's samples (`Mixer::PushSamples`, big-endian R/L at the DMA
rate, 32 kHz) are resampled to 48 kHz. Both are kept when their ticks are in the segment and timed
by them, so audio and video stay together whatever the PC's speed; a segment's audio is padded or
cut to its length, and the next segment starts where it ended. After a clip's pause ReplayPlayback
waits until no frame has come for 500 ms (the dumper hands frames over a frame late) and ends the
segment. Encoding runs on its own thread with a queue of 6 frames, which holds the emulation back
instead of dropping frames. A render request turns Immediately Present XFB off for that Dolphin (on
by default, for latency): it stamps a frame with an estimate of the next field's time, wrong
whenever the GPU thread is ahead of or behind the CPU's (on macOS 228 of a clip's 300 frames came
with an earlier frame's time and were left out). While a clip records, a readback still waiting
for its frame's end goes to the dumper's thread before the next one is taken. Each segment's log
line counts the frames left out (before it, after it, not newer than the last: the paused frame
drawn again while the clip's last frames come in, ~20 a clip). The overlay and OSD are drawn on the window, not the XFB: they are not in
the video.

Encoders are the operating system's, so rendering adds nothing to the download:

| | Encoder | Where |
|---|---|---|
| Windows | Media Foundation's sink writer: the GPU's H.264 encoder, else Microsoft's software one; its AAC encoder ("N" editions need the Media Feature Pack) | `ClipEncoderMF.cpp` (mfplat, mfreadwrite, mfuuid) |
| macOS | AVFoundation's asset writer (VideoToolbox H.264, Apple's AAC) | `ClipEncoderAVF.mm` (ARC; AVFoundation, CoreMedia, CoreVideo) |
| Linux | FFmpeg as Dolphin links it (the AppImage's): libx264, OpenH264 or MPEG-4 Part 2, FFmpeg's AAC | `ClipEncoderFFmpeg.cpp` |

Dolphin's own Windows FFmpeg (FFmpeg-bin) has neither H.264 nor MP4, and our macOS build has none.

Qt `--render` (with `--batch`): MainWindow never shows the render widget and boots with an empty
WindowSystemInfo: D3D11 and Metal run without a swap chain, and no window appears at all; on macOS
the process is also background-only (`Common::HideFromDock`: no Dock icon, `lsappinfo` type
"BackgroundOnly").

Measured here (Windows, D3D11, hardware encoder): the 2026-10-20 1635 Room G1 replay's clips
3000-3300 and 6000-6180 in one file, 13.8 s from Dolphin's start to the finished file (481 frames,
video 8.041 s, audio 8.043 s); from the launcher, one highlight 16.5 s from the click with a cold
Dolphin; the 2026-10-15 2153 Ranked G1 (Project+ v3.2.0, on its own card) as a 5-highlight combo
movie, No Music: 26 s after the patch was set up (1717 frames, 28.6 s of video; the first clip's
fast-forward to frame 8334 is most of it). macOS (the M4 Max test Mac, Metal, an hl-src build over
SSH): clips 1000-1300 and 2000-2180 of the same replay, 483 frames at exactly 60000/1001, video
and audio both 8.0417 s, ~13 s from Dolphin's start.

Harness: `test_replay.py::test_launcher_comm_file_renders_highlights` (a recorded Direct game; two
clips into one file, then one more in the same match into another; the frame count; with ffprobe
on PATH, the streams and their lengths).

## 6. Replays > Highlights (2026-10-21)

Replays has two tabs as Home has: **Replays** (the replay browser) and **Highlights**. The header's
Replays button always opens the Replays tab (no longer the last stats page). A replay row's stats
button is a star, "Show highlights" (its stats page, which lists that game's highlights). The
stats page's header no longer shows the platform ("Brawlonline" on every one of ours).

The Highlights tab (`all_highlights_page.tsx`) lists every replay's highlights, then the L+R+Z
clips, each row with its game's date; no game header. Sorted
Best first (the private score, across games) or Newest first; clips newest first. Play, ↑/↓,
selecting, rendering and combo movies work as on a stats page (`HighlightRows`, shared); a combo of
several games is named after when it was made: `2026-10-21 0845 Combo movie (2 highlights)`.

`all_highlights.ts` (main): the .brep files in the replay folders (root and extra, 4 levels down),
those whose highlights are in `highlight_cache` (as the file, detector and settings are now) at
once, the others afterwards one replay at a time in the highlights worker with 50 ms between them
(a first run over a thousand replays never bogs the PC down), each sent as an event with the count
done (the tab: "Finding highlights in your replays... 12 of 120"). Opening the tab again starts
over: a fill still running stops at its next replay. Measured here: 54 replays none of which were
cached, all found in 5.4 s.

**Favorites** (the user, 2026-10-22). Every row (here and on a stats page) starts with a star (it ended with it until 2026-10-24), hollow
until clicked, then filled: a favorite. They are kept in the settings (`favoriteHighlights`), each
as `<replay path>|<highlight or clip>|<start frame>` (`favorites.ts`), so a moved or renamed replay
loses its stars, as does a highlight whose start a new detector version moves. The back arrow went
(the page starts right below the tabs); on the left a sidebar as Replays' folders, the same darker
background, the open entry the page's own so it joins the list: **All highlights** (open on
arriving) and **Favorites**, the same list of the starred rows only. A row unstarred in Favorites
stays (hollow) until Favorites is opened again, so the rows under it don't move while playing.

**Search and more sections** (the user, 2026-10-24). Left of the sort, a search box keeps the
rows showing a player whose name or code has the text (any case), in every section: a highlight's
attacker and victim (in teams, their teammates too), an L+R+Z clip's player, any player of a manual
clip (its row shows them all). A free-for-all's third player doesn't bring up A vs B's rows. Between
All highlights and Favorites the sidebar has **My Highlights** (highlights whose attacker is this
machine's player: the replay's recorder port, kept in each replay's highlights as `recorderPort`),
**Opponent Highlights** (attacker not me, nor my teammate in a team battle) and **L+R+Z
Highlights** (the clips only; hovering it says "Press L+R+Z in-game to clip the last 20 seconds.",
from `CLIP_BEFORE_SECONDS`). A replay without a recorder port is in neither mine nor my opponents'.
Highlights cached before `recorderPort` was kept get it from the replay's header the first time
they are read (`highlight_service.ts`), written back to the cache, so no replay is analysed again.

## 7. Settings > Clips > Video Resolution, and a playback that never starts (2026-10-21)

Video Resolution (the user's request): a dropdown of 480p, 720p (the default), 1080p and 1440p
(`clipResolution`, @common/clip_resolution). A render job takes it when queued; its row says it
("720p · w/ Music").

"Starting the replay..." could stay forever (the user, 2026-10-21): the playback Dolphin's window
closed while it was starting (seen here: two closed while booting, the banner stayed), so no
status ever came. The playback Dolphin's CLOSED event now clears the list's banner, unless the
launcher closed it itself to start it again on another Project+ release's card (`replaced`: the
request goes on there). The banner has a close button, and after 20 s of starting it adds "Still
starting. If the Dolphin window is asking something, answer it there; or close it and try again."

## 8. Render to video, a whole match (2026-10-21)

The user's request: a replay row has a button left of Show highlights, the clapperboard with a
download arrow taped to its corner (`render_video_icon.tsx`: ClapperboardIcon's board and Lucide's
arrow-down-to-line, the board cut away around the arrow), tooltip "Render to video". A click opens Render (w/ Music) / Render (No Music), as highlights have. It
renders the whole match as highlights are rendered: one render job, the match as one clip,
Settings > Clips' Video Resolution and folder, the video named as the replay is (`2026-10-12 2040 Ranked G1 - sarahkitty2 (Snake) vs sarahkitty
(Lucas).mp4`). `RenderRequest.wholeMatch`: main reads each replay's frames (`match_frames.ts`):
ENDG's `firstFrame` to `lastFrame`, the frames recorded, else (a game that crashed has no ENDG)
the frames decoded. On this PC's 56 replays both give the same frames (9 have no ENDG).

While it renders, the button has a progress ring ("Rendering to video... 42% (click to cancel)";
queued: "Waiting to render"), and a click cancels it. Once done a toast says "Rendered
<name>.mp4"; a failure says why. The job is also listed on the replay's stats page with Show in
folder (`RenderJobInfo.wholeMatch`; the list's hook is `use_match_renders.ts`).

Harness: `test_replay.py::test_launcher_comm_file_renders_whole_match`.

## 9. Manual clips (2026-10-24)

The user's request: "clipping to highlights / replays / spectator views ... as segments. like you
click a "clip" button, and then the replay ui shows you a left and right point, ending at the point
you clicked "clip" and starting 20s earlier by default, when you're in clip mode the buttons become
"cancel clip", "save as highlight" -- we can categorize them in the highlights menu as "Manual
Clips"".

**Dolphin** (`VideoCommon/ReplayOverlayUI.cpp`). The replay overlay's time row has a **Clip**
button left of the time, in the launcher's playback Dolphin whenever a replay of its request is
armed (a replay, a highlight, a spectated game; not the render Dolphin, nor a replay opened
without the launcher: `Replay::Playback::CanSaveClip`). A click enters clip mode: the clip ends at
the frame shown and starts 20 s before it (the match's start at the earliest; at least 1 s long),
drawn on the bar as a blue span with a handle at each end, taller than the bar, and the replay is
paused at the clip's start (2026-10-24), so Play plays it: its end pauses as a highlight's does
(`Session::SetPlaybackStop`). A handle moves only once dragged 4 px (scaled); from then on it
follows the mouse's movement since the press, never jumping to the mouse, so it can be nudged
back a pixel; a press and release within that is a click and seeks to that end. Letting a dragged
handle go seeks there too, so the screen shows the frame chosen (hovered: "Clip start (1:03)"). A
press elsewhere on the bar still seeks. The row says "Clip 1:03 -
1:23 (20 s): drag its ends", and the buttons are **Cancel clip** and **Save as highlight** ("Clip
saved to Replays > Highlights > Manual Clips" on screen). Clip mode ends with either, or when
another replay is armed (a spectated set's next game). A live game's clip ends at the live edge at
the latest.

**Handing it over** (`Rollback/ReplayPlayback.cpp` `SaveClip`): the clip is of the replay armed
(its path as the comm file gave it; the session's replay must still be the one armed). Every clip
saved in the Dolphin's run is in the status file, `"savedClips": [{"path", "startFrame",
"endFrame"}]` (the same frames again are one; 500 at most), written at once. Its star is on the
bar at once and stays for that replay while the Dolphin runs (spectating: that game, until the
window closes or the set's next game; it is a highlight of the game's kept copy afterwards). The launcher reads the
status every 250 ms and once more as the Dolphin closes (`instance.ts`), so a clip saved just
before closing is not lost.

**Launcher** (`replays/highlights/manual_clips.ts`, main): each clip of a status not seen yet is
kept once in the settings, `manualClips` (oldest first), with what its rows show: the players from
the replay's header, teams, the recorder's port, the game's date, when it was saved. A clip of a
**spectated** game (a file under the playback User folder's `Spectate`, cleared at every start)
keeps a copy of the game in `<Root Replay Folder>/Spectated/<date> Spectated <Mode> G<n> - <names>.brep`
and points at it (`spectated: true`, no recorder). The game may still be going: the copy is cut
after its last whole chunk (it plays as a replay that ended there, as a crashed game's does) and
copied again every 5 s while the file grows, until its trailer is there (or 15 minutes without a
change). The copy is a replay like any other, in the Replays list too.

**Replays > Highlights**: the sidebar's **Manual Clips** (between L+R+Z Highlights and Favorites;
hovering it: "Click Clip on the seek bar of a replay or a spectated game, then Save as
highlight.") lists them newest saved first; All highlights lists them last, under "Manual clips",
and Favorites the starred ones; the player search applies. A row: a crop icon, "Clip (0:20)" and
every player of the game (teams: team vs team), the game's date; it plays, renders (named
`<date> Clip - sarahkitty (Snake) vs t3o7 (Lucas).mp4`) and stars as the others do, and has a
remove button left of its Play button (no undo). A replay's stats page lists its manual clips too, also
when it has no highlight data (a spectated game's copy). My and Opponent Highlights don't list
them.

Tests: `manual_clips.test.ts` (a clip kept once with its players; a spectated game copied up to
its last whole chunk, then whole once it ended); harness
`test_replay.py::test_launcher_comm_file_saves_manual_clips` (clips saved in a comm file
playback, listed in the status file, the same frames once, an empty clip refused, the replay
playing on with no checksum differing).
