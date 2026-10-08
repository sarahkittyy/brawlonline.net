import { MAC_SELF_UPDATE } from "./product";

/**
 * How the launcher handles a new version from its update feed:
 *
 * - `install`: electron-updater downloads it in the background and "Install update" restarts into it
 *   (Slippi's behaviour; Windows and Linux).
 * - `download`: the update is only found, not downloaded, and the update bar opens the website's
 *   download instead (macOS while the build is not Developer ID signed, see MAC_SELF_UPDATE).
 */
export type LauncherUpdateMode = "install" | "download";

export function getLauncherUpdateMode(platform: string, macSelfUpdate: boolean = MAC_SELF_UPDATE): LauncherUpdateMode {
  return platform === "darwin" && !macSelfUpdate ? "download" : "install";
}
