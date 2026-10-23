/**
 * A GameCube game's saves in Other Games rooms (docs/design/brawl-mods.md §6.5): the game's memory
 * card in a room is a GCI folder card with the build's saves, `<netplay User>/Saves/<first 16 hex
 * digits of the build hash>/GC/`, one `.gci` file per save (Dolphin's GCMemcardDirectory, with only
 * the game's own saves). The room's host plays it and keeps what the game saved when the room's
 * game closes; a joiner plays the host's card.
 *
 * Dolphin's own memory cards are never read, so a player brings their saves in here, from
 * - a `.gci` file (Dolphin's GCI folders and its Memory Card Manager's export),
 * - a memory card image (`.raw`, `.gcp`: Dolphin's raw cards, a real card's dump), or
 * - a folder: a Dolphin User folder (its `GC` folder), a GCI folder (`GC/USA/Card A`), or one with
 *   `.gci` files or card images in it.
 * Only the game's saves are taken: the save's game code and maker are the disc's game ID (a game
 * opens only those; a hack that keeps the game code under another maker has its own). The saves it
 * replaces go to `<netplay User>/SaveBackups/<16 hex>/<time>/GC`.
 */

import { randomUUID } from "crypto";
import type { Dirent } from "fs";
import { mkdir, readdir, readFile, rename, rm, stat, writeFile } from "fs/promises";
import path from "path";

import type { SaveSummary } from "./save_import";
import { escapeNandName, KEEP_BACKUPS, saveStoreDir } from "./save_import";

export const GC_BLOCK_SIZE = 0x2000;
export const DENTRY_SIZE = 0x40;
/** The card's own blocks: header, directory and its backup, block map and its backup. */
const SYSTEM_BLOCKS = 5;
const DIRECTORY_ENTRIES = 127;
/** The biggest card (2043 blocks, 16 MB) and its blocks. */
const MAX_CARD_BYTES = 2048 * GC_BLOCK_SIZE;
const MAX_SAVE_BLOCKS = 2043;
/** A folder's files and card images looked at, at most. */
const MAX_CANDIDATES = 4096;

/** One save: the file's 0x40-byte directory entry and its blocks, as a `.gci` holds them. */
export type GciSave = {
  /** The save's game code ("GALE"), maker ("01") and file name, from its directory entry. */
  gameCode: string;
  maker: string;
  fileName: string;
  /** The entry's modification time (seconds since 2000), to keep the newest of two copies. */
  modified: number;
  /** The whole `.gci`: entry and blocks. */
  data: Buffer;
};

export function gcSaveDir(userFolder: string, buildHash: string): string {
  return path.join(saveStoreDir(userFolder, buildHash), "GC");
}

/** A save's game ID: its game code and maker ("GALE01"), as the disc's. */
const saveId = (save: Pick<GciSave, "gameCode" | "maker">) => `${save.gameCode}${save.maker}`;

const text = (b: Buffer) => {
  const end = b.indexOf(0);
  return b.subarray(0, end < 0 ? b.length : end).toString("latin1");
};

function readEntry(entry: Buffer): Omit<GciSave, "data"> & { firstBlock: number; blocks: number } {
  return {
    gameCode: text(entry.subarray(0, 4)),
    maker: text(entry.subarray(4, 6)),
    fileName: text(entry.subarray(8, 40)),
    modified: entry.readUInt32BE(0x28),
    firstBlock: entry.readUInt16BE(0x36),
    blocks: entry.readUInt16BE(0x38),
  };
}

/** The name Dolphin gives a save in a GCI folder (GenerateDefaultGCIFilename): maker, game code
 * and file name, escaped as host file names are. */
export function gciFileName(save: Pick<GciSave, "maker" | "gameCode" | "fileName">): string {
  return escapeNandName(`${save.maker}-${save.gameCode}-${save.fileName}.gci`);
}

/** A `.gci` file: its entry and exactly as many blocks as the entry says. */
export function readGci(file: Buffer): GciSave {
  if (file.length < DENTRY_SIZE || (file.length - DENTRY_SIZE) % GC_BLOCK_SIZE !== 0) {
    throw new Error("This file is not a GameCube save (.gci).");
  }
  const entry = readEntry(file);
  const blocks = (file.length - DENTRY_SIZE) / GC_BLOCK_SIZE;
  if (entry.blocks !== blocks || blocks === 0 || blocks > MAX_SAVE_BLOCKS || entry.gameCode.length !== 4) {
    throw new Error("This file is not a GameCube save (.gci).");
  }
  return {
    gameCode: entry.gameCode,
    maker: entry.maker,
    fileName: entry.fileName,
    modified: entry.modified,
    data: file,
  };
}

/** The newer of a block and its backup by their update counters (Dolphin's GCMemcard: the one
 * with the higher signed counter). */
