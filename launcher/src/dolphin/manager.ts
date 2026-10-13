import { Preconditions } from "@common/preconditions";
import { DOLPHIN_ONLINE_DIR, PRODUCT_NAME } from "@common/product";
import { readTestMode } from "@common/test_mode";
import type { SettingsManager } from "@settings/settings_manager";
import type { MessageBoxOptions } from "electron";
import { app, BrowserWindow, dialog } from "electron";
import electronLog from "electron-log";
import { existsSync } from "fs";
import { readdir } from "fs/promises";
import { Observable, Subject } from "observable-fns";
import path from "path";
import { fileExists } from "utils/file_exists";
import { isSubdirectory } from "utils/is_subdirectory";

import { isAbnormalExit, keepCrashLog } from "./crash_log";
import { brawlSaveDir, ensureBrawlSave } from "./install/brawl_save";
import { bundledDolphinSource, installBundledDolphin } from "./install/bundled_dolphin";
import { fetchLatestVersion } from "./install/fetch_latest_version";
import { LocalDolphinInstallation } from "./install/local_installation";
import type { GameFilesTarget } from "./install/move_game_files";
import { chooseGameFilesTarget, GameFilesMoveError, moveGameFiles, removeGameFiles } from "./install/move_game_files";
import type { DolphinPathEnv } from "./install/paths";
import {
  defaultDolphinExecutable,
  defaultNetplayFolder,
  defaultPluginLocation,
  defaultUserTemplate,
  launchTypeFolder,
  netplayFolder,
  patchedSdCardFolder,
  projectPlusStoreFolder,
} from "./install/paths";
import type { PPlusProgress, PPlusTarget } from "./install/pplus_release";
import {
  ensureNetplaySave,
  installProjectPlusFiles,
  missingProjectPlusFiles,
  NETPLAY_SAVE_DIR,
  netplaySaveStore,
  PPLUS_RELEASE,
} from "./install/pplus_release";
import { installPluginOnSdCard, loadPlugin } from "./install/sd_card";
import { DolphinInstance, MacOsRosettaRequiredError, PlaybackDolphinInstance } from "./instance";
import { buildNetplayDolphinArgs, buildPlaybackDolphinArgs } from "./netplay_args";
import { clearRoomRequest } from "./room_handoff";
import { createProgressThrottle, runWithProgress } from "./setup_progress";
import type { DolphinEvent, DolphinSetupPhase, ReplayCommunication } from "./types";
import { DolphinErrorType, DolphinEventType, DolphinLaunchType } from "./types";

/** Progress events are sent at most this often per launch type (a 2 GB step has thousands of chunks). */
const PROGRESS_INTERVAL_MS = 100;

const PPLUS_PHASES: Record<PPlusProgress["phase"], DolphinSetupPhase> = {
  download: "downloadProjectPlus",
  verify: "verifyProjectPlus",
  extract: "extractProjectPlus",
};

const log = electronLog.scope("dolphin/manager");

// DolphinManager should be in control of all dolphin instances that get opened for actual use.
// This includes playing netplay, viewing replays and configuring Dolphin.
//
// Differences from Slippi: one Dolphin build (ours) serves both netplay and playback; it ships
// inside the launcher package and is installed into userData/netplay from there (so it updates
// with the launcher) instead of being downloaded; P+'s own files (SD card, launcher DOLs, Brawl
// save template) are downloaded from P+'s official release at setup, with Slippi's Dolphin
// download progress; and Play boots P+'s netplay launcher DOL with the Brawl disc as Dolphin's
// default ISO, the way P+ does.
export class DolphinManager {
  private playbackDolphinInstances = new Map<string, PlaybackDolphinInstance>();
  private netplayDolphinInstance: DolphinInstance | null = null;
  private versionCache = new Map<string, string>();
  private bundledInstall: Promise<void> | null = null;
  private projectPlusInstall: Promise<void> | null = null;
  /** True while Settings moves the game files (moveGameFiles): nothing may start Dolphin. */
  private movingGameFiles = false;
  private eventSubject = new Subject<DolphinEvent>();
  private shouldSendProgress = createProgressThrottle(PROGRESS_INTERVAL_MS);
  /** Runs before a new Dolphin starts and throws to stop it (the launcher's update check). */
  private launchGate: () => Promise<void> = async () => undefined;
  events = Observable.from(this.eventSubject);

  constructor(private settingsManager: SettingsManager) {}

  /** No new Dolphin (Play, replays, Configure) starts while `gate` throws. A running one is left alone. */
  setLaunchGate(gate: () => Promise<void>): void {
    this.launchGate = gate;
  }

  /** True while the netplay Dolphin the launcher started (Play, or Configure) is running. */
  isNetplayRunning(): boolean {
    return this.netplayDolphinInstance != null;
  }

