import { resolveServiceUrls } from "@accounts/config";
import type { LauncherUpdateMode } from "@common/launcher_update";
import { getLauncherUpdateMode } from "@common/launcher_update";
import type { SettingsManager } from "@settings/settings_manager";
import { app } from "electron";
import log from "electron-log";
import type { ProgressInfo, UpdateInfo } from "electron-updater";
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
/** How often a downloaded update looks again whether the launcher is free to restart (installWhenIdle). */
export const INSTALL_IDLE_POLL_MS = 15 * 1000; // 15 seconds
/**
 * A download that has received nothing for this long is cancelled and started again. electron-updater
 * has no timeout of its own here: its 60 s socket timeout hooks a "socket" event that Electron's net
 * requests never emit, so a stalled download waited until the network itself gave up (bug report #119).
 */
export const DOWNLOAD_STALL_MS = 60 * 1000; // 1 minute
/** How long after each failed download it is tried again; past the last, the bar offers the manual download. */
export const DOWNLOAD_RETRY_DELAYS_MS = [5 * 1000, 30 * 1000, 2 * 60 * 1000];

type UpdateDownload = { promise: Promise<unknown>; cancel: () => void; stalled: boolean; announced: boolean };

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
  private installTimer: NodeJS.Timeout | undefined;
  /** Set once the launcher installed (or tried to install) a downloaded update by itself. */
  private autoInstallStarted = false;
  /** True while the launcher must not restart (DolphinManager.isBusy: a Dolphin open, an install). */
  private isBusy: () => boolean = () => false;
  /** The download electron-updater is running, from the check that started it (later checks join it). */
  private download: UpdateDownload | undefined;
  private stallTimer: NodeJS.Timeout | undefined;
  private retryTimer: NodeJS.Timeout | undefined;
  /** Failed downloads in a row, each tried again after DOWNLOAD_RETRY_DELAYS_MS. */
  private downloadFailures = 0;
  /** The version whose download was last logged as done (the periodic checks find it again every time). */
  private loggedDownloaded: string | undefined;

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
    autoUpdater.on("download-progress", (progress: ProgressInfo) => this.onDownloadProgress(progress));
    autoUpdater.on("update-downloaded", (info: UpdateInfo) => {
      if (this.loggedDownloaded !== info.version) {
        this.loggedDownloaded = info.version;
        log.warn(`Launcher update ${info.version} downloaded`);
      }
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
    // A check while a download runs gets that download's promise again, already watched.
    const downloadPromise = result?.downloadPromise;
    if (downloadPromise && downloadPromise !== this.download?.promise) {
      this.watchDownload(downloadPromise, () => result?.cancellationToken?.cancel());
    }
    return { updateAvailable: result?.isUpdateAvailable ?? false };
  }

  /**
   * Restarts the download when it stalls (DOWNLOAD_STALL_MS) and tries a failed one again
   * (DOWNLOAD_RETRY_DELAYS_MS) before the bar offers the website's download: a 100-150 MB download
   * over a flaky line or QUIC failed once and stayed failed until the next periodic check (#119).
   */
  private watchDownload(promise: Promise<unknown>, cancel: () => void): void {
    const download: UpdateDownload = { promise, cancel, stalled: false, announced: false };
    this.download = download;
    const settle = () => {
      if (this.download === download) {
        this.download = undefined;
        this.clearStallTimer();
      }
    };
    promise
      .then(() => {
        settle();
        this.downloadFailures = 0;
      })
      .catch((err: unknown) => {
        settle();
        this.onDownloadError(download, err);
      });
  }

  private onDownloadProgress(progress: ProgressInfo): void {
    const download = this.download;
    if (!download) {
      return;
    }
    if (!download.announced) {
      // Logged at the first data, so the periodic checks finding a finished download again log nothing.
      download.announced = true;
      log.warn(`Launcher update ${this.availableVersion ?? ""} downloading (${progress.total} bytes)`);
    }
    this.clearStallTimer();
    // Watched from the first data to the last: on macOS Squirrel.Mac then installs the downloaded zip
    // (no progress events), which is not a stall.
    if (progress.transferred < progress.total) {
      this.stallTimer = setTimeout(() => {
        this.stallTimer = undefined;
        log.warn(
          `Launcher update download received nothing for ${DOWNLOAD_STALL_MS / 1000} s ` +
            `at ${Math.floor(progress.percent)}%: starting it again`,
        );
        download.stalled = true;
        download.cancel();
      }, DOWNLOAD_STALL_MS);
    }
  }

  private clearStallTimer(): void {
    if (this.stallTimer) {
      clearTimeout(this.stallTimer);
      this.stallTimer = undefined;
    }
  }

  private onDownloadError(download: UpdateDownload, err: unknown): void {
    const reason = download.stalled ? "stalled" : err instanceof Error ? err.message : String(err);
    if (this.downloadFailures < DOWNLOAD_RETRY_DELAYS_MS.length) {
      const delay = DOWNLOAD_RETRY_DELAYS_MS[this.downloadFailures];
      this.downloadFailures += 1;
      log.warn(`Launcher update download failed (${reason}): trying again in ${delay / 1000} s`);
      if (!this.retryTimer) {
        this.retryTimer = setTimeout(() => {
          this.retryTimer = undefined;
          this.checkForUpdates().catch(() => undefined);
        }, delay);
      }
      return;
    }
    log.error(`Launcher update download failed ${this.downloadFailures + 1} times in a row (${reason})`);
    // The next periodic check starts over with its own retries.
    this.downloadFailures = 0;
    this.onDownloadFailed();
  }

  private onDownloadFailed(): void {
    // The bar then offers the website's download.
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

  /** What installWhenIdle waits for: true while the launcher must not quit (a Dolphin is open...). */
  setBusyCheck(isBusy: () => boolean): void {
    this.isBusy = isBusy;
  }

  /**
   * The update is downloaded (pendingUpdateVersion written): with auto updates on, it installs and
   * restarts the launcher by itself once nothing is running (no Dolphin open, no install or move),
   * looking again every INSTALL_IDLE_POLL_MS until then. No Dolphin starts until the launcher is
   * updated anyway, and installing it only on quit left players stuck on "update the launcher"
   * (bug reports #116 and #117: "doesnt auto update on startup", "how do i update this? do i just
   * restart it?"). Install update in the bar and the install on quit stay as they were.
   */
  installWhenIdle(version: string): void {
    if (this.mode !== "install" || this.autoInstallStarted || this.installTimer) {
      return;
    }
    // Installing this version failed last time (verifyPendingUpdate): trying again at every start
    // could restart the launcher over and over, so the bar's Install update is left to the player.
    if (this.updateState?.status === "failed" && this.updateState.version === version) {
      log.warn(`Launcher update ${version} failed to install last time: not installing it by itself`);
      return;
    }
    const attempt = () => {
      this.installTimer = undefined;
      if (!this.settingsManager.get().settings.autoUpdateLauncher) {
        return;
      }
      if (this.isBusy()) {
        this.installTimer = setTimeout(attempt, INSTALL_IDLE_POLL_MS);
        return;
      }
      this.autoInstallStarted = true;
      // The installer shows its progress and starts the new launcher (quitAndInstall(false, true)).
      log.warn(`Launcher update ${version} downloaded and nothing is running: installing it and restarting`);
      this.quitAndInstall().catch((err) => {
        // Not tried again by itself: the bar's Install update (and Download manually) remain.
        log.error(`Installing launcher update ${version} by itself failed:`, err);
      });
    };
    attempt();
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
