# EmissaryRecomp as a second game engine (design)

Status: design 2026-11-10, decisions 2026-11-12; the launcher part is being built on branch `emissary-recomp` (§11), not on main. The user's request (Sarah's board, 2026-11-10): "my goal is to get this brawlrecompiled working as a nice convenient clean ux from the launcher. we can replace this "Play Button Action" that no one uses with just, like, "Launch P+ Dolphin", "Launch P+ Recomp" ... just like project punch, we need the ability to download and set it up automatically, see how much it takes to download, and be able to delete it, and auto update it ... we also need proper handling for if the user doesn't have the brawl iso ... it also needs to work with all of our flows, including opening automatically, navigating to a room, joining and leaving, switching queues, direct invites, our in-game ui plugin injection, etc."

The decisions (Sarah's board, 2026-11-12): "we will probably use https://github.com/kalomaze/EmissaryRecomp/pulls as the repo. i believe he's handling CI/CD and stuff, there's releases ... i'm happy to just have our launcher pull from this repo's CI, rather than compiling it on people's PCs. we can test windows and linux for now ... i think the recomp should be allowed everywhere. all work for this should be done in a separate branch kept up to date with main as this requires extra manual testing before pushing on main. The name of the recomp project is EmissaryRecomp, Add-ons and "Play Project+ with" are all OK."

Scope: the launcher's UX, install/update and the launcher <-> game contract. The game itself, its releases and its rollback are kalomaze's (`kalomaze/EmissaryRecomp`, a fork of JosiahG's Brawl Recompiled; the rollback client is PR #2, `rollback-support`); this design only states what the launcher needs from it. "Brawl Recompiled" below is the upstream program, EmissaryRecomp the release the launcher installs.

## 1. What Brawl Recompiled is, and where it stands (2026-11-10)

- JosiahG's static recompilation of Brawl (`josiahg2502-glitch/BrawlRecompiled`, GPLv3, on WiiCompiled): `main.dol` and the 126 RELs translated to C++, compiled into `engine_usa.dll` (~180 MB, plus a ~180 MB `engine_usa_v2.dll` for CPUs without AVX2). The game's data is read from the player's own extracted disc. Release v0.5.3: Windows and Linux zips (~200 MB each); macOS builds from source but has no release.
- P+ runs from an SD card image or folder; Gecko codes are installed by its own handler from a `.gct` + `gc.txt`. Code a mod replaces runs in a PowerPC interpreter: with P+ that is every fighter module (`REL module ft_marth ... is not the disc's: linking it in place, untranslated`) and ~1,240 code-patched functions.
- kalomaze's branch adds GekkoNet rollback over our gp-v29 region set and our gameplay checksum. Tested here on 2026-11-09 (`reference-brawl-recompiled-eval` memory): three of Sarah's room replays, 19,720 frames, matched Dolphin's recorded checksums with a 3-frame rollback every frame, after a one-line fix to the stage-start hook. As of 2026-11-10 it also plays a **Direct** match against a normal Brawl Online Dolphin (`docs/online-client.txt` there: "1,069 frames and 12 rollbacks with zero desyncs") through a Python "online companion" plus our `mmclient` crate. Ranked, rooms, teams and spectators are not implemented.
- Performance is not there yet: ~21 ms of CPU per P+ frame in WSL (interpreted fighters; `GuestCodePatch::SyncPages` ~1,700 times a frame), vs ~2.8 ms for our Dolphin. Others are on it; the launcher work below does not depend on it, but **nothing ships to players until a P+ match holds 60 fps with rollback on a mid-range PC** (§9 gates).

## 2. Answers in short

| Question | Answer |
|---|---|
| Replace "Play Button Action"? | Yes: **Settings > Game > "Play Project+ with": Dolphin / Brawl Recompiled (Beta)**. "Launch Dolphin" (open Dolphin without a game) moves to a button in Settings > Netplay Dolphin (§4.2). |
| Separate section or combined? | Combined: Settings > Training becomes **Settings > Add-ons**, with one card per add-on (Project Punch, Brawl Recompiled), the same layout and the same state machine (§4.1). |
| Download, size, delete, auto-update | As Project Punch, plus a "Keep up to date" toggle and staged updates that apply only while the game is closed (§5). |
| Central repository, CI? | `kalomaze/EmissaryRecomp`: its GitHub releases are what the launcher downloads (the latest release's `-Windows.zip` / `-Linux.tar.gz`, checked against GitHub's sha256). No fork of ours, nothing compiled on players' PCs (§7). |
| No Brawl ISO? | The recomp is not an alternative to the ISO: it plays the disc's files and needs the player's own copy, like Dolphin. With no valid ISO, its card says so and offers the same ISO picker; the files are extracted from whatever image the launcher already accepts (§6). |
| All Brawl mods in Other Games? | Not at first. Other Games rooms play whole-machine rollback (`MachineSession`), which a recomp can never match. **Brawl Recompiled plays Project+ only (Play's queues, rooms, Direct); Other Games always opens Dolphin**, automatically (§8.4). Mods on the recomp (P+Ex, REX, Minus) would be recomp-only rooms later, and they run mostly interpreted. |
| All our flows | The recomp speaks the **same launcher contract** as the netplay Dolphin: the same `User/Online` files (`user.json`, `join-room.json`, `game-status.json`, `input-delay.json`, `quit-if-idle*.json`, `mods-*.json`), the same SD card with our plugin, the same netplay save. The launcher picks the engine once, at launch; every flow after that is unchanged (§8). Each recomp release declares which modes it can play, so it ships before parity, with Dolphin taking the rest (§8.3). |

## 3. Principles

1. **One engine choice, made at launch.** Everything after a launch (room clicks, invites, queue switches) goes to whichever client is running, through the files it already uses. No mid-session engine switches.
2. **Same contract, not a second launcher path.** The recomp side (companion or in-process) implements the existing `User/Online` file contract and the plugin's PPOM block. The launcher gets a `GameClient` abstraction with two implementations; the flows don't know which one runs.
3. **Never block play.** When the recomp can't take a request (not installed, updating, out of date, mode not supported), Dolphin plays it, and the player is told why in one line. The only prompt is when a running recomp has to close.
4. **Beta behind a flag first.** `ENABLE_RECOMP` (launcher flags) hides all of it until the gates in §9 pass.
5. **Mirror Project Punch.** Same component, same IPC shape (`getState / install / cancel / remove / onState`), same download-continues-with-popup-closed behavior.

## 4. UX

### 4.1 Settings > Add-ons (was Training)

Two cards, the same component (`AddonCard`), one per add-on.

```
ADD-ONS

┌ Project Punch ─────────────────────────────── v1.3.1 · Installed ┐
│ Training mode tools by WispSSBM (lab in Training).               │
│ SD card: …/project-punch/sd.raw  (2.1 GB)        [Open] [Remove] │
└──────────────────────────────────────────────────────────────────┘

┌ Brawl Recompiled  BETA ───────────────────────── v0.6.0 · Ready ┐
│ Brawl as a native PC program instead of Dolphin.                 │
│ Plays Project+ online with Dolphin players. Other Games and        │
│ replays still use Dolphin.                                        │
│                                                                   │
│ Program 248 MB · Game files 6.8 GB (from your disc image)         │
│ [x] Keep up to date automatically                                 │
│ [Play with it ▸ Settings > Game]   [Open folder]   [Remove]       │
└──────────────────────────────────────────────────────────────────┘
```

What the Brawl Recompiled card shows per state (§5): `Not installed` → **Download (101 MB download, 7.1 GB on disk)**; downloading `43 / 101 MB` with Cancel; `Preparing game files 1,204 / 3,869` with Cancel; `Update available: v0.6.1 (101 MB)` → **Update**; `Update ready, applies when the game closes`; `Needs your Brawl disc` → **Choose disc image**; `Not available on this computer` (macOS until it has a release, a CPU without SSE4.2) with no buttons; errors with **Retry**.

The download figure is the real one for this PC: the AVX2 or the v2 engine only, not both (§7.3).

### 4.2 Settings > Game: "Play Project+ with"

Replaces "Play Button Action":

```
Play Project+ with
  (•) Dolphin
  ( ) Brawl Recompiled  BETA      Not installed · Set up
```

- Picking Brawl Recompiled when it is not installed opens its Add-ons card with Download focused (and keeps the choice: when the install finishes, it is selected).
- The status beside it is the card's one-line state: `Ready`, `Updating 43%`, `Needs your Brawl disc`, `Update required` (red).
- Disabled while a game is running ("Close the game to change"), as the ISO setting is.
- The old "Launch Dolphin" choice becomes **Open Dolphin** in Settings > Netplay Dolphin (beside Configure). Existing `launchGameOnPlay = false` users are migrated to "Dolphin" with that button shown once in a toast.

### 4.3 Home

- The Play button stays one button. With the recomp chosen and ready, a small `RECOMP` tag sits under its label; tooltip "Plays with Brawl Recompiled (beta). Change in Settings > Game".
- When a launch falls back to Dolphin (§8.3), a toast: "Brawl Recompiled can't play Ranked yet: playing with Dolphin." Once per mode per launcher run.
- The update badge: while a required update downloads, Play shows the progress as it does for P+ setup (`Updating Brawl Recompiled 43%`), then launches.

### 4.4 In game

- Controllers are set in the game (F10 / Home on a controller), the recomp's own menu. The launcher's Controls entry for the recomp opens nothing extra in v1; it explains "Set up controllers in game with F10".
- GameCube adapter: the recomp must read the adapter's raw values as Dolphin does (today SDL rescales them and a fixed deadzone of 8000/32767 zeroes small tilts). This is a fork change (§7.5), required before beta: the same player must feel the same stick on both engines.

## 5. Add-on state machine (Brawl Recompiled)

State is computed in main (`dolphin/recomp/recomp.ts`, like `project_punch.ts`) and pushed to the renderer with `ipc_recompState`.

| State | Card shows | Events → next state |
|---|---|---|
| `Unavailable` | "Not available on this computer" (macOS until released; CPU without SSE4.2) | none (terminal) |
| `NeedsDisc` | "Needs your Brawl disc" + **Choose disc image** | valid ISO set → `NotInstalled` (no program) or `PreparingFiles` (program installed, files missing/other disc) |
| `NotInstalled` | **Download** (`101 MB download, 7.1 GB on disk`) | Download → `Downloading`; ISO cleared/invalid → `NeedsDisc` |
| `Downloading` | `43 / 101 MB`, Cancel | verified → `PreparingFiles` (or `Ready` if the game files are current); Cancel → previous state, partial file deleted; error → previous state + `error` |
| `PreparingFiles` | `Preparing game files 1,204 / 3,869`, Cancel | done → `Ready`; Cancel → `NotInstalled`-with-program (shows **Prepare game files**); not enough space → same + `error` |
| `Ready` | version, sizes, auto-update toggle, Open folder, Remove | launch → `Running`; newer release seen → `UpdateAvailable`; Remove → `Removing`; ISO with other game data → `PreparingFiles` on next Play; ISO cleared → `NeedsDisc` |
| `Running` | "Open" (Remove and Update disabled: "Close the game first") | exit → `Ready` (or `Staged` → applied → `Ready`) |
| `UpdateAvailable` | `Update available: v0.6.1 (101 MB)` + **Update** | auto-update on, or Update → `Downloading` (into `versions/<new>`) → `Staged` |
| `Staged` | "Update ready, applies when the game closes" | no recomp running → switch `current.json` → `Ready` (old version deleted) |
| `Removing` | spinner | done → `NotInstalled` |

Plus two flags on top of `Ready`:

- `required`: the installed release's `protocol` differs from the launcher's `RECOMP_PROTOCOL` (§7.4). Play with the recomp first downloads the update (progress on Play), then launches. Never plays a mismatched pair.
- `error`: the last job's error, with the phase; Retry restarts that phase only.

Rules:

- **Downloading** survives the popup and the page closing (as Project Punch); the job is in main. Cancel deletes the partial file; nothing half-installed stays.
- **PreparingFiles** extracts the game files from the Settings ISO with the bundled `DolphinTool extract` (every format the launcher accepts, §6). It's a separate phase so a new disc (or a removed one) only redoes this part. It checks free space first (`needs 6.8 GB on <drive>, 2.1 GB free`).
- **Auto-update** (default on): checked at launcher start and every 6 h. A newer release downloads in the background into `versions/<ver>/`; when it's verified it is **Staged**, and the switch (one `current.json` write) happens when no recomp is running. With the toggle off, the card shows **Update** and the update happens only when `required`.
- **Remove** (game closed): deletes the program, the extracted game files and its user data except the saves folder (kept in the Add-ons folder, mentioned in the confirm: "Your Brawl Recompiled settings are deleted. Replays stay.").
- `Running` comes from the client manager (§8.1); Remove and Staged→Ready wait for it.

On disk (`<netplay>/brawl-recompiled/`, in `PROTECTED_ENTRIES` (`install/bundled_dolphin.ts`) like `project-punch` so it survives launcher updates and moves with the game files):

```
brawl-recompiled/
  current.json            {version, engine: "avx2"|"v2", installedAt, discGameData}
  versions/0.6.0/         the release (engine_usa.dll or _v2, loader exe, DLLs)
  game/DATA/              the extracted disc (sys/, files/)
  user/                   the recomp's UserData: Config.toml, Logs, Cache (no NAND, no SD: §8.2)
```

## 6. The disc

- The launcher already has the player's ISO (Settings > Game), verified by game data (`docs/design/disc-images.md`): NTSC-U Rev 1 or Rev 2, ISO / RVZ / WBFS / scrubbed. The recomp needs the files extracted: `DolphinTool extract -i <iso> -o <brawl-recompiled>/game` (we ship DolphinTool). Rev 1 and Rev 2 have the same game files and their `main.dol` differs in one word; the recomp runs its own translated `main.dol`, so **both revisions work** (to verify: the recomp's boot reads the disc header's revision).
- `current.json.discGameData` remembers which disc the files came from. A changed ISO setting with the same `game_data` changes nothing; a different one re-runs PreparingFiles.
- **No ISO, or an invalid one:** state `NeedsDisc`. The card says "Brawl Recompiled plays the files of your own Brawl disc. Choose your disc image in Settings > Game first." with a **Choose disc image** button (the same picker). The quick start never asks about the recomp.
- Later, to save the 6.8 GB: a DVD backend in the runtime that reads the image directly (Wii partition decryption per 0x8000 cluster; ISO and WBFS easy, RVZ via Dolphin's code). Not v1.

## 7. EmissaryRecomp's releases

### 7.1 Where they come from

`https://api.github.com/repos/kalomaze/EmissaryRecomp/releases/latest`, as Project Punch's (`PPO_EMISSARY_TAG` installs another tag, to test updates). v0.6.0 (2026-11-12) has `BrawlRecompiled-v0.6.0-Windows.zip` (208 MB: the upstream launcher, `engine/brawl-engine.exe`, `engine_usa.dll` and `engine_usa_v2.dll` for CPUs without AVX2, the loader picks one) and `BrawlRecompiled-v0.6.0-Linux.tar.gz` (118 MB; glibc 2.38+, e.g. Ubuntu 24.04), each with a sha256 digest. macOS has no release yet: the add-on is `unavailable` there.

v0.6.0 has no rollback yet (that is PR #2), and no release has the live online companion (§8.2).

### 7.2 What the launcher does with one

Unpacks it into `versions/<tag>/`, without the upstream launcher (`Brawl Recompiled[.exe]`: the game's own update check only works beside it, so the launcher stays the only updater) and without `engine/portable.txt` and `engine/UserData`. The runtime looks for `portable.txt` up to 4 folders above its program, so `<emissary-recomp>/portable.txt` makes `<emissary-recomp>/UserData` its settings, saves and logs, kept across versions.

### 7.3 What would help from the releases

- A small `manifest.json` asset: the protocol version (§7.4), the modes the release can play online (§8.3), and the least launcher version. Until then the launcher can only compare tags.
- One engine per download (AVX2 / v2), which halves it; later binary diffs between versions.

### 7.4 Compatibility

The recomp and Dolphin must simulate identically and speak the same protocol, so the launcher carries `RECOMP_PROTOCOL` (bumped whenever the plugin protocol, the gameplay region set, the setup sync or the checksum changes on the Dolphin side). A recomp whose manifest `protocol` differs is `required` (§5). mm learns the engine too: the hello, the ticket and the game report carry `engine: "dolphin" | "emissary"` and the engine's version (built 2026-11-19: brawl-online-core.md §10, "Which engine"), so desync and speed numbers can be split by engine (`pp-admin speed`), and Ranked could refuse it if its numbers were bad (it is allowed: §10, D3).

### 7.5 What the launcher needs from EmissaryRecomp

Done already (2026-11-12): the read-only SD card (`BRAWL_SD_VOLATILE_WRITES=1` in PR #2: the image is opened read-only, the game's writes stay in memory); the stage-start hook (fixed differently in PR #2); the launcher's `join-room.json` / `game-status.json` (kalomaze's companion speaks them, Direct and Unranked only). The launcher sets up the rest itself (`dolphin/emissary/launch.ts`): the card as `UserData/sd.raw` (a hard link), `NETPLAY.GCT` + `gc.txt` from the card as `UserData/Codes/`, the netplay save as `UserData/NAND`, `dvd_root` and the 16:9 mode in `Config.toml`.

Still needed:

1. **The online companion as part of the release, native** (§8.2): today it is Python (`tools/online` on `rollback-dolphin-crossplay`) plus our `mmclient`, which the launcher can't ship. Until then Play with EmissaryRecomp starts the game without online play.
2. **Raw GameCube adapter input** (§4.4).
3. The modes past Direct and Unranked (§8.5): rooms, 2v2, Ranked, being spectated.
4. `.brep` recording of its games (same format), so replays, highlights, stats and Ranked reports work; playback stays on the playback Dolphin.

## 8. Flows

### 8.1 The client manager

`dolphin/manager.ts` today has instance kinds `netplay` and `playback`. Add `recomp` as a second **online client** kind, behind one interface:

```ts
interface OnlineClient {
  kind: "dolphin" | "recomp";
  launch(opts: LaunchOptions): Promise<void>;   // spawn; resolves when the process runs
  onlineDir: string;                             // where join-room.json / game-status.json live
  running(): boolean;
  close(): Promise<void>;
  onExit(cb): void;                              // crash logs: the recomp writes UserData/Logs/<run>/console.log
}
```

At most one online client runs. `netplayOpened` in the store becomes `onlineClient: {kind, pid} | null`; the existing "game running" guards keep working.

### 8.2 What the recomp gets at launch (same as the netplay Dolphin)

| Dolphin today | Recomp |
|---|---|
| `-u <netplay>/User`, `User/Online/user.json` (account) | the same `User/Online` folder as its online dir (the account, the handoff files) |
| `pponline-sd/sd.raw` (P+ card + `PPOnline.rel`), read-only | the same image, read-only, codes from its `NETPLAY.GCT` |
| `Project+ Netplay Launcher.dol` boot | not needed: the runtime installs the codes itself |
| `NetplaySave` copied into the Wii NAND at boot | the same template copied into `user/NAND` at each launch (the save decides unlocks and rules, which the setup key compares) |
| `-C` overrides: mm/accounts URLs, music, input delay profile, potato mode, SYSCONF | the config file (§7.5.1): URLs, music, input delay profile; potato mode maps to resolution 1x + no interpolation |
| `game-status.json` written by Dolphin | written by the recomp side, same schema |

Whether the recomp side is in-process C++ (Dolphin's `Online/` sources built into the runtime against a small host interface: guest memory, frame hooks, the rollback session) or one native companion process (kalomaze's current shape, rewritten off Python), the launcher sees the same files. **Recommendation to the engine people:** share Dolphin's `Online/` code rather than re-implement it (`dolphin/Source/Core/Core/Online/*.cpp` is ~18,000 lines: GameBridge, Rooms, Matchmaking, GameSetup, Chat, RoomMessages, Ranked, Spectate, RoomWatch, SpeedCheck...; a second implementation would drift with every feature we add).

### 8.3 Choosing the engine for a launch

```
effectiveEngine(request):
  if request is Other Games room            -> dolphin              (§8.4)
  if setting != recomp                      -> dolphin
  if recomp state not Ready                 -> dolphin + toast "Brawl Recompiled is <state>: playing with Dolphin"
  if recomp.required                        -> update first (progress on Play), then recomp; on failure dolphin + toast
  if request.mode not in manifest.modes     -> dolphin + toast "...can't play <mode> yet"
  else                                      -> recomp
```

`request.mode` is what the launch is for: `menus` (Play), `room`, `create-room`, `direct`, `queue:unranked|ranked|twos`, `watch`. Play from Home opens the menus, where the player may go anywhere; there the plugin's own menus grey out modes the running engine can't play (a `Local` flag the recomp side sets from its manifest), so a recomp in the menus never tries Ranked before it's supported.

### 8.4 When a client is already running

| Running | Request | What happens |
|---|---|---|
| none | anything | launch `effectiveEngine`, then hand the request off as today (`room_handoff.ts`: write `join-room.json`, wait for `game-status.json`) |
| recomp | P+ room / Direct / queue it supports | hand off as today |
| recomp | Other Games room, or a P+ mode it doesn't support | dialog: "This needs Dolphin. Close Brawl Recompiled and open it in Dolphin?" [Open in Dolphin] [Cancel] (refused while `busy`, as today's busy rule) |
| dolphin | anything P+ | hand off to Dolphin as today (the engine setting applies from the next launch) |
| dolphin (Other Games game) | P+ request | today's behavior (unchanged) |

### 8.5 Flow checklist (what the recomp side must do, per flow)

Each item is a row in the recomp's `modes`/features list and a test in its CI or the harness:

1. Boot straight to the menus (the plugin's title skip), `game-status.json` within the launcher's timeout. Measure cold boot (the runtime reads ~1 GB of disc files into a cache: 16 s cold here).
2. `join-room.json`: room join, create (with ruleset, public/private), Direct (`connectCode`), queue (`unranked`/`ranked`/`twos`); `lastRequest` answers; `busy` refusals.
3. Rooms: slots, ready, teams, rulesets and stock control, sit out, open slots, room watch (watching the room's game inside the game), leave, host changes, room chat.
4. Queues: Unranked, Ranked (ping check popup, match quality abort, lone reports, set flow, character-first sets), 2v2 (party, pair room), switching queues from the launcher while searching.
5. Direct invites from the launcher and from the in-game keypad.
6. Live input delay (`input-delay.json`), quit-if-idle, the pause quit/runback rules per mode, desync recovery (restart from last agreed stocks).
7. Being spectated (Ranked always streams inputs to mm; the Spectate tab's viewers use the playback Dolphin) and `.brep` recording.
8. Speed check reports with `engine` (§7.4); crash logs collected into the launcher's bug report zip.
9. Chat key, name tags over fighters, music toggle, Wii Remotes (port rules of bug #89).

## 9. Plan

| Phase | What | Who | Done when |
|---|---|---|---|
| 0 | Decisions D1-D5 (§10). | Sarah | answered |
| 1 | Fork repo, merge kalomaze's branch, CI for Windows + Linux producing the §7.3 artifacts and manifest; encrypted inputs; the private replay gate (the 3 replays from 2026-11-09 + a corpus of recent prod replays from players who consent, or Sarah's own). | us + kalomaze | a tagged release builds from CI and passes the gate |
| 2 | Fork changes §7.5 1, 2, 4, 5 (launch config, read-only card, raw adapter, pinned state). | us | the recomp boots P+ online from launcher-made config with no manual files |
| 3 | Launcher add-on: `recomp.ts` state machine, download/verify/extract/update/remove, Settings > Add-ons card, Settings > Game choice, Home tag, migration of Play Button Action. Behind `ENABLE_RECOMP`. Storybook stories for every card state. | us | install → play menus → remove works on Win + Linux; Storybook reviewed |
| 4 | Client manager `OnlineClient`, `effectiveEngine`, the §8.4 table, the plugin's mode greying flag. | us | every row of §8.4 has a test |
| 5 | Online parity, mode by mode (Direct → Unranked → rooms → 2v2 → Ranked), each mode added to `modes` when its §8.5 items pass against a prod-version Dolphin in staging (`tools/staging/staging.py`). mm gets `engine` in tickets and reports. | engine people + us | per mode |
| 6 | Closed beta: flag on for testers; desync and speed split by engine in `pp-admin speed`. Gate to open beta: a P+ match with rollback holds 60 fps on a mid-range PC; desync rate not above Dolphin's. | Sarah decides | numbers in |
| 7 | Open beta (flag on for all, setting default Dolphin); macOS release; direct ISO reading (§6). | | |

Estimates for phases 3-4 (the launcher part): about a week of focused work; it can start before the engine is fast, against today's recomp.

## 10. Decisions for Sarah

All answered on 2026-11-12:

- D1: the launcher downloads kalomaze's releases (his CI); nothing is built on players' PCs.
- D2: Windows and Linux now; macOS later (it is being worked on upstream).
- D3: **the recomp is allowed everywhere**, Ranked included. mm still learns the engine (§7.4) so desyncs and speed can be split by engine.
- D4: "EmissaryRecomp" (with a BETA tag), "Add-ons", "Play Project+ with".
- D5: no repo ownership for now; kalomaze would hand it over if needed.

The questions as they were asked:

- **D1, distributing the engine.** The engine DLL contains Brawl's code, translated. Upstream ships it the same way (that's how v0.5.3 is distributed), but it is a different legal exposure from shipping Dolphin and a plugin. Options: (a) ship it built by our CI, as upstream does; (b) never ship it: the launcher compiles it on the player's PC from their disc (needs a ~1.5 GB toolchain download and 20-60 minutes of CPU with 16 GB of RAM, once per update); (c) download upstream's own release files and only add our pieces (not possible while the runtime and the game code are one DLL). Recommendation: (a), with the repo itself free of game code, and revisit if anyone objects.
- **D2, platforms.** Windows + Linux first (both have releases upstream), macOS (arm64) after. OK?
- **D3, Ranked.** Let recomp players into Ranked from day one of beta, or keep Ranked on Dolphin until the desync numbers by engine are in? Recommendation: keep Ranked on Dolphin until then.
- **D4, names.** "Brawl Recompiled (Beta)" in the UI, "Add-ons" for the settings section, "Play Project+ with" for the choice.
- **D5, ownership.** The fork under `sarahkittyy/`; kalomaze (and whoever else) as collaborators on it, with the online side built on shared `Online/` code (§8.2) rather than the Python companion. That is their call as much as ours; this design only needs the §8.2 contract.

## 11. Built so far (branch `emissary-recomp`)

Launcher, 2026-11-12:

- `dolphin/emissary/emissary.ts`: the add-on (§5): the latest release for this platform, download with progress and sha256, unpack into `versions/<tag>`, the disc's files with `DolphinTool extract -g` (progress from its `Extracting: ... | N%` lines; about 4 minutes, 6.8 GB), staged updates while the game runs, Cancel, Remove. `emissary.test.ts` (13 tests).
- `dolphin/emissary/launch.ts`: the launch set-up of §7.5; `manager.ts` `_launchEmissary`: Play starts it as the netplay instance when Settings > Game says so and it is ready, else Dolphin plays (logged).
- `dolphin/emissary/setup.ts`: IPC, the Settings disc's check, the auto-update (a minute after start, then every 6 hours; only once installed).
- Settings > Add-ons (`addons_settings/`): Project Punch's card and EmissaryRecomp's; Settings > Game: "Play Project+ with" (`play_project_plus_with_setting.tsx`), which replaced Play Button Action (whoever had Launch Dolphin gets the game on Play again; Configure Dolphin opens Dolphin without one). Stories: `lib/EmissaryRecomp`.
- `emissary-recomp` is a protected folder of the netplay install (launcher updates keep it, a game files move takes it along).

Not yet (then): the online companion (§7.5), the mode greying of §8.3, the §8.4 dialog, the Home tag and toasts (§4.3). §12 replaces the companion, the dialog and the fallback toast.

## 12. Playing with it for real (branch `emissary-launcher`, 2026-11-19)

Sarah, 2026-11-19: "other games and mods stay on dolphin. that's fine. we should close the recomp to play other games rooms" and "it should be versioned so new releases of the recomp get updated when people try to play with them ... so long as it works the same way (doesn't cancel a ranked set's game 2 or 3, doesn't interrupt a game in progress)". The companion is gone: the recomp runs Brawl Online's core in-process (docs/design/brawl-online-core.md §10).

**Launch** (`emissary/launch.ts`, `manager._launchEmissary`): `BRAWL_ONLINE_CORE=<netplay User folder>` (the core reads Config/*.ini, Online/ with the account and the hand-off files, writes Logs/dolphin.log and replays, as Dolphin with `-u`) and `BRAWL_SD_VOLATILE_WRITES=1`, plus the UserData set-up of §11. The core can't be given Dolphin's `-C` overrides, so the launcher writes them into those ini files before each start: the logging every Play gives Dolphin (`LOGGING_CONFIG_ARGS` into Logger.ini; without it the core logs nothing), and in test mode `PPO_DOLPHIN_EXTRA_ARGS` (the staging servers) into Dolphin.ini. The recomp is started with its output ignored (it keeps its own `UserData/Logs/<run>/console.log`). A release plays online when its program reads both `BRAWL_SD_VOLATILE_WRITES` and `BRAWL_ONLINE_CORE` (scanned at install; v0.6.0 and the companion's `BRAWL_ONLINE_IPC` builds do not).

**Releases** (what the launcher expects from EmissaryRecomp's CI): assets `EmissaryRecomp-<tag>-Windows.zip` / `-Linux.tar.gz` holding `WiiCompiled[.exe]` with its DLLs, `Sys/` and `wii_bootstrap/` (at the top or in one top folder; a `portable.txt` or `UserData/` in it is dropped: ours are two folders up), and `manifest.json` `{version, brawlOnline, protocol}`. `protocol` must equal the launcher's `EMISSARY_PROTOCOL` (1; bumped when what a release needs from the launcher's launch changes), else the release doesn't play (and a newer one for another protocol doesn't replace one that plays). `brawlOnline` is shown and logged; holding a recomp built from an older core to an update is mm's (the engine card). A release without a manifest plays if its program has both switches. `PPO_EMISSARY_RELEASES=<dir>` (test mode or unpackaged) serves releases from `<dir>/<tag>/` instead of GitHub.

**Updates, the way the launcher's own gate works** (app_updater.ts: no Dolphin starts while a newer launcher is out; a running one is left alone): Play asks for the latest release again when the last answer is older than a minute (5 s at most; offline, the installed release plays), switches to a release downloaded while the game ran, and downloads a newer one before the game starts, its progress on the Play button ("Updating"). If that download fails, Dolphin plays. While the recomp runs nothing stops it: the auto-update (every 30 minutes while Play plays with it, else 6 hours) downloads a newer release beside it (staged), and the next room, Direct or queue click closes the recomp only when its game is idle, then starts the new one with that click (`closeEmissaryForUpdate`). A set between its games is a session (`busy`), so Ranked games 2 and 3 are never cut.

**Idle** for the recomp (`emissary/close.ts`) is its game-status.json (written by the core in it, fresh, with its pid) saying `idle`: the menus, an idle online character select, a room between games. Dolphin answers quit-if-idle from its main window, which the recomp doesn't have; `closeNetplayIfIdle` answers false at once for it, so Project+ clicks go to the running recomp. Closing asks it as its window's close button does (WM_CLOSE through `taskkill` without /F; SIGTERM elsewhere), so the core leaves its room and logs out; it is ended after 15 s.

**Other Games, Project Punch** (`closeNetplayForOtherGame`, used by the mods manager and Project Punch): an idle recomp closes and Dolphin opens the room; one in a game stays and the click is refused as with a Dolphin in a game. This replaces §8.4's dialog. Quitting the launcher in its Launch Project+ mode closes the recomp too.

**Falling back to Dolphin is never silent** (§4.3): Play's result carries why (`emissaryFallback`: its state, a release that can't play online, or the update's error) and Home shows a warning toast. Play refuses a missing or invalid disc before the recomp comes into it (Dolphin couldn't play either), so the add-on's "needs your disc" only means its disc check hasn't come back yet: Play waits for that check instead of falling back. Its text reuses the card's strings (`noOnline`, `statusText`) with one new one, `usingDolphin`: "EmissaryRecomp: {why} Using Dolphin."

**Logs:** the bug report zip (and Download logs) has `emissary-recomp/Logs/<run>/` of its last 3 runs, its Config.toml, and the core's crash dumps `recomp-<pid>.dmp` from the netplay User folder's Logs (the newest 3, whole, only when they fit the report's per-file cap; they are no longer cut to their end with the netplay Dolphin's logs).

Run for real (Windows, dev launcher from this branch against local staging, a test profile on E:, releases from `PPO_EMISSARY_RELEASES` packed from the recomp build at 771c27b): Play installed v0.7.1 over v0.7.0 before starting it; the recomp booted to P+'s online menus logged in (mm: `online code=ACOR#102`); Create room (room JRTZ), a Direct (character select vs PEBB#101) and Play unranked reached it; with v0.7.2 staged while it ran, Play unranked closed it (idle, exit 0) and started v0.7.2 with the queue request; hosting an Other Games room closed it and Dolphin hosted the room; with its game files missing Play used Dolphin with the toast; quitting the launcher closed it. Not run: Linux, a recomp in a match or a set when a click comes (unit tests only), a real GitHub release, macOS.

## 13. UX pass (2026-11-24)

Sarah's board, after the UX pass of the whole flow: the Add-ons card in her words, a switch on it that mirrors "Play Project+ with", the way to it glowing once, set-up progress where P+'s first set-up shows, and Project Punch's popup when Play needs it first.

**Add-ons card** (`emissary_setting_view.tsx`): her description ("Play P+ as a native PC program instead of Dolphin. Uses Brawl Online's netcode, you can cross-play with Dolphin players. "Other Games" rooms still use Dolphin. Made by kalomaze"), a switch labelled **Emissary** (on) / **Dolphin** (off) that is Settings > Game's "Play Project+ with" (disabled while the game is open, as the radio is), Installed and Latest (no Status row: a line only for needs disc / needs files / unavailable / a release without online play), no Program or Game files rows, the download button with both sizes, and "Keep up to date automatically" without its description. "Play with it in Settings > Game" is gone. Remove still sets Play back to Dolphin (setup.ts).

**Settings > Game**: "EmissaryRecomp is a recompiled version of SSBB for native compatibility. Experimental. "Other Games" still use Dolphin." ("Until it is set up, Play uses Dolphin" and the replays exclusion are gone.)

**Play before it is set up** (`use_play.ts`, `emissaryNeedsSetUp`): with Play Project+ with = EmissaryRecomp and nothing installed (or its game files missing or being extracted), Play (and a room, Direct or queue hand-off, which go through it) opens EmissaryRecomp's popup (`emissary_dialog.tsx`): Project Punch's popup, the same layout and words ("You do not have it downloaded.", Download, the progress with Cancel, then Play vN). An installed release downloading its update is not this: main waits for it as before (§12). Other fallbacks (a release without online play, an update that failed) keep the toast.

**Progress** (`sidebar.tsx` `EmissarySetupStatus`): while Play is set to it, its download or extraction shows in the sidebar's set-up slot, as P+'s first set-up does: "Downloading EmissaryRecomp N%" by the bytes that came over the network (unpacking stays full), then "Extracting game files N%". The card and the popup show the same job.

**The way there glows once** (`use_emissary_hint.ts`, localStorage `EMISSARY_ADDONS_SEEN`): until Settings > Add-ons has been opened on this computer, the sidebar's Settings button (or the folded tools' menu button), then Settings' Add-ons entry, then the card (on that first visit) glow in the profile hint's gold; a new install or an update to this launcher starts with it unseen. Not on a computer without releases (macOS).

**In the recomp** (EmissaryRecomp `brawlonline/ux`): in the build with Brawl Online's core, F10's options menu has Resume, Graphics, Audio, Controls and Quit Game only: the recomp's own Online, Gameplay (fast-forward, fast menus, save states), Cheats, Mods and System (its updater and restart) are left out, and nothing restarts the program in game (a restart was a new process the launcher didn't know, started without the core's stop). The game always runs at its own speed there (fast menus off). The title card never says "Press Enter to start!" (no press was needed or read: it lifts by itself once the boot shaders are ready); while they compile it says "Compiling shaders... N to go". The game always boots widescreen with the core (SCGetAspectRatio = 1): F10's "Original 4:3" is the plugin's picture shape as on Dolphin, and a 4:3 aspect_mode saved in game can no longer boot the game's Wii aspect at 4:3 (the screen KO desync of 18e14c7a); the "after a restart" note is gone with it. ImGui's display-scale crash fix (e84faa8 from `brawlonline/modes`) is in.

**Any screen shape, for real** (Sarah, 2026-11-24: "i'd fix the bug, instead of limiting to 16:9. obviously the game camera needs to stay 16:9"). A 4:3 picture squeezed P+'s menus into ovals, and so did every shape but 16:9, in matches too (Fill Window at 21:9 stretched them). Two breaks, both fixed: the core's standalone compat had no presenter (`core/compat/src/Compat.cpp` `g_presenter` was null), so GameBridge's PutAspect always told the plugin 0 and it drew Brawl's own 16:9; and the recomp's GX HLE sent GXSetProjection to Aurora at once and kept nothing in the guest's GX state, where the plugin changes the projection (the SDK sends that state with the next primitive), so the change never reached the picture. The recomp (`brawlonline/ux` 06ab751) now keeps the projection and the viewport in the GX state as the SDK does and sends the GX state's projection to Aurora before each draw when it changed there. The cameras stay 16:9 on every machine; only the six numbers in the GX state differ, as on Dolphin. Checked with replay 004.brep through the core's playback at 4:3 and at 21:9 Fill: every frame's checksum matches (6,558 each), the stage drawn in proportion with more above and below or at the sides, the ONLINE menu's buttons round. Screenshots: `run/artifacts/emissary-ux/2*.png`. A release needs the core lock moved past this commit (brawl_online.lock). The card in E:\emissary\live has a pre-v12 plugin (LOCAL 0xF4, no shape field): tests of this need the current PPOnline.rel on the card.

## 14. Video settings (board card 774; recomp `brawlonline/video-settings`, 2026-11-24)

Sarah picked these from Melee Unlocked's Video page (github.com/Hero88go/melee-unlocked, GPL-3: the ideas, none of its code): anti-aliasing and anisotropic filtering, quality presets with defaults from measurements ("DEFAULTS MATTER MOST": a first-time player changes nothing), V-Sync that turns itself off on a G-Sync/FreeSync screen, and sharpening/brightness/contrast/vibrance in the present pass. All of it is display only: nothing reaches the game's memory, so players' choices can't desync. Checked: recomp-vs-recomp Direct sets (two switching resolution, MSAA, anisotropy and bloom live every 7-9 s on both sides, one on the first-start defaults; 34,855 checksums) and a crossplay set against Dolphin with the recomp at 4K, 8x MSAA, 16x anisotropic, sharpening 60%, brightness +10, vibrance +20 (10,047 checksums), 0 mismatches.

**Options (F10) > Graphics**, in order: Display, **Quality** (LOW / MEDIUM / HIGH / ULTRA, CUSTOM once a row differs), Resolution, **Anti-aliasing** (OFF / 2X / 4X / 8X MSAA), **Anisotropic filtering** (1X-16X), Aspect ratio, Frame rate, **V-Sync** (AUTO: ON / AUTO: OFF / ON / OFF), Bloom, **Sharpening** (0-100%), **Brightness**, **Contrast**, **Vibrance** (-50..+50), then the rest as before. Every row applies at once (MSAA at the next frame boundary). New text, for Sarah's review: Quality's hint "Sets resolution, anti-aliasing, texture filtering and bloom together."; V-Sync's hint gains "Auto: off on a G-Sync or FreeSync screen."; the anti-aliasing and anisotropic hints are Dolphin's own; the picture rows reuse the volume rows' "Left and right change it."

| Preset | Resolution | MSAA | Anisotropic | Bloom | GPU ms/frame, RTX 4070 |
|---|---|---|---|---|---|
| Low | 1x (480p) | off | 1x | off | 0.32 |
| Medium | 1.5x (720p) | off | 4x | on | 0.37 |
| High | 2.25x (1080p) | 4x | 4x | on | 0.61 |
| Ultra | 4.5x (4K) | 4x | 16x | on | 2.55 |

Every preset keeps 60 FPS (the in-between frames are experimental and cost a frame; card 771's Predict mode may change that). **First start** (no resolution, MSAA or anisotropy in Config.toml, as the launcher's Config.toml has none): Low on an integrated GPU or one with under 3 GiB of its own memory, else High. Aurora reads the device's type and memory from Vulkan itself (SDL_GPU passes neither on), so `GraphicsInfo::Integrated()` (and the 1080p cap for integrated GPUs of 2026-11) now works on Windows and Linux too, not only Apple silicon.

**Measurements** (Windows' per-process "GPU Engine" 3D utilization during a recomp-vs-recomp set, each setting 9 s; the GPU's own time per 60 Hz frame, for the game picture): 480p 0.32 ms, 720p 0.37, 1080p 0.47, 1080p + 2x/4x/8x MSAA 0.59/0.61/0.94, 1080p + 16x anisotropic 0.47 (free), 1440p + 4x 1.08, 4K 1.38, 4K + 2x/4x/8x 1.98/2.55/~4.0. Most of the low end is fixed work (copies, bloom passes), not pixels. No weak GPU could be measured: the 9800X3D's integrated Radeon is off in the BIOS (Windows lists only the RTX 4070). An integrated GPU has roughly 1/6 (Radeon 780M) to 1/40 (older Intel UHD) of the 4070's bandwidth and compute, so Low (~2-13 ms) fits everywhere, High (~4-24 ms) not on the weakest: hence Low for integrated.

**Anti-aliasing:** aurora already drew the EFB multisampled and resolved it at the end of every pass (before every EFB copy and the display copy); only the setting was missing. A change rebuilds the render targets at the next frame boundary (as a window resize does) and queues the cached pipelines again at the new sample count (pipeline_cache.bin records keep the count they were drawn with; the boot prewarm also queues them at the current count). Two things can't work multisampled under SDL_GPU (it can neither sample nor resolve a multisampled depth target): an EFB depth copy keeps its last contents (logged; it used to end the game) and depth peeks return nothing (logged). Neither showed up in Brawl across the test sets' stages.

**Anisotropic filtering** works like Dolphin's enhancement: every mipmapped, linearly filtered texture gets it (with linear mip blending), whatever the game asked for (before, aurora's fixed 16x applied only where the game itself asked for anisotropy).

**V-Sync, AUTO** (a fresh Config.toml says `vsync = "auto"`; `true`/`false` are the player's own choice): on, except while the screen follows the frames and the frame rate stays under 96% of its refresh. Detection (runtime/src/display_refresh.cpp, Windows): a thread samples D3DKMTGetScanLine for the window's screen ~300 times a second; a fixed-refresh screen is in its vertical blank ~5-14% of the time (the LG windowed), one that follows 60 frames at 144 Hz ~60% (the AORUS borderless with FreeSync: 55-66%; the LG read 72-73% while another program drove it fullscreen with G-Sync); two one-second windows in a row decide. A windowed game is composed by DWM and can't tear either way. Screens under 75 Hz stay "unknown" (V-Sync stays on). With V-Sync on, the present mode is now **Mailbox** when the swapchain has it (else Fifo, as before): no tearing, and a frame waiting for the screen is replaced by a newer one instead of queueing behind it. Measured present-to-screen (PresentMon, replay 001): windowed on the LG at 240 Hz, Fifo and off are the same (DWM composes both; p50 ~7 ms on a quiet desktop); borderless on the AORUS (independent flip, FreeSync) p50/p95: Fifo 1.86/3.23 ms, Mailbox 1.58/3.24, off 1.34/4.24, auto (off once detected) 1.10/2.41. A fixed-refresh fullscreen screen, where Fifo's queue costs most, was not available here (both fullscreen-capable screens run G-Sync).

**Picture adjustments** (aurora `draw_present_picture`): the game picture's draw into the window (not the menus over it) goes through a second pipeline while any adjustment is set: contrast-adaptive sharpening over the four neighbours on the coarser of the picture's and the window's pixel grids, then brightness, contrast and vibrance (saturation that lifts dull colours more) in the game's gamma space. Neutral settings use the plain copy.

**Config.toml `[video]`:** `msaa` (1/2/4/8), `anisotropy` (1/2/4/8/16), `sharpening` (0-100), `brightness`/`contrast`/`vibrance` (-50..50), `vsync` (`"auto"`, `true`, `false`). **Test hooks:** `BRAWL_VIDEO_SWEEP="scale:msaa:aniso:bloom,..."` (+ `BRAWL_VIDEO_SWEEP_SECONDS`) steps through settings over the matches, logging QPC and pid; `BRAWL_OPTIONS_TAB=1:9` opens a tab with a row picked; `AURORA_CAPTURE_PRESENTED=1` makes shots.txt captures the presented picture; `AURORA_GFX_FLAGS=fifo` keeps Fifo for V-Sync; `BRAWL_LOG_VRR=1` logs every detector window. Screenshots and before/after crops: `run/artifacts/emissary-video` (local, ignored).

## 15. Dolphin's controls (2026-11-24; recomp `brawlonline/dolphin-controls`)

A player on Discord: the recomp's Controls page "is pretty limited and I can't put my layout like I have on dolphin. For example letting me add multiple buttons or combinations to a stick axis ... I use this for diagonal cstick while playing on keyboard." Sarah: "most people have their controls and are familiar with dolphin. dolphin's ui is probably difficult to repeat for every wii controller ever. there should be an option to open the dolphin controls menu and adjust it while you're already in the recomp."

**What the recomp plays.** With Brawl Online's core on, the recomp reads the netplay User folder's `Config/GCPadNew.ini` (the file Dolphin's Controllers window writes) and Dolphin.ini's `[Core] SIDevice0-3`, per GameCube port:

| Dolphin.ini SIDevice | The recomp's port |
|---|---|
| 6, standard controller | Dolphin's GCPad for that port: any device line, every expression (`|`, `&`, `!`, functions, backticked names), sticks with modifier and range, dead zones, calibration and gate, mixed triggers and their threshold, Always Connected, Rumble/Motor |
| 12, GameCube adapter (Dolphin's default) | the recomp's own reading, as before (the adapter reaches it through SDL) |
| anything else (None, ...) | no controller |

Without a GCPadNew.ini (Dolphin never saved one) the recomp keeps its own controls. Real Wii Remotes are still the recomp's own (WiimoteNew.ini's emulated Wii Remotes are not read). The input gate is Dolphin's: the controls count while the game window has focus, or always with `[Input] BackgroundInput = True`.

**How: Dolphin's own code, not a copy of its behaviour.** core/standalone builds `brawl_online_dolphin_input` (`BrawlOnline/DolphinPads.h`, `core/compat/input/`) from Dolphin's sources as they are: Core/HW/GCPadEmu.cpp (P+'s GCPad, its stick ranges too), InputCommon's ControlReference (the expression parser), ControllerEmu (sticks, buttons, mixed triggers, numeric settings), ControllerInterface (devices, ids, the devices-changed hook), and on Windows its DInput (keyboard and mouse, joysticks), XInput and force feedback. Only what Dolphin does around them is new: InputConfig::LoadConfig's read (no per-game profiles), Core::UpdateInputGate's gate, a reload when GCPadNew.ini or Dolphin.ini change. SDL gamepads are Dolphin's SDL Gamepad class (SDLGamepad.cpp, the same SDL 3) over the recomp's SDL: Dolphin's SDL backend runs SDL on its own thread, which would take the recomp's events, so `SyncSdlDevices` compares SDL's joystick list instead (twice a second). Stand-ins for what core/standalone leaves out: Common/HRWrap.h without C++/WinRT, WGInput (none: llvm-mingw has no C++/WinRT, so `WGInput/` device lines find nothing), the DSU client (off), WiimoteReal, Core/Host.h's two focus questions. MinGW's xinput.h lacks XINPUT_DLL (set to xinput1_4.dll). The recomp's sparse fetch (tools/fetch_brawl_online.py) now takes `dolphin/Source/Core/InputCommon/` and Core/HW/GCPadEmu.*; a core from before this has no library and the recomp keeps its own controls.

**In the recomp:** the runtime's pad read (hle/input/pad.cpp ReadLocalPads, after aurora's PADRead and the runtime's bindings) passes the four ports to `BrawlOnlineCore::ApplyDolphinControls`; the core side (`brawl_online/brawl_online_dolphin_controls.cpp`) reads Dolphin's devices at most every 4 ms. The game's motor command goes to the port's Rumble/Motor (GCPad::SetOutput) for a port Dolphin drives. The log says `[brawl-online] Dolphin's controls: on (Config/GCPadNew.ini)` and each port's SIDevice and device line.

**Dolphin's window over the game.** The launcher passes `BRAWL_ONLINE_DOLPHIN` (the netplay Dolphin's executable) with the recomp's start (manager.ts `_launchEmissary`). F10 > Controls then has **Controllers** / OPEN, which starts `Dolphin.exe -u <netplay User> --controllers`, as the launcher's own "configure controllers" does; the row is greyed while that window is open. What it saves (on each change and when it closes) counts within a second, in game. On a port Dolphin drives, Controls shows Player, Vibration, Controllers and Wii Remotes only (the recomp's own keys and buttons would do nothing there), and Player's hint names Dolphin's device ("Controller on this port: DInput/0/Keyboard Mouse.": the existing words without "Every pad is set up automatically."). New text for Sarah's review: the row's name "Controllers" (Dolphin's own name for that window's button); "OPEN" is the other OPEN rows'.

**Checked:** `brawl_online_dolphin_pads_probe --selftest` (core/standalone) plays scripted keys through a GCPadNew.ini as Dolphin writes it: the player's layout (O on both `C-Stick/Up = I | O` and `C-Stick/Right = L | O`) gives the C-stick at 193,193 for O, the same as I and L together, 220 for one direction (C gate 0.7221); the modifier at 50% halves the control stick (229 -> 178); A + Start, L digital and analog, `` `1` & !LSHIFT `` on analog L; nothing without focus; an edit saved while it runs is read. On this PC the probe lists Dolphin's `DInput/0/Keyboard Mouse` and reads Sarah's netplay GCPadNew.ini (adapter on every port: the recomp's own reading). The window: started over a scratch User folder with the launcher's arguments, a second open refused while it showed, its save read again after it closed. In the recomp (a scratch User folder with SIDevice0 = 6 on `DInput/0/Keyboard Mouse`, C-Stick Up and Right also on the constant `'1'`): `Dolphin's controls: on`, port 1's C-stick at 193,193 as in the probe, and F10 > Controls as above (`run/artifacts/emissary-dolphin-controls`). A bare `1` in an expression is the keyboard's 1 key, in Dolphin too; a constant is `'1'`. `BRAWL_LOG_DOLPHIN_PADS=1` logs each port's pad when it changes.
