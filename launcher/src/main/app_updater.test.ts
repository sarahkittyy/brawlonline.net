import type { EventEmitter } from "events";
import { mkdtempSync, rmSync, writeFileSync } from "node:fs";
import os from "os";
import path from "path";
import type { Mock } from "vitest";
import { afterEach, beforeEach, describe, expect, it, vi } from "vitest";

const { failedEvent, closeAllConnections, fromPartition } = vi.hoisted(() => {
  const closeAllConnections = vi.fn(async () => undefined);
  return {
    failedEvent: vi.fn(async () => undefined),
    closeAllConnections,
    fromPartition: vi.fn(() => ({ closeAllConnections })),
  };
});

vi.mock("electron", () => ({ app: { getVersion: () => "0.1.0", isPackaged: true }, session: { fromPartition } }));
vi.mock("electron-log", () => ({ default: { info: vi.fn(), warn: vi.fn(), error: vi.fn() } }));
// electron-updater's autoUpdater, as an emitter whose checks the tests answer.
vi.mock("electron-updater", async () => {
  const { EventEmitter } = await import("events");
  const autoUpdater = Object.assign(new EventEmitter(), {
    checkForUpdates: vi.fn(),
    checkForUpdatesAndNotify: vi.fn(),
    setFeedURL: vi.fn(),
    quitAndInstall: vi.fn(),
  });
  return { autoUpdater, AppImageUpdater: class AppImageUpdater {} };
});
vi.mock("@accounts/config", () => ({
  resolveServiceUrls: () => ({ launcherUpdates: "https://example.test/updates" }),
}));
vi.mock("./ipc", () => ({ ipc_launcherUpdateFailedEvent: { main: { trigger: failedEvent } } }));

import { autoUpdater as realAutoUpdater } from "electron-updater";

import {
  AppUpdater,
  describeInstallerLog,
  DOWNLOAD_MIN_BYTES_PER_STALL_WINDOW,
  DOWNLOAD_RETRY_DELAYS_MS,
  DOWNLOAD_STALL_MS,
  INSTALL_IDLE_POLL_MS,
  INSTALLER_LOG_FILE,
  LauncherUpdateRequiredError,
  UPDATE_CHECK_INTERVAL_MS,
  UPDATE_DIR_ENV,
} from "./app_updater";

const autoUpdater = realAutoUpdater as unknown as EventEmitter & {
  checkForUpdates: Mock;
  quitAndInstall: Mock;
};

const settingsManager = { get: () => ({ settings: { autoUpdateLauncher: true } }) } as any;

/** The feed offers `version` (newer than 0.1.0) on the next checks. */
function feedOffers(
  version: string | null,
  downloadPromise: Promise<unknown> | null = null,
  cancellationToken?: { cancel: () => void },
) {
  autoUpdater.checkForUpdates.mockImplementation(async () => {
    if (version) {
      autoUpdater.emit("update-available", { version });
    } else {
      autoUpdater.emit("update-not-available", { version: "0.1.0" });
    }
    return { isUpdateAvailable: version != null, downloadPromise, cancellationToken };
  });
}

/** A download the test settles, with the cancellation token electron-updater hands out for it. */
function pendingDownload() {
  let settle!: { resolve: () => void; reject: (err: Error) => void };
  const promise = new Promise<void>((resolve, reject) => {
    settle = { resolve, reject };
  });
  const token = { cancel: vi.fn(() => settle.reject(new Error("cancelled"))) };
  return { promise, resolve: () => settle.resolve(), reject: (err: Error) => settle.reject(err), token };
}

/** Every check from now on starts a new download that fails at once. */
function downloadsFail() {
  autoUpdater.checkForUpdates.mockImplementation(async () => {
    autoUpdater.emit("update-available", { version: "0.2.0" });
    return { isUpdateAvailable: true, downloadPromise: Promise.reject(new Error("net::ERR_QUIC_PROTOCOL_ERROR")) };
  });
}

function progress(percent: number) {
  const total = 145_000_000;
  autoUpdater.emit("download-progress", { percent, total, transferred: (total * percent) / 100 });
}

