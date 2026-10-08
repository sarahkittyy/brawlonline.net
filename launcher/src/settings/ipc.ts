import type { SuccessPayload } from "utils/ipc";
import { _, makeEndpoint } from "utils/ipc";

import type { SettingUpdate } from "./types";

/**
 * Generic endpoints for the new settings system
 */

export const ipc_updateSettings = makeEndpoint.main(
  "updateSettings",
  <{ updates: SettingUpdate[] }>_,
  <SuccessPayload>_,
);

/**
 * Events
 */

// Incremental setting updates (used for syncing changes efficiently)
export const ipc_settingChangedEvent = makeEndpoint.renderer(
  "settings_settingChanged",
  <{ updates: SettingUpdate[] }>_,
);

export const ipc_openSettingsModalEvent = makeEndpoint.renderer("openSettingsModal", <Record<string, never>>_);
