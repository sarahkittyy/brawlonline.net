import { IsoValidity } from "@common/types";
import type { DolphinManager } from "@dolphin/manager";
import { DolphinLaunchType } from "@dolphin/types";
import type { SettingsManager } from "@settings/settings_manager";
import { format } from "date-fns";
import { app, dialog, ipcMain, nativeImage, shell } from "electron";
import electronLog from "electron-log";
import type { ProgressInfo, UpdateInfo } from "electron-updater";
import { autoUpdater } from "electron-updater";
import os from "os";
import path from "path";

import type { AppUpdater } from "./app_updater";
import { getAppBootstrap } from "./bootstrap";
import type { BrowserWindowManager } from "./browser_window_manager";
import { checkIso } from "./check_iso";
import type { ConfigFlags } from "./flags/flags";
import {
  ipc_checkForUpdate,
  ipc_checkValidIso,
  ipc_clearTempFolder,
  ipc_downloadLogs,
  ipc_installUpdate,
  ipc_launcherUpdateDownloadingEvent,
  ipc_launcherUpdateFoundEvent,
  ipc_launcherUpdateReadyEvent,
  ipc_openInNewBrowserWindow,
  ipc_requiredLauncherUpdate,
  ipc_runNetworkDiagnostics,
  ipc_showOpenDialog,
} from "./ipc";
import type { ArchiveFile } from "./logs_archive";
import { dolphinUserFiles, folderFiles, writeLogsArchive } from "./logs_archive";
import { getNetworkDiagnostics } from "./network_diagnostics";
import { clearTempFolder } from "./util";

const log = electronLog.scope("main/listeners");
const isMac = process.platform === "darwin";

const TRANSPARENT_PIXEL_PNG =
  "data:image/png;base64,iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAYAAAAfFcSJAAAADUlEQVR42mNkYPhfDwAChwGA60e6kgAAAABJRU5ErkJggg==";

