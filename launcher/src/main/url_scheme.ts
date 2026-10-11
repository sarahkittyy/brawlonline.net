/**
 * Registers the launcher for `brawlonline://` links (src/common/brawl_link.ts).
 *
 * - Windows: the NSIS installer registers it (electron-builder.json `protocols`); the launcher
 *   registers itself again on start (HKCU), which also covers a moved install.
 * - macOS: the bundle's Info.plist declares it (electron-builder `protocols`); registering on start
 *   makes this copy the handler when there are several.
 * - Linux (AppImage): nothing installs the AppImage, so the launcher writes a hidden desktop entry
 *   for the AppImage's current path (`~/.local/share/applications`) and makes it the scheme's
 *   handler with `xdg-mime`. Rewritten when the AppImage moves.
 *
 * Unpackaged and test-mode runs (development, staging builds next to a real install) don't
 * register: they would take the links away from the installed launcher.
 */

import { APP_ID, PRODUCT_NAME, URL_SCHEME } from "@common/product";
import { execFile } from "child_process";
import { app } from "electron";
import electronLog from "electron-log";
import { mkdir, readFile, writeFile } from "fs/promises";
import os from "os";
import path from "path";

const log = electronLog.scope("url-scheme");

/** The Linux desktop entry's file name. */
export const LINUX_DESKTOP_FILE = `${APP_ID}-links.desktop`;

/** Quotes an Exec argument as the desktop entry spec asks. */
function quoteExecArg(arg: string): string {
  return `"${arg.replace(/(["`$\\])/g, "\\$1")}"`;
}

/** The hidden desktop entry that opens `brawlonline://` links with the AppImage at `appImagePath`. */
export function linuxDesktopEntry(appImagePath: string): string {
  return [
    "[Desktop Entry]",
    "Type=Application",
    `Name=${PRODUCT_NAME}`,
    `Exec=${quoteExecArg(appImagePath)} %u`,
    "Terminal=false",
    "NoDisplay=true",
    `MimeType=x-scheme-handler/${URL_SCHEME};`,
    "",
  ].join("\n");
}

async function registerLinux(appImagePath: string): Promise<void> {
  const dataHome = process.env.XDG_DATA_HOME || path.join(os.homedir(), ".local", "share");
  const dir = path.join(dataHome, "applications");
  const file = path.join(dir, LINUX_DESKTOP_FILE);
  const entry = linuxDesktopEntry(appImagePath);
  const current = await readFile(file, "utf8").catch(() => null);
  if (current !== entry) {
    await mkdir(dir, { recursive: true });
    await writeFile(file, entry);
    log.info(`Wrote ${file} for ${appImagePath}`);
  }
  await new Promise<void>((resolve) => {
    execFile("xdg-mime", ["default", LINUX_DESKTOP_FILE, `x-scheme-handler/${URL_SCHEME}`], (err) => {
      if (err) {
        log.warn(`xdg-mime could not register ${URL_SCHEME}:// links: ${err.message}`);
      }
      resolve();
    });
  });
}

export async function registerUrlScheme(opts: { packaged: boolean; testMode: boolean }): Promise<void> {
  if (!opts.packaged || opts.testMode) {
    log.info(`Not registering ${URL_SCHEME}:// links (unpackaged or test mode)`);
    return;
  }
  try {
    if (process.platform === "linux") {
      const appImage = process.env.APPIMAGE;
      if (appImage) {
        await registerLinux(appImage);
      }
      return;
    }
    if (!app.isDefaultProtocolClient(URL_SCHEME)) {
      const ok = app.setAsDefaultProtocolClient(URL_SCHEME);
      log.info(`Registered for ${URL_SCHEME}:// links: ${ok}`);
    }
  } catch (err) {
    log.warn(`Could not register ${URL_SCHEME}:// links: ${err instanceof Error ? err.message : String(err)}`);
  }
}
