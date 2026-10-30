import type { TestModeOptions } from "@common/test_mode";
import path from "path";

import { LOGGING_CONFIG_ARGS } from "../netplay_args";
import type { ModTextures } from "./mod_textures";
import type { ModBuild, ModRoomBuild, ModsController, ModsSdChoice } from "./types";

/** What Dolphin's `--mods-room` takes (the JSON of PPR_MODS_ROOM, docs/design/brawl-mods.md §3),
 * the build with its parts for the room (§6.5). */
export type ModsRoomArg =
  | {
      host: true;
      build: ModRoomBuild;
      public: boolean;
      controller: ModsController;
      /** The codes every machine of the room runs (docs/design/room-codes.md); absent: none. */
      codes?: { name: string; kind: "gecko" | "ar"; lines: string[] }[];
    }
  /** `sd_sync`: the player's answer to the SD card question, asked before Dolphin started (Dolphin
   * asks through the room's status when it is left out: a card that only turned out different
   * there). */
  | { join: string; build: ModRoomBuild; controller: ModsController; sd_sync?: ModsSdChoice };

export type ModsArgsInput = {
  /** Arguments selecting the User folder (`-u <User>`), shared with Play (the login, the settings). */
  userArgs: string[];
  build: Pick<ModBuild, "discPath" | "launcherPath" | "sdPath">;
  /** The mod's own Dolphin folder's HD textures (mod_textures.ts), or null for the User folder's. */
  textures?: ModTextures | null;
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
 * Dolphin's command line for a Brawl Mods room: the build's SD card (or none), the mod folder's
 * `Load` folder with custom textures on when it has a pack for the game, the disc as the default
 * ISO (a launcher `.dol` boots it), the launcher or the disc itself as what to boot, and the room.
 * Settings go in Dolphin's command-line config layer (`-C`), not its ini files, so Play keeps P+'s.
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
  // A path with "=" would be cut short: the textures are left out rather than the room.
  const load = input.textures ? path.resolve(input.textures.loadPath) : null;
  if (load && !load.includes("=")) {
    args.push("-C", `Dolphin.General.LoadPath=${load}`, "-C", "GFX.Settings.HiresTextures=True");
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
