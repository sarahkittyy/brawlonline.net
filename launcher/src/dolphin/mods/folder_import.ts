/**
 * "Add a mod folder" (docs/design/brawl-mods.md §8): most Brawl mods ship as a whole Dolphin
 * folder (Brawl Minus, REX, Project+): a launcher (`.dol` or `.elf`) under `Launcher/`, and
 * maybe an SD card image under the User folder. This finds them so the player only picks the
 * folder.
 */

import { readdir, stat } from "fs/promises";
import path from "path";

export type FolderParts = {
  /** A name for the build: the folder's, without " - Dolphin" and the like. */
  name: string;
  launcherPath: string | null;
  sdPath: string | null;
};

const LAUNCHER_DIRS = ["Launcher", "launcher", "user/Launcher", "User/Launcher"];
const SD_FILES = ["user/Wii/sd.raw", "User/Wii/sd.raw", "User/Load/WiiSD.raw", "user/Load/WiiSD.raw", "sd.raw"];

async function isFile(p: string): Promise<boolean> {
  return !!(await stat(p).catch(() => null))?.isFile();
}

/** The folder itself, or its only subfolder when that holds the Dolphin (archives often nest one). */
async function dolphinRoot(dir: string): Promise<string> {
  const entries = await readdir(dir, { withFileTypes: true }).catch(() => []);
  const looksLikeDolphin = entries.some((e) => /^(Launcher|launcher|user|User|Sys)$/.test(e.name));
  if (looksLikeDolphin) {
    return dir;
  }
  const dirs = entries.filter((e) => e.isDirectory());
  return dirs.length === 1 ? path.join(dir, dirs[0].name) : dir;
}

/** Of a mod's launchers, the one for netplay ("REX Netplay Launcher.elf" over the offline one). */
export function pickLauncher(files: string[]): string | null {
  const launchers = files.filter((f) => /\.(dol|elf)$/i.test(f));
  if (launchers.length === 0) {
    return null;
  }
  return launchers.find((f) => /netplay/i.test(path.basename(f))) ?? launchers.sort()[0];
}

export function cleanFolderName(folder: string): string {
  const base = path.basename(folder);
  const cleaned = base
    .replace(/\s*[-–]\s*Dolphin\s*$/i, "")
    .replace(/\s*\(Dolphin\)\s*$/i, "")
    .replace(/\s+/g, " ")
    .trim();
  return (cleaned || base).slice(0, 64);
}

export async function findModParts(folder: string): Promise<FolderParts> {
  const root = await dolphinRoot(folder);
  const files: string[] = [];
  for (const sub of LAUNCHER_DIRS) {
    const dir = path.join(root, sub);
    const entries = await readdir(dir, { withFileTypes: true }).catch(() => []);
    files.push(...entries.filter((e) => e.isFile()).map((e) => path.join(dir, e.name)));
  }
  const launcherPath = pickLauncher(files);
  let sdPath: string | null = null;
  for (const rel of SD_FILES) {
    const p = path.join(root, rel);
    if (await isFile(p)) {
      sdPath = p;
      break;
    }
  }
  if (!launcherPath && !sdPath) {
    throw new Error("No mod launcher (.dol or .elf) or SD card was found in that folder.");
  }
  return { name: cleanFolderName(folder === root ? folder : folder), launcherPath, sdPath };
}
