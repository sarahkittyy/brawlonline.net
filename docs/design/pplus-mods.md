# Mods of Project+ in the P+ online flow (2026-11-15, REX 2026-11-16)

Cobalt and Project+ Ex are Project+ v3.2 with BrawlEx: more characters and stages on the same
engine. REX 0½ Hotfix 1 is an older P+ code base with BrawlEx and launchers of its own. They play online the way P+ does: the game's own online menus, matchmaking, rooms, the
waiting room and gameplay rollback, with our plugin on the mod's SD card. This is not a Brawl Mods
room (`brawl-mods.md`: the whole machine rolled back, the launcher holding the room), which stays
the way to play every other build.

Scope (the user's, 2026-11-14 and 15): Cobalt 1.34 and Project+ Ex 1.8; 1v1 matchmaking (Unranked,
Direct) and rooms. Ranked, the 2v2 queue and potato mode are P+'s alone. REX 0½ Hotfix 1 joined on
2026-11-16 ("Use the P+ legal stage list. Bring this all the way to parity."), with the Network heap
in memory IOS keeps (Dolphin only) and the MMU off for it; what it needed is `docs/game-code.md`
section 11, "REX 0½ Hotfix 1 on the plugin".

What is not built: the launcher's screens. Everything below them is.

## 0. The model (2026-11-16, after three mods)

A build is what a player boots: a disc, the file Dolphin boots, and the folder of an SD card the
game loads its files and codes from. Everything that differs between builds is one of five kinds,
and each kind has one home:

| Kind of fact | Examples | Home | A new build needs |
|---|---|---|---|
| **Identity** | which build is this | the key `<folder>:<hash>` (section 2) | nothing |
| **The game's own tables** | the roster, the stage select's pages, alt costumes, the Code Menu | read from the running game by the plugin's `Build` (`build.cpp`), by the code or data that is there, never by the build's name | nothing, or one more variant in `Build` when its lineage is new |
| **Placement** | where a line of ours has room on its screens | numbers in its entry of mm's games table (`layout`, section 3) | the numbers, when its menu art is not P+'s |
| **Policy** | modes, stage lists, pairing | its entry of mm's games table (section 3) | its entry |
| **Delivery** | which file boots it, Dolphin settings it needs, which of the plugin's files it takes | its entry (`ownLauncher`, `plugin`), read by the launcher (section 4) and the harness; the harness's template says of itself which build it is (`build.json`, section 5) | `ownLauncher` if it boots its own launcher, `plugin` if it is not of P+'s own kind |

So a build is one entry of one table (`server/config/games.json`), which mm, the launcher and the
harness read, and the plugin has nothing per build: no list, no row of a table, no name.

**One source, a plugin per kind of build (2026-11-16).** The plugin was one file for every build
until REX: what REX alone needs (its buttons as words, PLAY ONLINE brought back, the rules menu
made the vanilla way) came to 13.7 KB of code that Project+ carried too, in the Network heap its
biggest costumes also need, and a fix for one build could break another. Every build is tested on
its own anyway. So `game-code/build.sh` builds the one source three times
(`game-code/PPOnline/include/variant.h`):

| File | For | Has beyond P+'s own | In memory |
|---|---|---|---|
| `PPOnline.rel` | Project+ | nothing | 211,192 bytes |
| `PPOnline-pex.rel` | P+ v3.2 with BrawlEx (Project+ Ex, Cobalt) | BrawlEx's roster file, hidden alt costume sets, a Code Menu of up to 256 lines | 212,980 |
| `PPOnline-rex.rel` | REX | the above; P+'s older stage select; the roster file moved; models found by name; word buttons; PLAY ONLINE restored; the rules menu rebuilt; other fonts; the game's own boot path | 225,416 |

