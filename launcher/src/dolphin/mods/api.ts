/* eslint-disable import/no-default-export */
import {
  ipc_modsAddBuild,
  ipc_modsCodeLists,
  ipc_modsConfigureControllers,
  ipc_modsDatabaseCodes,
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
  ipc_modsSetCodes,
  ipc_modsSetController,
  ipc_modsUpdateBuild,
} from "./ipc";
import type { ModsService } from "./types";

const modsApi: ModsService = {
  async getState() {
    return ipc_modsGetState.renderer!.trigger({}).then(({ result }) => result);
  },
  async addBuild(input) {
    const { result } = await ipc_modsAddBuild.renderer!.trigger({ input });
    return result.build;
  },
  async updateBuild(id, input) {
    const { result } = await ipc_modsUpdateBuild.renderer!.trigger({ id, input });
    return result.build;
  },
  async removeBuild(id) {
    await ipc_modsRemoveBuild.renderer!.trigger({ id });
  },
  async inspectFolder(folder) {
    const { result } = await ipc_modsInspectFolder.renderer!.trigger({ folder });
    return result.parts;
  },
  async hashBuild(id) {
    const { result } = await ipc_modsHashBuild.renderer!.trigger({ id });
    return result.build;
  },
  async host(options) {
    await ipc_modsHost.renderer!.trigger(options);
  },
  async join(options) {
    const { result } = await ipc_modsJoin.renderer!.trigger(options);
    return result;
  },
  async joinByCode(options) {
    const { result } = await ipc_modsJoinByCode.renderer!.trigger(options);
    return result;
  },
  async request(request) {
    await ipc_modsRequest.renderer!.trigger({ request });
  },
  async setController(controller) {
    await ipc_modsSetController.renderer!.trigger({ controller });
  },
  async refreshControllers() {
    await ipc_modsRefreshControllers.renderer!.trigger({});
  },
  async configureControllers() {
    await ipc_modsConfigureControllers.renderer!.trigger({});
  },
  async saveInfo(id) {
    const { result } = await ipc_modsSaveInfo.renderer!.trigger({ id });
    return result.save;
  },
  async importSave(id, source) {
    const { result } = await ipc_modsImportSave.renderer!.trigger({ id, source });
    return result;
  },
  async openSaveFolder(id) {
    await ipc_modsOpenSaveFolder.renderer!.trigger({ id });
  },
  async codeLists(id) {
    const { result } = await ipc_modsCodeLists.renderer!.trigger({ id });
    return result;
  },
  async setCodes(id, codes) {
    const { result } = await ipc_modsSetCodes.renderer!.trigger({ id, codes });
    return result.codes;
  },
  async databaseCodes(id) {
    const { result } = await ipc_modsDatabaseCodes.renderer!.trigger({ id });
    return result.codes;
  },
  onEvent(handle) {
    const { destroy } = ipc_modsEvent.renderer!.handle(async (event) => handle(event));
    return destroy;
  },
};

export default modsApi;
