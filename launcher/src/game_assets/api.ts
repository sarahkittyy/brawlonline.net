/* eslint-disable import/no-default-export */
import { ipc_extractGameAssets, ipc_gameAssetsStateChangedEvent, ipc_getGameAssetsState } from "./ipc";
import type { GameAssetsApi } from "./types";

const gameAssetsApi: GameAssetsApi = {
  async getState() {
    const { result } = await ipc_getGameAssetsState.renderer!.trigger({});
    return result.state;
  },
  async extract() {
    const { result } = await ipc_extractGameAssets.renderer!.trigger({});
    return result.state;
  },
  onStateChange(handle) {
    const { destroy } = ipc_gameAssetsStateChangedEvent.renderer!.handle(async ({ state }) => {
      handle(state);
    });
    return destroy;
  },
};

export default gameAssetsApi;
