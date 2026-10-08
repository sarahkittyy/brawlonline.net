# CI/CD

GitHub Actions in the monorepo (`sarahkittyy/brawlonline.net`, `.github/workflows/`). Every
workflow runs only on a push to `main` or by hand (`workflow_dispatch`), never on pull requests:
the repository will be public, and two of the runners are self-hosted. Nothing here ships: docs
are excluded from every package.

| Workflow | Paths | Runs on | Does |
|---|---|---|---|
| `server.yml` | `server/**` except `server/deploy/**` and `*.md` | sarahvps2 | `cargo build --release` (nice 19, idle IO, `-j 2`, target dir kept in `~/ci-cache/server-target`, cleared above 3 GB), then `sudo pp-release deploy <binaries> <yyyymmdd>-<sha10>`: installs, backs up the database, switches, restarts `pp-accounts`/`pp-mm`, health-checks, rolls back by itself on failure, keeps 3 releases. Binaries equal to the active release's are not redeployed (nothing restarts). |
| `website.yml` | `website/**` except `README.md` | sarahvps2 | Stages `website/` without `*.md`, adds `?v=<sha>` to `script.js`, `style.css` and the video in `index.html` (Cloudflare and browsers cache those for 4 h; `index.html` is not cached), `sudo pp-release website <dir>`, then checks https://brawlonline.net/ serves it. |
| `client.yml` | `dolphin/**`, `launcher/**`, `game-code/**`, `tools/gamecode/**`, `NOTICE`, `.github/scripts/**` | see below | Version `0.1.<run number>`. Plugin, Dolphin (Linux + Windows), launcher tests and packages, then `sudo pp-release client <dir>`. |

## client.yml

