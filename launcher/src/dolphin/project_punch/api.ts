/* eslint-disable import/no-default-export */
import {
  ipc_projectPunchCancel,
  ipc_projectPunchGetState,
  ipc_projectPunchInstall,
  ipc_projectPunchPlay,
  ipc_projectPunchRemove,
  ipc_projectPunchState,
} from "./ipc";
import type { ProjectPunchService } from "./types";

const projectPunchApi: ProjectPunchService = {
  async getState(refresh = false) {
    const { result } = await ipc_projectPunchGetState.renderer!.trigger({ refresh });
    return result;
  },
  async install() {
    const { result } = await ipc_projectPunchInstall.renderer!.trigger({});
    if (!result.ok) {
      const err = new Error(result.message);
      (err as Error & { cancelled?: boolean }).cancelled = result.cancelled;
      throw err;
    }
    return result.state;
  },
  async cancel() {
    await ipc_projectPunchCancel.renderer!.trigger({});
  },
  async remove() {
    await ipc_projectPunchRemove.renderer!.trigger({});
  },
  async play() {
    await ipc_projectPunchPlay.renderer!.trigger({});
  },
  onState(handle) {
    const { destroy } = ipc_projectPunchState.renderer!.handle(async (state) => handle(state));
    return destroy;
  },
};

export default projectPunchApi;
