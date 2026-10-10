import type { EmptyPayload, SuccessPayload } from "utils/ipc";
import { _, makeEndpoint } from "utils/ipc";

import type { GeckoCode } from "./config/gecko_code";
import type { RoomHandOffResult } from "./room_handoff";
import type { DolphinEvent, DolphinLaunchType, PlayKey, ReplayQueueItem } from "./types";

// Handlers

export const ipc_downloadDolphin = makeEndpoint.main(
  "downloadDolphin",
  <{ dolphinType: DolphinLaunchType }>_,
  <SuccessPayload>_,
);

export const ipc_configureDolphin = makeEndpoint.main(
  "configureDolphin",
  <{ dolphinType: DolphinLaunchType }>_,
  <SuccessPayload>_,
);

export const ipc_hardResetDolphin = makeEndpoint.main(
  "hardResetDolphin",
  <{ dolphinType: DolphinLaunchType }>_,
  <SuccessPayload>_,
);

export const ipc_softResetDolphin = makeEndpoint.main(
  "softResetDolphin",
  <{ dolphinType: DolphinLaunchType }>_,
  <SuccessPayload>_,
);

export const ipc_openDolphinSettingsFolder = makeEndpoint.main(
  "openDolphinSettingsFolder",
  <{ dolphinType: DolphinLaunchType }>_,
  <SuccessPayload>_,
);

export const ipc_getDolphinPaths = makeEndpoint.main(
  "getDolphinPaths",
  <{ dolphinType: DolphinLaunchType }>_,
  <{ executable: string; userFolder: string; playKeyFile: string }>_,
);

export const ipc_storePlayKeyFile = makeEndpoint.main("storePlayKeyFile", <{ key: PlayKey }>_, <SuccessPayload>_);

export const ipc_checkPlayKeyExists = makeEndpoint.main(
  "checkPlayKeyExists",
  <{ key: PlayKey }>_,
  <{ exists: boolean }>_,
);

export const ipc_removePlayKeyFile = makeEndpoint.main("removePlayKeyFile", <EmptyPayload>_, <SuccessPayload>_);

export const ipc_viewSlpReplay = makeEndpoint.main("viewSlpReplay", <{ files: ReplayQueueItem[] }>_, <SuccessPayload>_);

export const ipc_launchNetplayDolphin = makeEndpoint.main("launchNetplayDolphin", <EmptyPayload>_, <SuccessPayload>_);

/** A room in the list was clicked while the game may be running (docs/rooms-protocol.md). */
export const ipc_joinRoomInGame = makeEndpoint.main("joinRoomInGame", <{ code: string }>_, <RoomHandOffResult>_);

/** Writes the room request a game that is about to start reads once it reaches the menus. */
export const ipc_prepareRoomLaunch = makeEndpoint.main("prepareRoomLaunch", <{ code: string }>_, <{ id: string }>_);

/** Takes a prepared room request back (Play did not start the game). */
export const ipc_cancelRoomLaunch = makeEndpoint.main("cancelRoomLaunch", <{ id: string }>_, <SuccessPayload>_);

export const ipc_fetchGeckoCodes = makeEndpoint.main(
  "fetchGeckoCodes",
  <{ dolphinType: DolphinLaunchType }>_,
  <{ codes: GeckoCode[] }>_,
);

export const ipc_saveGeckoCodes = makeEndpoint.main(
  "saveGeckoCodes",
  <{ dolphinType: DolphinLaunchType; geckoCodes: GeckoCode[] }>_,
  <SuccessPayload>_,
);

export const ipc_installRosetta = makeEndpoint.main("installRosetta", <EmptyPayload>_, <{ exitCode: number }>_);

// Events

export const ipc_dolphinEvent = makeEndpoint.renderer("dolphin_dolphinEvent", <DolphinEvent>_);
