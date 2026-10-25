// The Brawl Online folder (Settings > Dolphin, player feedback 2026-10-25: "it would be nice to
// have all these things in one place rather than have so many different settings to choose a path
// for"): one folder the player picks, with the game files (the netplay folder) in `Game Files`
// and the replays (with their `Highlights` clips and `Spectated` games) in `Replays`. Unset
// (`mainFolder` null) everything stays where it is, as before: the game files in
// `<userData>/netplay` or Settings' game files folder, the replays in `Documents/Brawl Online`.
// DolphinManager.moveMainFolder moves them; these are its pieces that need no launcher.
//
// SPDX-License-Identifier: GPL-3.0-or-later

import type { ManualClip } from "@replays/highlights/types";
import type { SettingsSchema, SettingUpdate } from "@settings/types";
import fs from "fs";
import path from "path";
import { isSubdirectory } from "utils/is_subdirectory";

export const MAIN_FOLDER_GAME_FILES = "Game Files";
export const MAIN_FOLDER_REPLAYS = "Replays";
const MAIN_FOLDER_ENTRIES = [MAIN_FOLDER_GAME_FILES, MAIN_FOLDER_REPLAYS];
// Files Explorer and Finder put in folders: they don't make a folder the player's.
const SYSTEM_FILES = ["desktop.ini", "thumbs.db", ".ds_store"];

export class MainFolderError extends Error {}

/** A move of a folder: everything at or under `from` is now under `to`. */
export type FolderMove = { from: string; to: string };

export const samePath = (a: string, b: string) => path.relative(path.resolve(a), path.resolve(b)) === "";

/** True if `dir` is missing, empty or holds nothing but a Brawl Online folder's own folders. */
async function usableMainFolder(dir: string): Promise<boolean> {
  let names: string[];
  try {
    names = await fs.promises.readdir(dir);
  } catch (err) {
    const code = (err as NodeJS.ErrnoException).code;
    if (code === "ENOENT") {
      return true;
    }
    if (code === "ENOTDIR") {
      return false;
    }
    throw err;
  }
  return names.every((name) => MAIN_FOLDER_ENTRIES.includes(name) || SYSTEM_FILES.includes(name.toLowerCase()));
}

/**
 * The Brawl Online folder when the player picks `picked`: the folder itself when it is missing,
 * empty or already a Brawl Online folder, else a `<subfolderName>` folder inside it (picking `D:\`
 * gives `D:\Brawl Online`), as the game files folder did.
 */
export async function chooseMainFolder(picked: string, subfolderName: string): Promise<string> {
  const folder = path.resolve(picked);
  if (await usableMainFolder(folder)) {
    return folder;
  }
  const inside = path.join(folder, subfolderName);
  if (await usableMainFolder(inside)) {
    return inside;
  }
  throw new MainFolderError(`${inside} already exists and is not empty.`);
}

/** `p` after `move`: its new path when it is `move.from` or inside it, else null. */
export function rebasePath(p: string, move: FolderMove): string | null {
  if (samePath(p, move.from)) {
    return move.to;
  }
  if (isSubdirectory(move.from, p)) {
    return path.join(move.to, path.relative(move.from, p));
  }
  return null;
}

const rebaseAll = (p: string, moves: readonly FolderMove[]) =>
  moves.reduce((current, move) => rebasePath(current, move) ?? current, p);

/**
 * The settings that hold paths of moved folders, changed to the new paths: the other replay
 * folders (Dolphin's own one is in the game files' User folder), the clips folder, the starred
 * highlights (keyed by replay path, favorites.ts) and the manual clips. Empty when none does.
 */
export function rebaseSettings(
  settings: Pick<SettingsSchema, "extraSlpPaths" | "clipsFolder" | "favoriteHighlights" | "manualClips">,
  moves: readonly FolderMove[],
): SettingUpdate[] {
  const updates: SettingUpdate[] = [];
  const extraSlpPaths = settings.extraSlpPaths.map((p) => rebaseAll(p, moves));
  if (extraSlpPaths.some((p, i) => p !== settings.extraSlpPaths[i])) {
    updates.push({ key: "extraSlpPaths", value: extraSlpPaths });
  }
  if (settings.clipsFolder != null) {
    const clipsFolder = rebaseAll(settings.clipsFolder, moves);
    if (clipsFolder !== settings.clipsFolder) {
      updates.push({ key: "clipsFolder", value: clipsFolder });
    }
  }
  // `<replay path>|<kind>|<start frame>`: the path may have `|` in it, the rest never does.
  const favoriteHighlights = settings.favoriteHighlights.map((key) => {
    const parts = key.split("|");
    if (parts.length < 3) {
      return key;
    }
    const replay = parts.slice(0, -2).join("|");
    return [rebaseAll(replay, moves), ...parts.slice(-2)].join("|");
  });
  if (favoriteHighlights.some((k, i) => k !== settings.favoriteHighlights[i])) {
    updates.push({ key: "favoriteHighlights", value: favoriteHighlights });
  }
  const manualClips: ManualClip[] = settings.manualClips.map((clip) => {
    const moved = rebaseAll(clip.path, moves);
    return moved === clip.path ? clip : { ...clip, path: moved };
  });
  if (manualClips.some((c, i) => c !== settings.manualClips[i])) {
    updates.push({ key: "manualClips", value: manualClips });
  }
  return updates;
}

/**
 * True if the replay folder `rootSlpPath` is the launcher's own, which moves with the Brawl Online
 * folder: the default one (`Documents/Brawl Online`) or the Brawl Online folder's `Replays`. A
 * replay folder the player chose stays where it is.
 */
export function isOwnReplayFolder(rootSlpPath: string, defaultRootSlpPath: string, mainFolder: string | null): boolean {
  return (
    samePath(rootSlpPath, defaultRootSlpPath) ||
    (mainFolder != null && samePath(rootSlpPath, path.join(mainFolder, MAIN_FOLDER_REPLAYS)))
  );
}
