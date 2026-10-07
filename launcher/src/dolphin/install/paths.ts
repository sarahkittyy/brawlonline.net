import path from "path";

import { DolphinLaunchType } from "../types";

/** Dolphin's binary name inside a build or install folder, per OS. */
export function dolphinExecutableName(platform: NodeJS.Platform = process.platform): string {
  switch (platform) {
    case "win32":
      return "Dolphin.exe";
    case "darwin":
      return path.join("Dolphin.app", "Contents", "MacOS", "Dolphin");
    default:
      return "dolphin-emu";
  }
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
 * 3. Otherwise where an installer would put it: `<userData>/netplay/<binary>`, Slippi's layout.
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
  return path.join(ctx.userDataDir, launchTypeFolder(DolphinLaunchType.NETPLAY), dolphinExecutableName(platform));
}

/**
 * Default Dolphin User folder template to seed a new User folder from (P+'s
 * `Launcher/` DOLs, `Wii/sd.raw`, `GameSettings/`, `Config/`):
 * `PPO_DOLPHIN_USER_TEMPLATE`, or in development `<repo>/../run/template-user`.
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