  /** The netplay User folder's online folder (`user.json`, the room hand-off files). */
  netplayOnlineDir(): string {
    return path.join(this.getInstallation(DolphinLaunchType.NETPLAY).userFolder, DOLPHIN_ONLINE_DIR);
  }

  /** The netplay folder: the bundled Dolphin, its User folder, P+'s files (paths.ts). */
  netplayFolder(): string {
    return netplayFolder(app.getPath("userData"), this.settingsManager.get().settings.gameFilesFolder);
  }

  private _pathEnv(): DolphinPathEnv {
    return {
      env: process.env,
      isDevelopment: !app.isPackaged,
      devRepoRoot: process.cwd(),
      netplayFolder: this.netplayFolder(),
    };
  }

  /** The configured executable, or the default for this machine. */
  getDolphinExecutablePath(launchType: DolphinLaunchType): string {
    return this.settingsManager.getDolphinPath(launchType) ?? defaultDolphinExecutable(launchType, this._pathEnv());
  }

  getInstallation(launchType: DolphinLaunchType): LocalDolphinInstallation {
    const exePath = this.getDolphinExecutablePath(launchType);
    const portable = existsSync(path.join(path.dirname(exePath), "portable.txt"));
    // Only the netplay User folder is seeded with P+'s files (2 GB SD card). Playback has
    // nothing to play until our replay format exists, so it gets a plain User folder.
    const template = launchType === DolphinLaunchType.NETPLAY ? defaultUserTemplate(this._pathEnv()) : null;
    const dataFolder =
      launchType === DolphinLaunchType.NETPLAY
        ? this.netplayFolder()
        : path.join(app.getPath("userData"), launchTypeFolder(launchType));
    return new LocalDolphinInstallation(launchType, exePath, dataFolder, template, portable);
  }

  /**
   * Slippi downloads or updates its Dolphin here. We check the configured build
   * and prepare its User folder; the update channel hook is a stub for now.
   */
  async installDolphin(dolphinType: DolphinLaunchType): Promise<void> {
    const useBeta = false;
    const update = await fetchLatestVersion(dolphinType, useBeta).catch((err) => {
      log.warn(`Could not check for Dolphin updates: ${err}`);
      return null;
    });
    if (update) {
      log.info(`Dolphin ${update.version} is available, but installing updates is not implemented yet`);
    }

    const dolphinInstall = this.getInstallation(dolphinType);
    try {
      await this._installBundledDolphin();
      if (dolphinType === DolphinLaunchType.NETPLAY) {
        await this.ensureNetplayUserFolder();
        await this._ensureProjectPlusFiles(dolphinInstall);
      }
      await dolphinInstall.validate({
        onStart: () => this._onStart(dolphinType),
        onProgress: (current, total) => this._onProgress(dolphinType, current, total),
        onComplete: () => undefined,
        dolphinDownloadInfo: { version: "", downloadUrls: { darwin: "", linux: "", win32: "" } },
      });
    } catch (err) {
      log.error(err);
      // Still mark it as ready so Play shows the real error (missing executable) when pressed.
      this._onComplete(dolphinType, undefined);
      throw err;
    }

    const isoPath = this.settingsManager.get().settings.isoPath;
    if (isoPath) {
      const gameDir = path.dirname(isoPath);
      await dolphinInstall.addGamePath(gameDir);
      if (dolphinType === DolphinLaunchType.NETPLAY) {
        await dolphinInstall.setDefaultIso(isoPath);
      }
    }

    // Ready as soon as the build is found. `--version` can take many seconds (our build
    // initialises Qt first), so report the version when it arrives, cached per executable.
    const exePath = this.getDolphinExecutablePath(dolphinType);
    this._onComplete(dolphinType, this._cachedVersion(dolphinType));
    if (!this.versionCache.has(exePath)) {
      void dolphinInstall.getDolphinVersion().then((version) => {
        if (version) {
          this.versionCache.set(exePath, version);
        }
        this._onComplete(dolphinType, version);
      });
    }
  }

