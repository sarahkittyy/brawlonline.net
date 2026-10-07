import electronLog from "electron-log";

import type { DolphinLaunchType } from "../types";

export type DolphinVersionResponse = {
  version: string;
  downloadUrls: {
    darwin: string;
    linux: string;
    win32: string;
  };
};

const log = electronLog.scope("dolphin/fetchLatestVersion");

/**
 * Update channel hook for our Dolphin builds.
 *
 * Slippi asks its GraphQL backend `getLatestDolphin(purpose, includeBeta)` for per-OS
 * download URLs and installs them into `userData/{netplay,playback}`. Our builds
 * are not published yet, so this returns `null`: the launcher then uses the
 * Dolphin executable configured in Settings and never downloads anything.
 *
 * To enable updates later: serve `GET /v1/dolphin/latest?purpose=&beta=` from the
 * accounts service (backend-design.md section 2.4) returning this shape, call it
 * here, and DolphinManager will download and install it with the existing
 * installers in this folder (download.ts, windows.ts, macos.ts, linux.ts).
 */
export async function fetchLatestVersion(
  dolphinType: DolphinLaunchType,
  includeBeta = false,
): Promise<DolphinVersionResponse | null> {
  log.debug(`Dolphin update channel not configured (type=${dolphinType}, beta=${includeBeta}); skipping`);
  return null;
}
