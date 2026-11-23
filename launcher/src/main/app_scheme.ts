import { GAME_ASSET_SCHEME_PRIVILEGES, resolveSchemePath } from "@game_assets/protocol";
import type { SettingsManager } from "@settings/settings_manager";
import type { CustomScheme } from "electron";
import { BrowserWindow, ipcMain, net, protocol } from "electron";
import electronLog from "electron-log";
import path from "path";
import { pathToFileURL } from "url";

const log = electronLog.scope("app_scheme");

/**
 * The launcher's page is served from app://launcher/ instead of file://. Chromium keeps V8's
 * compiled code of a page's scripts (its code cache) only for http(s) and schemes registered with
 * `codeCache`, never for file://, so every start compiled all of renderer.js again: about 150 ms
 * of each start on a fast PC. From this scheme a start uses the code the starts before it compiled.
 */
export const APP_SCHEME = "app";
const APP_HOST = "launcher";

const APP_SCHEME_PRIVILEGES: CustomScheme = {
  scheme: APP_SCHEME,
  privileges: { standard: true, secure: true, supportFetchAPI: true, codeCache: true },
};

/** Before the app is ready (Electron takes a single call, with every privileged scheme). */
export function registerPrivilegedSchemes() {
  protocol.registerSchemesAsPrivileged([GAME_ASSET_SCHEME_PRIVILEGES, APP_SCHEME_PRIVILEGES]);
}

/** The page `file` (in dist/renderer) as an app:// URL. */
export function appSchemeUrl(file: string): string {
  return `${APP_SCHEME}://${APP_HOST}/${file}`;
}

/** Serves app://launcher/<file> from `rendererDir`, nothing outside it. After the app is ready. */
export function handleAppScheme(rendererDir: string) {
  protocol.handle(APP_SCHEME, async (request) => {
    const file = resolveSchemePath(rendererDir, request.url, APP_HOST);
    if (!file) {
      return new Response("Not found", { status: 404 });
    }
    try {
      return await net.fetch(pathToFileURL(file).toString());
    } catch {
      return new Response("Not found", { status: 404 });
    }
  });
}

/** The page whose localStorage a launcher before app:// kept (any file:// page shares it). */
const LEGACY_STORAGE_PAGE = "storage.html";
const LEGACY_STORAGE_TIMEOUT_MS = 5000;

/**
 * localStorage belongs to the page's origin, and the launcher's moved from file:// to app://launcher,
 * so its entries (the language picked, pane widths, the replay folder, ...) stay behind. Once, at
 * the first start served from app://: a hidden window at a blank file:// page reads them, and the
 * main window's preload (`takeLegacyLocalStorage`) writes them into the new origin before any page
 * script reads it. Later starts skip this (`rendererStorageMoved`).
 */
export class LegacyLocalStorage {
  private entries: Promise<[string, string][] | null> | null = null;

  constructor(private readonly settingsManager: SettingsManager, private readonly rendererDir: string) {
    ipcMain.on("takeLegacyLocalStorage", (event) => {
      const entries = this.entries;
      this.entries = null;
      if (!entries) {
        event.returnValue = null;
        return;
      }
      // The preload waits (sendSync) until the hidden window has answered.
      void this.handOver(entries, event).catch(log.error);
    });
  }

  private async handOver(entries: Promise<[string, string][] | null>, event: Electron.IpcMainEvent) {
    event.returnValue = await entries;
    await this.settingsManager.updateSetting("rendererStorageMoved", true);
  }

  /** Starts reading the old entries if no start has moved them yet. Before the main window's page loads. */
  start() {
    if (this.entries || this.settingsManager.get().rendererStorageMoved) {
      return;
    }
    this.entries = withTimeout(this.read(), LEGACY_STORAGE_TIMEOUT_MS).catch((err) => {
      log.warn(`Could not read the file:// page's localStorage: ${err}`);
      return null;
    });
  }

  private async read(): Promise<[string, string][]> {
    const win = new BrowserWindow({ show: false, webPreferences: { sandbox: true } });
    try {
      await win.loadURL(pathToFileURL(path.join(this.rendererDir, LEGACY_STORAGE_PAGE)).toString());
      const json: string = await win.webContents.executeJavaScript("JSON.stringify(Object.entries(localStorage))");
      const entries = JSON.parse(json) as [string, string][];
      log.info(`Moving ${entries.length} localStorage entries from file:// to ${appSchemeUrl("")}`);
      return entries;
    } finally {
      win.destroy();
    }
  }
}

function withTimeout<T>(promise: Promise<T>, ms: number): Promise<T> {
  let timer: ReturnType<typeof setTimeout> | undefined;
  const timeout = new Promise<never>((_resolve, reject) => {
    timer = setTimeout(() => reject(new Error(`no answer in ${ms} ms`)), ms);
  });
  return Promise.race([promise, timeout]).finally(() => clearTimeout(timer));
}