  /**
   * Plays replays (docs/design/replays.md §6.2) in the playback Dolphin: the same executable as
   * Play, with its own User folder (its own graphics settings and window, and a Brawl save of its
   * own), booting the game from the netplay install's P+ files and patched SD card. A replay's
   * game settings come from the replay itself (Dolphin forces them for the run only).
   */
  async launchPlaybackDolphin(id: string, replayComm: ReplayCommunication): Promise<void> {
    const playbackInstallation = this.getInstallation(DolphinLaunchType.PLAYBACK);
    this._assertNotMoving();
    const netplayInstallation = this.getInstallation(DolphinLaunchType.NETPLAY);
    await this._repairBundledDolphin();
    const dolphinPath = await playbackInstallation.findDolphinExecutable();
    await playbackInstallation.ensureUserFolder();

    const configuring = this.playbackDolphinInstances.get("configure");
    if (configuring) {
      throw new Error("Cannot open dolphin if a configuring dolphin is open.");
    }
    let playbackInstance = this.playbackDolphinInstances.get(id);
    if (!playbackInstance) {
      await this.launchGate();
      const isoPath = await this._getIsoPath();
      Preconditions.checkExists(isoPath, "No Brawl disc image set. Choose one in Settings > Game.");
      await this.ensureNetplayUserFolder();
      await this._ensureProjectPlusFiles(netplayInstallation);
      await netplayInstallation.assertProjectPlusFiles();
      const storeDir = projectPlusStoreFolder(app.getPath("userData"));
      await this._ensureBrawlSave(playbackInstallation, storeDir);
      await playbackInstallation.addGamePath(path.dirname(isoPath));
      await playbackInstallation.setDefaultIso(isoPath);
      const sdCardImage = await this._withProgress(DolphinLaunchType.PLAYBACK, "prepareSdCard", (onProgress) =>
        this._installOnlinePlugin(netplayInstallation, true, onProgress),
      );
      const params = buildPlaybackDolphinArgs({
        userArgs: playbackInstallation.userArgs(),
        sdCardImage,
        bootFile: netplayInstallation.netplayLauncherDol,
        testMode: readTestMode(process.env, app.isPackaged),
      });
      // The playback NAND must have a Brawl save, or P+ asks to create one before every first replay
      await this._checkBrawlSave(playbackInstallation, storeDir);
      log.info(`Launching playback dolphin at path: ${dolphinPath} ${params.join(" ")}`);
      playbackInstance = new PlaybackDolphinInstance(dolphinPath, params);
      playbackInstance.on("close", async (exitCode) => {
        this.eventSubject.next({
          type: DolphinEventType.CLOSED,
          instanceId: id,
          dolphinType: DolphinLaunchType.PLAYBACK,
          exitCode,
        });

        // Remove the instance from the map on close
        this.playbackDolphinInstances.delete(id);
      });
      playbackInstance.on("error", (err: Error) => {
        log.error(err);
        throw err;
      });
      // A replay from another version: ask, as §6.5 of docs/design/replays.md says.
      const instance = playbackInstance;
      instance.onNeedsPlayAnyway = (status) => {
        log.info(`Replay refused: ${status.error ?? ""}`);
        void dialog
          .showMessageBox({
            type: "question",
            buttons: ["Play anyway", "Cancel"],
            defaultId: 1,
            cancelId: 1,
            title: "Replay",
            message: "This replay may not play as it was played.",
            detail: `${status.error ?? "It was recorded with another version."}\n\nPlay it anyway?`,
          })
          .then(({ response }) => (response === 0 ? instance.playAnyway() : undefined))
          .catch((err) => log.error(err));
      };

      this.playbackDolphinInstances.set(id, playbackInstance);
    }

    try {
      await playbackInstance.play(replayComm);
    } catch (err) {
      if (err instanceof MacOsRosettaRequiredError) {
        this.eventSubject.next({
          type: DolphinEventType.ERROR,
          errorType: DolphinErrorType.ROSETTA_REQUIRED,
          dolphinType: DolphinLaunchType.PLAYBACK,
        });
      }
      throw err;
    }
  }

