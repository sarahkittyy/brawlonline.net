import { INPUT_DELAY_AUTO } from "@common/input_delay";
import { LEGACY_PRODUCT_NAMES, PRODUCT_NAME } from "@common/product";
import { app } from "electron";
import log from "electron-log";
import fs from "fs";
import path from "path";

import type { AppSettings } from "./types";

function getDefaultRootSlpPath(): string {
  let root = app.getPath("home");
  if (process.platform === "win32") {
    try {
      root = app.getPath("documents");
    } catch {
      // there are rare cases where documents isn't defined so just use home instead
      log.error("Couldn't get the documents path");
    }
  }
  const current = path.join(root, PRODUCT_NAME);
  // Defaults are not persisted (SettingsManager merges them at read time), so a renamed product
  // would point existing profiles at a new, empty folder. Keep a folder from an earlier name.
  if (!fs.existsSync(current)) {
    const legacy = LEGACY_PRODUCT_NAMES.map((name) => path.join(root, name)).find((p) => fs.existsSync(p));
    if (legacy) {
      return legacy;
    }
  }
  return current;
}

export const defaultAppSettings: AppSettings = {
  accounts: {
    activeId: null,
    list: [],
  },
  settings: {
    isoPath: null,
    rootSlpPath: getDefaultRootSlpPath(),
    enableNetplayReplays: true,
    useMonthlySubfolders: true,
    extraSlpPaths: [],
    launchGameOnPlay: true,
    inputDelay: INPUT_DELAY_AUTO,
    autoUpdateLauncher: true,
    netplayDolphinPath: null,
    playbackDolphinPath: null,
    enableRankDisplay: true,
  },
};