1. `config` (sarahvps2): the Linux client runner labels, one line (or the repository variable `CLIENT_LINUX_RUNNER`, a JSON list).
2. `plugin` (Linux client runner): `tools/gamecode/setup_toolchain.py` (toolchain kept in `~/ci-cache/toolchains`), `game-code/build.sh` → artifact `plugin`.
3. `linux` (Linux client runner): `stamp-version.sh` (an annotated tag `v<version>` in the CI checkout, so Dolphin reports `Project+ Dolphin v<version>`; Dolphin's `Online::APP_VERSION`; `launcher/release/app/package.json`), `build-dolphin-linux.sh` (ccache in `~/ci-cache/ccache`, max 3 GB; build tree `~/ci-cache/dolphin-build-linux`; nice 19, idle IO, 3 jobs; linuxdeploy + its Qt plugin, pinned by sha256, make an AppDir with the libraries), launcher `npm ci`, build, `typecheck`, `npm test`, `electron-builder --linux` (AppImage), `check-package.sh` → artifact `launcher-linux` (`latest-linux.yml`, `*.AppImage`).
4. `windows` (GitHub-hosted `windows-latest`): same stamping, `build-dolphin-windows.ps1` (MSVC, the `ninja-release-x64` preset, ccache 4.14.1 pinned by sha256 with its cache in the Actions cache), `electron-builder --win` (NSIS) → artifact `launcher-windows` (`latest.yml`, `*.exe`, `*.exe.blockmap`).
5. `macos`: disabled (`ENABLE_MACOS` repository variable). Plan: osxcross on the Unraid runner (see the job's comment).
6. `publish` (sarahvps2): downloads the `launcher-*` artifacts, `sudo pp-release client <dir>`, checks the feed on https://brawlonline.net, deletes the run's artifacts (they also expire after a day).

The Windows job costs hosted minutes (private repository: Windows minutes count double). It only runs when the client paths change, and ccache plus the npm and Electron caches keep a rebuild short.

`check-package.sh` fails the build if a package contains any `.md` (also inside `app.asar`), game files (`.iso`, `.raw`, `.dol`, ...) or `Sys/NetplaySave`, or lacks `LICENSE`, `NOTICE`, the plugin, the Dolphin bundle or Dolphin's `COPYING`.

### What a release contains

- The launcher with `resources/dolphin/` (our Dolphin: Windows `Dolphin.exe` + DLLs + `Sys/`; Linux an AppDir `usr/bin/project-plus-dolphin` with `usr/lib/` and `usr/bin/Sys/`), `resources/plugins/PPOnline.{rel,json}`, `LICENSE`, `NOTICE`.
- No Brawl or P+ files: the launcher downloads P+'s pinned official release at setup and takes the SD card, the launcher DOLs and `Sys/NetplaySave` from it (`launcher/src/dolphin/install/pplus_release.ts`, PPLUS_PORTING.md section 3).

### Updates

The launcher's electron-updater feed is the generic provider at https://brawlonline.net/updates/launcher (`latest.yml`, `latest-linux.yml`, the installers and blockmaps). `pp-release client` checks every file a feed names against the feed's size and sha512, installs the files first and the feeds last, keeps the newest 3 versions, and hard-links the current ones to the stable names the website uses: `/downloads/BrawlOnline-Setup.exe`, `/downloads/BrawlOnline.AppImage` (and later `/downloads/BrawlOnline.dmg`). Installed launchers find the update at start-up (Slippi's flow: download, "restart to update", or install on quit). The bundled Dolphin and plugin update with the launcher: on the next start the launcher reinstalls the newer Dolphin into `<userData>/netplay`.

Cloudflare caches `.exe` (not `.yml` or `.AppImage`) for up to a few hours, so `/downloads/BrawlOnline-Setup.exe` may serve the previous version for a while after a release. That is harmless: the installed launcher updates itself on its first start. The versioned files the feed names are never cached stale (new names each version).

Versions: `0.1.<run number of client.yml>`. `LATEST_VERSION` in `/etc/ppserver/accounts.env` (written into `user.json`; a Dolphin older than it shows Slippi's "update required") and `MM_MIN_APP_VERSION` in `mm.env` are not touched by CI; raise them by hand when an old client must be refused.

## Runners

### sarahvps2 (production box)

Runner `sarahvps2-brawlonline`, labels `self-hosted, linux, x64, sarahvps2, brawlonline`, service `actions.runner.sarahkittyy-brawlonline.net.sarahvps2-brawlonline`, user `brawlrunner` (unprivileged; home `/home/brawlrunner`, runner in `~/actions-runner`). Its only sudo rule is `/etc/sudoers.d/brawlrunner` (copy in `server/deploy/sarahvps2/sudoers.d-brawlrunner`):

```
brawlrunner ALL=(root) NOPASSWD: /usr/local/sbin/pp-release
```

`pp-release` (`server/deploy/sarahvps2/pp-release`) only takes source folders under `/home/brawlrunner/actions-runner/_work` (or `/home/debian` for manual use), reads them with the caller's own permissions (`runuser`), validates everything in a root-only staging folder (`/var/tmp/pp-release`) and serialises on `/run/pp-release.lock`. It only restarts `pp-accounts` and `pp-mm`.

Each workflow checks out into its own folder of the shared runner workspace (`srv/`, `web/`, `client/`). Caches in `/home/brawlrunner/ci-cache`: `server-target/` (≤ 3 GB), `ccache/` (3 GB max), `dolphin-build-linux/` (~2 GB), `toolchains/`, `tools/` (linuxdeploy); plus `~/.npm`, `~/.cache/electron*`, `~/.cargo`, `~/.rustup`. Free disk was about 20 GB when this was set up; check with `df -h /` and `du -sh ~brawlrunner/ci-cache/*`. To reclaim space: `sudo rm -rf ~brawlrunner/ci-cache/dolphin-build-linux` (the next build is a full one, about an hour at nice 19).

Packages installed for the Linux Dolphin build (2026-10-08, `apt-get install --no-install-recommends`; nothing was upgraded): `cmake ninja-build ccache pkg-config gettext qt6-base-dev qt6-base-private-dev qt6-svg-dev qmake6 libavcodec-dev libavformat-dev libavutil-dev libswscale-dev libxi-dev libxrandr-dev libudev-dev libevdev-dev libsfml-dev libminiupnpc-dev libmbedtls-dev libcurl4-openssl-dev libhidapi-dev libsystemd-dev libbluetooth-dev libasound2-dev libpulse-dev libpugixml-dev libbz2-dev libzstd-dev liblzo2-dev libpng-dev libusb-1.0-0-dev libgl-dev libegl-dev file`. Rust is installed by the server workflow into `~brawlrunner/.cargo` (rustup, stable). Node comes from `actions/setup-node`.

The Linux Dolphin is built against Debian 13's glibc (2.41), so it needs a distribution at least that new (SteamOS 3.7+, Arch, Fedora 42+, Ubuntu 25.04+). Building in an older container on the Unraid runner would widen that.

### Moving the Linux client builds to the Unraid runner

Change the one line in `client.yml`'s `config` job (or set the repository variable `CLIENT_LINUX_RUNNER` to `["self-hosted","linux","x64","unraid","brawlonline"]`). The Unraid runner needs the same packages (or a container with them), Python 3, git and curl; the scripts keep their caches in `$CI_CACHE` (default `~/ci-cache`). Publishing stays on sarahvps2.

### Windows

GitHub-hosted `windows-latest`. Caches: `.ccache` (key `win-ccache-<sha>`), Electron downloads, npm.

## Secrets and settings

None are needed today. Signing is deferred; the builds are unsigned:

- Windows: add repository secrets `WIN_CSC_LINK` (base64 of the `.pfx`, or an https URL) and `WIN_CSC_KEY_PASSWORD`, then set `"signAndEditExecutable": true` in `launcher/electron-builder.json` (`win`). The windows job already passes them as `CSC_LINK`/`CSC_KEY_PASSWORD`.
- macOS (when the job exists): `CSC_LINK`, `CSC_KEY_PASSWORD`, `APPLE_API_KEY`, `APPLE_API_KEY_ID`, `APPLE_API_ISSUER`, and `"notarize": true`.

Repository variables (optional): `CLIENT_LINUX_RUNNER` (JSON list of labels), `ENABLE_MACOS` (`true` once a macOS build exists).

Artifacts: the account's free Actions storage is 500 MB and shared with other repositories; the client artifacts (~400 MB per run) live at most a day and are deleted by `publish` right after use.
