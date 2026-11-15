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

## Builds

`--ref` is a commit, branch or tag (default `main`); only committed work is built.

- **Windows** (`win build`): checks out the commit in `run/staging/src`, links the submodules and
  `toolchains/` to this checkout's copies, builds the server (`cargo build --workspace`),
  Dolphin (the `ninja-release-x64` preset, as CI configures it), the plugin and the launcher. It
  then stages Dolphin as `build-dolphin-windows.ps1` does and runs `electron-builder --win dir`
  (no installer). Builds run at below-normal priority. The first build is a full Dolphin build;
  later ones are incremental.
- **Mac** (`mac build`): pushes the commit to `~/code/brawlonline.net/brawl-staging/repo.git` over
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