export default function setupMainIpc({
  dolphinManager,
  settingsManager,
  flags,
  browserWindowManager,
  appUpdater,
}: {
  dolphinManager: DolphinManager;
  settingsManager: SettingsManager;
  flags: ConfigFlags;
  browserWindowManager: BrowserWindowManager;
  appUpdater: AppUpdater;
}) {
  ipcMain.on("onDragStart", (event, files: string[]) => {
    // The Electron.Item type declaration is missing the files attribute
    // so we'll just cast it as unknown for now.
    event.sender.startDrag({
      files,
      // Slippi drags with its file icon; a transparent pixel keeps drag-and-drop working without art.
      icon: nativeImage.createFromDataURL(TRANSPARENT_PIXEL_PNG),
    } as unknown as Electron.Item);
  });

  ipcMain.on("getAppBootstrapSync", (event) => {
    event.returnValue = getAppBootstrap(flags, appUpdater.getUpdateState());
  });

  ipc_checkValidIso.main!.handle(async ({ path: isoPath }) => {
    // Make sure we have a valid path
    if (!isoPath) {
      return { path: isoPath, valid: IsoValidity.UNVALIDATED };
    }

    return { path: isoPath, valid: await checkIso(isoPath) };
  });

  ipc_downloadLogs.main!.handle(async () => {
    const stamp = format(new Date(), "yyyy-MM-dd-HHmm");
    const { canceled, filePath } = await dialog.showSaveDialog({
      defaultPath: path.join(app.getPath("downloads"), `brawl-online-logs-${stamp}.zip`),
      filters: [{ name: "Zip", extensions: ["zip"] }],
    });
    if (canceled || !filePath) {
      return { path: null };
    }

    // The app name is set before logging starts (main.ts), so dev and packaged builds agree.
    const logsFolder = isMac ? app.getPath("logs") : path.resolve(app.getPath("userData"), "logs");
    const files: ArchiveFile[] = [
      { name: "system.txt", source: { text: await systemInfo(dolphinManager, settingsManager, flags) } },
      ...(await folderFiles(logsFolder, "launcher")),
    ];
    for (const [launchType, zipFolder] of [
      [DolphinLaunchType.NETPLAY, "netplay-dolphin"],
      [DolphinLaunchType.PLAYBACK, "playback-dolphin"],
    ] as const) {
      try {
        files.push(...(await dolphinUserFiles(dolphinManager.getInstallation(launchType).userFolder, zipFolder)));
      } catch (err) {
        log.error(`Could not find the ${launchType} Dolphin's User folder: `, err);
      }
    }

    await writeLogsArchive(filePath, files);
    shell.showItemInFolder(filePath);
    return { path: filePath };
  });

  // check for updates
  autoUpdater.on("update-available", (info: UpdateInfo) => {
    ipc_launcherUpdateFoundEvent.main!.trigger({ version: info.version }).catch(log.warn);
  });

  autoUpdater.on("download-progress", async (progress: ProgressInfo) => {
    if (progress.total !== 0) {
      ipc_launcherUpdateDownloadingEvent
        .main!.trigger({
          progressPercent: progress.percent,
        })
        .catch(log.warn);
    }
  });

  autoUpdater.on("update-downloaded", (info: UpdateInfo) => {
    settingsManager.updateSetting("pendingUpdateVersion", info.version).catch(log.error);
    ipc_launcherUpdateReadyEvent.main!.trigger({}).catch(log.warn);
  });

  ipc_installUpdate.main!.handle(async () => {
    try {
      log.info("Installing update via quitAndInstall");
      await appUpdater.quitAndInstall();
      return { success: true };
    } catch (err) {
      log.error("Failed to install update:", err);
      return { success: false, error: err instanceof Error ? err.message : String(err) };
    }
  });

  ipc_checkForUpdate.main!.handle(async () => {
    return await appUpdater.checkForUpdates(true);
  });

  ipc_requiredLauncherUpdate.main!.handle(async () => {
    return { version: (await appUpdater.requiredUpdate()) ?? null };
  });

  ipc_clearTempFolder.main!.handle(async () => {
    try {
      await clearTempFolder();
    } catch (err) {
      log.error(err);
      throw err;
    }
    return { success: true };
  });

  ipc_showOpenDialog.main!.handle(async (options) => {
    const { canceled, filePaths } = await dialog.showOpenDialog(options);
    return { canceled, filePaths };
  });

  ipc_runNetworkDiagnostics.main!.handle(async () => {
    return getNetworkDiagnostics();
  });

  ipc_openInNewBrowserWindow.main!.handle(async ({ url }: { url: string }) => {
    await browserWindowManager.openInNewBrowserWindow(url);
    return { success: true };
  });
}

/** system.txt in the logs zip: versions, machine and the settings that change how Dolphin starts. */
async function systemInfo(
  dolphinManager: DolphinManager,
  settingsManager: SettingsManager,
  flags: ConfigFlags,
): Promise<string> {
  const { settings } = settingsManager.get();
  const cpus = os.cpus();
  const gpu = (await app.getGPUInfo("basic").catch(() => null)) as { gpuDevice?: unknown } | null;
  const dolphin = (launchType: DolphinLaunchType) =>
    `${dolphinManager.getDolphinExecutablePath(launchType)} (${
      dolphinManager.cachedDolphinVersion(launchType) ?? "version unknown"
    })`;
  const lines: [string, unknown][] = [
    ["Created", new Date().toString()],
    ["Launcher", `${app.getVersion()}${app.isPackaged ? "" : " (development)"}`],
    ["Electron", process.versions.electron],
    ["OS", getAppBootstrap(flags).operatingSystem],
    ["Arch", process.arch],
    ["CPU", `${cpus[0]?.model ?? "unknown"} (${cpus.length} threads)`],
    ["Memory", `${Math.round(os.totalmem() / 2 ** 30)} GiB`],
    ["GPU", JSON.stringify(gpu?.gpuDevice ?? null)],
    ["Netplay Dolphin", dolphin(DolphinLaunchType.NETPLAY)],
    ["Playback Dolphin", dolphin(DolphinLaunchType.PLAYBACK)],
    ["Disc image set", settings.isoPath ? "yes" : "no"],
    ["Launch game on Play", settings.launchGameOnPlay],
  ];
  return lines.map(([label, value]) => `${label}: ${String(value)}\n`).join("");
}
