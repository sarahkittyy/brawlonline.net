import { ipcRenderer } from "electron";

import {
  ipc_checkForUpdate,
  ipc_checkValidIso,
  ipc_clearTempFolder,
  ipc_downloadLogs,
  ipc_installUpdate,
  ipc_isoVerificationProgressEvent,
  ipc_launcherUpdateDownloadingEvent,
  ipc_launcherUpdateFailedEvent,
  ipc_launcherUpdateFoundEvent,
  ipc_launcherUpdateReadyEvent,
  ipc_openInNewBrowserWindow,
  ipc_requiredLauncherUpdate,
  ipc_runNetworkDiagnostics,
  ipc_showOpenDialog,
} from "./ipc";

export default {
  onDragStart(filePaths: string[]) {
    ipcRenderer.send("onDragStart", filePaths);
  },
  async checkValidIso(path: string) {
    const { result } = await ipc_checkValidIso.renderer!.trigger({ path });
    return result;
  },
  /** The zip's path, or null if the player cancelled the save dialog. */
  async downloadLogs(): Promise<string | null> {
    const { result } = await ipc_downloadLogs.renderer!.trigger({});
    return result.path;
  },
  async checkForAppUpdates(): Promise<{ updateAvailable: boolean }> {
    const { result } = await ipc_checkForUpdate.renderer!.trigger({});
    return result;
  },
  /** The newer launcher version that is out, or null; Dolphin does not start while there is one. */
  async requiredAppUpdate(): Promise<string | null> {
    const { result } = await ipc_requiredLauncherUpdate.renderer!.trigger({});
    return result.version;
  },
  async installAppUpdate(): Promise<{ success: boolean; error?: string }> {
    const { result } = await ipc_installUpdate.renderer!.trigger({});
    return result;
  },
  async clearTempFolder() {
    const { result } = await ipc_clearTempFolder.renderer!.trigger({});
    return result;
  },
  async showOpenDialog(options: Electron.OpenDialogOptions) {
    const { result } = await ipc_showOpenDialog.renderer!.trigger(options);
    return result;
  },
  async runNetworkDiagnostics() {
    const { result } = await ipc_runNetworkDiagnostics.renderer!.trigger({});
    return result;
  },
  async openInNewBrowserWindow(url: string): Promise<void> {
    await ipc_openInNewBrowserWindow.renderer!.trigger({ url });
  },
  onAppUpdateFound(handle: (version: string) => void) {
    const { destroy } = ipc_launcherUpdateFoundEvent.renderer!.handle(async ({ version }) => {
      handle(version);
    });
    return destroy;
  },
  onAppUpdateDownloadProgress(handle: (percent: number) => void) {
    const { destroy } = ipc_launcherUpdateDownloadingEvent.renderer!.handle(async ({ progressPercent }) => {
      handle(progressPercent);
    });
    return destroy;
  },
  /** Progress of a running `checkValidIso` (bytes hashed), for the "Verifying" indicators. */
  onIsoVerificationProgress(handle: (progress: { path: string; current: number; total: number }) => void) {
    const { destroy } = ipc_isoVerificationProgressEvent.renderer!.handle(async (progress) => {
      handle(progress);
    });
    return destroy;
  },
  onAppUpdateReady(handle: () => void) {
    const { destroy } = ipc_launcherUpdateReadyEvent.renderer!.handle(async () => {
      handle();
    });
    return destroy;
  },
  onAppUpdateFailed(handle: () => void) {
    const { destroy } = ipc_launcherUpdateFailedEvent.renderer!.handle(async () => {
      handle();
    });
    return destroy;
  },
};
