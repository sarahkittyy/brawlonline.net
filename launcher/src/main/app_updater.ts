import { resolveServiceUrls } from "@accounts/config";
import type { LauncherUpdateMode } from "@common/launcher_update";
import { getLauncherUpdateMode } from "@common/launcher_update";
import type { SettingsManager } from "@settings/settings_manager";
import { app } from "electron";
import log from "electron-log";
import type { UpdateInfo } from "electron-updater";
import { autoUpdater } from "electron-updater";

import { ipc_launcherUpdateFailedEvent } from "./ipc";

export type UpdateState = {
  status: "succeeded" | "failed";
  version: string;
};

const INSTALL_UPDATE_TIMEOUT_MS = 5000; // 5 seconds
/** How often a running launcher asks the feed again, so a release reaches a launcher left open. */
export const UPDATE_CHECK_INTERVAL_MS = 5 * 60 * 1000; // 5 minutes
/** A check older than this is redone before Dolphin starts, so a release out since the last one counts. */
const LAUNCH_CHECK_MAX_AGE_MS = 60 * 1000; // 1 minute
/** How long a start waits for that check; past it (offline, feed down) Dolphin starts on what is known. */
const LAUNCH_CHECK_TIMEOUT_MS = 5000; // 5 seconds

/** Thrown when Dolphin is asked to start while a newer launcher is out. */
export class LauncherUpdateRequiredError extends Error {
  constructor(readonly version: string) {
    super(`Version ${version} of the launcher is out. Update before starting Dolphin.`);
    this.name = "LauncherUpdateRequiredError";
  }
}

export class AppUpdater {
  private updateState: UpdateState | undefined;
  /** The newer version the feed last offered, until the feed offers none (then undefined). */
  private availableVersion: string | undefined;
  private lastCheckAt = 0;
  private checkTimer: NodeJS.Timeout | undefined;

  /** "download" on macOS while the build cannot update itself (MAC_SELF_UPDATE in @common/product). */
  readonly mode: LauncherUpdateMode = getLauncherUpdateMode(process.platform);

  constructor(private readonly settingsManager: SettingsManager) {
    autoUpdater.logger = log;
    autoUpdater.autoInstallOnAppQuit = settingsManager.get().settings.autoUpdateLauncher;
    if (this.mode === "download") {
      // Squirrel.Mac would reject the update (signature mismatch), so it is only found, never
      // downloaded; the update bar links the website's download instead.
      autoUpdater.autoDownload = false;
      autoUpdater.autoInstallOnAppQuit = false;
    }

    // This is going to be the default at some point, right now if we don't
    // explicitly set this to true then electron-builder prints a (harmless)
    // warning when updating on Windows.
    // See: https://github.com/electron-userland/electron-builder/pull/6575
    autoUpdater.disableWebInstaller = true;
    // Disable differential downloads to fix Windows NSIS update issues
    // See: https://github.com/electron-userland/electron-builder/issues/9181
    autoUpdater.disableDifferentialDownload = true;
    // Same hooks as Slippi (electron-updater), but our feed comes from the host config:
    // a "generic" provider at launcherUpdates (electron-builder.json publishes to the same URL).
    autoUpdater.setFeedURL({ provider: "generic", url: resolveServiceUrls().launcherUpdates });
    // electron-updater skips an unpackaged launcher (development, the harness); one that is pointed at
    // a feed with PPO_UPDATES_URL checks it, so the update bar and the launch gate can be tried.
    if (!app.isPackaged && process.env.PPO_UPDATES_URL) {
      autoUpdater.forceDevUpdateConfig = true;
    }

    autoUpdater.on("update-available", (info: UpdateInfo) => {
      this.availableVersion = info.version;
    });
    autoUpdater.on("update-not-available", () => {
      this.availableVersion = undefined;
    });
  }

  /**
   * Asks the feed for a newer version (electron-updater then downloads it in "install" mode).
   * `notify` adds the system notification once it is downloaded: for the start and the header's
   * button, not the periodic checks, which find the same download again each time.
   */
  async checkForUpdates(notify = false): Promise<{ updateAvailable: boolean }> {
    this.lastCheckAt = Date.now();
    // In "download" mode nothing is downloaded, so there is nothing to notify about.
    const result =
      notify && this.mode === "install"
        ? await autoUpdater.checkForUpdatesAndNotify()
        : await autoUpdater.checkForUpdates();
    // Rejects with the download's error (already logged); the bar then offers the website's download.
    result?.downloadPromise?.catch(() => this.onDownloadFailed());
    return { updateAvailable: result?.isUpdateAvailable ?? false };
  }

  private onDownloadFailed(): void {
    ipc_launcherUpdateFailedEvent.main!.trigger({}).catch(log.warn);
  }

  /** Checks the feed every UPDATE_CHECK_INTERVAL_MS while the launcher runs. */
  startPeriodicChecks(): void {
    if (this.checkTimer) {
      return;
    }
    this.checkTimer = setInterval(() => {
      // A failed check (offline) is logged by electron-updater's "error" event.
      this.checkForUpdates().catch(() => undefined);
    }, UPDATE_CHECK_INTERVAL_MS);
  }

  /** The newer launcher version that is out, if any; checks again first when the last check is old. */
  async requiredUpdate(): Promise<string | undefined> {
    if (Date.now() - this.lastCheckAt > LAUNCH_CHECK_MAX_AGE_MS) {
      const timeout = new Promise<void>((resolve) => setTimeout(resolve, LAUNCH_CHECK_TIMEOUT_MS));
      await Promise.race([this.checkForUpdates().catch(() => undefined), timeout]);
    }
    return this.availableVersion;
  }

  /** Throws LauncherUpdateRequiredError while a newer launcher is out (DolphinManager's launch gate). */
  async assertUpToDate(): Promise<void> {
    const version = await this.requiredUpdate();
    if (version) {
      throw new LauncherUpdateRequiredError(version);
    }
  }

  async verifyPendingUpdate(): Promise<void> {
    const currentVersion = app.getVersion();
    const pendingVersion = this.settingsManager.get().pendingUpdateVersion;

    if (!pendingVersion) {
      return;
    }

    if (currentVersion === pendingVersion) {
      log.info(`Auto-update succeeded: version ${currentVersion}`);
      this.updateState = { status: "succeeded", version: currentVersion };
    } else {
      log.error(
        `Auto-update FAILED: expected ${pendingVersion}, running ${currentVersion}. ` +
          "Update file may have been missing, corrupted, or the installer was interrupted.",
      );
      this.updateState = { status: "failed", version: pendingVersion };
    }

    await this.settingsManager.updateSetting("pendingUpdateVersion", undefined);
  }

  getUpdateState(): UpdateState | undefined {
    return this.updateState;
  }

  private async _installUpdate(): Promise<void> {
    const pendingVersion = this.settingsManager.get().pendingUpdateVersion;
    if (!pendingVersion) {
      throw new Error("No update has been downloaded.");
    }

    autoUpdater.quitAndInstall(false, true);
  }

  async quitAndInstall(): Promise<void> {
    // The restart _should_ happen instantly.
    // If we still haven't restarted the app within the timeout, something probably went wrong.
    const timeout = new Promise<void>((_resolve, reject) => {
      setTimeout(() => {
        reject(new Error(`Timed out after ${INSTALL_UPDATE_TIMEOUT_MS / 1000}s trying to install update.`));
      }, INSTALL_UPDATE_TIMEOUT_MS);
    });

    await Promise.race([timeout, this._installUpdate()]);
  }
}
