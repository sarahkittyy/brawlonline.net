/* eslint-disable import/no-default-export */
import {
  ipc_emissaryCancel,
  ipc_emissaryGetState,
  ipc_emissaryInstall,
  ipc_emissaryRemove,
  ipc_emissaryState,
} from "./ipc";
import type { EmissaryService } from "./types";

const emissaryApi: EmissaryService = {
  async getState(refresh = false) {
    const { result } = await ipc_emissaryGetState.renderer!.trigger({ refresh });
    return result;
  },
  async install() {
    const { result } = await ipc_emissaryInstall.renderer!.trigger({});
    if (!result.ok) {
      const err = new Error(result.message);
      (err as Error & { cancelled?: boolean }).cancelled = result.cancelled;
      throw err;
    }
    return result.state;
  },
  async cancel() {
    await ipc_emissaryCancel.renderer!.trigger({});
  },
  async remove() {
    await ipc_emissaryRemove.renderer!.trigger({});
  },
  onState(handle) {
    const { destroy } = ipc_emissaryState.renderer!.handle(async (state) => handle(state));
    return destroy;
  },
};

export default emissaryApi;
