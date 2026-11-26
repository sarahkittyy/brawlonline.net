# Staging: test a build on Windows and the Mac before pushing

`tools/staging/staging.py` runs a staging server on this PC and builds packaged Windows and macOS
clients from one commit. The clients start like fresh installs and talk only to that server. Nothing
touches production, the real installs or the shared `dolphin/build`.

```
python tools/staging/staging.py server up           # Postgres + accounts + mm, reachable on the LAN
python tools/staging/staging.py win build            # package main (or --ref <commit>) for Windows
python tools/staging/staging.py mac build            # the same commit for the Mac (built on the Mac)
python tools/staging/staging.py win run --fresh      # first start, like a new install
python tools/staging/staging.py mac run --fresh
python tools/staging/staging.py server mail          # the sign-up verification links
python tools/staging/staging.py server seed-fake     # fake players, rooms, live games, history
python tools/staging/staging.py status
python tools/staging/staging.py down                 # stop clients and server; the database stays
```

## What it sets up

| | Where | Lifetime |
|---|---|---|
| Staging database | `run/staging/pgdata` (the portable Postgres from `run/postgres-portable`, 127.0.0.1:54339) | kept; `server reset-db --yes` empties it |
| accounts | `http://<this PC's LAN IP>:18080` | `server up` / `server down` |
| mm | UDP `<LAN IP>:43213`, spectating on (`MM_SPECTATE`; config.json `"spectate"`: off, witness, on) | same |
| Mail | `run/staging/mail.jsonl` (MAILER=file); `server mail` prints the links | kept |
| Windows build | worktree `run/staging/src` (own Dolphin build), packaged to `run/staging/win/app` | rebuilt by `win build` |
| Server binaries | `run/staging/cargo-target`, built from the same commit by `win build` | same |
| Windows profiles | `run/staging/win/profiles/<name>` | `win reset` / `run --fresh` delete one |
| Mac build | `~/code/brawlonline.net/brawl-staging/src`, app in `~/code/brawlonline.net/brawl-staging/app/Brawl Online Staging.app` | rebuilt by `mac build` |
| Mac profiles | `~/code/brawlonline.net/brawl-staging/profiles/<name>` | `mac reset` / `run --fresh` |
| P+ release zip | `run/staging/pplus/pplus-<version>.zip` (Mac: `~/code/brawlonline.net/brawl-staging/pplus/`) | kept; downloaded once per P+ release |

Ports and addresses are in `run/staging/config.json` (written on first use, with the database
password and play-key secret). The LAN IP is the one that reaches the Mac; edit `lan_ip` there if
it changes, then `server restart`.

## How the clients are pointed at staging

The packaged launcher runs in its test mode (`PPO_TEST_MODE=1`) with:
- `PPO_USER_DATA_DIR`: the profile folder. Settings, logins, logs, the replay database and the
  Dolphin the launcher installs all live there. Deleting it is a fresh install.
- `PPO_ACCOUNTS_URL` / `PPO_WEBSITE_URL`: the staging accounts server.
- `PPO_UPDATES_URL`: a URL on the staging server that has no feed, so a staging build never
  updates itself to a release.
- `PPO_DOLPHIN_EXTRA_ARGS`: `-C Dolphin.Online.MatchmakingHost/MatchmakingPort/AccountsUrl=...`,
  so Dolphin's matchmaking and user lookups go to staging too.

The same settings are also baked into each staging app as `resources/staging-env.json`, which the
launcher applies at start-up. So a staging app started any other way (the Dock, Finder, a
double-click on the `.exe`) still uses the staging profile `a` and the staging servers, never the
real install. Releases never contain that file.

A second account on the same machine: `win run --profile b` (or `mac run --profile b`).

## Accounts

Each `run` starts the client logged in to a staging account of its own, so the board's runs (each
on the next free profile) and its 2v2 are always different players:

| Profile | Windows | Mac |
|---|---|---|
| a | Red | Yellow |
| b | Blue | Green |
| c, d, e, f, g, h | Orange, Pink, Lime, Gold, Brown, Black | Cyan, Purple, Teal, Silver, White, Gray |

The account is `staging-<name>@staging.test`, password `staging-pass`, made in the staging database
the first time it is used: email verified, rules accepted, a connect code (`RED#1`, ...). Before the
client starts, `run` logs in to the accounts server and writes that session into the profile:
Settings' `accounts` then list only that account, and `sessions.json` holds its token (`plain:`,
which the launcher reads as well as its own keychain-encrypted ones). The rest of the profile is
kept. If the server can't give a session, `run` says so and the client starts with the profile's
own login.