function newer(card: Buffer, a: number, b: number, counterOffset: number): Buffer {
  const blockA = card.subarray(a * GC_BLOCK_SIZE, (a + 1) * GC_BLOCK_SIZE);
  const blockB = card.subarray(b * GC_BLOCK_SIZE, (b + 1) * GC_BLOCK_SIZE);
  return blockB.readInt16BE(counterOffset) > blockA.readInt16BE(counterOffset) ? blockB : blockA;
}

/** Every save on a memory card image (`.raw`, `.gcp`), as `.gci` files. */
export function readCardImage(card: Buffer): GciSave[] {
  if (
    card.length < (SYSTEM_BLOCKS + 1) * GC_BLOCK_SIZE ||
    card.length > MAX_CARD_BYTES ||
    card.length % GC_BLOCK_SIZE
  ) {
    throw new Error("This file is not a GameCube memory card.");
  }
  const totalBlocks = card.length / GC_BLOCK_SIZE;
  const directory = newer(card, 1, 2, 0x1ffa);
  const map = newer(card, 3, 4, 0x4);
  // The block map: entry i (at 0xa) is the block after block i + 5; 0xffff ends a save.
  const next = (block: number) => map.readUInt16BE(0xa + (block - SYSTEM_BLOCKS) * 2);
  const saves: GciSave[] = [];
  for (let i = 0; i < DIRECTORY_ENTRIES; i++) {
    const entryBytes = directory.subarray(i * DENTRY_SIZE, (i + 1) * DENTRY_SIZE);
    if (entryBytes.readUInt32BE(0) === 0xffffffff) {
      continue;
    }
    const entry = readEntry(entryBytes);
    if (entry.blocks === 0 || entry.blocks > totalBlocks - SYSTEM_BLOCKS) {
      continue;
    }
    const blocks: Buffer[] = [];
    const seen = new Set<number>();
    let block = entry.firstBlock;
    while (blocks.length < entry.blocks && block >= SYSTEM_BLOCKS && block < totalBlocks && !seen.has(block)) {
      seen.add(block);
      blocks.push(card.subarray(block * GC_BLOCK_SIZE, (block + 1) * GC_BLOCK_SIZE));
      block = next(block);
    }
    if (blocks.length !== entry.blocks) {
      continue; // a broken chain: not a save Dolphin would load either
    }
    saves.push({
      gameCode: entry.gameCode,
      maker: entry.maker,
      fileName: entry.fileName,
      modified: entry.modified,
      data: Buffer.concat([entryBytes, ...blocks]),
    });
  }
  return saves;
}

const isCardImage = (name: string) => /\.(raw|gcp)$/i.test(name);
const isGci = (name: string) => /\.gci$/i.test(name);

async function listFiles(dir: string): Promise<string[]> {
  const entries = await readdir(dir, { withFileTypes: true }).catch(() => [] as Dirent[]);
  return entries.filter((e) => e.isFile() && (isGci(e.name) || isCardImage(e.name))).map((e) => path.join(dir, e.name));
}

/** The `.gci` files and card images in a folder the player picked: the folder itself, and a
 * Dolphin User folder's `GC` folder with its region folders' GCI folders (`GC/USA/Card A`). */
async function findSaveFiles(folder: string): Promise<string[]> {
  const files = await listFiles(folder);
  for (const gc of [path.join(folder, "GC"), path.join(folder, "User", "GC"), folder]) {
    files.push(...(gc === folder ? [] : await listFiles(gc)));
    const regions = await readdir(gc, { withFileTypes: true }).catch(() => [] as Dirent[]);
    for (const region of regions.filter((e) => e.isDirectory())) {
      const regionDir = path.join(gc, region.name);
      if (/^card [ab]$/i.test(region.name)) {
        files.push(...(await listFiles(regionDir)));
        continue;
      }
      for (const card of ["Card A", "Card B"]) {
        files.push(...(await listFiles(path.join(regionDir, card))));
      }
    }
  }
  return [...new Set(files)].slice(0, MAX_CANDIDATES);
}

async function readSaveFile(file: string): Promise<GciSave[]> {
  const info = await stat(file);
  if (info.size > MAX_CARD_BYTES) {
    throw new Error(`${path.basename(file)} is too big to be a GameCube save or memory card.`);
  }
  const data = await readFile(file);
  return isCardImage(file) ? readCardImage(data) : [readGci(data)];
}

/**
 * The game's saves in `source` (a `.gci`, a card image or a folder), the newest of each save
 * when the folder has several copies. Throws, saying what is there, when there are none.
 */
