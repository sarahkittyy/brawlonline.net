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

import { AppUpdater, LauncherUpdateRequiredError, UPDATE_CHECK_INTERVAL_MS } from "./app_updater";

const autoUpdater = realAutoUpdater as unknown as EventEmitter & {
  checkForUpdates: Mock;
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
