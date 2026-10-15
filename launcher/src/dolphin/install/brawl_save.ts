// Gives the netplay NAND a Brawl save before Play boots P+, so P+ never asks
// "Create save file for Project+?".
//
// Brawl's boot save check (sora_menu_boot, muBootNandTask) opens that prompt whenever the NAND
// has no save under /title/00010000/52534245/data (RSBE). Play boots P+ from the User folder's
// own NAND (`<User>/Wii`, or Dolphin's [General] NANDRootPath), which starts empty, so every new
// player met the prompt on their first Play (and, with No, on every Play). Answering Yes spends
// about 13 s writing a fresh 14.6 MB save.
//
// The save comes from P+'s own Brawl save template, `Sys/NetplaySave` of P+'s official release
// (the files Dolphin's WiiRoot.cpp copies into the NAND of a Dolphin netplay session), which
// the launcher already downloads (pplus_release.ts). Only the save's data files are copied:
// Dolphin writes the title's TMD itself when the disc boots, and the template's would be the
// TMD of one disc revision.
//
// Never overwrites a save that has its banner.bin, so what the game writes (name tags, rules,
// records) stays. A save folder with files but no banner.bin is replaced: Brawl takes such a save
// for broken, deletes its own files and makes a new save, which it then refuses as "not enough
// available space in Wii system memory" because the files it left (filelist.dat, wc24pubk.mod)
// put the 128 blocks over the title's 16 MB NAND quota (NANDCheck), on every boot. Dolphin's
// whole-machine sessions used to leave the player's save like that before 2026-10-14 (WiiRoot.cpp,
// CleanUpWiiFileSystemContents; the user's PC 2026-10-15). The copy goes to a staging folder that
// is renamed into place, so an interrupted copy never leaves a partial save that would count as
// present.
//
// SPDX-License-Identifier: GPL-3.0-or-later

import fs from "fs";
import path from "path";

/** Brawl NTSC-U's save folder inside a NAND root (title 00010000/52534245, "RSBE"). */
export const BRAWL_SAVE_DIR = ["title", "00010000", "52534245", "data"] as const;

const STAGING_SUFFIX = ".seeding";
/** Brawl writes it last when it makes its save, and treats a save without it as broken. */
const BANNER = "banner.bin";

export type BrawlSaveResult =
  | { action: "present"; dir: string }
  | { action: "seeded"; dir: string; from: string; files: number; bytes: number; removed: string[] }
  | { action: "no-template"; dir: string; removed: string[] };

export function brawlSaveDir(nandRoot: string): string {
  return path.join(nandRoot, ...BRAWL_SAVE_DIR);
}

/** Whether a save folder's files are a save Brawl keeps (it has its banner). */
export function isUsableBrawlSave(files: string[]): boolean {
  return files.some((name) => name.toLowerCase() === BANNER);
}

async function listFiles(dir: string): Promise<fs.Dirent[] | null> {
  try {
    return (await fs.promises.readdir(dir, { withFileTypes: true })).filter((e) => e.isFile());
  } catch {
    return null;
  }
}

/**
 * Copies the Brawl save from the first template root that has one into `nandRoot`, unless the
 * NAND already has a save. A template root is laid out like a NAND root (`title/...`), as P+'s
 * `Sys/NetplaySave` is.
 */
export async function ensureBrawlSave({
  nandRoot,
  templateRoots,
  log = () => undefined,
}: {
  nandRoot: string;
  templateRoots: string[];
  log?: (message: string) => void;
}): Promise<BrawlSaveResult> {
  const dir = brawlSaveDir(nandRoot);
  const existing = await listFiles(dir);
  if (existing && isUsableBrawlSave(existing.map((e) => e.name))) {
    return { action: "present", dir };
  }
  // Files without a banner: what is left of a broken save, which only stops Brawl making a new one
  const removed = (existing ?? []).map((e) => e.name).sort();
  if (removed.length > 0) {
    for (const name of removed) {
      await fs.promises.rm(path.join(dir, name), { force: true });
    }
    log(`Removed a Brawl save without ${BANNER} from ${dir} (${removed.join(", ")})`);
  }

  for (const root of templateRoots) {
    const from = brawlSaveDir(root);
    const files = await listFiles(from);
    if (!files || files.length === 0) {
      continue;
    }
    const staging = dir + STAGING_SUFFIX;
    await fs.promises.rm(staging, { recursive: true, force: true });
    await fs.promises.mkdir(staging, { recursive: true });
    let bytes = 0;
    for (const f of files) {
      await fs.promises.copyFile(path.join(from, f.name), path.join(staging, f.name));
      bytes += (await fs.promises.stat(path.join(staging, f.name))).size;
    }
    // The game (or another Play) may have written a save meanwhile: then keep that one.
    const now = await listFiles(dir);
    if (now && isUsableBrawlSave(now.map((e) => e.name))) {
      await fs.promises.rm(staging, { recursive: true, force: true });
      return { action: "present", dir };
    }
    if (now) {
      // Empty (Dolphin made it at a disc boot), or what the removal above left
      await fs.promises.rm(dir, { recursive: true, force: true });
    }
    await fs.promises.rename(staging, dir);
    log(`Seeded the Brawl save in ${dir} from ${from} (${files.length} files, ${bytes} bytes)`);
    return { action: "seeded", dir, from, files: files.length, bytes, removed };
  }
  log(`No Brawl save template found (${templateRoots.join(", ")}); P+ will offer to create a save`);
  return { action: "no-template", dir, removed };
}

/**
 * The NAND root Dolphin uses for a User folder: `[General] NANDRootPath` of its Dolphin.ini when
 * set, else `<User>/Wii` (Dolphin's default, D_WIIROOT_IDX).
 */
export function nandRootFor(userFolder: string, configuredNandRoot: string | undefined): string {
  const configured = configuredNandRoot?.trim();
  if (configured) {
    return path.isAbsolute(configured) ? configured : path.resolve(userFolder, configured);
  }
  return path.join(userFolder, "Wii");
}