  /**
   * Starts the netplay Dolphin. It boots P+ when the Play button is set to (`launchGameOnPlay`) or
   * when `bootGame` says so: a hand-off (an invite, a room, Create room) leaves a request only a
   * booted game reads, and closing a Dolphin that never booted takes it back.
   */
  async launchNetplayDolphin(opts: { bootGame?: boolean } = {}) {
    Preconditions.checkState(this.netplayDolphinInstance == null, "Netplay dolphin is already open!");
    this._assertNotMoving();
    await this.launchGate();

    const netplayInstallation = this.getInstallation(DolphinLaunchType.NETPLAY);
    await this._repairBundledDolphin();
    const dolphinPath = await netplayInstallation.findDolphinExecutable();
    await netplayInstallation.ensureUserFolder();
    await this._updateDolphinSettings(DolphinLaunchType.NETPLAY);

    const testMode = readTestMode(process.env, app.isPackaged);
    const launchGameOnPlay = opts.bootGame || this.settingsManager.get().settings.launchGameOnPlay;
    let bootFile: string | null = null;
    if (launchGameOnPlay) {
      // Boot P+ the way P+ does: its netplay launcher DOL applies the codeset from the
      // virtual SD card, then boots Dolphin's default ISO (the Brawl disc).
      const isoPath = await this._getIsoPath();
      Preconditions.checkExists(isoPath, "No Brawl disc image set. Choose one in Settings > Game.");
      await netplayInstallation.assertProjectPlusFiles();
      const storeDir = projectPlusStoreFolder(this.netplayFolder());
      await ensureNetplaySave(storeDir, netplayInstallation.userFolder);
      await this._ensureBrawlSave(netplayInstallation, storeDir);
      await netplayInstallation.setDefaultIso(isoPath);
      bootFile = netplayInstallation.netplayLauncherDol;
    }
    const sdCardImage = await this._withProgress(DolphinLaunchType.NETPLAY, "prepareSdCard", (onProgress) =>
      this._installOnlinePlugin(netplayInstallation, launchGameOnPlay, onProgress),
    );
    const params = buildNetplayDolphinArgs({
      userArgs: netplayInstallation.userArgs(),
      sdCardImage,
      bootFile,
      testMode,
    });
    log.info(`Launching dolphin at path: ${dolphinPath} ${params.join(" ")}`);

    // Create the Dolphin instance and start it
    const dolphinInstance = new DolphinInstance(dolphinPath, params);
    dolphinInstance.on("close", async (exitCode: number | null, signal: string | null) => {
      try {
        await this._updateLauncherSettings(DolphinLaunchType.NETPLAY);
      } catch (e) {
        log.error("Error encountered updating launcher settings.", e);
      }
      this.eventSubject.next({
        type: DolphinEventType.CLOSED,
        dolphinType: DolphinLaunchType.NETPLAY,
        exitCode,
      });

      this.netplayDolphinInstance = null;
      // A room request the game never picked up must not act on the next start.
      void clearRoomRequest(this.netplayOnlineDir()).catch(() => undefined);
      log.warn(`Dolphin exit code: ${exitCode?.toString(16)}`);
      log.warn(`Dolphin exit signal: ${signal}`);
      await this._keepCrashLog(netplayInstallation, exitCode, signal);
    });
    dolphinInstance.on("error", (err: Error) => {
      log.error(err);
      throw err;
    });

    try {
      await dolphinInstance.start();
    } catch (err) {
      if (err instanceof MacOsRosettaRequiredError) {
        this.eventSubject.next({
          type: DolphinEventType.ERROR,
          errorType: DolphinErrorType.ROSETTA_REQUIRED,
          dolphinType: DolphinLaunchType.NETPLAY,
        });
      }
      throw err;
    }
    this.netplayDolphinInstance = dolphinInstance;
  }

  async configureDolphin(launchType: DolphinLaunchType) {
    log.debug(`configuring ${launchType} dolphin...`);
    this._assertNotMoving();

    const installation = this.getInstallation(launchType);
    const dolphinPath = await installation.findDolphinExecutable();
    await installation.ensureUserFolder();
    await this._updateDolphinSettings(launchType);
    if (launchType === DolphinLaunchType.NETPLAY && !this.netplayDolphinInstance) {
      await this.launchGate();
      const instance = new DolphinInstance(dolphinPath, installation.userArgs());
      instance.on("close", async (exitCode: number | null, signal: string | null) => {
        try {
          await this._updateLauncherSettings(launchType);
        } catch (e) {
          log.error("Error encountered updating launcher settings.", e);
        }
        this.eventSubject.next({
          type: DolphinEventType.CLOSED,
          dolphinType: DolphinLaunchType.NETPLAY,
          exitCode,
        });
        this.netplayDolphinInstance = null;
        await this._keepCrashLog(installation, exitCode, signal);
      });
      instance.on("error", (err: Error) => {
        log.error(err);
        throw err;
      });
      await instance.start();
      this.netplayDolphinInstance = instance;
    } else if (launchType === DolphinLaunchType.PLAYBACK && this.playbackDolphinInstances.size === 0) {
      await this.launchGate();
      const instanceId = "configure";
      const instance = new PlaybackDolphinInstance(dolphinPath, installation.userArgs());
      instance.on("close", async (exitCode) => {
        this.eventSubject.next({
          type: DolphinEventType.CLOSED,
          dolphinType: DolphinLaunchType.PLAYBACK,
          instanceId,
          exitCode,
        });

        // Remove the instance from the map on close
        this.playbackDolphinInstances.delete(instanceId);
      });
      instance.on("error", (err: Error) => {
        log.error(err);
        throw err;
      });
      await instance.start();
      this.playbackDolphinInstances.set(instanceId, instance);
    }
  }

  /**
   * Slippi's "Reset Dolphin" re-downloads its build. Ours is not downloaded, so a
   * soft reset clears the cache and a hard reset also recreates the User folder
   * (the online `user.json` is rewritten on the next Play).
   */
  async reinstallDolphin(launchType: DolphinLaunchType, cleanInstall?: boolean) {
    switch (launchType) {
      case DolphinLaunchType.NETPLAY: {
        if (this.netplayDolphinInstance !== null) {
          log.warn("A netplay dolphin is open");
          return;
        }
        break;
      }
      case DolphinLaunchType.PLAYBACK: {
        if (this.playbackDolphinInstances.size > 0) {
          log.warn("A playback dolphin is open");
          return;
        }
        break;
      }
    }

    this._assertNotMoving();
    const installation = this.getInstallation(launchType);
    this._onStart(launchType);
    await installation.clearCache();
    if (cleanInstall) {
      await installation.resetUserFolder();
    }
    await this.installDolphin(launchType);
  }

