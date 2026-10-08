import type { EmptyPayload } from "utils/ipc";
import { _, makeEndpoint } from "utils/ipc";

import type { GameAssetsState } from "./types";

export const ipc_getGameAssetsState = makeEndpoint.main(
  "gameAssets_getState",
  <EmptyPayload>_,
  <{ state: GameAssetsState }>_,
);

export const ipc_extractGameAssets = makeEndpoint.main(
  "gameAssets_extract",
  <EmptyPayload>_,
  <{ state: GameAssetsState }>_,
);

export const ipc_gameAssetsStateChangedEvent = makeEndpoint.renderer(
  "gameAssets_stateChanged",
  <{ state: GameAssetsState }>_,
);
