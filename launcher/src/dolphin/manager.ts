import { Preconditions } from "@common/preconditions";
import { readTestMode } from "@common/test_mode";
import type { SettingsManager } from "@settings/settings_manager";
import { app } from "electron";
import electronLog from "electron-log";
import { existsSync } from "fs";
import { Observable, Subject } from "observable-fns";
import path from "path";
import { fileExists } from "utils/file_exists";

import { fetchLatestVersion } from "./install/fetch_latest_version";
import { LocalDolphinInstallation } from "./install/local_installation";
import type { DolphinPathEnv } from "./install/paths";
import {
  defaultDolphinExecutable,
  defaultPluginLocation,
  defaultUserTemplate,
  patchedSdCardFolder,
} from "./install/paths";
import { installPluginOnSdCard, loadPlugin } from "./install/sd_card";
import { DolphinInstance, MacOsRosettaRequiredError, PlaybackDolphinInstance } from "./instance";
import { buildNetplayDolphinArgs } from "./netplay_args";
import type { DolphinEvent, ReplayCommunication } from "./types";
import { DolphinErrorType, DolphinEventType, DolphinLaunchType } from "./types";

const log = electronLog.scope("dolphin/manager");

// DolphinManager should be in control of all dolphin instances that get opened for actual use.
// This includes playing netplay, viewing replays and configuring Dolphin.
//
// Differences from Slippi: one Dolphin build (ours) serves both netplay and playback; it is
// not downloaded (see install/fetch_latest_version.ts for the update hook); and Play boots
// P+'s netplay launcher DOL with the Brawl disc as Dolphin's default ISO, the way P+ does.
export class DolphinManager {
  private playbackDolphinInstances = new Map<string, PlaybackDolphinInstance>();
  private netplayDolphinInstance: DolphinInstance | null = null;
  private versionCache = new Map<string, string>();
  private eventSubject = new Subject<DolphinEvent>();
  events = Observable.from(this.eventSubject);

  constructor(private settingsManager: SettingsManager) {}

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
    this._onComplete(dolphinType, this.versionCache.get(exePath));
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
      await netplayInstallation.setDefaultIso(isoPath);
      bootFile = netplayInstallation.netplayLauncherDol;
    }
    const sdCardImage = await this._installOnlinePlugin(netplayInstallation, launchGameOnPlay);
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
   * Puts our game plugin on a launcher-managed copy of the user's P+ SD card
   * (`<userData>/netplay/pponline-sd/sd.raw`) and returns its path for Dolphin. The user's
   * card (`<User>/Wii/sd.raw`) and the User template are never written. Without a card,
   * nothing is installed unless Play boots the game (`required`), which then fails.
   */
  private async _installOnlinePlugin(
    installation: LocalDolphinInstallation,
    required: boolean,
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

  private _onProgress(dolphinType: DolphinLaunchType, current: number, total: number) {
    this.eventSubject.next({
      type: DolphinEventType.DOWNLOAD_PROGRESS,
      dolphinType,
      progress: { current, total },
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
