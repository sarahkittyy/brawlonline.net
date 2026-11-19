import type { LauncherUpdateMode } from "@common/launcher_update";
import { getLauncherUpdateMode } from "@common/launcher_update";
import { readTestMode } from "@common/test_mode";
import { app } from "electron";
import log from "electron-log";
import os from "os";
import osName from "os-name";

import type { UpdateState } from "./app_updater";
import type { ConfigFlags } from "./flags/flags";

export type AppBootstrap = {
  operatingSystem: string;
  flags: ConfigFlags;
  isDevelopment: boolean;
  isMac: boolean;
  isLinux: boolean;
  isWindows: boolean;
  /** process.arch: "x64" or "arm64" (Linux has a build of each). */
  arch: string;
  locale: string;
  updateState?: UpdateState;
  /** How a launcher update is offered: installed in place, or as a download (macOS, see MAC_SELF_UPDATE). */
  launcherUpdateMode: LauncherUpdateMode;
  /** The launcher's test mode (common/test_mode.ts): previews for screenshots and tests. */
  testMode: boolean;
};

/**
 * The OS for the build info and bug reports, e.g. "Windows 11 (10.0.26100)" or "Linux 6.8 (6.8.0-45)".
 * Not os-name on Windows: for Windows 10 and 11 it runs `wmic os get Caption` to tell Windows Server
 * apart, or PowerShell where wmic is gone (Windows 11 24H2 on), synchronously, and the page's preload
 * waited for it (getAppBootstrapSync) at every start. Electron runs on Windows 10 and 11 only.
 */
export function operatingSystemName(platform: NodeJS.Platform, release: string): string {
  if (platform === "win32") {
    const version = /^(\d+)\.(\d+)\.(\d+)/.exec(release);
    const name =
      version && version[1] === "10" && version[2] === "0"
        ? Number(version[3]) >= 22000
          ? "Windows 11"
          : "Windows 10"
        : "Windows";
    return `${name} (${release})`;
  }
  try {
    return `${osName(platform, release)} (${release})`;
  } catch (err) {
    log.error(err);
    return release;
  }
}

let operatingSystem: string | undefined;

export function getAppBootstrap(flags: ConfigFlags, updateState?: UpdateState): AppBootstrap {
  operatingSystem ??= operatingSystemName(os.platform(), os.release());

  const bootstrap: AppBootstrap = {
    operatingSystem,
    flags,
    isDevelopment: process.env.NODE_ENV !== "production",
    isMac: process.platform === "darwin",
    isLinux: process.platform === "linux",
    isWindows: process.platform === "win32",
    arch: process.arch,
    locale: app.getLocale(),
    updateState,
    launcherUpdateMode: getLauncherUpdateMode(process.platform),
    testMode: readTestMode(process.env, app.isPackaged).active,
  };
  return bootstrap;
}
