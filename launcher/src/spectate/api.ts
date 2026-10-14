/* eslint-disable import/no-default-export */
import {
  ipc_spectateGetState,
  ipc_spectateList,
  ipc_spectateSetAutoPlay,
  ipc_spectateStateChangedEvent,
  ipc_spectateStop,
  ipc_spectateWatch,
} from "./ipc";
import type { SpectateApi } from "./types";

const spectateApi: SpectateApi = {
  async list(uid) {
    const { result } = await ipc_spectateList.renderer!.trigger({ uid });
    return result.list;
  },
  async watch(uid, stream) {
    const { result } = await ipc_spectateWatch.renderer!.trigger({ uid, stream });
    return result.result;
  },
  async stop() {
    await ipc_spectateStop.renderer!.trigger({});
  },
  async getState() {
    const { result } = await ipc_spectateGetState.renderer!.trigger({});
    return result.state;
  },
  async setAutoPlay(on) {
    await ipc_spectateSetAutoPlay.renderer!.trigger({ on });
  },
  onStateChange(handle) {
    const { destroy } = ipc_spectateStateChangedEvent.renderer!.handle(async ({ state }) => {
      handle(state);
    });
    return destroy;
  },
};

export default spectateApi;