describe("AppUpdater", () => {
  let updater: AppUpdater;

  beforeEach(() => {
    vi.useFakeTimers();
    autoUpdater.removeAllListeners();
    autoUpdater.checkForUpdates.mockReset();
    autoUpdater.quitAndInstall.mockReset();
    failedEvent.mockClear();
    closeAllConnections.mockClear();
    fromPartition.mockClear();
    updater = new AppUpdater(settingsManager);
  });

  afterEach(() => {
    vi.useRealTimers();
  });

  it.runIf(process.platform === "win32")(
    "tells the Windows installer the folder it runs from (bug report #359)",
    () => {
      expect(process.env[UPDATE_DIR_ENV]).toBe(path.dirname(process.execPath));
    },
  );

  it("lets Dolphin start while no newer launcher is out", async () => {
    feedOffers(null);
    await expect(updater.assertUpToDate()).resolves.toBeUndefined();
    expect(autoUpdater.checkForUpdates).toHaveBeenCalledTimes(1);
  });

  it("stops Dolphin while a newer launcher is out, checking again first", async () => {
    feedOffers("0.2.0");
    const err = await updater.assertUpToDate().catch((e) => e);
    expect(err).toBeInstanceOf(LauncherUpdateRequiredError);
    expect(err.version).toBe("0.2.0");
    expect(await updater.requiredUpdate()).toBe("0.2.0");
    // The check a moment ago is recent enough.
    expect(autoUpdater.checkForUpdates).toHaveBeenCalledTimes(1);
  });

  it("finds a release from a launcher left open, with its periodic checks", async () => {
    feedOffers(null);
    await updater.checkForUpdates();
    updater.startPeriodicChecks();
    expect(await updater.requiredUpdate()).toBeUndefined();

    feedOffers("0.2.0");
    await vi.advanceTimersByTimeAsync(UPDATE_CHECK_INTERVAL_MS);
    expect(autoUpdater.checkForUpdates).toHaveBeenCalledTimes(2);
    expect(await updater.requiredUpdate()).toBe("0.2.0");
  });

  it("does not hold Dolphin back when the check does not answer (offline)", async () => {
    autoUpdater.checkForUpdates.mockImplementation(() => new Promise(() => undefined));
    const start = updater.assertUpToDate();
    await vi.advanceTimersByTimeAsync(5000);
    await expect(start).resolves.toBeUndefined();
  });

  it("does not hold Dolphin back when the check fails", async () => {
    autoUpdater.checkForUpdates.mockRejectedValue(new Error("net::ERR_INTERNET_DISCONNECTED"));
    await expect(updater.assertUpToDate()).resolves.toBeUndefined();
  });

  it("tries a failed download again before telling the renderer it failed", async () => {
    downloadsFail();
    await updater.checkForUpdates();
    for (const delay of DOWNLOAD_RETRY_DELAYS_MS) {
      await vi.advanceTimersByTimeAsync(delay - 1);
      expect(failedEvent).not.toHaveBeenCalled();
      await vi.advanceTimersByTimeAsync(1);
    }
    await vi.advanceTimersByTimeAsync(0);
    expect(autoUpdater.checkForUpdates).toHaveBeenCalledTimes(1 + DOWNLOAD_RETRY_DELAYS_MS.length);
    expect(failedEvent).toHaveBeenCalledTimes(1);
  });

  it("does not tell the renderer when a download tried again succeeds", async () => {
    downloadsFail();
    await updater.checkForUpdates();
    feedOffers("0.2.0", Promise.resolve());
    await vi.advanceTimersByTimeAsync(DOWNLOAD_RETRY_DELAYS_MS[0]);
    expect(autoUpdater.checkForUpdates).toHaveBeenCalledTimes(2);
    await vi.advanceTimersByTimeAsync(10 * 60 * 1000);
    expect(failedEvent).not.toHaveBeenCalled();
  });

  it("starts a stalled download again (bug report #119)", async () => {
    const first = pendingDownload();
    feedOffers("0.2.0", first.promise, first.token);
    await updater.checkForUpdates();
    progress(10);
    await vi.advanceTimersByTimeAsync(DOWNLOAD_STALL_MS - 1000);
    progress(11);
    await vi.advanceTimersByTimeAsync(DOWNLOAD_STALL_MS - 1000);
    expect(first.token.cancel).not.toHaveBeenCalled();

    const second = pendingDownload();
    feedOffers("0.2.0", second.promise, second.token);
    await vi.advanceTimersByTimeAsync(1000);
    expect(first.token.cancel).toHaveBeenCalledTimes(1);
    await vi.advanceTimersByTimeAsync(DOWNLOAD_RETRY_DELAYS_MS[0]);
    expect(autoUpdater.checkForUpdates).toHaveBeenCalledTimes(2);
    expect(failedEvent).not.toHaveBeenCalled();
    second.resolve();
  });

  it("starts a crawling download again, on new connections (bug report #385)", async () => {
    const first = pendingDownload();
    feedOffers("0.2.0", first.promise, first.token);
    await updater.checkForUpdates();
    // Some data every 10 s (so the stall check never fires), 0.5% (725 KB) a minute: the bar stays at 0%.
    progress(0.1);
    for (let i = 1; i < 6; i++) {
      await vi.advanceTimersByTimeAsync(10_000);
      progress(0.1 + i * 0.08);
      expect(first.token.cancel).not.toHaveBeenCalled();
    }
    const second = pendingDownload();
    feedOffers("0.2.0", second.promise, second.token);
    await vi.advanceTimersByTimeAsync(10_000);
    progress(0.6);
    expect(first.token.cancel).toHaveBeenCalledTimes(1);
    await vi.advanceTimersByTimeAsync(DOWNLOAD_RETRY_DELAYS_MS[0]);
    expect(fromPartition).toHaveBeenCalledWith("electron-updater", { cache: false });
    expect(closeAllConnections).toHaveBeenCalledTimes(1);
    expect(autoUpdater.checkForUpdates).toHaveBeenCalledTimes(2);
    expect(closeAllConnections.mock.invocationCallOrder[0]).toBeLessThan(
      autoUpdater.checkForUpdates.mock.invocationCallOrder[1],
    );
    expect(failedEvent).not.toHaveBeenCalled();
    second.resolve();
  });

  it("lets a slow download that keeps going finish", async () => {
    const download = pendingDownload();
    feedOffers("0.2.0", download.promise, download.token);
    await updater.checkForUpdates();
    // 1% (1.45 MB) a minute, over the minimum.
    expect(145_000_000 / 100).toBeGreaterThan(DOWNLOAD_MIN_BYTES_PER_STALL_WINDOW);
    for (let second = 0; second <= 10 * 60; second += 5) {
      progress(second / 60);
      await vi.advanceTimersByTimeAsync(5000);
    }
    expect(download.token.cancel).not.toHaveBeenCalled();
    download.resolve();
  });

  it("does not cancel a download once all of it has arrived (macOS then installs it, with no progress)", async () => {
    const download = pendingDownload();
    feedOffers("0.2.0", download.promise, download.token);
    await updater.checkForUpdates();
    progress(100);
    await vi.advanceTimersByTimeAsync(DOWNLOAD_STALL_MS * 3);
    expect(download.token.cancel).not.toHaveBeenCalled();
    download.resolve();
  });

  it("handles a download once when another check joins it", async () => {
    const download = pendingDownload();
    // electron-updater hands a check made during a download the same promise (a new, unused token).
    feedOffers("0.2.0", download.promise, download.token);
    await updater.checkForUpdates();
    feedOffers("0.2.0", download.promise, { cancel: vi.fn() });
    await updater.checkForUpdates();
    downloadsFail();
    download.reject(new Error("net::ERR_QUIC_PROTOCOL_ERROR"));
    await vi.advanceTimersByTimeAsync(DOWNLOAD_RETRY_DELAYS_MS[0]);
    // One retry, not one per check.
    expect(autoUpdater.checkForUpdates).toHaveBeenCalledTimes(3);
  });
});

