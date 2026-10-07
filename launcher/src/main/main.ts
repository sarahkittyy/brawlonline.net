/* eslint-disable @typescript-eslint/no-var-requires */
/* eslint global-require: off, no-console: off, promise/always-return: off */

/**
 * This module executes inside of electron's main process. You can start
 * electron renderer process from here and communicate with the other processes
 * through IPC.
 *
 * When running `npm run build` or `npm run build:main`, this file is compiled to
 * `./src/main.js` using webpack. This gives us some performance wins.
 */
import { delay } from "@common/delay";
import { Preconditions } from "@common/preconditions";
import { PRODUCT_NAME } from "@common/product";
import { DolphinLaunchType } from "@dolphin/types";
import { registerGameAssetScheme } from "@game_assets/protocol";
import { ipc_statsPageRequestedEvent } from "@replays/ipc";
import { ipc_openSettingsModalEvent } from "@settings/ipc";
import type CrossProcessExports from "electron";
import { app, BrowserWindow, shell } from "electron";
import log from "electron-log";
import get from "lodash/get";
import last from "lodash/last";
import path from "path";
import { fileExists } from "utils/file_exists";

import { getConfigFlags } from "./flags/flags";
import { installModules } from "./install_modules";
import { MenuBuilder } from "./menu";
import { clearTempFolder, resolveHtmlPath } from "./util";

// Neutral until the renderer applies the theme (see renderer/styles/theme.ts).
const BACKGROUND_COLOR = "#1c1c1c";

const isDevelopment = process.env.NODE_ENV === "development" || process.env.DEBUG_PROD === "true";

const isMac = process.platform === "darwin";

let menu: CrossProcessExports.Menu | null = null;
let mainWindow: BrowserWindow | null = null;
let didFinishLoad = false;

// In development Electron would use the generic "Electron" profile folder, shared by every dev app;
// keep ours separate (and apart from an installed copy, which uses the product name).
if (!app.isPackaged) {
  app.setName(`${PRODUCT_NAME}-dev`);
}

log.initialize();
log.errorHandler.startCatching();
log.transports.file.level = isDevelopment ? "info" : "warn";

// Only allow a single app instance in production
const lockObtained = !app.isPackaged || app.requestSingleInstanceLock();
if (!lockObtained) {
  app.quit();
}

registerGameAssetScheme();

const flags = getConfigFlags();
const { dolphinManager, appUpdater, browserWindowManager } = installModules(flags);

if (isDevelopment) {
  require("electron-debug")();

  // Disable IPC hooks in development to prevent duplicate console logs
  // In dev mode, both main and renderer import electron-log, which causes duplication
  log.transports.ipc.level = false;

  // Enable remote debugging for React DevTools
  app.commandLine.appendSwitch("remote-debugging-port", "9222");
}

const installExtensions = async () => {
  const installer = require("electron-devtools-installer");
  const forceDownload = Boolean(process.env.UPGRADE_EXTENSIONS);
  const extensions = ["REACT_DEVELOPER_TOOLS"];

  return installer
    .default(
      extensions.map((name) => installer[name]),
      forceDownload,
    )
    .catch(console.log);
};

const createWindow = async () => {
  if (isDevelopment) {
    await installExtensions();
  }

  // clear temp files safely before the app has fully started and replays are being loaded for playback
  try {
    await clearTempFolder();
  } catch (err) {
    // silently fail since this isn't a critical issue
    log.error(
      `Could not clear temp folder on startup due to:
      ${err instanceof Error ? err.message : JSON.stringify(err)}`,
    );
  }

  mainWindow = new BrowserWindow({
    show: false,
    width: 1100,
    height: 750,
    minHeight: isDevelopment ? undefined : 450,
    minWidth: isDevelopment ? undefined : 900,
    backgroundColor: BACKGROUND_COLOR,
    // No app icon: Slippi's icon is its branding and we have no art of our own (Electron default).

    // This setting only takes effect on macOS, and simply opts it into the modern
    // Big-Sur frame UI for the window style.
    titleBarStyle: "hiddenInset",
    autoHideMenuBar: true,

    webPreferences: {
      sandbox: false,
      preload: app.isPackaged ? path.join(__dirname, "preload.js") : path.join(__dirname, "../../.erb/dll/preload.js"),
    },
  });

  mainWindow.loadURL(resolveHtmlPath("index.html")).catch(log.error);

  mainWindow.on("ready-to-show", () => {
    Preconditions.checkExists(mainWindow, '"mainWindow" is not defined');

    didFinishLoad = true;

    if (process.env.START_MINIMIZED) {
      mainWindow.minimize();
    } else {
      mainWindow.show();
    }
  });

  mainWindow.on("closed", () => {
    mainWindow = null;
  });

  mainWindow.on("page-title-updated", (event) => {
    // Always keep the initial window title
    event.preventDefault();
  });

  const menuBuilder = new MenuBuilder({
    mainWindow,
    browserWindowManager,
    onOpenPreferences: () => {
      void openPreferences().catch(log.error);
    },
    onOpenReplayFile: playReplayAndShowStats,
    createWindow,
    onOpenAppSupportFolder: () => {
      const path = app.getPath("userData");
      void shell.openPath(path);
    },
    enableDevTools: isDevelopment,
  });
  menu = menuBuilder.buildMenu();

  // Open urls in the user's browser
  mainWindow.webContents.setWindowOpenHandler((edata) => {
    void shell.openExternal(edata.url);
    return { action: "deny" };
  });
};

