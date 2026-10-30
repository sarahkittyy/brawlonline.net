import type { EmptyPayload, SuccessPayload } from "utils/ipc";
import { _, makeEndpoint } from "utils/ipc";

import type {
  ModBuild,
  ModBuildCodes,
  ModBuildInput,
  ModCode,
  ModCodeLists,
  ModFolderParts,
  ModRoomRequest,
  ModSaveImportResult,
  ModSaveInfo,
  ModsController,
  ModsEvent,
  ModsHostOptions,
  ModsJoinByCodeOptions,
  ModsJoinByCodeResult,
  ModsJoinOptions,
  ModsJoinResult,
  ModsState,
} from "./types";

export const ipc_modsGetState = makeEndpoint.main("mods_getState", <EmptyPayload>_, <ModsState>_);

export const ipc_modsAddBuild = makeEndpoint.main("mods_addBuild", <{ input: ModBuildInput }>_, <{ build: ModBuild }>_);

export const ipc_modsUpdateBuild = makeEndpoint.main(
  "mods_updateBuild",
  <{ id: string; input: Partial<ModBuildInput> }>_,
  <{ build: ModBuild }>_,
);

export const ipc_modsRemoveBuild = makeEndpoint.main("mods_removeBuild", <{ id: string }>_, <SuccessPayload>_);

export const ipc_modsHashBuild = makeEndpoint.main("mods_hashBuild", <{ id: string }>_, <{ build: ModBuild }>_);

export const ipc_modsInspectFolder = makeEndpoint.main(
  "mods_inspectFolder",
  <{ folder: string }>_,
  <{ parts: ModFolderParts }>_,
);

export const ipc_modsHost = makeEndpoint.main("mods_host", <ModsHostOptions>_, <SuccessPayload>_);

export const ipc_modsJoin = makeEndpoint.main("mods_join", <ModsJoinOptions>_, <ModsJoinResult>_);

export const ipc_modsJoinByCode = makeEndpoint.main(
  "mods_joinByCode",
  <ModsJoinByCodeOptions>_,
  <ModsJoinByCodeResult>_,
);

export const ipc_modsRequest = makeEndpoint.main("mods_request", <{ request: ModRoomRequest }>_, <SuccessPayload>_);

export const ipc_modsSetController = makeEndpoint.main(
  "mods_setController",
  <{ controller: ModsController }>_,
  <SuccessPayload>_,
);

export const ipc_modsRefreshControllers = makeEndpoint.main(
  "mods_refreshControllers",
  <EmptyPayload>_,
  <SuccessPayload>_,
);

export const ipc_modsConfigureControllers = makeEndpoint.main(
  "mods_configureControllers",
  <EmptyPayload>_,
  <SuccessPayload>_,
);

export const ipc_modsSaveInfo = makeEndpoint.main("mods_saveInfo", <{ id: string }>_, <{ save: ModSaveInfo | null }>_);

export const ipc_modsImportSave = makeEndpoint.main(
  "mods_importSave",
  <{ id: string; source: string }>_,
  <ModSaveImportResult>_,
);

export const ipc_modsOpenSaveFolder = makeEndpoint.main("mods_openSaveFolder", <{ id: string }>_, <SuccessPayload>_);

export const ipc_modsCodeLists = makeEndpoint.main(
  "mods_codeLists",
  <{ id: string }>_,
  <{ codes: ModBuildCodes; lists: ModCodeLists }>_,
);

export const ipc_modsSetCodes = makeEndpoint.main(
  "mods_setCodes",
  <{ id: string; codes: ModBuildCodes }>_,
  <{ codes: ModBuildCodes }>_,
);

export const ipc_modsDatabaseCodes = makeEndpoint.main(
  "mods_databaseCodes",
  <{ id: string }>_,
  <{ codes: ModCode[] }>_,
);

// Events

export const ipc_modsEvent = makeEndpoint.renderer("mods_event", <ModsEvent>_);
