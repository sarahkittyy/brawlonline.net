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
python tools/staging/staging.py status
python tools/staging/staging.py down                 # stop clients and server; the database stays
```

## What it sets up

| | Where | Lifetime |
|---|---|---|
| Staging database | `run/staging/pgdata` (the portable Postgres from `run/postgres-portable`, 127.0.0.1:54339) | kept; `server reset-db --yes` empties it |
| accounts | `http://<this PC's LAN IP>:18080` | `server up` / `server down` |
| mm | UDP `<LAN IP>:43213` | same |
| Mail | `run/staging/mail.jsonl` (MAILER=file); `server mail` prints the links | kept |
| Windows build | worktree `run/staging/src` (own Dolphin build), packaged to `run/staging/win/app` | rebuilt by `win build` |
| Server binaries | `run/staging/cargo-target`, built from the same commit by `win build` | same |
| Windows profiles | `run/staging/win/profiles/<name>` | `win reset` / `run --fresh` delete one |
| Mac build | `~/brawl-staging/src`, app in `~/brawl-staging/app/Brawl Online Staging.app` | rebuilt by `mac build` |
| Mac profiles | `~/brawl-staging/profiles/<name>` | `mac reset` / `run --fresh` |

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
- **Mac** (`mac build`): pushes the commit to `~/brawl-staging/repo.git` over SSH, copies the
  plugin built here, and runs CI's `build-dolphin-macos.sh` and `package-launcher-macos.sh` on the
  Mac without a signing identity (ad-hoc signed, not notarized). Qt comes from
  `~/brawl-dev/qt/6.8.3/macos`, and Node 24 from your nvm. Dolphin is built with Homebrew hidden, as
  on CI: Homebrew's Qt 6.11 had leaked in through an rpath and made Dolphin abort at start, and a
  build that still refers to `/opt/homebrew` is refused. `mac setup` checks these and writes a
  pass-through `ccache` into `~/brawl-staging/bin` (nothing is installed).
- The Dolphin bundle's version is `0.1.0-staging.<commit>`, so a new build replaces the Dolphin an
  existing profile installed.

Logs: `run/staging/logs/` (`win-build.log`, `mac-build.log`, `accounts.log`, `mm.log`,
`postgres.log`).

## First-time notes

- Windows asks once whether `accounts.exe`, `mm.exe` and the staging `Dolphin.exe` may accept
  connections. Allow them on private networks, or the Mac can't reach the server or play against
  this PC.
- A fresh profile goes through the real first run: pick the Brawl ISO, download P+, sign up. Sign-up
  needs the verification link: `server mail` prints it, or `server verify <email>` skips it.
- The Mac connection settings (`mac_host`, `mac_user`, `mac_key`, `mac_qt`) are in the config file.

## Tear down

- `down`: stops the clients (here and on the Mac) and the server. The database stays.
- `win reset` / `mac reset [--profile b]`: deletes a profile (the next start is a fresh install).
- `server reset-db --yes`: deletes the staging database.
- `destroy --yes [--mac]`: everything. It unlinks the worktree's junctions before removing it,
  then deletes `run/staging`, and with `--mac` also `~/brawl-staging` on the Mac.
