import type { EmptyPayload, SuccessPayload } from "utils/ipc";
import { _, makeEndpoint } from "utils/ipc";

import type { EmissaryState } from "./types";

export const ipc_emissaryGetState = makeEndpoint.main("emissary_getState", <{ refresh: boolean }>_, <EmissaryState>_);

export const ipc_emissaryInstall = makeEndpoint.main(
  "emissary_install",
  <EmptyPayload>_,
  <{ ok: true; state: EmissaryState } | { ok: false; cancelled: boolean; message: string }>_,
);

export const ipc_emissaryCancel = makeEndpoint.main("emissary_cancel", <EmptyPayload>_, <SuccessPayload>_);

export const ipc_emissaryRemove = makeEndpoint.main("emissary_remove", <EmptyPayload>_, <SuccessPayload>_);

// Events

export const ipc_emissaryState = makeEndpoint.renderer("emissary_state", <EmissaryState>_);
