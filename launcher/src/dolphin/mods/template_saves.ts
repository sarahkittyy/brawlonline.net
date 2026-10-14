/**
 * Clears, once, the Brawl saves of Brawl Mods builds that started from P+'s save
 * (docs/design/brawl-mods.md §6.5).
 *
 * Each build keeps its save in `<User>/Saves/<16 hex digits of its hash>/`, laid out like a NAND
 * root. Until 2026-10-14 a Brawl-based build with no save yet started from P+'s template save
 * (every Brawl mod uses Brawl's title, RSBE), kept it at the end, and so had P+'s name tags and
 * controls ("NoTap") for good (prod: REX, vanilla Brawl). Dolphin now starts such a build without
 * a save; a build's store that holds a Brawl save is deleted here once, so the game makes its own
 * next time. Other games' stores (another title) never had P+'s save and are left alone.
 */

import { mkdir, readdir, rm, stat, writeFile } from "fs/promises";
import path from "path";

/** Brawl NTSC-U's save inside a NAND root, the title P+'s template save is for. */
const BRAWL_TITLE = ["title", "00010000", "52534245"] as const;
/** In `<User>/Saves/` once the cleanup has run. */
export const TEMPLATE_SAVES_CLEARED = ".pplus-template-saves-cleared";

const BUILD_STORE = /^[0-9a-f]{16}$/i;

async function isDirectory(p: string): Promise<boolean> {
  try {
    return (await stat(p)).isDirectory();
  } catch {
    return false;
  }
}

/**
 * Deletes every build store in `<userFolder>/Saves` that holds a Brawl save, unless that was done
 * before; returns the stores deleted. Run while no Dolphin uses the User folder.
 */
export async function clearTemplateSeededSaves(
  userFolder: string,
  log: (message: string) => void = () => undefined,
): Promise<string[]> {
  const saves = path.join(userFolder, "Saves");
  const marker = path.join(saves, TEMPLATE_SAVES_CLEARED);
  // No build has kept a save yet: the marker still goes in, or the saves this Dolphin makes from
  // now on (without P+'s) would be cleared at the next launch.
  await mkdir(saves, { recursive: true });
  const entries = await readdir(saves);
  if (entries.includes(TEMPLATE_SAVES_CLEARED)) {
    return [];
  }
  const cleared: string[] = [];
  for (const name of entries) {
    const store = path.join(saves, name);
    if (BUILD_STORE.test(name) && (await isDirectory(path.join(store, ...BRAWL_TITLE)))) {
      await rm(store, { recursive: true, force: true });
      cleared.push(name);
    }
  }
  await writeFile(marker, `${new Date().toISOString()}\n`, "utf8");
  log(
    cleared.length > 0
      ? `Cleared the Brawl saves of ${cleared.length} build(s) that started from P+'s save: ${cleared.join(", ")}`
      : "No build's Brawl save started from P+'s save",
  );
  return cleared;
}
