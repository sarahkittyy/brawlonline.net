/**
 * HD textures from a mod's own Dolphin folder (player feedback 2026-10-25: "Make it so the "Other
 * Games" Mod folder selected also uses that native folder location for HD Texture loading"). A
 * room's Dolphin is the netplay one, so it read custom textures only from `<netplay
 * User>/Load/Textures`, which starts empty. Now, when the Dolphin folder the build's launcher or SD
 * card is in (up to three folders above either, as findDolphinSaveNear) has a pack for the game in
 * its `Load/Textures`, the room's Dolphin uses that `Load` folder (`General.LoadPath`) with custom
 * textures on, unless that Dolphin has them off. Both are command-line settings (mods_args.ts),
 * never written to an ini: Play, the next room and a move of the Brawl Online folder never see them.
 */

import { readdir, readFile, stat } from "fs/promises";
import path from "path";

import type { ModBuild } from "./types";

/** Dolphin's own name for it (D_LOAD_DIR); mods ship their User folder as `User` or `user`. */
const LOAD_DIRS = ["Load", path.join("User", "Load"), path.join("user", "Load")];

export type ModTextures = { loadPath: string };

async function isDir(p: string): Promise<boolean> {
  return !!(await stat(p).catch(() => null))?.isDirectory();
}

/**
 * The IDs Dolphin looks for textures under (SConfig::GetGameIDsForTextures): a launcher boot is
 * `ID-<launcher's name>` and keeps it in front of the disc's ID once the launcher boots the disc.
 */
export function textureGameIds(gameId: string, launcherPath: string | null): string[] {
  if (!launcherPath) {
    return [gameId];
  }
  const name = path.basename(launcherPath);
  const dot = name.lastIndexOf(".");
  return [`ID-${dot < 0 ? name : name.slice(0, dot)}-${gameId}`, gameId];
}

/**
 * True if Dolphin finds textures for `gameId` in `textures` (VideoCommon's
 * GetTextureDirectoriesWithGameId): a folder named for the game ID (or a launcher's ID for it,
 * textureGameIds) or its first three characters, or a `<game ID>.txt`, `<first three>.txt` or
 * `all.txt` anywhere under it.
 */
export async function hasTexturePack(
  textures: string,
  gameId: string,
  launcherPath: string | null = null,
): Promise<boolean> {
  for (const name of [...textureGameIds(gameId, launcherPath), gameId.slice(0, 3)]) {
    if (await isDir(path.join(textures, name))) {
      return true;
    }
  }
  const markers = new Set([gameId, gameId.slice(0, 3), "all"].map((name) => `${name}.txt`));
  const search = async (dir: string): Promise<boolean> => {
    const entries = await readdir(dir, { withFileTypes: true }).catch(() => []);
    if (entries.some((e) => e.isFile() && markers.has(e.name))) {
      return true;
    }
    for (const e of entries) {
      if (e.isDirectory() && (await search(path.join(dir, e.name)))) {
        return true;
      }
    }
    return false;
  };
  return search(textures);
}

/** False when the Dolphin whose User folder holds `load` has custom textures off in its GFX.ini. */
async function customTexturesOn(load: string): Promise<boolean> {
  const ini = await readFile(path.join(load, "..", "Config", "GFX.ini"), "utf8").catch(() => "");
  let section = "";
  for (const raw of ini.split(/\r?\n/)) {
    const line = raw.trim();
    const header = /^\[(.*)\]$/.exec(line);
    if (header) {
      section = header[1];
      continue;
    }
    const setting = /^HiresTextures\s*=\s*(\S+)/i.exec(line);
    if (section === "Settings" && setting) {
      return setting[1].toLowerCase() !== "false";
    }
  }
  return true;
}

/**
 * The `Load` folder of the mod's Dolphin folder when it has textures for the build's game and that
 * Dolphin uses them, else null (the netplay User folder's own `Load`, as before).
 */
export async function findModTextures(
  build: Pick<ModBuild, "launcherPath" | "sdPath" | "gameId">,
  netplayUserFolder: string,
): Promise<ModTextures | null> {
  const { gameId, launcherPath } = build;
  if (!gameId) {
    return null;
  }
  const ownLoad = path.resolve(netplayUserFolder, "Load");
  for (const file of [launcherPath, build.sdPath]) {
    if (!file) {
      continue;
    }
    let dir = path.dirname(path.resolve(file));
    for (let up = 0; up < 3; up++) {
      for (const sub of LOAD_DIRS) {
        const load = path.join(dir, sub);
        if (path.relative(load, ownLoad) === "") {
          // The netplay User folder's own (P+'s card as a build): what Dolphin reads anyway.
          return null;
        }
        if (await hasTexturePack(path.join(load, "Textures"), gameId, launcherPath)) {
          return (await customTexturesOn(load)) ? { loadPath: load } : null;
        }
      }
      const parent = path.dirname(dir);
      if (parent === dir) {
        break;
      }
      dir = parent;
    }
  }
  return null;
}
