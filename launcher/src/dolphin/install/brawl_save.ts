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
// Never overwrites: a NAND that has any file in the save folder keeps it, so what the game
// writes (name tags, rules, records) stays. The copy goes to a staging folder that is renamed
// into place, so an interrupted copy never leaves a partial save that would count as present.
//
// SPDX-License-Identifier: GPL-3.0-or-later

import fs from "fs";
import path from "path";

/** Brawl NTSC-U's save folder inside a NAND root (title 00010000/52534245, "RSBE"). */
export const BRAWL_SAVE_DIR = ["title", "00010000", "52534245", "data"] as const;

const STAGING_SUFFIX = ".seeding";

export type BrawlSaveResult =
  | { action: "present"; dir: string }
  | { action: "seeded"; dir: string; from: string; files: number; bytes: number }
  | { action: "no-template"; dir: string };

export function brawlSaveDir(nandRoot: string): string {
  return path.join(nandRoot, ...BRAWL_SAVE_DIR);
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
  if (existing && existing.length > 0) {
    return { action: "present", dir };
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
    if (now && now.length > 0) {
      await fs.promises.rm(staging, { recursive: true, force: true });
      return { action: "present", dir };
    }
    if (now) {
      await fs.promises.rm(dir, { recursive: true, force: true }); // empty: Dolphin made it at a disc boot
    }
    await fs.promises.rename(staging, dir);
    log(`Seeded the Brawl save in ${dir} from ${from} (${files.length} files, ${bytes} bytes)`);
    return { action: "seeded", dir, from, files: files.length, bytes };
  }
  log(`No Brawl save template found (${templateRoots.join(", ")}); P+ will offer to create a save`);
  return { action: "no-template", dir };
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
