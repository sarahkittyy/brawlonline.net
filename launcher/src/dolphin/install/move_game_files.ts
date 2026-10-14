// Moves the netplay folder (`<userData>/netplay` by default: the User folder with P+'s 2 GB SD
// card, P+'s files and the 2 GB patched copy of the card) to a folder chosen in Settings > Dolphin,
// so the game files need not stay on the system drive (player feedback 2026-10-12: setting the
// Dolphin executable to a copy on another drive left the launcher installing everything on C:
// again). Only the launcher's data is moved; the bundled Dolphin build is installed into the new
// folder afterwards, as after a launcher update.
//
// SPDX-License-Identifier: GPL-3.0-or-later

import fs from "fs";
import path from "path";
import type { ByteProgress } from "utils/copy_file";
import { copyFileWithProgress } from "utils/copy_file";
import { isSubdirectory } from "utils/is_subdirectory";

import { DOLPHIN_MANIFEST_NAME, OLD_DOLPHIN_DIR, PROTECTED_ENTRIES, readDolphinManifest } from "./bundled_dolphin";
import { patchedSdCardFolder } from "./paths";
import { relocatePatchedSdSource, statSdSource } from "./sd_card";

/** The netplay folder's entries that hold the launcher's and the player's data (not the Dolphin build). */
export const GAME_DATA_ENTRIES = [...PROTECTED_ENTRIES].filter((name) => name !== DOLPHIN_MANIFEST_NAME);

export class GameFilesMoveError extends Error {}

/**
 * - `move`: the game files are moved into `folder` (missing or empty).
 * - `adopt`: `folder` already holds game files (a copy the player made): they are used as they
 *   are and the old ones are deleted, once the player agrees.
 */
export type GameFilesTarget = { folder: string; kind: "move" | "adopt" };

const samePath = (a: string, b: string) => path.relative(path.resolve(a), path.resolve(b)) === "";

const sdCardOf = (netplayDir: string) => path.join(netplayDir, "User", "Wii", "sd.raw");

async function entries(dir: string): Promise<string[] | null> {
  try {
    return await fs.promises.readdir(dir);
  } catch (err) {
    if ((err as NodeJS.ErrnoException).code === "ENOENT") {
      return null;
    }
    throw err;
  }
}

/** True if `dir` has a Dolphin User folder: game files the player copied there. */
async function holdsGameFiles(dir: string): Promise<boolean> {
  try {
    return (await fs.promises.stat(path.join(dir, "User"))).isDirectory();
  } catch {
    return false;
  }
}

/**
 * Where the game files go when the player picks `picked`: the folder itself when it is empty (or
 * missing) or already holds game files, else a `<subfolderName>` folder inside it (picking
 * `D:\Games` gives `D:\Games\Brawl Online`). `current` is the netplay folder in use.
 */
export async function chooseGameFilesTarget(
  picked: string,
  current: string,
  subfolderName: string,
): Promise<GameFilesTarget> {
  const check = async (folder: string): Promise<GameFilesTarget | null> => {
    if (samePath(folder, current)) {
      throw new GameFilesMoveError(`The game files are already in ${folder}.`);
    }
    if (isSubdirectory(current, folder) || isSubdirectory(folder, current)) {
      throw new GameFilesMoveError(`Choose a folder outside ${current}.`);
    }
    if (await holdsGameFiles(folder)) {
      return { folder, kind: "adopt" };
    }
    const names = await entries(folder);
    return names == null || names.length === 0 ? { folder, kind: "move" } : null;
  };
  const folder = path.resolve(picked);
  const target = (await check(folder)) ?? (await check(path.join(folder, subfolderName)));
  if (!target) {
    throw new GameFilesMoveError(`${path.join(folder, subfolderName)} already exists and is not empty.`);
  }
  return target;
}

async function treeSize(p: string): Promise<number> {
  const st = await fs.promises.lstat(p);
  if (!st.isDirectory()) {
    return st.isFile() ? st.size : 0;
  }
  let total = 0;
  for (const child of await fs.promises.readdir(p)) {
    total += await treeSize(path.join(p, child));
  }
  return total;
}

