import type { DolphinManager } from "@dolphin/manager";
import { DolphinLaunchType } from "@dolphin/types";
import { ipcMain } from "electron";
import { autoUpdater } from "electron-updater";
import path from "path";

import { ipc_updateSettings } from "./ipc";
import type { SettingsManager } from "./settings_manager";

export default function setupSettingsIpc({
  settingsManager,
  dolphinManager,
}: {
  settingsManager: SettingsManager;
  dolphinManager: DolphinManager;
}) {
  // NEW: Subscribe to setting changes for side effects
  // This inverts the dependency - SettingsManager doesn't need to know about DolphinManager
  setupSettingsSubscriptions(settingsManager, dolphinManager);

  // Synchronous getter
  ipcMain.on("getAppSettingsSync", (event) => {
    const settings = settingsManager.get();
    event.returnValue = settings;
  });

  // Generic batch update
  ipc_updateSettings.main!.handle(async ({ updates }) => {
    await settingsManager.updateSettings(updates);
    return { success: true };
  });
}

/**
 * Setup subscriptions to setting changes
 * Each module handles its own side effects by subscribing to relevant settings
 */
function setupSettingsSubscriptions(settingsManager: SettingsManager, dolphinManager: DolphinManager) {
  // Subscribe to ISO path changes
  // The callback receives properly typed values - no casting needed! ✓
  settingsManager.onSettingChange("isoPath", async (isoPath) => {
    if (isoPath) {
      // TypeScript knows isoPath is string | null here
      const gameDir = path.dirname(isoPath);
      const netplayInstall = dolphinManager.getInstallation(DolphinLaunchType.NETPLAY);
      const playbackInstall = dolphinManager.getInstallation(DolphinLaunchType.PLAYBACK);
      await Promise.all([
        netplayInstall.addGamePath(gameDir),
        playbackInstall.addGamePath(gameDir),
        netplayInstall.setDefaultIso(isoPath),
      ]);
    }
  });

  settingsManager.onSettingChange("rootSlpPath", async (replayPath) => {
    const installation = dolphinManager.getInstallation(DolphinLaunchType.NETPLAY);
    await installation.updateSettings({ replayPath });
  });

  settingsManager.onSettingChange("enableNetplayReplays", async (enableNetplayReplays) => {
    const installation = dolphinManager.getInstallation(DolphinLaunchType.NETPLAY);
    await installation.updateSettings({ enableNetplayReplays });
  });

  settingsManager.onSettingChange("useMonthlySubfolders", async (enableMonthlySubfolders) => {
    const installation = dolphinManager.getInstallation(DolphinLaunchType.NETPLAY);
    await installation.updateSettings({ enableMonthlySubfolders });
  });

  // A different Dolphin build: re-check it like Slippi re-checks after an install
  settingsManager.onSettingChange("netplayDolphinPath", async () => {
    await dolphinManager.installDolphin(DolphinLaunchType.NETPLAY);
  });

  settingsManager.onSettingChange("playbackDolphinPath", async () => {
    await dolphinManager.installDolphin(DolphinLaunchType.PLAYBACK);
  });

  settingsManager.onSettingChange("autoUpdateLauncher", (autoUpdateLauncher) => {
    autoUpdater.autoInstallOnAppQuit = autoUpdateLauncher;
  });
}