The switches are by what a build has (`PPON_BRAWLEX`, `PPON_OLD_STAGE_SELECT`,
`PPON_WORD_BUTTONS`, ...), never by its name, and the code under one still finds its thing in the
running game as before; a variant is a set of them. What a switch turns on is a file of its own
(`PPOnline/source/variant`: `word_buttons.cpp`, `rebuild_rules.cpp`, `restore_online.cpp`,
`other_fonts.cpp`, `brawlex.cpp`, `old_stage_select.cpp`, `models_by_name.cpp`), empty in the
plugins built without it, behind a header whose functions do nothing there; the files every
plugin has call those and carry no `#if` on a switch, so a change to one build's own code cannot
change another build's plugin (2026-11-17; with it Project+'s plugin came out 24 bytes smaller
and REX's, whose parts are now separate functions, 1.9 KB larger in its own 1.3 MB pool). A
build's entry names its file (`plugin`:
`pex`, `rex`; none is P+'s own), the launcher and the harness put that file on its card (as
`PPOnline.rel` there, the name the loader reads itself by), and the plugin's profile carries its
variant (bits 16-17; Dolphin's log: "plugin pex"). The wrong file does not half-work: a build of
a kind the plugin on its card was not built for (BrawlEx under P+'s own, P+'s older stage select
under a v3.2 one) does not meet the contract (`0x800`), so its online modes are locked and
Dolphin's log names the three files (`test_build_profile.py`, on each mod with P+'s own plugin).
`build.sh` also fails when a plugin is over its memory budget (222,000 bytes for the two that
live in P+'s Network heap; REX's is linked into a pool of its own), and prints what each has left.