  /**
   * Settings > Dolphin's game files folder: moves the netplay folder to the folder the player
   * picked (null: back to `<userData>/netplay`), see move_game_files.ts, with the Play button
   * showing the progress. A folder that already holds game files (a copy the player made) is used
   * as it is once the player agrees. The old files are deleted afterwards and the bundled Dolphin
   * is installed into the new folder.
   */
  async moveGameFiles(picked: string | null): Promise<GameFilesMoveResult> {
    if (this.movingGameFiles) {
      throw new GameFilesMoveError("The game files are already being moved.");
    }
    this.movingGameFiles = true;
    try {
      return await this._moveGameFiles(picked);
    } finally {
      this.movingGameFiles = false;
    }
  }

  private async _moveGameFiles(picked: string | null): Promise<GameFilesMoveResult> {
    if (this.netplayDolphinInstance || this.playbackDolphinInstances.size > 0) {
      throw new GameFilesMoveError("Close Dolphin first.");
    }
    if (this.projectPlusInstall) {
      throw new GameFilesMoveError("Wait for the Project+ download to finish first.");
    }
    const from = this.netplayFolder();
    const defaultFolder = defaultNetplayFolder(app.getPath("userData"));
    const target: GameFilesTarget =
      picked == null
        ? await defaultGameFilesTarget(defaultFolder, from)
        : await chooseGameFilesTarget(picked, from, PRODUCT_NAME);
    if (target.kind === "adopt" && !(await confirmAdoptGameFiles(target.folder, from))) {
      return { action: "cancelled", folder: from, leftBehind: null };
    }
    // The bundled Dolphin's install must not write into the old folder while it is moved.
    await this.bundledDolphinReady();

    this._onStart(DolphinLaunchType.NETPLAY);
    try {
      if (target.kind === "move") {
        await moveGameFiles({
          from,
          to: target.folder,
          onProgress: (current, total) => this._onProgress(DolphinLaunchType.NETPLAY, current, total, "moveGameFiles"),
          log: (message) => log.info(message),
        });
      } else {
        log.info(`Using the game files already in ${target.folder}; deleting the ones in ${from}`);
      }
      const isDefault = path.relative(target.folder, defaultFolder) === "";
      await this.settingsManager.updateSetting("gameFilesFolder", isDefault ? null : target.folder);
    } catch (err) {
      this._onComplete(DolphinLaunchType.NETPLAY, this._cachedVersion(DolphinLaunchType.NETPLAY));
      throw err;
    }

    // An executable set to the old folder is gone, and one set to the new folder is the default now.
    const settings = this.settingsManager.get().settings;
    const inMovedFolder = (p: string | null) =>
      p != null && [from, target.folder].some((dir) => isSubdirectory(dir, p));
    if (inMovedFolder(settings.netplayDolphinPath)) {
      await this.settingsManager.updateSetting("netplayDolphinPath", null);
    }
    if (inMovedFolder(settings.playbackDolphinPath)) {
      await this.settingsManager.updateSetting("playbackDolphinPath", null);
    }

    let leftBehind: string | null = null;
    try {
      await removeGameFiles(from);
    } catch (err) {
      leftBehind = from;
      log.warn(`Could not delete the old game files in ${from}: ${err instanceof Error ? err.message : String(err)}`);
    }

    this.bundledInstall = null;
    for (const dolphinType of [DolphinLaunchType.NETPLAY, DolphinLaunchType.PLAYBACK]) {
      void this.installDolphin(dolphinType).catch((err) => log.error(err));
    }
    return { action: target.kind === "move" ? "moved" : "adopted", folder: target.folder, leftBehind };
  }

  private _assertNotMoving() {
    if (this.movingGameFiles) {
      throw new Error("The game files are being moved. Try again when the move is done.");
    }
  }

  /**
   * The netplay User folder, seeded from the development template the first time (2 GB, with the
   * Play button's progress). Start-up's theme extraction waits for the same seed.
   */
  ensureNetplayUserFolder(): Promise<void> {
    const installation = this.getInstallation(DolphinLaunchType.NETPLAY);
    let started = false;
    return installation.ensureUserFolder((current, total) => {
      if (!started) {
        started = true;
        this._onStart(DolphinLaunchType.NETPLAY);
      }
      this._onProgress(DolphinLaunchType.NETPLAY, current, total, "copyUserFolder");
    });
  }

