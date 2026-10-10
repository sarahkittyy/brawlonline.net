import { DOLPHIN_ONLINE_DIR } from "@common/product";
import { shell } from "electron";
import log from "electron-log";
import isEqual from "lodash/isEqual";
import { mkdir, readFile } from "node:fs/promises";
import path from "path";
import { fileExists } from "utils/file_exists";

import {
  ipc_cancelRoomLaunch,
  ipc_checkPlayKeyExists,
  ipc_configureDolphin,
  ipc_dolphinEvent,
  ipc_downloadDolphin,
  ipc_fetchGeckoCodes,
  ipc_getDolphinPaths,
  ipc_hardResetDolphin,
  ipc_installRosetta,
  ipc_joinRoomInGame,
  ipc_launchNetplayDolphin,
  ipc_openDolphinSettingsFolder,
  ipc_prepareRoomLaunch,
  ipc_removePlayKeyFile,
  ipc_saveGeckoCodes,
  ipc_softResetDolphin,
  ipc_storePlayKeyFile,
  ipc_viewSlpReplay,
} from "./ipc";
import type { DolphinManager } from "./manager";
import { deletePlayKeyFile, writePlayKeyFile } from "./playkey";
import { clearRoomRequest, handOffRoom, isRoomCode, writeRoomRequest } from "./room_handoff";
import { installRosettaElevated } from "./rosetta/install_rosetta";
import { DolphinLaunchType } from "./types";
import { fetchGeckoCodes, saveGeckoCodes } from "./util";

export default function setupDolphinIpc({ dolphinManager }: { dolphinManager: DolphinManager }) {
  dolphinManager.events.subscribe((event) => {
    void ipc_dolphinEvent.main!.trigger(event).catch(log.error);
  });

  ipc_downloadDolphin.main!.handle(async ({ dolphinType }) => {
    await dolphinManager.installDolphin(dolphinType);
    return { success: true };
  });

  ipc_configureDolphin.main!.handle(async ({ dolphinType }) => {
    console.log("configuring dolphin...");
    await dolphinManager.configureDolphin(dolphinType);
    return { success: true };
  });

  ipc_softResetDolphin.main!.handle(async ({ dolphinType }) => {
    console.log("soft resetting dolphin...");
    await dolphinManager.reinstallDolphin(dolphinType, false);
    return { success: true };
  });

  ipc_openDolphinSettingsFolder.main!.handle(async ({ dolphinType }) => {
    // Our Dolphin always runs with `-u <User folder>`, so that is where its settings are.
    const dolphinInstall = dolphinManager.getInstallation(dolphinType);
    await dolphinInstall.ensureUserFolder();
    await shell.openPath(dolphinInstall.userFolder);
    return { success: true };
  });

  ipc_hardResetDolphin.main!.handle(async ({ dolphinType }) => {
    console.log("hard resetting dolphin...");
    await dolphinManager.reinstallDolphin(dolphinType, true);
    return { success: true };
  });

  ipc_getDolphinPaths.main!.handle(async ({ dolphinType }) => {
    const installation = dolphinManager.getInstallation(dolphinType);
    const userFolder = installation.userFolder;
    const playKeyFile = path.join(userFolder, DOLPHIN_ONLINE_DIR, "user.json");
    return { executable: dolphinManager.getDolphinExecutablePath(dolphinType), userFolder, playKeyFile };
  });

  ipc_storePlayKeyFile.main!.handle(async ({ key }) => {
    const installation = dolphinManager.getInstallation(DolphinLaunchType.NETPLAY);
    await writePlayKeyFile(installation, key);
    return { success: true };
  });

  ipc_checkPlayKeyExists.main!.handle(async ({ key }) => {
    const installation = dolphinManager.getInstallation(DolphinLaunchType.NETPLAY);
    const keyPath = await installation.findPlayKey();
    const exists = await fileExists(keyPath);
    if (!exists) {
      return { exists: false };
    }

    try {
      const jsonKey = await readFile(keyPath);
      const fileContents = jsonKey.toString();
      const storedKey = JSON.parse(fileContents);
      return { exists: isEqual(storedKey, key) };
    } catch (err) {
      log.warn(err);
      return { exists: false };
    }
  });

  ipc_removePlayKeyFile.main!.handle(async () => {
    const installation = dolphinManager.getInstallation(DolphinLaunchType.NETPLAY);
    await deletePlayKeyFile(installation);
    return { success: true };
  });

  ipc_viewSlpReplay.main!.handle(async ({ files }) => {
    await dolphinManager.launchPlaybackDolphin("playback", {
      mode: "queue",
      queue: files,
    });
    return { success: true };
  });

  ipc_launchNetplayDolphin.main!.handle(async () => {
    await dolphinManager.launchNetplayDolphin();
    return { success: true };
  });

  ipc_joinRoomInGame.main!.handle(async ({ code }) => {
    if (!isRoomCode(code)) {
      throw new Error(`Not a room code: ${code}`);
    }
    if (!dolphinManager.isNetplayRunning()) {
      return { outcome: "not-running" };
    }
    const onlineDir = dolphinManager.netplayOnlineDir();
    await mkdir(onlineDir, { recursive: true });
    const result = await handOffRoom(onlineDir, code);
    log.info(`Room ${code} handed to the running game: ${result.outcome}`);
    return result;
  });

  ipc_prepareRoomLaunch.main!.handle(async ({ code }) => {
    const onlineDir = dolphinManager.netplayOnlineDir();
    await mkdir(onlineDir, { recursive: true });
    const req = await writeRoomRequest(onlineDir, code);
    log.info(`Room ${code} requested for the game about to start (${req.id})`);
    return { id: req.id };
  });

  ipc_cancelRoomLaunch.main!.handle(async ({ id }) => {
    await clearRoomRequest(dolphinManager.netplayOnlineDir(), id);
    return { success: true };
  });

  ipc_fetchGeckoCodes.main!.handle(async ({ dolphinType }) => {
    const installation = dolphinManager.getInstallation(dolphinType);
    const codes = await fetchGeckoCodes(installation);
    return { codes };
  });

  ipc_saveGeckoCodes.main!.handle(async ({ dolphinType, geckoCodes }) => {
    const installation = dolphinManager.getInstallation(dolphinType);
    await saveGeckoCodes(installation, geckoCodes);
    return { success: true };
  });

  ipc_installRosetta.main!.handle(async () => {
    const exitCode = await installRosettaElevated();
    return { exitCode };
  });

  return { dolphinManager };
}
