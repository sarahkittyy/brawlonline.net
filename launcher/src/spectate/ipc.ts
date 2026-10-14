import type { EmptyPayload, SuccessPayload } from "utils/ipc";
import { _, makeEndpoint } from "utils/ipc";

import type { LiveList, SpectateState, WatchResult } from "./types";

export const ipc_spectateList = makeEndpoint.main("spectate_list", <{ uid: string }>_, <{ list: LiveList }>_);

export const ipc_spectateWatch = makeEndpoint.main(
  "spectate_watch",
  <{ uid: string; stream: string }>_,
  <{ result: WatchResult }>_,
);

export const ipc_spectateStop = makeEndpoint.main("spectate_stop", <EmptyPayload>_, <SuccessPayload>_);

export const ipc_spectateGetState = makeEndpoint.main(
  "spectate_getState",
  <EmptyPayload>_,
  <{ state: SpectateState }>_,
);

export const ipc_spectateSetAutoPlay = makeEndpoint.main("spectate_setAutoPlay", <{ on: boolean }>_, <SuccessPayload>_);

// Events

export const ipc_spectateStateChangedEvent = makeEndpoint.renderer(
  "spectate_stateChanged",
  <{ state: SpectateState }>_,
);
