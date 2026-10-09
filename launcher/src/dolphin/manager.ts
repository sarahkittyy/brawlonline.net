import { Preconditions } from "@common/preconditions";
import { DOLPHIN_ONLINE_DIR } from "@common/product";
import { readTestMode } from "@common/test_mode";
import type { SettingsManager } from "@settings/settings_manager";
import { app } from "electron";
import electronLog from "electron-log";
import { existsSync } from "fs";
import { Observable, Subject } from "observable-fns";
import path from "path";
import { fileExists } from "utils/file_exists";

import { ensureBrawlSave } from "./install/brawl_save";
import { bundledDolphinSource, installBundledDolphin } from "./install/bundled_dolphin";
import { fetchLatestVersion } from "./install/fetch_latest_version";
import { LocalDolphinInstallation } from "./install/local_installation";
import type { DolphinPathEnv } from "./install/paths";
import {
  defaultDolphinExecutable,
  defaultPluginLocation,
  defaultUserTemplate,
  netplayInstallFolder,
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
import { buildNetplayDolphinArgs } from "./netplay_args";
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
  private eventSubject = new Subject<DolphinEvent>();
  private shouldSendProgress = createProgressThrottle(PROGRESS_INTERVAL_MS);
  events = Observable.from(this.eventSubject);

  constructor(private settingsManager: SettingsManager) {}

  /** True while the netplay Dolphin the launcher started (Play, or Configure) is running. */
  isNetplayRunning(): boolean {
    return this.netplayDolphinInstance != null;
  }

  /** The netplay User folder's online folder (`user.json`, the room hand-off files). */
  netplayOnlineDir(): string {
    return path.join(this.getInstallation(DolphinLaunchType.NETPLAY).userFolder, DOLPHIN_ONLINE_DIR);
  }

  private _pathEnv(): DolphinPathEnv {
    return {
      env: process.env,
      isDevelopment: !app.isPackaged,
      devRepoRoot: process.cwd(),
      userDataDir: app.getPath("userData"),
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
    return new LocalDolphinInstallation(launchType, exePath, app.getPath("userData"), template, portable);
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

  async launchPlaybackDolphin(id: string, replayComm: ReplayCommunication): Promise<void> {
    const playbackInstallation = this.getInstallation(DolphinLaunchType.PLAYBACK);
    const dolphinPath = await playbackInstallation.findDolphinExecutable();
    await playbackInstallation.ensureUserFolder();

    const configuring = this.playbackDolphinInstances.get("configure");
    if (configuring) {
      throw new Error("Cannot open dolphin if a configuring dolphin is open.");
    }
    let playbackInstance = this.playbackDolphinInstances.get(id);
    if (!playbackInstance) {
      playbackInstance = new PlaybackDolphinInstance(dolphinPath, playbackInstallation.userArgs());
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

  async launchNetplayDolphin() {
    Preconditions.checkState(this.netplayDolphinInstance == null, "Netplay dolphin is already open!");

    const netplayInstallation = this.getInstallation(DolphinLaunchType.NETPLAY);
    const dolphinPath = await netplayInstallation.findDolphinExecutable();
    await netplayInstallation.ensureUserFolder();
    await this._updateDolphinSettings(DolphinLaunchType.NETPLAY);

    const testMode = readTestMode(process.env, app.isPackaged);
    const launchGameOnPlay = this.settingsManager.get().settings.launchGameOnPlay;
    let bootFile: string | null = null;
    if (launchGameOnPlay) {
      // Boot P+ the way P+ does: its netplay launcher DOL applies the codeset from the
      // virtual SD card, then boots Dolphin's default ISO (the Brawl disc).
      const isoPath = await this._getIsoPath();
      Preconditions.checkExists(isoPath, "No Brawl disc image set. Choose one in Settings > Game.");
      await netplayInstallation.assertProjectPlusFiles();
      const storeDir = projectPlusStoreFolder(app.getPath("userData"));
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

    const installation = this.getInstallation(launchType);
    const dolphinPath = await installation.findDolphinExecutable();
    await installation.ensureUserFolder();
    await this._updateDolphinSettings(launchType);
    if (launchType === DolphinLaunchType.NETPLAY && !this.netplayDolphinInstance) {
      const instance = new DolphinInstance(dolphinPath, installation.userArgs());
      instance.on("close", async (exitCode) => {
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
      });
      instance.on("error", (err: Error) => {
        log.error(err);
        throw err;
      });
      await instance.start();
      this.netplayDolphinInstance = instance;
    } else if (launchType === DolphinLaunchType.PLAYBACK && this.playbackDolphinInstances.size === 0) {
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

    const installation = this.getInstallation(launchType);
    this._onStart(launchType);
    await installation.clearCache();
    if (cleanInstall) {
      await installation.resetUserFolder();
    }
    await this.installDolphin(launchType);
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
        destDir: netplayInstallFolder(app.getPath("userData")),
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
   * Downloads P+'s files from P+'s official release (pinned, sha256-checked) into the netplay
   * User folder when they are missing, and puts the Brawl save template into the User folder
   * (`<User>/NetplaySave`). Nothing is downloaded when they are in place (also when a development User
   * template provided them and the Dolphin build has its own save template).
   */
  private _ensureProjectPlusFiles(installation: LocalDolphinInstallation): Promise<void> {
    if (!this.projectPlusInstall) {
      const storeDir = projectPlusStoreFolder(app.getPath("userData"));
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
      if (result.action !== "seeded") {
        log.info(`Brawl save: ${result.action} (${result.dir})`);
      }
    } catch (err) {
      log.error(`Could not seed the Brawl save: ${err instanceof Error ? err.message : String(err)}`);
    }
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
        outputDir: patchedSdCardFolder(env.userDataDir),
        plugin,
        protectedPaths: template ? [template] : [],
        log: (message) => log.info(message),
        onProgress,
      });
      log.info(`Online plugin ${plugin.sha256.slice(0, 12)} on ${result.image}: ${result.action}`);
      return result.image;
    } catch (err) {
      log.error(err);
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
