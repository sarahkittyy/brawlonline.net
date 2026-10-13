import type { SettingsManager } from "@settings/settings_manager";
import electronLog from "electron-log";

import { dolphinToolNextTo } from "../dolphin_tool";
import type { DolphinManager } from "../manager";
import { DolphinLaunchType } from "../types";
import {
  ipc_modsAddBuild,
  ipc_modsEvent,
  ipc_modsGetState,
  ipc_modsHashBuild,
  ipc_modsHost,
  ipc_modsInspectFolder,
  ipc_modsJoin,
  ipc_modsRemoveBuild,
  ipc_modsRequest,
  ipc_modsUpdateBuild,
} from "./ipc";
import { ModsManager } from "./mods_manager";

const log = electronLog.scope("dolphin/mods");

export default function setupModsIpc({
  settingsManager,
  dolphinManager,
}: {
  settingsManager: SettingsManager;
  dolphinManager: DolphinManager;
}) {
  const mods = new ModsManager(settingsManager, dolphinManager, async () => {
    // DolphinTool runs from the bundled Dolphin's install folder (as for the game assets).
    await dolphinManager.bundledDolphinReady();
    return dolphinToolNextTo(dolphinManager.getDolphinExecutablePath(DolphinLaunchType.NETPLAY));
  });
  mods.onEvent((event) => {
    void ipc_modsEvent.main!.trigger(event).catch(log.error);
  });

  ipc_modsGetState.main!.handle(async () => mods.getState());
  ipc_modsAddBuild.main!.handle(async ({ input }) => ({ build: await mods.addBuild(input) }));
  ipc_modsUpdateBuild.main!.handle(async ({ id, input }) => ({ build: await mods.updateBuild(id, input) }));
  ipc_modsRemoveBuild.main!.handle(async ({ id }) => {
    await mods.removeBuild(id);
    return { success: true };
  });
  ipc_modsHashBuild.main!.handle(async ({ id }) => ({ build: await mods.hashBuild(id) }));
  ipc_modsInspectFolder.main!.handle(async ({ folder }) => ({ parts: await mods.inspectFolder(folder) }));
  ipc_modsHost.main!.handle(async (options) => {
    await mods.host(options);
    return { success: true };
  });
  ipc_modsJoin.main!.handle(async (options) => {
    await mods.join(options);
    return { success: true };
  });
  ipc_modsRequest.main!.handle(async ({ request }) => {
    await mods.request(request);
    return { success: true };
  });
  return mods;
}
