import type { LauncherUpdateMode } from "@common/launcher_update";
import { getLauncherUpdateMode } from "@common/launcher_update";
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
  locale: string;
  updateState?: UpdateState;
  /** How a launcher update is offered: installed in place, or as a download (macOS, see MAC_SELF_UPDATE). */
  launcherUpdateMode: LauncherUpdateMode;
};

export function getAppBootstrap(flags: ConfigFlags, updateState?: UpdateState): AppBootstrap {
  let release = os.release();
  try {
    const name = osName(os.platform(), release);
    release = `${name} (${release})`;
  } catch (err) {
    log.error(err);
  }

  const bootstrap: AppBootstrap = {
    operatingSystem: release,
    flags,
    isDevelopment: process.env.NODE_ENV !== "production",
    isMac: process.platform === "darwin",
    isLinux: process.platform === "linux",
    isWindows: process.platform === "win32",
    locale: app.getLocale(),
    updateState,
    launcherUpdateMode: getLauncherUpdateMode(process.platform),
  };
  return bootstrap;
}