  /** A step shown on the Play button from its first progress report until it ends (see runWithProgress). */
  private _withProgress<T>(
    dolphinType: DolphinLaunchType,
    phase: DolphinSetupPhase,
    step: (onProgress: (current: number, total: number) => void) => Promise<T>,
  ): Promise<T> {
    return runWithProgress(step, {
      start: () => this._onStart(dolphinType),
      progress: (current, total) => this._onProgress(dolphinType, current, total, phase),
      complete: () => this._onComplete(dolphinType, this._cachedVersion(dolphinType)),
    });
  }

  /** The version `--version` printed for this Dolphin, once it has (undefined before). */
  cachedDolphinVersion(dolphinType: DolphinLaunchType): string | undefined {
    return this._cachedVersion(dolphinType);
  }

  private _cachedVersion(dolphinType: DolphinLaunchType): string | undefined {
    return this.versionCache.get(this.getDolphinExecutablePath(dolphinType));
  }

  /**
   * Installs the Dolphin build bundled with the launcher into `<userData>/netplay` when it is
   * missing or older (after a launcher update). Shared by the netplay and playback set-up, which
   * run at the same time on start-up. Progress is reported like Slippi's Dolphin download.
   */
  private _installBundledDolphin(): Promise<void> {
    const source = bundledDolphinSource({
      env: process.env,
      isPackaged: app.isPackaged,
      resourcesPath: process.resourcesPath,
    });
    if (!source) {
      return Promise.resolve();
    }
    if (!this.bundledInstall) {
      let started = false;
      this.bundledInstall = installBundledDolphin({
        sourceDir: source,
        destDir: this.netplayFolder(),
        onProgress: (current, total) => {
          if (!started) {
            started = true;
            this._onStart(DolphinLaunchType.NETPLAY);
          }
          this._onProgress(DolphinLaunchType.NETPLAY, current, total, "installDolphin");
        },
        log: (message) => log.info(message),
      })
        .then(({ action, manifest }) => {
          log.info(`Bundled Dolphin ${manifest.version}: ${action}`);
        })
        .catch((err) => {
          this.bundledInstall = null;
          throw err;
        });
    }
    return this.bundledInstall;
  }

  /**
   * Waits for the bundled Dolphin's install, starting it if it has not started. The theme
   * extraction runs DolphinTool from the install folder, and the install cannot replace a running
   * DolphinTool on Windows. Never throws: a failed install is reported by the Dolphin set-up.
   */
  async bundledDolphinReady(): Promise<void> {
    await this._installBundledDolphin().catch(() => undefined);
  }

  /**
   * Start-up's install of the bundled Dolphin removes the old build before it copies the new one,
   * so one that stopped half-way (a file in use) leaves no executable. Play then installs it
   * again, with the set-up progress, instead of failing until the launcher restarts.
   */
  private async _repairBundledDolphin(): Promise<void> {
    if (await fileExists(this.getDolphinExecutablePath(DolphinLaunchType.NETPLAY))) {
      return;
    }
    try {
      await this._installBundledDolphin();
    } finally {
      this._onComplete(DolphinLaunchType.NETPLAY, this._cachedVersion(DolphinLaunchType.NETPLAY));
    }
  }

  /**
   * Downloads P+'s files from P+'s official release (pinned, sha256-checked) into the netplay
   * User folder when they are missing, and puts the Brawl save template into the User folder
   * (`<User>/NetplaySave`). Nothing is downloaded when they are in place (also when a development User
   * template provided them and the Dolphin build has its own save template).
   */
  private _ensureProjectPlusFiles(installation: LocalDolphinInstallation): Promise<void> {
    if (!this.projectPlusInstall) {
      const storeDir = projectPlusStoreFolder(this.netplayFolder());
      const target: PPlusTarget = {
        userFolder: installation.userFolder,
        storeDir,
        downloadDir: path.join(storeDir, "downloads"),
      };
      const release = readTestMode(process.env, app.isPackaged).pplusRelease ?? PPLUS_RELEASE;
      this.projectPlusInstall = (async () => {
        let missing = await missingProjectPlusFiles(target, release);
        // A development build may carry its own template in Sys (Dolphin falls back to it).
        if (missing.includes(NETPLAY_SAVE_DIR) && existsSync(path.join(installation.sysFolder, NETPLAY_SAVE_DIR))) {
          missing = missing.filter((m) => m !== NETPLAY_SAVE_DIR);
        }
        if (missing.length > 0) {
          // The button changes with the first progress report (installDolphin ends it).
          let started = false;
          await installProjectPlusFiles({
            target,
            release,
            onProgress: ({ phase, current, total }) => {
              if (!started) {
                started = true;
                this._onStart(DolphinLaunchType.NETPLAY);
              }
              this._onProgress(DolphinLaunchType.NETPLAY, current, total, PPLUS_PHASES[phase]);
            },
            log: (message) => log.info(message),
          });
        }
        await ensureNetplaySave(storeDir, installation.userFolder);
      })().finally(() => {
        this.projectPlusInstall = null;
      });
    }
    return this.projectPlusInstall;
  }

