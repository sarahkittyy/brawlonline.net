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
4. `macos` (Unraid runner, only when the repository variable `ENABLE_MACOS` is `true`): `setup-macos-toolchain.sh` (Qt, rcodesign, libdmg-hfsplus into `$CI_CACHE/toolchains/macos`), the same stamping, `build-dolphin-macos.sh` (osxcross cross-compile, Apple silicon) and `package-launcher-macos.sh` (ad-hoc signed app, updater zip, DMG, `latest-mac.yml`) → artifact `launcher-mac`. See "macOS" below. A failed or skipped macOS build does not hold back the Linux and Windows release.
5. `publish` (sarahvps2): downloads the `launcher-*` artifacts, `sudo pp-release client <dir>`, checks the feed on https://brawlonline.net, deletes the run's artifacts (they also expire after a day).

The Linux client runner is written in the `plugin` and `linux` jobs; the optional repository variable `CLIENT_LINUX_RUNNER` (a JSON list of labels) overrides both. It is meant to be the Unraid runner (`["self-hosted","linux","x64","unraid","brawlonline"]`), but is GitHub-hosted `ubuntu-24.04` for now (see "Unraid" below). On a GitHub-hosted runner the jobs install Dolphin's build packages with apt and keep ccache (`linux-ccache-*`) and the game-code toolchain in the Actions cache; on a self-hosted runner they use its image and `$CI_CACHE`.

The Linux and Windows jobs cost hosted minutes (private repository: 2,000 a month on the free plan, Windows minutes count double). They only run when the client paths change, and ccache plus the npm and Electron caches keep a rebuild short.

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

### Unraid: Linux client builds (on hold: unstable under load)

`unraid-brawlonline` (`docs/unraid-runner.md`): Ubuntu 24.04 image with Dolphin's build dependencies, Node 24, Python 3.12, Xvfb; `CI_CACHE=/cache` (ccache, the Dolphin build tree, toolchains, linuxdeploy, npm and Electron caches), `JOBS=12`.