describe("AppUpdater installing a downloaded update by itself", () => {
  /** Settings with auto updates `autoUpdate` and 0.2.0 downloaded (pendingUpdateVersion). */
  function settingsWith(autoUpdate: boolean) {
    const state: { settings: { autoUpdateLauncher: boolean }; pendingUpdateVersion?: string } = {
      settings: { autoUpdateLauncher: autoUpdate },
      pendingUpdateVersion: "0.2.0",
    };
    return {
      state,
      get: () => state,
      updateSetting: vi.fn(async (key: "pendingUpdateVersion", value: string | undefined) => {
        state[key] = value;
      }),
    } as any;
  }

  beforeEach(() => {
    vi.useFakeTimers();
    autoUpdater.removeAllListeners();
    autoUpdater.quitAndInstall.mockReset();
  });

  afterEach(() => {
    vi.useRealTimers();
  });

  it("installs and restarts as soon as it is downloaded while nothing runs", () => {
    const updater = new AppUpdater(settingsWith(true));
    updater.installWhenIdle("0.2.0");
    expect(autoUpdater.quitAndInstall).toHaveBeenCalledTimes(1);
    // Not silent, and the new launcher starts after (the same as Install update).
    expect(autoUpdater.quitAndInstall).toHaveBeenCalledWith(false, true);
  });

  it("waits for Dolphin (or an install) to finish first", async () => {
    const updater = new AppUpdater(settingsWith(true));
    let busy = true;
    updater.setBusyCheck(() => busy);
    updater.installWhenIdle("0.2.0");
    await vi.advanceTimersByTimeAsync(INSTALL_IDLE_POLL_MS * 3);
    expect(autoUpdater.quitAndInstall).not.toHaveBeenCalled();

    busy = false;
    await vi.advanceTimersByTimeAsync(INSTALL_IDLE_POLL_MS);
    expect(autoUpdater.quitAndInstall).toHaveBeenCalledTimes(1);
  });

  it("installs once, however often the download is found again", async () => {
    const updater = new AppUpdater(settingsWith(true));
    let busy = true;
    updater.setBusyCheck(() => busy);
    // The periodic checks find the same download each time (update-downloaded again).
    updater.installWhenIdle("0.2.0");
    updater.installWhenIdle("0.2.0");
    busy = false;
    updater.installWhenIdle("0.2.0");
    await vi.advanceTimersByTimeAsync(INSTALL_IDLE_POLL_MS * 2);
    updater.installWhenIdle("0.2.0");
    expect(autoUpdater.quitAndInstall).toHaveBeenCalledTimes(1);
  });

  it("leaves it to Install update while auto updates are off", async () => {
    const settings = settingsWith(false);
    const updater = new AppUpdater(settings);
    updater.installWhenIdle("0.2.0");
    await vi.advanceTimersByTimeAsync(INSTALL_IDLE_POLL_MS * 2);
    expect(autoUpdater.quitAndInstall).not.toHaveBeenCalled();
  });

  it("stops waiting when auto updates are turned off meanwhile", async () => {
    const settings = settingsWith(true);
    const updater = new AppUpdater(settings);
    let busy = true;
    updater.setBusyCheck(() => busy);
    updater.installWhenIdle("0.2.0");
    settings.state.settings.autoUpdateLauncher = false;
    busy = false;
    await vi.advanceTimersByTimeAsync(INSTALL_IDLE_POLL_MS * 2);
    expect(autoUpdater.quitAndInstall).not.toHaveBeenCalled();
  });

  it("does not install a version by itself again after it failed to install", async () => {
    // Started on 0.1.0 though 0.2.0 was to be installed: that install failed.
    const settings = settingsWith(true);
    const updater = new AppUpdater(settings);
    await updater.verifyPendingUpdate();
    expect(updater.getUpdateState()).toEqual({ status: "failed", version: "0.2.0" });

    // The start's check finds the same download: no restart (it could fail at every start).
    await settings.updateSetting("pendingUpdateVersion", "0.2.0");
    updater.installWhenIdle("0.2.0");
    expect(autoUpdater.quitAndInstall).not.toHaveBeenCalled();
    // A newer one is installed by itself.
    updater.installWhenIdle("0.3.0");
    expect(autoUpdater.quitAndInstall).toHaveBeenCalledTimes(1);
  });
});

