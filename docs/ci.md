# CI/CD

GitHub Actions in the monorepo (`sarahkittyy/brawlonline.net`, `.github/workflows/`). Every
workflow runs only on a push to `main` or by hand (`workflow_dispatch`), never on pull requests:
the repository will be public, and two of the runners are self-hosted. Nothing here ships: docs
are excluded from every package.

| Workflow | Paths | Runs on | Does |
|---|---|---|---|
| `server.yml` | `server/**` except `server/deploy/**` and `*.md` | sarahvps2 | `cargo build --release` (nice 19, idle IO, `-j 2`, target dir kept in `~/ci-cache/server-target`, cleared above 3 GB), then `sudo pp-release deploy <binaries> <yyyymmdd>-<sha10>`: installs, backs up the database, switches, restarts `pp-accounts`/`pp-mm`, health-checks, rolls back by itself on failure, keeps 3 releases. Binaries equal to the active release's are not redeployed (nothing restarts). |
| `website.yml` | `website/**` except `README.md` | sarahvps2 | Stages `website/` without `*.md`, adds `?v=<sha>` to `script.js`, `style.css` and the video in `index.html` (Cloudflare and browsers cache those for 4 h; `index.html` is not cached), `sudo pp-release website <dir>`, then checks https://brawlonline.net/ serves it. |
| `client.yml` | `dolphin/**`, `launcher/**`, `game-code/**`, `tools/gamecode/**`, `NOTICE`, `.github/scripts/**` | GitHub-hosted Linux and Windows (builds), sarahvps2 (publish) | Version `0.1.<run number>`. Plugin, Dolphin (Linux + Windows), launcher tests and packages, then `sudo pp-release client <dir>`. |

## client.yml