/**
 * Add event listeners...
 */

app.on("window-all-closed", () => {
  // On macOS, the window closing shouldn't quit the actual process.
  // Instead, grab and activate a hidden menu item to enable the user to
  // recreate the window on-demand.
  if (isMac && menu) {
    const macMenuItem = menu.getMenuItemById("macos-window-toggle");
    if (macMenuItem) {
      macMenuItem.enabled = true;
      macMenuItem.visible = true;
    }
    return;
  }

  app.quit();
});

const waitForMainWindow = async () => {
  let retryIdx = 0;
  while (!didFinishLoad && retryIdx < 200) {
    // It's okay to await in loop, we want things to be slow in this case
    await delay(100); // eslint-disable-line
    retryIdx += 1;
  }

  if (retryIdx >= 100) {
    throw "Timed out waiting for mainWindow to exist."; // eslint-disable-line
  }

  log.info(`Found mainWindow after ${retryIdx} tries.`);
};

/**
 * Opens a replay file passed on the command line or through the OS file association.
 * Slippi also registers a `slippi://` URL scheme that downloads replays from its cloud
 * storage; we have no replay storage yet, so only local files are handled.
 */
const handleOpenFileAsync = async (aUrl: string) => {
  log.info("Handling file...");
  log.info(aUrl);

  if (!(await fileExists(aUrl))) {
    return;
  }

  // When handling a request, focus the window
  if (mainWindow) {
    if (mainWindow.isMinimized()) {
      mainWindow.restore();
    }
    mainWindow.focus();
  } else {
    await createWindow();
  }

  await playReplayAndShowStats(aUrl);
};

const handleOpenFile = (aUrl: string) => {
  // Filter out command line parameters and invalid urls
  if (aUrl.startsWith("-")) {
    return;
  }

  handleOpenFileAsync(aUrl).catch((err) => {
    log.error("Handling file encountered error");
    log.error(err);
  });
};

app.on("open-url", (_, aUrl) => {
  log.info(`Received open-url event: ${aUrl}`);
  handleOpenFile(aUrl);
});

app.on("open-file", (_, aUrl) => {
  log.info(`Received open-file event: ${aUrl}`);
  handleOpenFile(aUrl);
});

app.on("second-instance", (_, argv) => {
  log.info("Second instance detected...");
  log.info(argv);

  const lastItem = last(argv);
  if (argv.length === 1 || !lastItem) {
    return;
  }

  handleOpenFile(lastItem);
});

app.on("activate", () => {
  // On macOS it's common to re-create a window in the app when the
  // dock icon is clicked and there are no other windows open.
  if (mainWindow === null) {
    void createWindow();
  }
});

const playReplayAndShowStats = async (filePath: string, startFrame?: number) => {
  // Ensure playback dolphin is actually installed
  await dolphinManager.installDolphin(DolphinLaunchType.PLAYBACK);

  // Launch the replay
  await dolphinManager.launchPlaybackDolphin("playback", {
    mode: "normal",
    replay: filePath,
    startFrame,
  });

  // Show the stats page
  await waitForMainWindow();
  if (mainWindow) {
    await ipc_statsPageRequestedEvent.main!.trigger({ filePath });
  }
};

const main = async () => {
  await app.whenReady();
  if (!lockObtained) {
    return;
  }

  await appUpdater.verifyPendingUpdate();
  await createWindow();

  // Handle a replay file if provided
  const argURI = get(process.argv, 1);
  if (argURI) {
    handleOpenFile(argURI);
  }
};

const openPreferences = async () => {
  if (!mainWindow) {
    await createWindow();
  }
  await ipc_openSettingsModalEvent.main!.trigger({});
};

main().catch(log.error);
