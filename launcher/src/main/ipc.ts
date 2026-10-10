import type { IsoValidity, NatType, PortMapping, Presence } from "@common/types";
import type { EmptyPayload, SuccessPayload } from "utils/ipc";
import { _, makeEndpoint } from "utils/ipc";

export const ipc_checkValidIso = makeEndpoint.main(
  "checkValidIso",
  <{ path: string }>_,
  <{ path: string; valid: IsoValidity }>_,
);

/** Asks where to save, then zips the launcher's and Dolphin's logs there (null if the player cancelled). */
export const ipc_downloadLogs = makeEndpoint.main("downloadLogs", <EmptyPayload>_, <{ path: string | null }>_);

export const ipc_checkForUpdate = makeEndpoint.main("checkForUpdate", <EmptyPayload>_, <{ updateAvailable: boolean }>_);

export const ipc_installUpdate = makeEndpoint.main(
  "installUpdate",
  <EmptyPayload>_,
  <{ success: boolean; error?: string }>_,
);

export const ipc_showOpenDialog = makeEndpoint.main(
  "showOpenDialog",
  <Electron.OpenDialogOptions>_,
  <{ filePaths: string[]; canceled: boolean }>_,
);

export const ipc_clearTempFolder = makeEndpoint.main("clearTempFolder", <EmptyPayload>_, <SuccessPayload>_);

export const ipc_runNetworkDiagnostics = makeEndpoint.main(
  "runNetworkDiagnostics",
  <EmptyPayload>_,
  <{ address: string; cgnat: Presence; natType: NatType; portMapping: PortMapping }>_,
);

// Events

export const ipc_openInNewBrowserWindow = makeEndpoint.main(
  "openInNewBrowserWindow",
  <{ url: string }>_,
  <SuccessPayload>_,
);

export const ipc_launcherUpdateFoundEvent = makeEndpoint.renderer("launcherupdate_found", <{ version: string }>_);

export const ipc_launcherUpdateDownloadingEvent = makeEndpoint.renderer(
  "launcherupdate_download",
  <{ progressPercent: number }>_,
);

/** Bytes of the disc image hashed so far while `checkValidIso` verifies `path`. */
export const ipc_isoVerificationProgressEvent = makeEndpoint.renderer(
  "isoVerification_progress",
  <{ path: string; current: number; total: number }>_,
);

export const ipc_launcherUpdateReadyEvent = makeEndpoint.renderer("launcherupdate_ready", <EmptyPayload>_);