/** Copies a tree, keeping symbolic links and modification times, reporting each file's bytes. */
async function copyTree(from: string, to: string, onBytes: (fileDone: number) => void): Promise<void> {
  const st = await fs.promises.lstat(from);
  if (st.isDirectory()) {
    await fs.promises.mkdir(to, { recursive: true });
    for (const child of await fs.promises.readdir(from)) {
      await copyTree(path.join(from, child), path.join(to, child), onBytes);
    }
  } else if (st.isSymbolicLink()) {
    await fs.promises.symlink(await fs.promises.readlink(from), to);
  } else if (st.isFile()) {
    let last = 0;
    await copyFileWithProgress(from, to, {
      onProgress: (done) => {
        onBytes(done - last);
        last = done;
      },
    });
    await fs.promises.utimes(to, st.atime, st.mtime);
  }
}

/**
 * Moves the game data entries (GAME_DATA_ENTRIES) of `from` into `to`: renamed where both are on
 * one drive (instant), else copied with byte progress. On failure everything already moved is put
 * back and the copies are removed, so `from` is whole again. The sources of copied entries stay
 * until the caller removes the old folder (removeGameFiles), once the new one is in use.
 */
export async function moveGameFiles({
  from,
  to,
  onProgress,
  log = () => undefined,
  rename = fs.promises.rename,
}: {
  from: string;
  to: string;
  onProgress?: ByteProgress;
  log?: (message: string) => void;
  /** Test hook: fails with EXDEV between drives. */
  rename?: (from: string, to: string) => Promise<void>;
}): Promise<{ renamed: string[]; copied: string[] }> {
  const present = ((await entries(from)) ?? []).filter((name) => GAME_DATA_ENTRIES.includes(name));
  const sdBefore = await statSdSource(sdCardOf(from));
  await fs.promises.mkdir(to, { recursive: true });

  const renamed: string[] = [];
  const copied: string[] = [];
  const toCopy: string[] = [];
  try {
    for (const name of present) {
      try {
        await rename(path.join(from, name), path.join(to, name));
        renamed.push(name);
      } catch (err) {
        if ((err as NodeJS.ErrnoException).code !== "EXDEV") {
          throw err;
        }
        toCopy.push(name);
      }
    }
    if (toCopy.length > 0) {
      let total = 0;
      for (const name of toCopy) {
        total += await treeSize(path.join(from, name));
      }
      let done = 0;
      onProgress?.(0, total);
      for (const name of toCopy) {
        copied.push(name);
        await copyTree(path.join(from, name), path.join(to, name), (bytes) => {
          done = Math.min(total, done + bytes);
          onProgress?.(done, total);
        });
      }
      onProgress?.(total, total);
    }
  } catch (err) {
    log(
      `Moving the game files to ${to} failed (${err instanceof Error ? err.message : String(err)}); putting them back`,
    );
    for (const name of copied) {
      await fs.promises.rm(path.join(to, name), { recursive: true, force: true }).catch(() => undefined);
    }
    for (const name of renamed) {
      await fs.promises.rename(path.join(to, name), path.join(from, name)).catch((e) => {
        log(`Could not put ${name} back into ${from}: ${e instanceof Error ? e.message : String(e)}`);
      });
    }
    throw err;
  }

  if (sdBefore && (await relocatePatchedSdSource(patchedSdCardFolder(to), sdBefore, sdCardOf(to)))) {
    log("The patched SD card now refers to the moved card");
  }
  log(
    `Game files moved from ${from} to ${to}` +
      (renamed.length > 0 ? `; renamed: ${renamed.join(", ")}` : "") +
      (copied.length > 0 ? `; copied: ${copied.join(", ")}` : ""),
  );
  return { renamed, copied };
}

/**
 * Removes the launcher's files from an old netplay folder: the game data entries, the Dolphin
 * build its manifest lists, old builds moved aside, and the folder itself if nothing else is left
 * in it (it may be a folder the player also uses for other things).
 */
export async function removeGameFiles(dir: string): Promise<void> {
  const manifest = await readDolphinManifest(dir);
  const names = [...GAME_DATA_ENTRIES, ...(manifest?.entries ?? []), DOLPHIN_MANIFEST_NAME, OLD_DOLPHIN_DIR];
  for (const name of names) {
    if (!name || name.includes("/") || name.includes("\\") || name === "." || name === "..") {
      continue;
    }
    await fs.promises.rm(path.join(dir, name), { recursive: true, force: true, maxRetries: 10, retryDelay: 200 });
  }
  const left = await entries(dir);
  if (left != null && left.length === 0) {
    await fs.promises.rmdir(dir).catch(() => undefined);
  }
}