  /** After a run that ended abnormally, keeps its log out of Dolphin's two-run rotation (crash_log.ts). */
  private async _keepCrashLog(
    installation: LocalDolphinInstallation,
    exitCode: number | null,
    signal: string | null,
  ): Promise<void> {
    if (!isAbnormalExit(exitCode, signal)) {
      return;
    }
    try {
      const kept = await keepCrashLog(path.join(installation.userFolder, "Logs"));
      if (kept) {
        log.warn(`Kept the crashed run's log as ${kept}`);
      }
    } catch (e) {
      log.error("Could not keep the crashed run's log.", e);
    }
  }

  /**
   * Seeds the NAND Play boots with P+'s Brawl save template when it has no Brawl save, so P+
   * never asks "Create save file for Project+?" (install/brawl_save.ts). An existing save is never
   * touched. A failure is logged and Play goes on: the game then asks, as P+ does.
   */
  private async _ensureBrawlSave(installation: LocalDolphinInstallation, storeDir: string): Promise<void> {
    try {
      const result = await ensureBrawlSave({
        nandRoot: await installation.nandRoot(),
        templateRoots: [
          netplaySaveStore(storeDir),
          path.join(installation.userFolder, NETPLAY_SAVE_DIR),
          path.join(installation.sysFolder, NETPLAY_SAVE_DIR),
        ],
        log: (message) => log.info(message),
      });
      if (result.action === "no-template") {
        log.warn(`Brawl save: no template for ${result.dir}`);
      } else if (result.action !== "seeded") {
        log.info(`Brawl save: ${result.action} (${result.dir})`);
      }
    } catch (err) {
      log.error(`Could not seed the Brawl save: ${err instanceof Error ? err.message : String(err)}`);
    }
  }

  /**
   * Right before a Dolphin starts: its NAND has a Brawl save, else it is seeded again (a playback
   * Dolphin on the user's prod, 2026-10-12, met P+'s "Create save file" prompt although the seed ran
   * earlier in the same launch). Logged as warnings, which a release's log keeps.
   */
  private async _checkBrawlSave(installation: LocalDolphinInstallation, storeDir: string): Promise<void> {
    const hasSave = async () => {
      const dir = brawlSaveDir(await installation.nandRoot());
      try {
        return { dir, ok: (await readdir(dir, { withFileTypes: true })).some((e) => e.isFile()) };
      } catch {
        return { dir, ok: false };
      }
    };
    const before = await hasSave();
    if (before.ok) {
      return;
    }
    log.warn(`No Brawl save in ${before.dir} before starting Dolphin (${installation.userFolder}); seeding it`);
    await this._ensureBrawlSave(installation, storeDir);
    const after = await hasSave();
    log.warn(`Brawl save in ${after.dir}: ${after.ok ? "seeded" : "still missing (P+ will ask to create one)"}`);
  }

  /**
   * Puts our game plugin on a launcher-managed copy of the user's P+ SD card
   * (`<userData>/netplay/pponline-sd/sd.raw`) and returns its path for Dolphin. The user's
   * card (`<User>/Wii/sd.raw`) and the User template are never written. Without a card,
   * nothing is installed unless Play boots the game (`required`), which then fails.
   */
  private async _installOnlinePlugin(
    installation: LocalDolphinInstallation,
    required: boolean,
    onProgress?: (current: number, total: number) => void,
  ): Promise<string | null> {
    const sourceImage = installation.sdCardImage;
    if (!required && !(await fileExists(sourceImage))) {
      return null;
    }
    const env = this._pathEnv();
    const template = defaultUserTemplate(env);
    try {
      const plugin = await loadPlugin(defaultPluginLocation({ ...env, resourcesPath: process.resourcesPath }));
      const result = await installPluginOnSdCard({
        sourceImage,
        outputDir: patchedSdCardFolder(env.netplayFolder),
        plugin,
        protectedPaths: template ? [template] : [],
        log: (message) => log.info(message),
        onProgress,
      });
      log.info(`Online plugin ${plugin.sha256.slice(0, 12)} on ${result.image}: ${result.action}`);
      return result.image;
    } catch (err) {
      log.error(err);
      const code = (err as NodeJS.ErrnoException | undefined)?.code;
      if (code === "EBUSY" || code === "EPERM") {
        // Windows: a Dolphin started before a launcher update still has the card open
        throw new Error("Another Dolphin is using the game files. Close it, then try again.");
      }
      throw new Error(
        `Could not install the online game plugin on the Project+ SD card: ${
          err instanceof Error ? err.message : String(err)
        }`,
      );
    }
  }

  private async _getIsoPath(): Promise<string | undefined> {
    const isoPath = this.settingsManager.get().settings.isoPath ?? undefined;
    if (isoPath) {
      // Make sure the file actually exists
      if (!(await fileExists(isoPath))) {
        throw new Error(`Could not find ISO file: ${isoPath}`);
      }
    }
    return isoPath;
  }