describe("describeInstallerLog", () => {
  let dir: string;
  beforeEach(() => {
    dir = mkdtempSync(path.join(os.tmpdir(), "installer-log-"));
  });
  afterEach(() => {
    rmSync(dir, { recursive: true, force: true });
  });

  it("gives the installer's log of the update (bug report #280)", () => {
    expect(INSTALLER_LOG_FILE).toBe("Brawl Online update.log");
    writeFileSync(
      path.join(dir, INSTALLER_LOG_FILE),
      "2026-11-11 11:22:16.120 installer 0.1.138: started\r\n" +
        "2026-11-11 11:22:30.500 installer 0.1.138: the old version's uninstaller exited with 2\r\n",
    );
    const text = describeInstallerLog(dir);
    expect(text).toContain(`The installer's log (${path.join(dir, INSTALLER_LOG_FILE)}, written `);
    expect(text).toMatch(/started\r\n.*exited with 2$/s);
  });

  it("keeps the end of a long log", () => {
    writeFileSync(path.join(dir, INSTALLER_LOG_FILE), `${"x".repeat(20000)}the end`);
    const text = describeInstallerLog(dir);
    expect(text.endsWith("the end")).toBe(true);
    expect(text.length).toBeLessThan(8200);
  });

  it("says so when the installer left none", () => {
    expect(describeInstallerLog(dir)).toMatch(/^The installer left no log/);
  });
});
