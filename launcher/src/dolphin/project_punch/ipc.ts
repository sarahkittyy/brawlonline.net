import type { EmptyPayload, SuccessPayload } from "utils/ipc";
import { _, makeEndpoint } from "utils/ipc";

import type { ProjectPunchState } from "./types";

export const ipc_projectPunchGetState = makeEndpoint.main(
  "projectPunch_getState",
  <{ refresh: boolean }>_,
  <ProjectPunchState>_,
);

export const ipc_projectPunchInstall = makeEndpoint.main(
  "projectPunch_install",
  <EmptyPayload>_,
  <{ ok: true; state: ProjectPunchState } | { ok: false; cancelled: boolean; message: string }>_,
);

export const ipc_projectPunchCancel = makeEndpoint.main("projectPunch_cancel", <EmptyPayload>_, <SuccessPayload>_);

export const ipc_projectPunchRemove = makeEndpoint.main("projectPunch_remove", <EmptyPayload>_, <SuccessPayload>_);

export const ipc_projectPunchPlay = makeEndpoint.main("projectPunch_play", <EmptyPayload>_, <SuccessPayload>_);

// Events

export const ipc_projectPunchState = makeEndpoint.renderer("projectPunch_state", <ProjectPunchState>_);
