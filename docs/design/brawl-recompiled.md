# Brawl Recompiled as a second game engine (design)

Status: design, 2026-11-10. Not built. The user's request (Sarah's board, 2026-11-10): "my goal is to get this brawlrecompiled working as a nice convenient clean ux from the launcher. we can replace this "Play Button Action" that no one uses with just, like, "Launch P+ Dolphin", "Launch P+ Recomp" ... just like project punch, we need the ability to download and set it up automatically, see how much it takes to download, and be able to delete it, and auto update it ... we also need proper handling for if the user doesn't have the brawl iso ... it also needs to work with all of our flows, including opening automatically, navigating to a room, joining and leaving, switching queues, direct invites, our in-game ui plugin injection, etc."

Scope: the launcher's UX, install/update, the launcher <-> game contract and the release pipeline. Making the recomp fast and desync-free is someone else's work (kalomaze and others, `kalomaze/BrawlRecompiled` and `kalomaze/brawlonline.net`, branch `rollback-dolphin-crossplay`); this design only states what it must provide.

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
| Central repository, CI? | Yes: our own fork, `sarahkittyy/brawl-recompiled` (GPLv3, separate repo, not in the monorepo), with CI for Windows and Linux (macOS later). The translated game code needs `main.dol` and the RELs, which can't be public: they come from an encrypted 17 MB input only CI can read (§7). **Distributing a translated engine is the one decision for Sarah (§10, D1).** |
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

## 7. Our fork, releases and CI

### 7.1 Repository

`sarahkittyy/brawl-recompiled`: a fork of `josiahg2502-glitch/BrawlRecompiled` with kalomaze's `rollback-dolphin-crossplay` merged, kept as its own repo (it has its own toolchain, a 71 MB tree and GPLv3; upstream merges stay simple). Branches: `main` (what the launcher ships), upstream tracking branches. It depends on this monorepo for GekkoNet and the plugin protocol: CI checks out `brawlonline.net` at the commit named in `brawlonline.lock`.

### 7.2 What CI builds

| Job | Runner | Output |
|---|---|---|
| translate | ubuntu-latest | `generated/` from `main.dol` + RELs (the C# translator; minutes) |
| windows | windows-latest (or ubuntu cross with llvm-mingw) | `engine_usa.dll`, `engine_usa_v2.dll`, loader, DLLs |
| linux | ubuntu-latest | `brawl-engine` (both ISA levels) |
| macos (later) | macos-14 | arm64 app, notarized through the existing mac publish flow |
| contracts | all | kalomaze's transport/protocol/snapshot unit tests |
| replay gate | **private machine** (Sarah's PC or a self-hosted runner that has the disc files) | the replay corpus (§9) through the release build: zero checksum mismatches |

The translation needs `main.dol` and the 126 RELs (17 MB, Nintendo's code). They never go in the repo: CI gets them from an encrypted archive (age/GPG) in a private location, decrypted with a repository secret, on `push` to `main` and tags only (never on pull requests). The full disc (game files) is only on the private replay-gate machine. A Windows build took 16 minutes with 12 jobs here; a 4-core hosted runner will take about an hour, fine for releases.

### 7.3 Release artifacts

Per release, on GitHub Releases of the fork (or our updates host, as `updates/pplus`):

- `brawl-recompiled-<ver>-windows-avx2.zip`, `-windows-v2.zip`, `-linux-avx2.tar.zst`, `-linux-v2.tar.zst` (each ~100 MB: one engine, not two).
- `manifest.json`: `{version, protocol, sim, ppom, modes: ["direct","unranked",...], minLauncher, files: [{platform, isa, url, size, sha256}]}`.

The launcher reads the manifest, picks its platform and ISA (a new AVX2/FMA check in main; the loader makes the same choice upstream), shows `size`, checks `sha256`. Later: binary diffs between consecutive versions (the engine DLL is one 180 MB file; most updates change little of it).

### 7.4 Compatibility

The recomp and Dolphin must simulate identically and speak the same protocol, so the launcher carries `RECOMP_PROTOCOL` (bumped whenever the plugin protocol, the gameplay region set, the setup sync or the checksum changes on the Dolphin side). A recomp whose manifest `protocol` differs is `required` (§5). mm learns the engine too: the ticket and the game report carry `engine: "dolphin" | "recomp"` and the recomp's version, so desync and speed numbers can be split by engine (`pp-admin speed`), and Ranked can refuse the recomp until we trust it (§10, D3).

### 7.5 Fork changes the launcher needs

1. **Launch config from the launcher**: a `--config <toml>` (or env) naming the user dir, the disc files, the SD card image (read-only), the codes file *inside* that card (`/Project+/NETPLAY.GCT` + `gc.txt`; today it wants a host file), the NAND template, the mm server and the online folder. No portable.txt assumptions.
2. **Read-only SD card** (as Dolphin's `WiiSDCardAllowWrites=False`): the runtime writes files back to a card folder at exit today.
3. **The online contract in-process or as one bundled native companion** (§8.2): no Python on players' PCs.
4. **Raw GameCube adapter input** (§4.4).
5. Pinned game state that must match Dolphin: the game's widescreen flag (Dolphin pins `SYSCONF.IPL.AR=True`), the RTC, the language; F10's 4:3 hidden or ignored online. Cheats and fast-forward are already off in deterministic mode.
6. The stage-start hook fix (`GuestCodePatch` path, found 2026-11-09) if kalomaze hasn't already.
7. `.brep` recording of recomp games (same format), so replays, highlights, stats and ranked reports work; playback stays on the playback Dolphin.

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

- **D1, distributing the engine.** The engine DLL contains Brawl's code, translated. Upstream ships it the same way (that's how v0.5.3 is distributed), but it is a different legal exposure from shipping Dolphin and a plugin. Options: (a) ship it built by our CI, as upstream does; (b) never ship it: the launcher compiles it on the player's PC from their disc (needs a ~1.5 GB toolchain download and 20-60 minutes of CPU with 16 GB of RAM, once per update); (c) download upstream's own release files and only add our pieces (not possible while the runtime and the game code are one DLL). Recommendation: (a), with the repo itself free of game code, and revisit if anyone objects.
- **D2, platforms.** Windows + Linux first (both have releases upstream), macOS (arm64) after. OK?
- **D3, Ranked.** Let recomp players into Ranked from day one of beta, or keep Ranked on Dolphin until the desync numbers by engine are in? Recommendation: keep Ranked on Dolphin until then.
- **D4, names.** "Brawl Recompiled (Beta)" in the UI, "Add-ons" for the settings section, "Play Project+ with" for the choice.
- **D5, ownership.** The fork under `sarahkittyy/`; kalomaze (and whoever else) as collaborators on it, with the online side built on shared `Online/` code (§8.2) rather than the Python companion. That is their call as much as ours; this design only needs the §8.2 contract.