  async importConfig(launchType: DolphinLaunchType, dolphinPath: string): Promise<void> {
    const installation = this.getInstallation(launchType);
    await installation.importConfig(dolphinPath);
    if (launchType === DolphinLaunchType.NETPLAY) {
      await this._updateLauncherSettings(launchType);
    }
  }

  private async _updateDolphinSettings(launchType: DolphinLaunchType) {
    const installation = this.getInstallation(launchType);
    await installation.updateSettings({
      replayPath: this.settingsManager.getRootSlpPath(),
      enableNetplayReplays: this.settingsManager.getEnableNetplayReplays(),
      enableMonthlySubfolders: this.settingsManager.getEnableMonthlySubfolders(),
      inputDelay: this.settingsManager.getInputDelay(),
      enableMusic: this.settingsManager.getEnableMusic(),
    });
  }

  private async _updateLauncherSettings(launchType: DolphinLaunchType) {
    const installation = this.getInstallation(launchType);
    const newSettings = await installation.getSettings();

    await this._updateLauncherSetting(
      this.settingsManager.getRootSlpPath(),
      path.normalize(newSettings.replayPath),
      (val) => this.settingsManager.updateSetting("rootSlpPath", val),
    );
    await this._updateLauncherSetting(
      this.settingsManager.getEnableNetplayReplays(),
      newSettings.enableNetplayReplays,
      (val) => this.settingsManager.updateSetting("enableNetplayReplays", val),
    );
    await this._updateLauncherSetting(
      this.settingsManager.getEnableMonthlySubfolders(),
      newSettings.enableMonthlySubfolders,
      (val) => this.settingsManager.updateSetting("useMonthlySubfolders", val),
    );
    await this._updateLauncherSetting(this.settingsManager.getInputDelay(), newSettings.inputDelay, (val) =>
      this.settingsManager.updateSetting("inputDelay", val),
    );
    await this._updateLauncherSetting(this.settingsManager.getEnableMusic(), newSettings.enableMusic, (val) =>
      this.settingsManager.updateSetting("enableMusic", val),
    );
  }

  private async _updateLauncherSetting<T>(currentVal: T, newVal: T, update: (val: T) => Promise<void>) {
    if (currentVal === newVal) {
      return;
    }
    await update(newVal);
  }

  private _onStart(dolphinType: DolphinLaunchType) {
    this.eventSubject.next({
      type: DolphinEventType.DOWNLOAD_START,
      dolphinType,
    });
  }

  /**
   * Sends progress, at most every PROGRESS_INTERVAL_MS per launch type; the start and end of a
   * step and a change of phase are always sent.
   */
  private _onProgress(dolphinType: DolphinLaunchType, current: number, total: number, phase?: DolphinSetupPhase) {
    if (!this.shouldSendProgress(dolphinType, current, total, phase)) {
      return;
    }
    this.eventSubject.next({
      type: DolphinEventType.DOWNLOAD_PROGRESS,
      dolphinType,
      progress: { current, total, phase },
    });
  }

  private _onComplete(dolphinType: DolphinLaunchType, dolphinVersion: string | undefined) {
    this.eventSubject.next({
      type: DolphinEventType.DOWNLOAD_COMPLETE,
      dolphinType,
      dolphinVersion,
    });
  }
}

export type GameFilesMoveResult = {
  action: "moved" | "adopted" | "cancelled";
  /** The netplay folder now in use. */
  folder: string;
  /** The old folder, when some of its files could not be deleted. */
  leftBehind: string | null;
};

/** Moving back to `<userData>/netplay`: files left there (the launcher's own) are replaced. */
async function defaultGameFilesTarget(defaultFolder: string, current: string): Promise<GameFilesTarget> {
  if (path.relative(defaultFolder, current) === "") {
    throw new GameFilesMoveError(`The game files are already in ${defaultFolder}.`);
  }
  if (await fileExists(path.join(defaultFolder, "User"))) {
    return { folder: defaultFolder, kind: "adopt" };
  }
  await removeGameFiles(defaultFolder);
  return { folder: defaultFolder, kind: "move" };
}

async function confirmAdoptGameFiles(folder: string, current: string): Promise<boolean> {
  // On the launcher's window even when it is not focused, never a free-floating box over a game.
  const window = BrowserWindow.getFocusedWindow() ?? BrowserWindow.getAllWindows()[0];
  const options: MessageBoxOptions = {
    type: "question",
    buttons: ["Use these files", "Cancel"],
    defaultId: 0,
    cancelId: 1,
    message: `${folder} already has game files.`,
    detail: `Use them? The game files in ${current} are deleted.`,
  };
  const { response } = window ? await dialog.showMessageBox(window, options) : await dialog.showMessageBox(options);
  return response === 0;
}
