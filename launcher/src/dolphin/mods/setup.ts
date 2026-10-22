import type { AccountsManager } from "@accounts/accounts_manager";
import type { SettingsManager } from "@settings/settings_manager";
import { shell } from "electron";
import electronLog from "electron-log";

import { dolphinToolNextTo } from "../dolphin_tool";
import type { DolphinManager } from "../manager";
import { DolphinLaunchType } from "../types";
import {
  ipc_modsAddBuild,
  ipc_modsConfigureControllers,
  ipc_modsEvent,
  ipc_modsGetState,
  ipc_modsHashBuild,
  ipc_modsHost,
  ipc_modsImportSave,
  ipc_modsInspectFolder,
  ipc_modsJoin,
  ipc_modsJoinByCode,
  ipc_modsOpenSaveFolder,
  ipc_modsRefreshControllers,
  ipc_modsRemoveBuild,
  ipc_modsRequest,
  ipc_modsSaveInfo,
  ipc_modsSetController,
  ipc_modsUpdateBuild,
} from "./ipc";
import { ModsManager } from "./mods_manager";

const log = electronLog.scope("dolphin/mods");

export default function setupModsIpc({
  settingsManager,
  dolphinManager,
  accountsManager,
}: {
  settingsManager: SettingsManager;
  dolphinManager: DolphinManager;
  accountsManager: AccountsManager;
}) {
  const mods = new ModsManager(
    settingsManager,
    dolphinManager,
    async () => {
      // DolphinTool runs from the bundled Dolphin's install folder (as for the game assets).
      await dolphinManager.bundledDolphinReady();
      return dolphinToolNextTo(dolphinManager.getDolphinExecutablePath(DolphinLaunchType.NETPLAY));
    },
    async (uid, code) => {
      const result = await accountsManager.modRoom(uid, code);
      if (result.ok) {
        return result.value;
      }
      if (result.error.code === "unknown_room") {
        return null;
      }
      throw new Error(result.error.message);
    },
    undefined,
    {
      upload: async (uid, root, listing) => {
        const result = await accountsManager.uploadSdListing(uid, root, listing);
        if (!result.ok) {
          throw new Error(result.error.message);
        }
      },
      fetch: async (uid, root) => {
        const result = await accountsManager.sdListing(uid, root);
        if (result.ok) {
          return result.value;
        }
        if (result.error.status === 404) {
          return null;
        }
        throw new Error(result.error.message);
      },
    },
  );
  mods.onEvent((event) => {
    void ipc_modsEvent.main!.trigger(event).catch(log.error);
  });
  // Games hashed by an older launcher, or whose files changed: hashed again for the room list.
  void mods.rehashChanged();

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
  ipc_modsJoin.main!.handle(async (options) => await mods.join(options));
  ipc_modsJoinByCode.main!.handle(async (options) => await mods.joinByCode(options));
  ipc_modsRequest.main!.handle(async ({ request }) => {
    await mods.request(request);
    return { success: true };
  });
  ipc_modsSetController.main!.handle(async ({ controller }) => {
    await mods.setController(controller);
    return { success: true };
  });
  ipc_modsRefreshControllers.main!.handle(async () => {
    await mods.refreshControllers();
    return { success: true };
  });
  ipc_modsConfigureControllers.main!.handle(async () => {
    await mods.configureControllers();
    return { success: true };
  });
  ipc_modsSaveInfo.main!.handle(async ({ id }) => ({ save: await mods.saveInfo(id) }));
  ipc_modsImportSave.main!.handle(async ({ id, source }) => await mods.importSave(id, source));
  ipc_modsOpenSaveFolder.main!.handle(async ({ id }) => {
    const error = await shell.openPath(await mods.saveFolder(id));
    if (error) {
      throw new Error(error);
    }
    return { success: true };
  });
  return mods;
}
