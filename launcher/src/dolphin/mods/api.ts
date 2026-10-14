/* eslint-disable import/no-default-export */
import {
  ipc_modsAddBuild,
  ipc_modsConfigureControllers,
  ipc_modsEvent,
  ipc_modsGetState,
  ipc_modsHashBuild,
  ipc_modsHost,
  ipc_modsInspectFolder,
  ipc_modsJoin,
  ipc_modsJoinByCode,
  ipc_modsRefreshControllers,
  ipc_modsRemoveBuild,
  ipc_modsRequest,
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
    await ipc_modsJoin.renderer!.trigger(options);
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
  onEvent(handle) {
    const { destroy } = ipc_modsEvent.renderer!.handle(async (event) => handle(event));
    return destroy;
  },
};

export default modsApi;