export async function readGcSaves(source: string, gameId: string): Promise<GciSave[]> {
  const code = gameId.toUpperCase();
  const info = await stat(source);
  const files = info.isDirectory() ? await findSaveFiles(source) : [source];
  const all: GciSave[] = [];
  for (const file of files) {
    try {
      all.push(...(await readSaveFile(file)));
    } catch (err) {
      if (!info.isDirectory()) {
        throw err;
      }
    }
  }
  const newest = new Map<string, GciSave>();
  for (const save of all.filter((s) => saveId(s).toUpperCase() === code)) {
    const key = `${save.maker}\0${save.fileName}`;
    const had = newest.get(key);
    if (!had || save.modified > had.modified) {
      newest.set(key, save);
    }
  }
  if (newest.size > 0) {
    return [...newest.values()];
  }
  if (!info.isDirectory() && all.length > 0) {
    const others = [...new Set(all.map(saveId))].slice(0, 3).join(", ");
    throw new Error(
      isCardImage(source)
        ? `This memory card has no save of ${code} (it has ${others}).`
        : `This save is for another game (${others}), not ${code}.`,
    );
  }
  throw new Error(
    info.isDirectory()
      ? `There's no save of ${code} in this folder. Pick your Dolphin's User folder (the one with a GC ` +
        `folder in it), a GCI folder like GC/USA/Card A, or a .gci file or memory card.`
      : `This memory card has no saves.`,
  );
}

/** What the store holds: its `.gci` files (not the `.deleted` ones Dolphin keeps of replaced
 * saves), their size, the newest change. */
export async function summarizeGcSave(folder: string): Promise<SaveSummary> {
  const summary: SaveSummary = { folder, files: 0, bytes: 0, modified: null };
  let newest = 0;
  for (const file of (await listFiles(folder)).filter(isGci)) {
    const s = await stat(file);
    summary.files += 1;
    summary.bytes += s.size;
    newest = Math.max(newest, s.mtimeMs);
  }
  summary.modified = newest > 0 ? new Date(newest).toISOString() : null;
  return summary;
}

const stamp = (d: Date) => d.toISOString().replace(/[-:]/g, "").replace("T", "-").replace(".", "-").slice(0, 19);

export type ImportGcSaveOptions = {
  /** A `.gci`, a memory card image, or a folder (`readGcSaves`). */
  source: string;
  gameId: string;
  /** The netplay Dolphin's User folder. */
  userFolder: string;
  buildHash: string;
  now?: Date;
};

/**
 * Puts the game's saves in `source` in the build's store, the ones there before (if any) moved to
 * SaveBackups. The new saves are written in full first, so a bad source leaves the store as it was.
 */
export async function importGcSave(
  options: ImportGcSaveOptions,
): Promise<{ save: SaveSummary; backup: string | null }> {
  const { source, gameId, userFolder, buildHash } = options;
  const saves = await readGcSaves(source, gameId);
  const dest = gcSaveDir(userFolder, buildHash);
  const backups = path.join(userFolder, "SaveBackups");
  const staging = path.join(backups, `.import-${randomUUID()}`);
  const staged = path.join(staging, "GC");
  try {
    await mkdir(staged, { recursive: true });
    const names = new Set<string>();
    for (const save of saves) {
      let name = gciFileName(save);
      for (let n = 1; names.has(name.toLowerCase()); n++) {
        name = gciFileName(save).replace(/\.gci$/, `-${n}.gci`);
      }
      names.add(name.toLowerCase());
      await writeFile(path.join(staged, name), save.data);
    }

    let backup: string | null = null;
    const current = await readdir(dest).catch(() => [] as string[]);
    if (current.length > 0) {
      backup = path.join(backups, buildHash.slice(0, 16).toLowerCase(), stamp(options.now ?? new Date()), "GC");
      await mkdir(path.dirname(backup), { recursive: true });
      await rename(dest, backup);
    } else {
      await rm(dest, { recursive: true, force: true });
    }
    await mkdir(path.dirname(dest), { recursive: true });
    try {
      await rename(staged, dest);
    } catch (err) {
      if (backup) {
        await rename(backup, dest).catch(() => undefined);
      }
      throw err;
    }
    if (backup) {
      await pruneBackups(path.dirname(path.dirname(backup)));
    }
    return { save: await summarizeGcSave(dest), backup };
  } finally {
    await rm(staging, { recursive: true, force: true }).catch(() => undefined);
  }
}

/** Keeps the newest `KEEP_BACKUPS` of a build's backups (their names sort by time). */
async function pruneBackups(dir: string): Promise<void> {
  const names = (await readdir(dir)).filter((n) => /^\d{8}-\d{6}/.test(n)).sort();
  for (const name of names.slice(0, Math.max(0, names.length - KEEP_BACKUPS))) {
    await rm(path.join(dir, name), { recursive: true, force: true });
  }
}