1. `plugin` (Linux client runner): `tools/gamecode/setup_toolchain.py` (toolchain kept in `$CI_CACHE/toolchains`), `game-code/build.sh` → artifact `plugin`.
2. `linux` (Linux client runner): `stamp-version.sh` (an annotated tag `v<version>` in the CI checkout, so Dolphin reports `Project+ Dolphin v<version>`; Dolphin's `Online::APP_VERSION`; `launcher/release/app/package.json`), `build-dolphin-linux.sh` (ccache and the build tree in `$CI_CACHE`; linuxdeploy + its Qt plugin, pinned by sha256, make an AppDir with the libraries), launcher `npm ci`, build, `typecheck`, `npm test`, `electron-builder --linux` (AppImage), `check-package.sh` → artifact `launcher-linux` (`latest-linux.yml`, `*.AppImage`).
3. `windows` (GitHub-hosted `windows-latest`): same stamping, `build-dolphin-windows.ps1` (MSVC, the `ninja-release-x64` preset, ccache 4.14.1 pinned by sha256 with its cache in the Actions cache), `electron-builder --win` (NSIS) → artifact `launcher-windows` (`latest.yml`, `*.exe`, `*.exe.blockmap`).
4. `macos` (GitHub-hosted `macos-15`, Apple silicon; only while the repository is public, and the repository variable `DISABLE_MACOS` = `true` skips it): the same stamping, `build-dolphin-macos.sh` (native build, Qt from aqtinstall, `macdeployqt`, Developer ID `codesign`) and `package-launcher-macos.sh` (Developer ID signed, notarized and stapled app; signed and notarized DMG; updater zip; `latest-mac.yml`) → artifact `launcher-mac`. See "macOS" below. A failed or skipped macOS build does not hold back the Linux and Windows release.
5. `publish` (sarahvps2): downloads the `launcher-*` artifacts, `sudo pp-release client <dir>`, checks the feed on https://brawlonline.net, deletes the run's artifacts (they also expire after a day).

The Linux client runner is written in the `plugin` and `linux` jobs; the optional repository variable `CLIENT_LINUX_RUNNER` (a JSON list of labels) overrides both. The default is GitHub-hosted `ubuntu-24.04`. On a GitHub-hosted runner the jobs install Dolphin's build packages with apt and keep ccache (`linux-ccache-*`) and the game-code toolchain in the Actions cache; on a self-hosted runner they use its image and `$CI_CACHE`.

The Linux and Windows jobs cost hosted minutes while the repository is private (2,000 a month on the free plan, Windows minutes count double, macOS ten times); on a public repository GitHub-hosted runners are free. They only run when the client paths change, and ccache plus the npm and Electron caches keep a rebuild short.

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

It is the production box (6 cores, little free disk), so the client builds stay on GitHub-hosted runners; don't point `CLIENT_LINUX_RUNNER` at it.

### Linux

The Linux Dolphin is built against Ubuntu 24.04's glibc (2.39) either way: it runs on distributions at least that new (SteamOS 3.6+, Ubuntu 24.04+, Debian 13, Fedora 40+, Arch).

### macOS

GitHub-hosted `macos-15` (Apple silicon), a native build. It only runs while the repository is public: macOS minutes are free there and count ten times on a private repository.

- **Dolphin** (`build-dolphin-macos.sh`): Qt 6.8.3 for macOS from Qt's online repository via aqtinstall (cached in the Actions cache), Ninja and ccache from Homebrew, Release, `-DENABLE_VULKAN=OFF` (Metal and OpenGL remain; MoltenVK can come later), `-DMACOS_CODE_SIGNING=OFF`. The bundle is renamed to what the launcher runs (`Dolphin.app/Contents/MacOS/Dolphin`, with `dolphin-tool` next to it), `macdeployqt` deploys Qt's frameworks and plugins, and Dolphin's own `Tools/mac-codesign.sh` signs it with the Developer ID (hardened runtime, secure timestamp, Dolphin's entitlements).
- **Launcher** (`package-launcher-macos.sh`): `electron-builder --mac dir` without signing, `app-update.yml` added, then `@electron/osx-sign` signs every Mach-O file inside out and then the app (Developer ID, hardened runtime, secure timestamp, `assets/entitlements.mac.plist`); Dolphin.app inside keeps its own signature and entitlements. The app is notarized (`notarytool`, an App Store Connect API key, at most 40 minutes per submission; the log shows the submission ID right after the upload and Apple's status every 30 seconds, so `notarytool info <id>` can look up a slow one) and stapled, `spctl` must accept it, then `--prepackaged` makes the DMG (signed by electron-builder), the updater zip and `latest-mac.yml` from that app. The DMG is notarized too but not stapled (stapling would change it after `latest-mac.yml` recorded its sha512; the app inside carries its own ticket). `pp-release` accepts these names and links the DMG as `/downloads/BrawlOnline.dmg`.
- **Signing in CI.** The step "Signing keychain" decodes `MACOS_SIGNING_P12` into a throwaway keychain (random password, unlocked for the job, first in the search list), exports the identity's SHA-1 as `MAC_SIGN_IDENTITY` and writes the API key to `$RUNNER_TEMP`; the last steps delete both. It runs after `npm ci` and the launcher build. Without the secrets the job fails: the launcher updates itself in place on macOS (`MAC_SELF_UPDATE` in `launcher/src/common/product.ts`, through Squirrel.Mac, which requires the new version's signature to match the running one's), so an ad-hoc build must never reach the feed. Without `MAC_SIGN_IDENTITY` the two scripts still sign ad-hoc, for local test builds.
- **For players.** Gatekeeper opens the app without a prompt (notarized, Developer ID "Sarah Ohlin"), and the launcher updates itself like on Windows and Linux.
- **Not yet:** Intel Macs (a second build on `macos-13`, or a universal one), Vulkan via MoltenVK, and the first run (the next client push to `main` since the repository went public).
- Dolphin also builds when cross-compiled from Linux with osxcross (the fixes are in Dolphin: `ScmRevGen.cmake`, `DolphinInjectVersionInfo.cmake` without PlistBuddy, the hidapi rename), but CI doesn't use that.

### Windows

GitHub-hosted `windows-latest` (Visual Studio 18). Caches: `.ccache` (key `win-ccache-<run id>`, saved right after the Dolphin build), Electron downloads, npm. Dolphin is configured with `DOLPHIN_MSVC_PCH=OFF` (our option in `dolphin/Source/PCH/CMakeLists.txt`, default ON): `pch.h` is still force-included (`/FI`) into the 746 `use_pch` compiles, but as an ordinary header, without `/Yu`, which ccache cannot cache. With Dolphin's precompiled header a rebuild took about 35 minutes even with ccache hitting 99.9% of what it could cache, since those 746 of ~2,000 compiles always ran. Now all of them are cacheable; a build from a cold cache (or after a change to a header most files include) is slower than with the precompiled header.

## Verified end to end (2026-10-08)

- Server: run `server #1` deployed `20261008-618d212ddc` (backup, switch, restart, health check); `server #2` built the same binaries and changed nothing. Later pushes deployed by themselves.
- Client `#12` published 0.1.12, `#13` 0.1.13 (Linux and Windows, feeds checked by `pp-release` against sha512). The NSIS installer and the AppImage contain no `.md`, no game files, `LICENSE`, `NOTICE`, the plugin and the Dolphin bundle (Qt libraries bundled on Linux).
- On a Windows PC: 0.1.12 installed silently (`/S`, per user), started with a temporary profile (`PPO_TEST_MODE=1`, `PPO_USER_DATA_DIR`); it installed its Dolphin into the profile, downloaded P+ v3.2.0 (1.86 GB, sha256 verified, 2.5 min), extracted the SD card, both launcher DOLs (identical to a local P+ install's) and the 12 files of `Sys/NetplaySave`, deleted the zip, and showed the progress on Slippi's Play button. After 0.1.13 was published, its update check downloaded `Brawl-Online-Setup-0.1.13.exe`, "install update" ran the installer and restarted the launcher; the next start logged `Auto-update succeeded: version 0.1.13`, replaced the profile's Dolphin 0.1.12 with 0.1.13, put the save template back into `Sys/` and kept the P+ files. Then uninstalled (`/S /currentuser`).

## Secrets and settings

Windows signing is deferred; the Windows build is unsigned:

- Windows: add repository secrets `WIN_CSC_LINK` (base64 of the `.pfx`, or an https URL) and `WIN_CSC_KEY_PASSWORD`, The windows job already passes them as `CSC_LINK`/`CSC_KEY_PASSWORD`, and `"signAndEditExecutable"` is already `true` in `launcher/electron-builder.json` (`win`): without a certificate it only writes the icon and version info into the exe.

macOS (Apple Developer Program, the `macos` job):

| Secret | Value |
|---|---|
| `MACOS_SIGNING_P12` | base64 of a `.p12` with the Developer ID Application certificate, its private key and Apple's Developer ID intermediate |
| `MACOS_SIGNING_P12_PASSWORD` | the `.p12`'s password |
| `APPLE_API_KEY_P8` | the text of the App Store Connect API key (`AuthKey_<id>.p8`, Developer role) |
| `APPLE_API_KEY_ID` | its key ID |
| `APPLE_API_ISSUER` | the issuer ID shown above the team keys |

The certificate's private key was made on the Mac and lives in its login keychain (`codesign --sign "Developer ID Application: …"` works there for any app; `xcrun notarytool … --keychain-profile notary` uses the stored API key). The certificate (team `CNM6N64AS4`, issued by Apple's G2 Developer ID CA) expires on 2031-09-17: make a new one from a new CSR, import it on the Mac, and replace `MACOS_SIGNING_P12` and its password. Revoking the API key in App Store Connect only stops notarization; make a new key and replace the three `APPLE_API_*` secrets.

Repository variables (optional, none set): `CLIENT_LINUX_RUNNER` (JSON list of labels, overrides the default in `client.yml`), `DISABLE_MACOS` (`true` skips the macOS job).

Artifacts: the account's free Actions storage is 500 MB and shared with other repositories; the client artifacts (~400 MB per run) live at most a day and are deleted by `publish` right after use.
