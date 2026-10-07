import type { DolphinManager } from "@dolphin/manager";
import { DolphinLaunchType } from "@dolphin/types";
import type { SettingsManager } from "@settings/settings_manager";
import { app } from "electron";
import electronLog from "electron-log";
import { existsSync } from "fs";
import path from "path";

import { extractGameAssets } from "./extractor";
import type { DiscSource } from "./game_assets_manager";
import { GameAssetsManager } from "./game_assets_manager";
import { ipc_extractGameAssets, ipc_gameAssetsStateChangedEvent, ipc_getGameAssetsState } from "./ipc";
import { handleGameAssetProtocol } from "./protocol";

const log = electronLog.scope("game_assets");

/** Runtime lookup (not `process.env.X`) so webpack does not inline the value at build time. */
const readEnv = (name: string): string | undefined => {
  const value = (process.env as Record<string, string | undefined>)[name];
  return value && value.trim() !== "" ? value.trim() : undefined;
};

/**
 * Cache location: `PPO_ASSET_CACHE`, else `<launcher repo>/.asset-cache` in development
 * (gitignored), else `<userData>/game-assets`. The extracted files live in a `theme`
 * subfolder that is replaced on every extraction.
 */
function cacheRoot(): string {
  const fromEnv = readEnv("PPO_ASSET_CACHE");
  if (fromEnv) {
    return fromEnv;
  }
  if (!app.isPackaged) {
    return path.join(process.cwd(), ".asset-cache");
  }
  return path.join(app.getPath("userData"), "game-assets");
}

/**
 * Development shortcut: read the disc files from an already extracted copy instead
 * of extracting them from the ISO with DolphinTool. `PPO_DISC_FOLDER`, or in
 * development `<repo>/../game/rev1-extract/DATA/files` if it exists. The ISO must
 * still be chosen first: until then the launcher shows the plain fallback look.
 */
function devDiscFolder(): string | null {
  const fromEnv = readEnv("PPO_DISC_FOLDER");
  if (fromEnv) {
    return fromEnv;
  }
  if (!app.isPackaged) {
    const folder = path.resolve(process.cwd(), "..", "game", "rev1-extract", "DATA", "files");
    return existsSync(folder) ? folder : null;
  }
  return null;
}

function dolphinToolNextTo(dolphinExecutable: string): string {
  const dir = path.dirname(dolphinExecutable);
  switch (process.platform) {
    case "win32":
      return path.join(dir, "DolphinTool.exe");
    case "darwin":
      // <install>/Dolphin.app/Contents/MacOS/Dolphin -> dolphin-tool in the same folder
      return path.join(dir, "dolphin-tool");
    default:
      return path.join(dir, "dolphin-tool");
  }
}

export default function setupGameAssetsIpc({
  settingsManager,
  dolphinManager,
}: {
  settingsManager: SettingsManager;
  dolphinManager: DolphinManager;
}) {
  const cacheDir = path.join(cacheRoot(), "theme");

  const manager = new GameAssetsManager(
    cacheDir,
    async () => {
      const isoPath = settingsManager.get().settings.isoPath;
      if (!isoPath || !existsSync(isoPath)) {
        return null;
      }
      const installation = dolphinManager.getInstallation(DolphinLaunchType.NETPLAY);
      const sdRawPath = path.join(installation.userFolder, "Wii", "sd.raw");
      const folder = devDiscFolder();
      const disc: DiscSource = folder
        ? { kind: "folder", path: folder }
        : {
            kind: "iso",
            path: isoPath,
            dolphinToolPath: dolphinToolNextTo(dolphinManager.getDolphinExecutablePath(DolphinLaunchType.NETPLAY)),
          };
      return { disc, sdRawPath: existsSync(sdRawPath) ? sdRawPath : undefined };
    },
    extractGameAssets,
  );

  manager.onStateChange((state) => {
    void ipc_gameAssetsStateChangedEvent.main!.trigger({ state }).catch(() => undefined);
  });

  ipc_getGameAssetsState.main!.handle(async () => ({ state: manager.getState() }));
  ipc_extractGameAssets.main!.handle(async () => ({ state: await manager.refresh({ force: true }) }));

  // A new disc (or none) changes the look; so does the first run with a disc.
  settingsManager.onSettingChange("isoPath", async () => {
    // The User folder may need seeding first so the SD card is there.
    await dolphinManager
      .getInstallation(DolphinLaunchType.NETPLAY)
      .ensureUserFolder()
      .catch(() => undefined);
    await manager.refresh();
  });

  const onReady = async () => {
    await app.whenReady();
    handleGameAssetProtocol(() => cacheDir);
    log.info(`Game asset cache: ${cacheDir}`);
    try {
      await dolphinManager.getInstallation(DolphinLaunchType.NETPLAY).ensureUserFolder();
    } catch (err) {
      log.warn(`Could not prepare the Dolphin User folder: ${err}`);
    }
    await manager.refresh();
  };
  void onReady().catch(log.error);

  return { gameAssetsManager: manager };
}
