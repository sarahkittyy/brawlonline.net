import path from "path";

import { DolphinLaunchType } from "../types";

/** Dolphin's binary name inside a build folder (`Binaries/`), per OS. */
export function dolphinExecutableName(platform: NodeJS.Platform = process.platform): string {
  switch (platform) {
    case "win32":
      return "Dolphin.exe";
    case "darwin":
      return path.join("Dolphin.app", "Contents", "MacOS", "Dolphin");
    default:
      return "project-plus-dolphin";
  }
}

/**
 * The executable inside the Dolphin bundle the launcher ships and installs into
 * `<userData>/netplay` (see bundled_dolphin.ts). On Linux the bundle is an AppDir-style tree
 * (`usr/bin`, `usr/lib`) made by linuxdeploy, with `Sys` next to the binary.
 */
export function installedDolphinExecutable(platform: NodeJS.Platform = process.platform): string {
  return platform === "linux"
    ? path.join("usr", "bin", dolphinExecutableName(platform))
    : dolphinExecutableName(platform);
}

export type DolphinPathEnv = {
  /** `process.env` */
  env: Record<string, string | undefined>;
  isDevelopment: boolean;
  /** The launcher repository (process.cwd() when running `npm start`). */
  devRepoRoot: string;
  userDataDir: string;
  platform?: NodeJS.Platform;
};

/** Folder name under userData for a launch type, as in Slippi (`netplay`, `playback`). */
export function launchTypeFolder(launchType: DolphinLaunchType): string {
  return launchType === DolphinLaunchType.NETPLAY ? "netplay" : "playback";
}

/**
 * Default Dolphin executable when none is configured in Settings:
 *
 * 1. `PPO_DOLPHIN_PATH` if set.
 * 2. Development: our Dolphin fork's build next to the launcher repo,
 *    `<repo>/../dolphin/build/release/x64/Binaries/Dolphin.exe`
 *    (D:\code\pm_rollback\dolphin\build\release\x64\Binaries\Dolphin.exe on the dev machine).
 * 3. Otherwise the bundled build the launcher installs into `<userData>/netplay` (Slippi's layout).
 *
 * The playback Dolphin defaults to the netplay one: we ship a single build.
 */
export function defaultDolphinExecutable(_launchType: DolphinLaunchType, ctx: DolphinPathEnv): string {
  const platform = ctx.platform ?? process.platform;
  const fromEnv = ctx.env.PPO_DOLPHIN_PATH;
  if (fromEnv && fromEnv.trim() !== "") {
    return fromEnv.trim();
  }
  if (ctx.isDevelopment) {
    const binaries =
      platform === "win32"
        ? path.join("build", "release", "x64", "Binaries")
        : platform === "darwin"
        ? path.join("build", "Binaries")
        : path.join("build", "Binaries");
    return path.resolve(ctx.devRepoRoot, "..", "dolphin", binaries, dolphinExecutableName(platform));
  }
  return path.join(ctx.userDataDir, launchTypeFolder(DolphinLaunchType.NETPLAY), installedDolphinExecutable(platform));
}

/**
 * Development and tests only: a Dolphin User folder to seed a new User folder from (P+'s
 * `Launcher/` DOLs, `Wii/sd.raw`, ...), `PPO_DOLPHIN_USER_TEMPLATE`, or in development
 * `<repo>/../run/template-user`. Otherwise the launcher downloads P+'s files from P+'s official
 * release (pplus_release.ts).
 */
export function defaultUserTemplate(ctx: DolphinPathEnv): string | null {
  const fromEnv = ctx.env.PPO_DOLPHIN_USER_TEMPLATE;
  if (fromEnv && fromEnv.trim() !== "") {
    return fromEnv.trim();
  }
  if (ctx.isDevelopment) {
    return path.resolve(ctx.devRepoRoot, "..", "run", "template-user");
  }
  return null;
}

/** Our game plugin's file name; P+'s Syriinge loads every `/Project+/pf/plugins/*.rel`. */
export const PLUGIN_FILE_NAME = "PPOnline.rel";
/** Where the plugin goes on the P+ SD card (see docs/game-code.md section 2). */
export const PLUGIN_SD_PATH = `/Project+/pf/plugins/${PLUGIN_FILE_NAME}`;

export type PluginLocation = {
  /** The plugin binary. */
  binary: string;
  /**
   * A manifest shipped with it (`{"file", "size", "sha256"}`) that the binary must match, or
   * null where none is expected (development builds, `PPO_PLUGIN_PATH`).
   */
  manifest: string | null;
};

/**
 * Where the game plugin (`PPOnline.rel`) installed on the SD card on Play comes from:
 *
 * 1. `PPO_PLUGIN_PATH` if set (no manifest).
 * 2. Development: game-code's build output next to the launcher repo,
 *    `<repo>/../game-code/PPOnline/PPOnline.rel` (no manifest).
 * 3. Packaged: `<resources>/plugins/PPOnline.rel` with its manifest `<resources>/plugins/PPOnline.json`,
 *    shipped through electron-builder's `extraResources` (see PPLUS_PORTING.md, "Shipping the plugin").
 */
export function defaultPluginLocation(ctx: DolphinPathEnv & { resourcesPath: string }): PluginLocation {
  const fromEnv = ctx.env.PPO_PLUGIN_PATH;
  if (fromEnv && fromEnv.trim() !== "") {
    return { binary: fromEnv.trim(), manifest: null };
  }
  if (ctx.isDevelopment) {
    return { binary: path.resolve(ctx.devRepoRoot, "..", "game-code", "PPOnline", PLUGIN_FILE_NAME), manifest: null };
  }
  const dir = path.join(ctx.resourcesPath, "plugins");
  return { binary: path.join(dir, PLUGIN_FILE_NAME), manifest: path.join(dir, "PPOnline.json") };
}

/**
 * The launcher-managed copy of the user's P+ SD card with the plugin installed:
 * `<userData>/netplay/pponline-sd/sd.raw`, plus `manifest.json` next to it.
 */
export function patchedSdCardFolder(userDataDir: string): string {
  return path.join(userDataDir, launchTypeFolder(DolphinLaunchType.NETPLAY), "pponline-sd");
}

/** The netplay install folder (`<userData>/netplay`), where the bundled Dolphin is installed. */
export function netplayInstallFolder(userDataDir: string): string {
  return path.join(userDataDir, launchTypeFolder(DolphinLaunchType.NETPLAY));
}

/** Launcher-owned P+ data: the Brawl save template and the marker (`<userData>/netplay/pplus`). */
export function projectPlusStoreFolder(userDataDir: string): string {
  return path.join(netplayInstallFolder(userDataDir), "pplus");
}
