# CI/CD

GitHub Actions in the monorepo (`sarahkittyy/brawlonline.net`, `.github/workflows/`). Every
workflow runs only on a push to `main` or by hand (`workflow_dispatch`), never on pull requests:
the repository will be public, and two of the runners are self-hosted. Nothing here ships: docs
are excluded from every package.

| Workflow | Paths | Runs on | Does |
|---|---|---|---|
| `server.yml` | `server/**` except `server/deploy/**` and `*.md` | sarahvps2 | `cargo build --release` (nice 19, idle IO, `-j 2`, target dir kept in `~/ci-cache/server-target`, cleared above 3 GB), then `sudo pp-release deploy <binaries> <yyyymmdd>-<sha10>`: installs, backs up the database, switches, restarts `pp-accounts`/`pp-mm`, health-checks, rolls back by itself on failure, keeps 3 releases. Binaries equal to the active release's are not redeployed (nothing restarts). |
| `website.yml` | `website/**` except `README.md` | sarahvps2 | Stages `website/` without `*.md`, adds `?v=<sha>` to `script.js`, `style.css` and the video in `index.html` (Cloudflare and browsers cache those for 4 h; `index.html` is not cached), `sudo pp-release website <dir>`, then checks https://brawlonline.net/ serves it. |
| `client.yml` | `dolphin/**`, `launcher/**`, `game-code/**`, `tools/gamecode/**`, `NOTICE`, `.github/scripts/**` | Unraid (builds), GitHub Windows, sarahvps2 (publish) | Version `0.1.<run number>`. Plugin, Dolphin (Linux + Windows), launcher tests and packages, then `sudo pp-release client <dir>`. |

## client.yml

1. `plugin` (Linux client runner): `tools/gamecode/setup_toolchain.py` (toolchain kept in `$CI_CACHE/toolchains`), `game-code/build.sh` → artifact `plugin`.
2. `linux` (Linux client runner): `stamp-version.sh` (an annotated tag `v<version>` in the CI checkout, so Dolphin reports `Project+ Dolphin v<version>`; Dolphin's `Online::APP_VERSION`; `launcher/release/app/package.json`), `build-dolphin-linux.sh` (ccache and the build tree in `$CI_CACHE`; linuxdeploy + its Qt plugin, pinned by sha256, make an AppDir with the libraries), launcher `npm ci`, build, `typecheck`, `npm test`, `electron-builder --linux` (AppImage), `check-package.sh` → artifact `launcher-linux` (`latest-linux.yml`, `*.AppImage`).
3. `windows` (GitHub-hosted `windows-latest`): same stamping, `build-dolphin-windows.ps1` (MSVC, the `ninja-release-x64` preset, ccache 4.14.1 pinned by sha256 with its cache in the Actions cache), `electron-builder --win` (NSIS) → artifact `launcher-windows` (`latest.yml`, `*.exe`, `*.exe.blockmap`).
4. `macos`: disabled (`ENABLE_MACOS` repository variable). Plan: osxcross on the Unraid runner, which has macOS SDKs (see the job's comment).
5. `publish` (sarahvps2): downloads the `launcher-*` artifacts, `sudo pp-release client <dir>`, checks the feed on https://brawlonline.net, deletes the run's artifacts (they also expire after a day).

The Linux client runner is the Unraid runner (`unraid-brawlonline`, labels `self-hosted, linux, x64, unraid, brawlonline`, `docs/unraid-runner.md`). The default is written in the `plugin` and `linux` jobs; the optional repository variable `CLIENT_LINUX_RUNNER` (a JSON list of labels) overrides both.

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

### sarahvps2 (production box): server builds and publishing only

Runner `sarahvps2-brawlonline`, labels `self-hosted, linux, x64, sarahvps2, brawlonline`, service `actions.runner.sarahkittyy-brawlonline.net.sarahvps2-brawlonline`, user `brawlrunner` (unprivileged; home `/home/brawlrunner`, runner in `~/actions-runner`). It runs `server.yml`, `website.yml` and `client.yml`'s `publish` job. Its only sudo rule is `/etc/sudoers.d/brawlrunner` (copy in `server/deploy/sarahvps2/sudoers.d-brawlrunner`):

```
brawlrunner ALL=(root) NOPASSWD: /usr/local/sbin/pp-release
```

`pp-release` (`server/deploy/sarahvps2/pp-release`) only takes source folders under `/home/brawlrunner/actions-runner/_work` (or `/home/debian` for manual use), reads them with the caller's own permissions (`runuser`), validates everything in a root-only staging folder (`/var/tmp/pp-release`) and serialises on `/run/pp-release.lock`. It only restarts `pp-accounts` and `pp-mm`.

Each workflow checks out into its own folder of the shared runner workspace (`srv/`, `web/`). Kept between runs: `/home/brawlrunner/ci-cache/server-target` (cleared above 3 GB), `~brawlrunner/.cargo` and `~brawlrunner/.rustup` (rustup stable, installed by the first server run). About 18 GB of disk were free after setup; check with `df -h /`.

Dolphin's Linux build packages were installed here on 2026-10-08 for a first plan (client builds on this box) and purged again the same day, exactly the 247 packages of that apt transaction, once the Unraid runner took the client builds. Do not point `CLIENT_LINUX_RUNNER` at sarahvps2.

### Unraid: Linux client builds

`unraid-brawlonline` (`docs/unraid-runner.md`): Ubuntu 24.04 image with Dolphin's build dependencies, Node 24, Python 3.12, Xvfb; `CI_CACHE=/cache` (ccache, the Dolphin build tree, toolchains, linuxdeploy, npm and Electron caches), `JOBS=12`. The Linux Dolphin is therefore built against Ubuntu 24.04's glibc (2.39): it runs on distributions at least that new (SteamOS 3.6+, Ubuntu 24.04+, Debian 13, Fedora 40+, Arch).

### Windows

GitHub-hosted `windows-latest`. Caches: `.ccache` (key `win-ccache-<sha>`), Electron downloads, npm.

## Secrets and settings

None are needed today. Signing is deferred; the builds are unsigned:

- Windows: add repository secrets `WIN_CSC_LINK` (base64 of the `.pfx`, or an https URL) and `WIN_CSC_KEY_PASSWORD`, then set `"signAndEditExecutable": true` in `launcher/electron-builder.json` (`win`). The windows job already passes them as `CSC_LINK`/`CSC_KEY_PASSWORD`.
- macOS (when the job exists): `CSC_LINK`, `CSC_KEY_PASSWORD`, `APPLE_API_KEY`, `APPLE_API_KEY_ID`, `APPLE_API_ISSUER`, and `"notarize": true`.

Repository variables (optional, none set): `CLIENT_LINUX_RUNNER` (JSON list of labels, overrides the Unraid default), `ENABLE_MACOS` (`true` once a macOS build exists).

Artifacts: the account's free Actions storage is 500 MB and shared with other repositories; the client artifacts (~400 MB per run) live at most a day and are deleted by `publish` right after use.
