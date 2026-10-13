import type { TestModeOptions } from "@common/test_mode";
import path from "path";

import { LOGGING_CONFIG_ARGS } from "../netplay_args";
import type { ModBuild, ModsController } from "./types";

/** What Dolphin's `--mods-room` takes (the JSON of PPR_MODS_ROOM, docs/design/brawl-mods.md §3). */
export type ModsRoomArg =
  | { host: true; build: { hash: string; name: string }; public: boolean; controller: ModsController }
  | { join: string; build: { hash: string; name: string }; controller: ModsController };

export type ModsArgsInput = {
  /** Arguments selecting the User folder (`-u <User>`), shared with Play (the login, the settings). */
  userArgs: string[];
  build: Pick<ModBuild, "discPath" | "launcherPath" | "sdPath">;
  room: ModsRoomArg;
  testMode: Pick<TestModeOptions, "active" | "harnessPort" | "dolphinExtraArgs">;
};

/** Dolphin splits `-C` values at '=': such a path would be cut short. */
function configPath(file: string, what: string): string {
  const resolved = path.resolve(file);
  if (resolved.includes("=")) {
    throw new Error(`The ${what} path cannot contain "=": ${resolved}`);
  }
  return resolved;
}

/**
 * Dolphin's command line for a Brawl Mods room: the build's SD card (or none), the disc as the
 * default ISO (a launcher `.dol` boots it), the launcher or the disc itself as what to boot, and
 * the room. Settings go in Dolphin's command-line config layer (`-C`), not its ini files, so Play
 * keeps P+'s.
 */
export function buildModsDolphinArgs(input: ModsArgsInput): string[] {
  const { build } = input;
  const args = [...input.userArgs, ...LOGGING_CONFIG_ARGS];
  if (build.sdPath) {
    args.push(
      "-C",
      `Dolphin.General.WiiSDCardPath=${configPath(build.sdPath, "SD card")}`,
      "-C",
      "Dolphin.Core.WiiSDCard=True",
      "-C",
      "Dolphin.Core.WiiSDCardEnableFolderSync=False",
    );
  } else {
    args.push("-C", "Dolphin.Core.WiiSDCard=False");
  }
  const disc = configPath(build.discPath, "disc");
  args.push("-C", `Dolphin.Core.DefaultISO=${disc}`);
  args.push("-e", build.launcherPath ? path.resolve(build.launcherPath) : disc);
  args.push("--mods-room", JSON.stringify(input.room));
  if (input.testMode.active) {
    if (input.testMode.harnessPort !== null) {
      args.push("--harness-port", String(input.testMode.harnessPort));
    }
    args.push(...input.testMode.dolphinExtraArgs);
  }
  return args;
}
