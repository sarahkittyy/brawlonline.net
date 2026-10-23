import type { EventEmitter } from "events";
import type { Mock } from "vitest";
import { afterEach, beforeEach, describe, expect, it, vi } from "vitest";

const { failedEvent } = vi.hoisted(() => ({ failedEvent: vi.fn(async () => undefined) }));

vi.mock("electron", () => ({ app: { getVersion: () => "0.1.0", isPackaged: true } }));
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
  return { autoUpdater };
});
vi.mock("@accounts/config", () => ({
  resolveServiceUrls: () => ({ launcherUpdates: "https://example.test/updates" }),
}));
vi.mock("./ipc", () => ({ ipc_launcherUpdateFailedEvent: { main: { trigger: failedEvent } } }));

import { autoUpdater as realAutoUpdater } from "electron-updater";

import { AppUpdater, INSTALL_IDLE_POLL_MS, LauncherUpdateRequiredError, UPDATE_CHECK_INTERVAL_MS } from "./app_updater";

const autoUpdater = realAutoUpdater as unknown as EventEmitter & {
  checkForUpdates: Mock;
  quitAndInstall: Mock;
};

const settingsManager = { get: () => ({ settings: { autoUpdateLauncher: true } }) } as any;

/** The feed offers `version` (newer than 0.1.0) on the next checks. */
function feedOffers(version: string | null, downloadPromise: Promise<unknown> | null = null) {
  autoUpdater.checkForUpdates.mockImplementation(async () => {
    if (version) {
      autoUpdater.emit("update-available", { version });
    } else {
      autoUpdater.emit("update-not-available", { version: "0.1.0" });
    }
    return { isUpdateAvailable: version != null, downloadPromise };
  });
}

describe("AppUpdater", () => {
  let updater: AppUpdater;

  beforeEach(() => {
    vi.useFakeTimers();
    autoUpdater.removeAllListeners();
    autoUpdater.checkForUpdates.mockReset();
    autoUpdater.quitAndInstall.mockReset();
    failedEvent.mockClear();
    updater = new AppUpdater(settingsManager);
  });

  afterEach(() => {
    vi.useRealTimers();
  });

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

  it("tells the renderer when downloading the update failed", async () => {
    feedOffers("0.2.0", Promise.reject(new Error("download failed")));
    await updater.checkForUpdates();
    await vi.advanceTimersByTimeAsync(0);
    expect(failedEvent).toHaveBeenCalledTimes(1);
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