`--account <name>` picks another of the names, `--account none` keeps whatever the profile is
logged in as (an account you made by hand). `--fresh` logs in to nothing unless `--account` is
given, so its first start still signs up or logs in by hand. Fake players (`server seed-fake`)
give these accounts friends and history only once they exist: run the seed again after their
first use.

`--controls pad|keyboard` on `win run` / `mac run` sets port 1 for that profile before it starts:
`pad` is the GameCube adapter (`[Core] SIDevice0 = 12`), `keyboard` a standard controller
(`SIDevice0 = 6`, which Dolphin sets up on the keyboard in `GCPadNew.ini`), the other ports empty.
It is written into the profile's own `netplay/User/Config/Dolphin.ini` (made if the profile is new:
the launcher's installs keep it), not given as `-C`: this Dolphin defaults every port to the adapter
and opens it while any config layer names it, so a keyboard window with only `-C` would still take
the adapter from the other one. It also sets EmissaryRecomp's `[gc_keyboard] enabled` in the
profile's `netplay/emissary-recomp/UserData/Config.toml` (`pad`: port 1 is the first controller SDL
finds; `keyboard`: the keyboard, while its window has focus).

## Stages

`--stage NAME` before the command (or `STAGING_STAGE=NAME`) is a second staging beside the first,
for a branch whose server the first's database can't go back from: its own folder
`run/staging-NAME` (database, server binaries, client build, profiles), its own ports (the first's
+ 100, unless its `config.json` says otherwise; a new stage takes the first's Mac and LAN settings)
and its own Mac folder `~/code/brawlonline.net/brawl-staging-NAME`. Only P+'s release zip and the
Mac's `ccache` shim are shared.

## EmissaryRecomp

Since the merge of `emissary-recomp` (439552bb4), main's launcher can play Project+ with
EmissaryRecomp, so any stage built from main can have it too:

```
python tools/staging/staging.py win build
python tools/staging/staging.py emissary build
python tools/staging/staging.py server up
python tools/staging/staging.py win run --profile a --engine emissary --controls pad
```

- `emissary build` builds EmissaryRecomp (`config.json` `emissary_repo`, default `E:/emissary/src`,
  at `emissary_ref`, default `brawlonline/windows-build`, or `--ref`) in a worktree in the stage's
  `emissary/src`, with the stage's `src/` as its Brawl Online core (`-DBRAWL_ONLINE=<src>`): the
  same core as the stage's Dolphin, server and launcher, whatever the recomp's `brawl_online.lock`
  pins. It uses that checkout's `toolchain/` and translates the extracted disc in `emissary_game`.
  The build is packed as a release `v<VERSION>-staging.<time>` in `emissary/releases/`, alone (a
  CI-style `EmissaryRecomp-<tag>-Windows.zip` + `manifest.json`, appVersion = the stage's
  `latest_version`, protocol 1). Full builds take ~15 min at 6 jobs; later ones are incremental.
  A change to the recomp alone needs only this, not `win build`; a change to `core/` needs
  `win build` first (the recomp takes its core from the stage's `src/`).
- `win run` gives the launcher that folder as its release feed (`PPO_EMISSARY_RELEASES`) whenever
  the stage has a build, so Settings > Add-ons installs and updates EmissaryRecomp from it (a running
  launcher takes a new build before its next game). `--engine emissary` sets the profile's "Play
  Project+ with" to EmissaryRecomp (`--engine dolphin` back); without `--engine` the profile keeps
  what was picked in its launcher. A profile that hasn't installed it yet is asked to on its first
  Play.
- `emissary build --mac` builds it on the Mac (Apple silicon) for the stage's Mac clients: the same
  `emissary_ref` (or `--ref`), pushed into `<Mac stage>/emissary/repo.git`, checked out in
  `emissary/src` and built by the recomp's own `tools/release-macos.sh` with the Mac's `src/` (the
  commit of its `mac build`, which must be 5310e80fa or newer) as its core. The disc is extracted
  once from `mac_emissary_disc` (default `~/Games/SSBB_NTSC.iso`) into `mac_emissary_game`
  (default `~/code/brawlonline.net/emissary-game/DATA`, shared by every stage). The release
  (`EmissaryRecomp-<tag>-macOS.tar.gz` + `manifest.json`) goes to the Mac's `emissary/releases/`.
  A full build takes ~40 min there; later ones are incremental. The recomp ref needs EmissaryRecomp's
  macOS build (`brawlonline/macos`, c02d7a0).
- `mac run` gives the Mac's launcher that folder as its feed in the same way; `--engine` there sets
  the Mac profile's "Play Project+ with". `PPO_REMOTE_DEBUGGING_PORT=<port>` before `mac run` opens
  DevTools on the Mac's launcher (127.0.0.1 there: tunnel it with `ssh -L`).
- The `emissary` stage (`run/staging-emissary`, a junction to
  `C:\Users\sarah\brawl-staging\staging-emissary`, its own server and the accounts
  `recomp@staging.test` / `dolphin@staging.test`, password `staging-pass`, from
  `run\emissary-test\seed.py`) was the branch's staging before the merge. Nothing builds it any
  more; `--stage emissary destroy --yes` removes it once its accounts aren't wanted.

## The agent board's staging buttons

The board's header (D:\code\agent-board) has Run Windows and Run Mac (a client on the next free
profile of the first staging, built from `main`) and a Staging panel: what each build holds (its
commit, how far behind `main` it is, the recomp's core), whether the server is up, and the builds:
Windows, Windows recomp (`emissary build`), Mac, Mac recomp (`emissary build --mac`), Build
everything (Windows, its recomp, then the Mac) and Run 2v2. Each Run on a machine gets the other
controls: the controller while no staging client running on that machine (any stage) has it, else
the keyboard. Each Run is logged in to its profile's account ("Accounts" above), so Run 2v2 is
Red, Blue, Yellow and Green. Run 2v2 puts profile a on the controller and b on the keyboard on each machine. The
engine a client plays with is its profile's Settings > Game "Play Project+ with".

## Builds

`--ref` is a commit, branch or tag (default `main`); only committed work is built.

- **Windows** (`win build`): checks out the commit in `run/staging/src`, links the submodules and
  `toolchains/` to this checkout's copies, builds the server (`cargo build --workspace`),
  Dolphin (the `ninja-release-x64` preset, as CI configures it), the plugin and the launcher. It
  then stages Dolphin as `build-dolphin-windows.ps1` does and runs `electron-builder --win dir`
  (no installer). Builds run at below-normal priority. The first build is a full Dolphin build;
  later ones are incremental.
- **Mac** (`mac build`): pushes the commit to `~/code/brawlonline.net/brawl-staging/repo.git` (a stage's own folder) over
  SSH, copies the plugin built here, and runs CI's `build-dolphin-macos.sh` and
  `package-launcher-macos.sh` on the Mac without a signing identity (ad-hoc signed, not notarized).
  Qt comes from `~/code/brawlonline.net/brawl-dev/qt/6.8.3/macos`, and Node 24 from your nvm.
  Dolphin is built with Homebrew hidden, as on CI: Homebrew's Qt 6.11 had leaked in through an
  rpath and made Dolphin abort at start, and a build that still refers to `/opt/homebrew` is
  refused. `mac setup` checks these and writes a pass-through `ccache` into
  `~/code/brawlonline.net/brawl-staging/bin` (nothing is installed).
- The Dolphin bundle's version is `0.1.0-staging.<commit>`, so a new build replaces the Dolphin an
  existing profile installed.

Logs: `run/staging/logs/` (`win-build.log`, `mac-build.log`, `accounts.log`, `mm.log`,
`postgres.log`).

## Fake players

A staging server has only your own test accounts on it, so most of the launcher is empty. `server
seed-fake` fills it with made-up players (staging only: production never sets the variable that
turns the live part on).

```
python tools/staging/staging.py server seed-fake            # after `server up`; again after new sign-ups
python tools/staging/staging.py server seed-fake --remove   # take every fake row out again
```

It needs a staging server built from a commit that has `server/crates/accounts/src/staging_fake.rs`
(`win build`, then `server restart` once, so accounts starts with `STAGING_FAKE_STATE`).

**In the database** (`tools/staging/seed_fake.py`, the same rows the server writes):
- 60 fake accounts, `fake-<name>@staging.test`, password `staging-pass` (any of them can log in).
  Profiles with a country, 1-3 mains and a few socials; a few hide their flag or main.
- Ranked: every tier, Bronze 3 to Legendary, plus three still placing and four who never played
  Ranked. Each fake's rating history follows the server's Elo rules: an early climb 60-26 days ago
  against opponents without accounts (shown as deleted accounts), then sets among the fakes.
  `admin ranked recompute` gives the ranks from the history.
- Unranked, Direct, room (1v1, FFA, teams, some with a published fake ruleset) and 2v2 matches among
  the fakes over the last 30 days, with the game reports the match history reads. `admin unranked
  rebuild` rates the Unranked games; the 2v2 ratings are rated as the server does.
- Three published rulesets by fakes: Auto L Cancel v4, Catfight BO5, Friday Doubles. They stay
  when the seed runs again (an account may publish 10 an hour); `--remove` deletes them.
- Every real account with a connect code (not seed_ranked.py's `seed-*` accounts, unless
  `--include-seed-ranked`): 8 fake friends, 2 friend requests from fakes and 2 to fakes, and 40
  matches against fakes over the last two weeks, more of them lately: 22 Ranked sets (their rating
  goes on from where it was, after their own last set, so rating history and rank move; a few
  against the same player), 7 Unranked, 3 Direct with friends, 5 room games and 3 2v2 matches.
  Accounts made later get theirs when the seed runs again.
- `queue_searches`: two weeks of searches by the fakes. Unranked waits about 0:22, Ranked about
  0:40, 2v2 about 1:00 most hours; late at night (UTC 11-14) Ranked and 2v2 searches mostly give up
  ("never") and Unranked has too few to tell.

**Live** (`run/staging/fake-state.json`, which accounts reads while `STAGING_FAKE_STATE` names it and
reads again when it changes; `accounts/src/staging_fake.rs`): about 34 fakes online in the social
stream's list, 6 P+ rooms (Peachy's friends-only FFA, a named waiting FFA, two full 1v1s and a
teams room in game, a lone waiting 1v1), 3 Brawl Mods rooms (REX with an SD card and codes,
Tatsunoko vs. Capcom with codes, Mario Kart Wii) and 7 live games (2 Ranked with the top players,
Direct, 2 Unranked, a room's game and a 2v2), with spectator counts and the queue and in-game
counts. Every real account's friends are spread over it: Peachy in her friends-only room, one
friend in a live game, two in the launcher, four offline.

**What fails when clicked**, each with the server's usual answer: joining a fake P+ room (the game
says the room doesn't exist), joining a fake Brawl Mods room (its game isn't yours), watching a fake
game ("This game has ended."), and a message or invite to a fake ("That player is offline."). Friend
requests, profiles, the leaderboard and match history work as with real players.

`--remove` deletes the fake accounts and every row naming one (matches, reports, rating changes,
friends, requests, searches, rulesets), takes the fake sets' changes back out of the real accounts'
ratings and 2v2 ratings (as they were, if they played nothing since), recomputes ranks and Unranked
ratings, and deletes the state file, which turns the live fakes off at once.

## First-time notes

- Windows asks once whether `accounts.exe`, `mm.exe` and the staging `Dolphin.exe` may accept
  connections. Allow them on private networks, or the Mac can't reach the server or play against
  this PC.
- A fresh profile goes through the real first run: pick the Brawl ISO, install P+, sign up. Sign-up
  needs the verification link: `server mail` prints it, or `server verify <email>` skips it.
- P+ is downloaded once, not by every profile. `win run` / `mac run` give a profile without the
  launcher's pinned P+ release (a new profile, `--fresh`, or one on an older release) a hard link to
  the kept release zip in its `netplay/pplus/downloads/`, where the launcher looks for a download it
  can reuse: it checks the zip's sha256 and installs P+ from it (the install's own progress, no
  download), then deletes its link. The first time, `run` downloads the zip itself (1.9 GB) and keeps
  it. The release is the one `pplus_release.ts` pins in the commit that was built.
- The Mac connection settings (`mac_host`, `mac_user`, `mac_key`, `mac_qt`, `mac_ssh`) are in the
  config file. On Windows the Mac commands use Git's `ssh`/`scp` (`<Git>\usr\bin`), not Windows'
  own: its OpenSSH 9.5 stopped passing on a remote command's output after the first chunk and never
  exited, so `mac build` hung after the Mac had finished. `"mac_ssh"` names another `ssh.exe` (its
  `scp` is the one beside it); empty picks Git's, else the one on PATH.
- Start the staging commands (and the agent board's staging buttons) from the same kind of window
  each time. A server started from an administrator window can't be stopped from a normal one, so
  `win build` (which stops it to replace `accounts.exe`/`mm.exe`) says which pid to stop from an
  administrator PowerShell. `server up` finds a running `accounts.exe`/`mm.exe` from the staging
  ports when `pids.json` has lost it, and keeps it.
- A Visual Studio update reinstalls `cl.exe` and the Windows SDK in their same folders with their
  old file dates, and ninja doesn't notice: the old precompiled header fails every Dolphin object
  with C1853 ("from a different version of the compiler"), and old objects don't link with new ones
  (LNK2038 'C++/WinRT version'). `win build` stamps the compiler's and the SDKs' files in
  `staging-toolchain.txt` in Dolphin's build dir and, when they change, cleans the build dir first,
  so that build is a full one.

## Tear down

- `down`: stops the clients (here and on the Mac) and the server. The database stays.
- `win reset` / `mac reset [--profile b]`: deletes a profile (the next start is a fresh install).
- `server reset-db --yes`: deletes the staging database.
- `destroy --yes [--mac]`: everything. It unlinks the worktree's junctions before removing it,
  then deletes `run/staging`, and with `--mac` also `~/code/brawlonline.net/brawl-staging` on the Mac.