**The contract.** The plugin is built on P+'s codes, so a build plays online when it has them
where a known design keeps them: the character select's slot table behind Brawl's own lookup, one
of P+'s two stage select designs, and the File Patch Code's folder (which names the build to mm).
`Build::profile()` (`build.h`) says what was found, in one word; it goes to Dolphin in PPOM's
BUILD (`profile`, `+0x48`), which logs it ("GameBridge: the game's build: stage
select P+ v3.2's, alt costumes hidden alt sets, Code Menu yes, alternate stages yes; it meets the
plugin's contract"), and `tools/gamecode/ppom.py read_build` reads it for the tests. A build that
does not meet the contract has its online modes locked on the ONLINE page, the buzzer on A and
"This game can't play online here." in the description line (`OnlineMenu::modesLocked`, as for a
player who is not logged in), and Dolphin's log says what is missing, instead of a match setup
that fails halfway. Checked on
the plain Brawl disc, which has none of P+'s codes: "online modes locked, missing: a stage select
of P+'s".

**A table that is not found is not there.** It reads as empty and nothing is written, never P+'s
address used anyway: that address is something else on another build (on Brawl's disc P+'s
alternate-stage word is an instruction of the game's code, and P+'s stage pages are other data).
Three older fallbacks to P+'s lists remain, each only where a build gives nothing to read: the
roster when the slot table's lookup is not the one `Build` knows, the legal stages when mm sends
no list (the tests' mm), and Dolphin's character list before the game published its own.

## 1. The game

The plugin and Dolphin read what differs between builds from the running game instead of P+'s
tables (`docs/game-code.md` section 11, "The build's own tables: P+ mods"): the character select's
slot table and roster, the alt costume code, the Code Menu's lines, the card folder, the stage
builder page, the waiting room's fighter, a room's cards under a taller grid; for REX also its
stage select's older tables, its stage kinds, a Network heap where the game destroyed its own, the
room's rules menu made from the build's main-menu archive, the room's buttons as words on a
character select without Brawl's online art, the character select's models found by name, the
connect code's '#' in the keypad font's own glyph, and the main menu's PLAY ONLINE button brought
back where REX had moved it off the screen. Nothing is keyed to a build's name; what only one
kind of build needs is compiled into that kind's plugin alone (section 0).

## 2. Which build a game is

P+ and these mods boot from the same launcher files and differ in their folder on the SD card. So
a build is named by what decides its code (`Core/Online/GameBuild.h`):

    <folder>:<32 hex digits>

- `folder`: the folder the running game loads its files from, read from the File Patch Code's path
  in the game's memory (`0x80406920`: `/Project+/`, `/Cobalt/./`, `/P+Ex/././`, `/rex_/`).
- the digits: the first 32 of the SHA-256 of `boot=<sha256>\n` (the file Dolphin booted: P+'s
  launcher for P+ and its v3.2 mods, REX's own `REX Netplay Launcher.elf` or Offline one for REX) and
  `<NAME>=<sha256>\n` for each of `NETPLAY.GCT`, `NETBOOST.GCT`, `RSBE01.GCT`, `BOOST.GCT` in that
  folder (`-` for a file that is not there).

`tools/gamecode/game_key.py CARD FOLDER BOOT_FILE` computes it from a card image; the launcher has
the same function (`mods/plus_family.ts gameKey`); the three agree on a test vector.

Dolphin resolves the key once per boot, when GameBridge first finds the plugin's block (the game
is up; a few seconds after boot), before it services anything the game asks. The online connection
waits for it: `hello` is sent only once the key is known (or 45 s after a boot without the plugin,
or at once for a whole-machine session, which has none). A create or join the launcher handed over
during the boot waits for the build itself, up to 180 s (`REQUEST_WAIT_SECONDS`), and does not time
out meanwhile: the request is for the game's own menus, and a mod whose card boots slowly must not
be taken for P+ (an empty `game`), which would put it in a P+ room. If the build still becomes
known after a `hello` that named none or another (the plugin came up late, another game was
booted), the connection is closed and made again when no room would go with it; a request still
pending goes again on the new one. Never under a whole-machine session. `hello` and every
`create-ticket` carry the key as `game`; a Dolphin that knows none sends none.

## 3. The server

`server/config/games.json` (`MM_GAMES_FILE` replaces it; `mm::games`) lists the builds:

```json
{"games": [
  {"id": "pplus", "name": "Project+", "folder": "Project+"},
  {"id": "pex-1.8", "name": "Project+ Ex 1.8", "folder": "P+Ex", "codes": ["812d…", "1107…"]},
  {"id": "cobalt-1.34", "name": "Cobalt 1.34", "folder": "Cobalt", "codes": ["fe22…", "ee8a…"],
   "layout": {"sssLine": [726, -191, 1046, -231, 0.8], "cardPlates": [-22.6, 14.5, 4.3]},
   "starters": [1, 46, 33, 25, 74], "counterpicks": [19, 42, 7, 4, 2, 76, 12, 35, 68, 30],
   "rulesets": {"direct": {…}, "unranked": {…}}},
  {"id": "rex-0.5", "name": "REX 0.5", "folder": "rex_", "codes": ["747b…", "2ca7…"],
   "ownLauncher": true, "layout": {"sssLine": […], "cssBar": […], "cssButtons": [[…], …]},
   "starters": [1, 4, 41, 33, 46], "counterpicks": [12, 7, 35, 45],
   "rulesets": {"direct": {…}, "unranked": {…}}}
]}
```

- The first entry is the default, P+. No `game` (every build before the field) is P+, and so is
  any codeset in P+'s folder: players' own P+ codesets played before this, at their own risk, and
  still do. Nothing changes for P+.
- A mod must be listed with the exact hash of its release (`codes`: one per boot file, the Netplay
  and the Offline Launcher's). Anything else is refused at `hello` and at a ticket: "This version
  of Cobalt can't play online here yet." A new release of a mod plays once its entry is added, as
  another `id`, never against the old one. That is the version pin, of the same kind as P+'s own
  (the launcher knows a P+ release by its `NETPLAY.GCT`): the codesets, not the files under `pf/`
  (section 7).
- **Pairing.** A ticket keeps its game's id. Unranked pairs only equal ids; Direct between two
  builds tells both ("BB#22 is playing Project+."); a room keeps its creator's id, refuses a join
  from another ("That room plays another game."), and refuses a game ticket from another.
- **Modes.** A mod plays Direct, Unranked and rooms unless its entry lists `modes`. Ranked and
  the 2v2 queue answer "Ranked isn't on Cobalt 1.34 yet."
- **Stages.** A mod's `rulesets` entries replace `config/rulesets.json`'s per mode; a mode without
  one plays the shared entry. `starters` and `counterpicks` are the stages of a room's default
  ruleset. P+ Ex's stage select is P+'s, so it has neither.
- **Rulesets.** A stage id names another stage on another build, so a published ruleset says
  which build its stage ids are of: `game` in its JSON, mm's id of the mod, left out for P+ (so
  every P+ ruleset keeps its JSON and hash; `docs/rulesets-protocol.md`). A room plays its own
  build's rulesets only: `room-create {ruleset}` and `room-ruleset {code}` with another build's
  answer "That ruleset is for another game." The account's remembered ruleset is one ruleset: a
  room takes it when it is of the room's build (the game's Create Room always asks for it), a
  ruleset of the room's build becomes it, and `room-ruleset` without a code forgets it only when
  it is of the room's build. Nothing publishes a mod's rulesets yet but the tests (the launcher's
  Rulesets page makes P+'s); a mod's room otherwise plays its build's default and the host's own
  edits of it (the stage switch, the rules pages).
- **Spectating.** A mod's match is streamed between its players' games as any other (the
  digests are compared) and never listed: a spectator's Dolphin plays a stream on P+'s card,
  where a mod's characters and stages are not. Also for a match read back after mm restarted
  (`mm_matches.game`).
- **Placement** (`layout`; the user's, 2026-11-15: a per-build placement; as numbers since
  2026-11-16). A few of the plugin's own things sit where the build's menu art leaves room, which
  nothing in the game says. The plugin places them by the numbers in its LAYOUT (a section of the
  PPOM block named by two header words that were reserved: no version change; `layout.h` says
  each number), which hold P+'s places from the moment it is loaded (`layout.cpp`). A build whose
  art differs has its own in its entry: mm sends the entry's `layout` in the `hello-resp` that
  answers that build's `hello`, and Dolphin keeps it (`GameBuild::Layout`) and writes it over
  P+'s in LAYOUT while the game is on its menus; a group the entry has not stays P+'s. So a build with
  other art is an entry's numbers, not a change of the plugin. (Until 2026-11-16 the entry had two
  row numbers, `sssLine` and `cardPlate`, of tables inside the plugin, in a byte of LOCAL: each new
  layout was a plugin change and a table change, and one number chose three tables.) The groups:
  - `sssLine` (x1, y1, x2, y2, scale): the stage select's turn line ("ANYONE: Everyone picks at
    once", a set's strikes and picks). P+'s is the header strip, on the "STAGE SELECT" line.
    Cobalt's stage select draws the name of the stage under the cursor there (its own menu art,
    not one of the stage select's 29 message windows), so its line is under the button legend
    right of the preview; REX's is the strip along the top between BACK and "STAGE SELECT".
    Ranked's player cards stand where Cobalt's line is: a mod that gets Ranked needs another.
  - `cardPlates` (P1's plate's middle, the step to the next card's, half its width): on a room's
    character select, A on a card's name plate sits that player out, and the pencil beside it
    opens the name tags. P+'s card has the pencil in a strip left of the plate; Brawl's own card,
    which Cobalt has, has it at the plate's right end, inside the game's one button for both, so
    with P+'s numbers an A on Cobalt's pencil sat the player out.
  - `cssBar` (14 numbers: the status line, the own code, the room's lines, the text size) and
    `cssButtons` (per button the window and the hand's rect): the character select's bar, and
    the room's buttons as words where the character select has no online art. REX has both.
- **Records.** `mm_matches.game` (`migrations/0038`; `0039` lets mm's own database role read it
  back with its recent matches after a restart) is the id, empty for P+. The match history
  carries it (`game`), the room list carries a mod room's id and name (`game`, `gameName`), and
  Unranked's hidden rating is moved by P+ games only.

### REX 0.5's stages (P+'s legal list, the user's, 2026-11-16)

REX plays P+'s lists: the same stages by name, in REX's own slots (its `StageTable.asm`). Temple of
Time is REX's `0x29` (P+ `0x09`), Fountain of Dreams `0x07` (P+ `0x1F`), Metal Cavern `0x79` (P+
`0x05`), Bowser's Castle `0x6B` (P+ `0x06`); the rest have P+'s kinds. So REX's entry is P+'s
starters, counterpicks, Direct and Unranked lists mapped, Fountain of Dreams with its hazards as
on P+ (`hazardStages: [7]`); `mm::games`' test holds it to P+'s lists. Its `layout` has the turn line's
place (the strip along the top between BACK and "STAGE SELECT"), its bar's windows and its
buttons as words; its cards are P+'s.

### Cobalt 1.34's stages (the user's list, 2026-11-15)

Stage kinds as Cobalt's stage slots number them. Each line of the list is one stage, or two that
count as one:

| | | |
|---|---|---|
| Starters | Battlefield `0x01` / Zoness `0x13` | Pokémon Stadium 2 `0x2E` / Midair Stadium `0x2A` |
| | Smashville `0x21` | Castle Siege `0x19` / Dive to the Heart `0x07` |
| | Radical Highway `0x4A` / Luigi's Mansion `0x04` | |
| Counterpicks | Final Destination `0x02` / High Entia Tomb `0x4C` | Frigate Husk `0x0C` |
| | Green Hill Zone `0x23` / Dead Line `0x44` | Skyworld `0x1E` |

- Unranked draws from all fifteen kinds, and Direct's list is the same.
- A room's default ruleset has the five first-named starters (struck 1-2-1 in a set) and the other
  ten as counterpicks.
- "Any PS2 variant" needs nothing: an alternate is the same kind with a button held.
- Sectonia's Gem, Frigate Husk's partner, is Fountain of Dreams' slot (`0x1F`) with L held. A
  stage list is a list of kinds and can't name one alternate, so that slot is not in the lists: it
  can be picked where the pick is free (Direct, a room's loser) and is never drawn or offered from
  a list.

## 4. The launcher (main process)

`mods/plus_family.ts`: the supported builds (`PLUS_FAMILY_BUILDS`) are mm's games table itself,
bundled with the launcher (`server/config/games.json` without its first entry, Project+: id,
name, folder, the keys of its boots, and `ownLauncher` for a build that boots its own launcher,
REX), so a build is listed once; the client workflow builds on a change of that file too.
`identifyPlusFamilyCard` (reads a Brawl Mods game's SD card: which supported build
it is and the file to boot, P+'s Netplay Launcher or the game's own `launcherPath`, or none),
where the plugin goes (`/<folder>/pf/plugins/PPOnline.rel`), which of the plugin's files that is
(the entry's `plugin`: `defaultPluginLocation(env, build.plugin)`, `<resources>/plugins/PPOnline-pex.rel`
with its own manifest) and where the card copy lives (`<netplay>/pponline-sd-<id>`). For a build with its own launcher the netplay Dolphin also gets
`GameSettings/ID-<boot file>.ini` with `MMU = False` (`plusFamilyGameSettings`: REX ships with the
MMU on, and its online CSS makes null accesses that P+ makes too and that are fatal with it).

`DolphinManager.launchPlusFamilyDolphin(build)`: the game's own card is copied once with the
plugin on it (`installPluginOnSdCard`, the player's card only read), and the netplay Dolphin boots
P+'s Netplay Launcher on that copy and the game's disc. From there the game is in P+'s online
menus and names its build to mm itself. One start at a time, as Play: a second call for the same
game while it starts gets that start's result, and Play or a playback waits for it (the first
start copies a card of several GB).

A replay of a mod's match (`build.gameBuild` in its INFO) is neither played nor rendered: the
playback Dolphin has P+'s cards only. Play and the render queue answer `unplayable` with the
mod's name and `mod: true`, also when asked to play it on the launcher's release anyway.

IPC for the screens to come: `mods_onlineBuildOf {id}` (is this game a supported mod?) and
`mods_playOnline {id}` (start it); `ModsService.onlineBuildOf` / `playOnline`.

## 5. Tests

- Dolphin: `GameBuildTest` (the key), `RoomMessagesTest` (`hello`'s `game`, `hello-resp`'s `layout`), `SpecialAssetsTest`
  (the card folder, the rules menu's archive), `PeerDataTest` (the build's characters and stages).
- Server: `mm::games` (the table, bad files, REX's lists as P+'s by name), `mm::engine` (pairing by build, unknown builds,
  modes, Direct between builds, a room's build, its stages and record),
  `crates/e2e/tests/games.rs` (the same against Postgres and both services: the queue, the record,
  the history, no rating, the room list).
- Launcher: `mods/plus_family.test.ts` (the list from mm's table, REX's own launcher and game
  settings).
- The contract: `harness/tests/test_build_profile.py` (on every build: the profile the plugin
  publishes, the plugin on the card being the one its entry names, Dolphin's log line, the ONLINE
  page open; on a mod, also P+'s own plugin on its card: online modes locked, the log naming
  the files).
- The game: P+'s harness tests run on a mod's card (`harness/README.md`, "Mods of P+"). A build's
  template says which build it is (`build.json`: its folder, and its own launchers when it has
  them; `harness/tools/mod_template.py` writes it), so `PPHARNESS_TEMPLATE` alone names the build
  to the harness, its tools and `mod_suite.py`; results per build are in `docs/game-code.md`
  section 11. Tests of files a build may not have (alt costumes) are marked
  `needs_files` and skip on a card without them. Tests of Ranked and the 2v2 queue are marked
  `pplus_only` and skipped on a mod, where one test checks that the search is refused by name;
  the others read what differs between builds from the running game, so the same tests pass on
  P+ and on a mod.

## 6. Adding a build

1. `harness/tools/mod_template.py` on its card, then `harness/tools/mod_suite.py --template` on
   the template. The plugin's log line says at once whether the build meets the contract and
   which of P+'s codes it has. Fix what the suite finds in the plugin or Dolphin for every build,
   never by the build's name: a table read through `Build`, and when the build is of a lineage
   `Build` does not know, one more design there. What only this kind of build needs goes into
   a file of its own in `PPOnline/source/variant`, under a switch of `variant.h` that the
   build's variant has (a new variant, a set of switches, when no set fits), behind a header
   whose functions do nothing in the other plugins: they do not grow, and their code is not
   touched; `build.sh` prints each plugin's room.
2. `mod_template.py` prints its two codes (its key with P+'s Netplay and Offline Launcher, or
   with the build's own two: then `ownLauncher`).
3. One entry in `server/config/games.json`: its codes, its stage lists from someone who plays it,
   `ownLauncher` if it boots its own launcher, `plugin` if it is not of P+'s own kind (the
   smallest variant it meets the contract with: Dolphin's log says), and, where its menu art is not P+'s, the places of
   the plugin's own things as `layout` numbers (measured on its screens; the tests of the turn
   line and the cards' plates read them back). mm, the launcher and the harness read that entry;
   nothing else lists the build.

## 7. Known issues

- **Sectonia's Gem is in no Cobalt stage list.** It is Fountain of Dreams' slot (`0x1F`) with L
  held, and a stage list is a list of stage kinds, which cannot name one alternate of a slot
  (section 3). It can be picked where the pick is free (Direct, a room's loser); Unranked never
  draws it and a room's list never offers it. Left as it is for now (the user, 2026-11-15); to
  list it, a list entry needs the alternate's buttons beside the kind (mm's rulesets, SESSION's
  stage masks and the plugin's legal check).
- **Items under rollback.** With items on, some two-player matches drift from a replay without
  rollbacks or hang; stock P+ does the same in the same runs (`docs/game-code.md` section 11,
  Checks). The engine's, not a mod's: deferred, it does not hold the mods back (the user,
  2026-11-15). Matchmaking plays without items; a room's host can turn them on.
- **Final Smashes.** One Project+ Ex sync test with Final Smashes forced hung (Waluigi, Jukain,
  Dark Samus, Ridley). Final Smashes are known to be broken on these builds without rollback
  too (the user, 2026-11-15): deferred.
- **Unranked on Cobalt draws from all fifteen listed stages**, so a stage and its partner
  (Battlefield / Zoness) each have a turn: wanted (the user, 2026-11-15).
- **The stage builder page is turned past online on every build**, P+ with custom stages on the
  card included: wanted (the user, 2026-11-15).
- **The version pin is the codesets.** A build's key is its folder, the booted file and the four
  codesets, as P+'s own pin is its `NETPLAY.GCT`. A release of a mod that changed only files under
  `pf/` (fighters, modules), or a card a player edited there, has the same key and is paired with
  the listed release; the match then ends as a desync, as it does on P+ with edited files. A file
  check would have to tell a new moveset from a costume or a stage skin, which players add: not
  built.
- **A mod's replays and spectating.** A replay of a mod's match is recorded (with its build) and
  cannot be played or rendered by the launcher yet, and a mod's matches are not listed for
  spectators: the playback Dolphin has no card of the mod.
- **REX needs Dolphin.** Its Network heap lives in MEM2 that IOS keeps on a Wii (`docs/game-code.md`
  section 11), and it plays with the MMU off. Both are Dolphin's to give; online play is Dolphin's.
- **REX's characters to Dolphin.** Dolphin takes any of the 249 characters REX's slot table defines,
  while 131 are on its roster: a modified game could name one REX has no files for. The fighter
  configs would be the exact list.
- **REX's gameplay codes are not in its key.** REX loads gameplay codes from `pf/injects/*.gct`,
  which the key does not cover (it covers the folder's four codesets): an edited inject plays as
  REX and desyncs, as an edited file under `pf/` does on every build.
- **Not checked:** Windows and macOS, two PCs over a real network. The launcher has no screens
  for this yet.