**Not used yet, 2026-10-08:** compiles there fail at random under parallel load. The Linux Dolphin build stopped with GCC "internal compiler error: Segmentation fault" (in `ggc_set_mark`, the compiler's garbage collector) and even parse errors inside unchanged system headers (`atomic_base.h: expected '{' before '=' token`), on a different DolphinQt file each time, also when resumed with 8, 5 and 3 jobs. The same file compiled cleanly 3 times out of 3 when built alone; compiled 16 at a time, one per CPU (`taskset`), 4 of 80 compiles failed, on CPUs 3, 8, 10 and 15, without ccache. Data corruption on several cores points at the machine (memory or CPU stability, e.g. an EXPO/XMP memory profile or a Curve Optimizer/PBO undervolt), not at one bad core or the build; a memory test (MemTest86) and stock memory/CPU settings are the next step. A build from such a machine cannot be shipped: a corruption that does not crash the compiler ends up in the binary, and ccache would keep it. Once it is stable, set the default in `client.yml` back to the Unraid labels (one line in each of the two jobs) and clear `/cache/ccache` and `/cache/dolphin-build-linux` there first.

The Linux Dolphin is built against Ubuntu 24.04's glibc (2.39) either way: it runs on distributions at least that new (SteamOS 3.6+, Ubuntu 24.04+, Debian 13, Fedora 40+, Arch).

### macOS (cross-compiled on Unraid)

There is no Mac runner. The Unraid runner cross-compiles, the way legacycraft's release workflow does:

- **Toolchain.** In the runner image (`docs/unraid-runner.md`): LLVM 20 from apt.llvm.org (Apple's SDK 26.5 libc++ headers need clang 19 or newer; Ubuntu's clang 18 fails on `__builtin_ctzg`) and osxcross (llvm flavor, arm64 and x86_64, deployment target 12.0) with the SDK from the legacycraft folder, plus compiler-rt's darwin builtins. osxcross's `<triple>-ranlib` is a wrapper that drops Apple's `-no_warning_for_no_symbols -c`: CMake's Darwin rules always pass them, llvm-ranlib refuses them, and overriding `CMAKE_RANLIB` or `CMAKE_<LANG>_ARCHIVE_FINISH` doesn't stick (the platform files reset them). In `$CI_CACHE/toolchains/macos` (`setup-macos-toolchain.sh`): Qt 6.8.3 for macOS (universal frameworks, from Qt's online repository via aqtinstall) and the same Qt's Linux host tools (`QT_HOST_PATH`), rcodesign 0.29.0 and libdmg-hfsplus's `dmg`.
- **Dolphin** (`build-dolphin-macos.sh`): osxcross's cmake wrapper, Ninja, ccache, Release, `-DENABLE_VULKAN=OFF` (Dolphin builds MoltenVK with xcodebuild; Metal and OpenGL remain), `-DMACOS_CODE_SIGNING=OFF`. The bundle is renamed to what the launcher runs (`Dolphin.app/Contents/MacOS/Dolphin`, with `dolphin-tool` next to it), Qt's frameworks and the cocoa, style and SVG plugins are copied in (`qt.conf` points at `PlugIns`), and the app is ad-hoc signed with rcodesign and Dolphin's entitlements.
- **Launcher** (`package-launcher-macos.sh`): better-sqlite3's prebuilt macOS binary for our Electron version, `electron-builder --mac dir` with no signing and no native rebuild, the whole app ad-hoc signed with rcodesign, then the updater zip (symlinks kept), a DMG (genisoimage HFS hybrid compressed by `dmg`) and `latest-mac.yml`. `pp-release` already accepts these names and links the DMG as `/downloads/BrawlOnline.dmg`.
- **What ad-hoc signing means for players.** Apple silicon runs it. Gatekeeper blocks the first start until the user allows it (System Settings > Privacy & Security > Open Anyway). The launcher can't update itself in place: Squirrel.Mac checks the new version's signature against the running one, and ad-hoc signatures never match. A Developer ID certificate and notarization (secrets below) fix both.
- **Not yet:** Intel Macs (x86_64 is in the toolchain; it needs a second Dolphin and launcher build, or a universal one), Vulkan via a prebuilt MoltenVK, and a run on a real Mac.
- **The Unraid memory problem applies here too** (see "Unraid" above): keep `ENABLE_MACOS` off for releases until the box is stable.

### Windows

GitHub-hosted `windows-latest` (Visual Studio 18). Caches: `.ccache` (key `win-ccache-<run id>`, saved right after the Dolphin build), Electron downloads, npm. A rebuild takes about 35 minutes: ccache hits 99.9% of what it can cache, but 746 of Dolphin's ~2,000 compiles use Dolphin's own MSVC precompiled header (`Source/PCH`, `use_pch`), which ccache cannot cache. An option in Dolphin's CMake to build without `use_pch` would cut that; it is a Dolphin change, not done here.

## Verified end to end (2026-10-08)

- Server: run `server #1` deployed `20261008-618d212ddc` (backup, switch, restart, health check); `server #2` built the same binaries and changed nothing. Later pushes deployed by themselves.
- Client `#12` published 0.1.12, `#13` 0.1.13 (Linux and Windows, feeds checked by `pp-release` against sha512). The NSIS installer and the AppImage contain no `.md`, no game files, `LICENSE`, `NOTICE`, the plugin and the Dolphin bundle (Qt libraries bundled on Linux).
- On a Windows PC: 0.1.12 installed silently (`/S`, per user), started with a temporary profile (`PPO_TEST_MODE=1`, `PPO_USER_DATA_DIR`); it installed its Dolphin into the profile, downloaded P+ v3.2.0 (1.86 GB, sha256 verified, 2.5 min), extracted the SD card, both launcher DOLs (identical to a local P+ install's) and the 12 files of `Sys/NetplaySave`, deleted the zip, and showed the progress on Slippi's Play button. After 0.1.13 was published, its update check downloaded `Brawl-Online-Setup-0.1.13.exe`, "install update" ran the installer and restarted the launcher; the next start logged `Auto-update succeeded: version 0.1.13`, replaced the profile's Dolphin 0.1.12 with 0.1.13, put the save template back into `Sys/` and kept the P+ files. Then uninstalled (`/S /currentuser`).

## Secrets and settings

None are needed today. Signing is deferred; the builds are unsigned:

- Windows: add repository secrets `WIN_CSC_LINK` (base64 of the `.pfx`, or an https URL) and `WIN_CSC_KEY_PASSWORD`, then set `"signAndEditExecutable": true` in `launcher/electron-builder.json` (`win`). The windows job already passes them as `CSC_LINK`/`CSC_KEY_PASSWORD`.
- macOS: a Developer ID certificate and Apple's notary API key (`APPLE_API_KEY`, `APPLE_API_KEY_ID`, `APPLE_API_ISSUER`). The cross build signs with rcodesign, which can sign with a Developer ID (`--p12-file`) and notarize (`rcodesign notary-submit`) on Linux; `package-launcher-macos.sh` and `build-dolphin-macos.sh` sign ad-hoc until then.

Repository variables (optional, none set): `CLIENT_LINUX_RUNNER` (JSON list of labels, overrides the default in `client.yml`), `ENABLE_MACOS` (`true` runs the macOS job).

Artifacts: the account's free Actions storage is 500 MB and shared with other repositories; the client artifacts (~400 MB per run) live at most a day and are deleted by `publish` right after use.
