/**
 * A Wii game's save in Other Games rooms (docs/design/brawl-mods.md §6.5): the game plays from its
 * build's save store, `<netplay User>/Saves/<first 16 hex digits of the build hash>/`, laid out
 * like a NAND root (the game's files in `title/<hi>/<lo>/data/`). Dolphin's own NAND is never
 * read, so a player brings a save of their own in here (bug report #55: an NSMB Wii save imported
 * into Dolphin's NAND never reached the room).
 *
 * A save comes from either
 * - a `data.bin`: a save exported by Dolphin (Export Wii Save) or a Wii's Data Management, the
 *   files encrypted with the Wii's SD key (Dolphin's `Core/HW/WiiSave.cpp`, ported), or
 * - a folder: a Dolphin User folder, its `Wii` folder (a NAND root), or the save's own folder.
 *
 * The store's files are plain host files. One the store's `fst.bin` does not list gets owner 0,
 * group 0 and read-write for everyone (Dolphin's HostFileSystem), as every file a session wrote
 * back has (a non-empty file cannot be given another owner), so copied files play the same. File
 * names keep Dolphin's host escaping (`Common::EscapeFileName`).
 *
 * The save it replaces goes to `<netplay User>/SaveBackups/<16 hex>/<time>/data`; the newest
 * `KEEP_BACKUPS` are kept.
 */

import { createDecipheriv, createHash, randomUUID } from "crypto";
import type { Dirent } from "fs";
import { copyFile, mkdir, readdir, readFile, rename, rm, stat, writeFile } from "fs/promises";
import path from "path";

/** The Wii's SD key and the IV a save's header starts with (Dolphin's IOSC.cpp, WiiSave.cpp). */
const SD_KEY = Buffer.from("ab01b9d8e1622b08afbad84dbfc2a55d", "hex");
const SD_INITIAL_IV = Buffer.from("216712e6aa1f689f95c5a22324dc6a98", "hex");
const MD5_BLANKER = Buffer.from("0e65378199be4517ab06ec22451a5793", "hex");

const HEADER_SIZE = 0xf0c0;
const BK_HEADER_SIZE = 0x80;
const FILE_HEADER_SIZE = 0x80;
const BLOCK_SIZE = 0x40;
const BANNER_SIZE = 0x60a0;
const ICON_SIZE = 0x1200;
const FULL_BANNER_MIN = 0x72a0;
const FULL_BANNER_MAX = 0xf0a0;
const BK_LISTED_SIZE = 0x70;
const BK_MAGIC = 0x426b0001;
const FILE_MAGIC = 0x03adf17e;
const FULL_CERT_SIZE = 0x3c0;

/** A save is a few files of a few MB: more than this is not a save folder. */
const MAX_FILES = 1024;
const MAX_BYTES = 128 * 1024 * 1024;
export const KEEP_BACKUPS = 10;

/** One entry of a save, its path inside the title's `data` folder ("/" between folders); a
 * folder has no data. */
export type SaveEntry = { path: string; data: Buffer | null };

export type SaveSummary = {
  /** The save's folder in the store. */
  folder: string;
  files: number;
  bytes: number;
  /** The newest file's modification time (ISO), or null for no save. */
  modified: string | null;
};

export function saveStoreDir(userFolder: string, buildHash: string): string {
  return path.join(userFolder, "Saves", buildHash.slice(0, 16).toLowerCase());
}

/**
 * A build hashed again to another hash (a newer launcher's build hash) keeps its save: the store
 * of its old hash becomes its new hash's, unless the new one has a store already. Returns whether
 * it moved.
 */
export async function moveSaveStore(userFolder: string, fromHash: string, toHash: string): Promise<boolean> {
  const from = saveStoreDir(userFolder, fromHash);
  const to = saveStoreDir(userFolder, toHash);
  const present = (dir: string) =>
    stat(dir).then(
      () => true,
      () => false,
    );
  if (from === to || !(await present(from)) || (await present(to))) {
    return false;
  }
  await rename(from, to);
  return true;
}

function titleParts(titleId: string): [string, string] {
  if (!/^[0-9a-f]{16}$/i.test(titleId)) {
    throw new Error(`Not a Wii title ID: ${titleId}`);
  }
  const t = titleId.toLowerCase();
  return [t.slice(0, 8), t.slice(8)];
}

/** DolphinTool's `title_id` (a JSON number; Wii title IDs are below 2^53). */
export function titleIdHex(id: number): string {
  return BigInt(id).toString(16).padStart(16, "0");
}

/** The title's game ID part ("SMNE" of 00010000534d4e45), else its hex. */
export function titleGameId(titleId: string): string {
  const lo = Buffer.from(titleParts(titleId)[1], "hex");
  return lo.every((c) => c >= 0x30 && c <= 0x5a) ? lo.toString("latin1") : titleId;
}

export function titleDataDir(storeDir: string, titleId: string): string {
  const [hi, lo] = titleParts(titleId);
  return path.join(storeDir, "title", hi, lo, "data");
}

/** Dolphin's host name for a NAND file name (`Common::EscapeFileName`, NandPaths.cpp). */
export function escapeNandName(name: string): string {
  if (/^\.+$/.test(name)) {
    return name.replace(/\./g, "__2e__");
  }
  let out = "";
  for (const c of name.replace(/__/g, "__5f____5f__")) {
    const code = c.charCodeAt(0);
    out += code <= 0x1f || '"*/:<>?\\|\x7f'.includes(c) ? `__${code.toString(16).padStart(2, "0")}__` : c;
  }
  return out;
}

function decrypt(data: Buffer, iv: Buffer): Buffer {
  const decipher = createDecipheriv("aes-128-cbc", SD_KEY, iv).setAutoPadding(false);
  return Buffer.concat([decipher.update(data), decipher.final()]);
}

const alignUp = (n: number, to: number) => Math.ceil(n / to) * to;

/** A save file's name inside the save: folders split by "/", none empty or "." / "..". */
function checkSavePath(name: string): string {
  const parts = name.split("/");
  if (name.length === 0 || parts.some((p) => p === "" || p === "." || p === "..")) {
    throw new Error(`This data.bin has a file with a bad name: "${name}".`);
  }
  return name;
}

/**
 * Reads a `data.bin` (Dolphin's DataBinStorage): its title and every file and folder of the save,
 * `banner.bin` (the Wii Menu's banner, from the header) first.
 */
export function readDataBin(bin: Buffer): { titleId: string; entries: SaveEntry[] } {
  const notASave = new Error("This file is not a Wii save (data.bin).");
  if (bin.length < HEADER_SIZE + BK_HEADER_SIZE) {
    throw notASave;
  }
  const header = decrypt(bin.subarray(0, HEADER_SIZE), SD_INITIAL_IV);
  const bannerSize = header.readUInt32BE(8);
  if (bannerSize < FULL_BANNER_MIN || bannerSize > FULL_BANNER_MAX || (bannerSize - BANNER_SIZE) % ICON_SIZE !== 0) {
    throw notASave;
  }
  // The header's MD5 is of the header with the blanker in its place.
  const md5 = Buffer.from(header.subarray(14, 30));
  MD5_BLANKER.copy(header, 14);
  if (!createHash("md5").update(header).digest().equals(md5)) {
    throw new Error("This data.bin is damaged (its header does not check out).");
  }
  const titleId = header.readBigUInt64BE(0).toString(16).padStart(16, "0");

  const bk = bin.subarray(HEADER_SIZE, HEADER_SIZE + BK_HEADER_SIZE);
  const fileCount = bk.readUInt32BE(12);
  if (
    bk.readUInt32BE(0) !== BK_LISTED_SIZE ||
    bk.readUInt32BE(4) !== BK_MAGIC ||
    bk.readUInt32BE(16) + FULL_CERT_SIZE !== bk.readUInt32BE(28) ||
    fileCount > MAX_FILES
  ) {
    throw notASave;
  }

  const entries: SaveEntry[] = [{ path: "banner.bin", data: Buffer.from(header.subarray(32, 32 + bannerSize)) }];
  let pos = HEADER_SIZE + BK_HEADER_SIZE;
  let bytes = 0;
  for (let i = 0; i < fileCount; i++) {
    const fh = bin.subarray(pos, pos + FILE_HEADER_SIZE);
    if (fh.length < FILE_HEADER_SIZE || fh.readUInt32BE(0) !== FILE_MAGIC) {
      throw new Error("This data.bin is cut short or damaged.");
    }
    pos += FILE_HEADER_SIZE;
    const size = fh.readUInt32BE(4);
    const type = fh[10];
    const rawName = fh.subarray(11, 11 + 0x40);
    const end = rawName.indexOf(0);
    const name = checkSavePath(rawName.subarray(0, end < 0 ? rawName.length : end).toString("latin1"));
    if (type === 2) {
      entries.push({ path: name, data: null });
    } else if (type === 1) {
      const rounded = alignUp(size, BLOCK_SIZE);
      bytes += size;
      if (pos + rounded > bin.length || bytes > MAX_BYTES) {
        throw new Error("This data.bin is cut short or damaged.");
      }
      const iv = Buffer.from(fh.subarray(80, 96));
      entries.push({ path: name, data: decrypt(bin.subarray(pos, pos + rounded), iv).subarray(0, size) });
      pos += rounded;
    } else {
      throw notASave;
    }
  }
  return { titleId, entries };
}

async function isDirectory(p: string): Promise<boolean> {
  return stat(p).then(
    (s) => s.isDirectory(),
    () => false,
  );
}

/**
 * The title's save folder in what the player picked: a Dolphin User folder, its `Wii` folder, a
 * NAND's `title/<hi>/<lo>` folder or the `data` folder in it. Throws, saying what to pick, when
 * there is none (or it is another game's).
 */
export async function findNandSave(folder: string, titleId: string): Promise<string> {
  const [hi, lo] = titleParts(titleId);
  const candidates: string[] = [];
  const base = path.basename(folder).toLowerCase();
  const parent = path.basename(path.dirname(folder)).toLowerCase();
  if (base === "data") {
    if (/^[0-9a-f]{8}$/.test(parent) && parent !== lo) {
      throw new Error(
        `This folder is the save of another game (${titleGameId(hi + parent)}), not ${titleGameId(titleId)}.`,
      );
    }
    candidates.push(folder);
  }
  if (base === lo) {
    candidates.push(path.join(folder, "data"));
  }
  for (const root of [folder, path.join(folder, "Wii"), path.join(folder, "User", "Wii")]) {
    candidates.push(path.join(root, "title", hi, lo, "data"));
  }
  for (const c of candidates) {
    if (await isDirectory(c)) {
      return c;
    }
  }
  throw new Error(
    `There's no save of ${titleGameId(titleId)} in this folder. Pick your Dolphin's User folder (the one ` +
      `with a Wii folder in it), or the save's own folder: Wii/title/${hi}/${lo}/data.`,
  );
}

/**
 * The title's save in the Dolphin a build's files sit in: a mod's Dolphin folder holds its
 * launcher (`<root>/User/Launcher/`, `<root>/Launcher/`) and SD card (`<root>/User/Wii/sd.raw`)
 * next to the NAND its own Dolphin plays (bug report #144: Cobalt's save, played in its own
 * Dolphin, never reached the room). Null when none of the folders up to three above a file has one.
 */
export async function findDolphinSaveNear(
  files: (string | null | undefined)[],
  titleId: string,
): Promise<string | null> {
  for (const file of files) {
    if (!file) {
      continue;
    }
    let dir = path.dirname(file);
    for (let up = 0; up < 3; up++) {
      const found = await findNandSave(dir, titleId).catch(() => null);
      if (found && (await summarizeSave(found)).files > 0) {
        return found;
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

/** Copies a save folder's files and folders (no links), refusing what is too big to be a save. */
async function copySaveTree(from: string, to: string, count = { files: 0, bytes: 0 }): Promise<typeof count> {
  await mkdir(to, { recursive: true });
  const entries: Dirent[] = await readdir(from, { withFileTypes: true });
  for (const e of entries) {
    const src = path.join(from, e.name);
    const dst = path.join(to, e.name);
    if (e.isDirectory()) {
      await copySaveTree(src, dst, count);
    } else if (e.isFile()) {
      count.files += 1;
      count.bytes += (await stat(src)).size;
      if (count.files > MAX_FILES || count.bytes > MAX_BYTES) {
        throw new Error("This folder is too big to be a game's save. Pick the save's own folder.");
      }
      await copyFile(src, dst);
    }
  }
  return count;
}

async function writeEntries(entries: SaveEntry[], to: string): Promise<number> {
  await mkdir(to, { recursive: true });
  let files = 0;
  for (const entry of entries) {
    const dst = path.join(to, ...entry.path.split("/").map(escapeNandName));
    if (entry.data === null) {
      await mkdir(dst, { recursive: true });
    } else {
      await mkdir(path.dirname(dst), { recursive: true });
      await writeFile(dst, entry.data);
      files += 1;
    }
  }
  return files;
}

/** What the store holds for the title: its files, their size, the newest change. */
export async function summarizeSave(folder: string): Promise<SaveSummary> {
  const summary: SaveSummary = { folder, files: 0, bytes: 0, modified: null };
  let newest = 0;
  const walk = async (dir: string): Promise<void> => {
    const entries = await readdir(dir, { withFileTypes: true }).catch(() => [] as Dirent[]);
    for (const e of entries) {
      const p = path.join(dir, e.name);
      if (e.isDirectory()) {
        await walk(p);
      } else if (e.isFile()) {
        const s = await stat(p);
        summary.files += 1;
        summary.bytes += s.size;
        newest = Math.max(newest, s.mtimeMs);
      }
    }
  };
  await walk(folder);
  summary.modified = newest > 0 ? new Date(newest).toISOString() : null;
  return summary;
}

/** "20261018-223011-123": sorts by time, and two imports in one second still differ. */
const stamp = (d: Date) => d.toISOString().replace(/[-:]/g, "").replace("T", "-").replace(".", "-").slice(0, 19);

export type ImportSaveOptions = {
  /** A `data.bin`, or a folder with the save (`findNandSave`). */
  source: string;
  titleId: string;
  /** The netplay Dolphin's User folder. */
  userFolder: string;
  buildHash: string;
  now?: Date;
};

export type ImportSaveResult = { save: SaveSummary; backup: string | null };

/**
 * Puts the save in `source` in the build's store, the one there before (if any) moved to
 * SaveBackups. The new save is written in full first, so a bad source leaves the store as it was.
 */
export async function importSave(options: ImportSaveOptions): Promise<ImportSaveResult> {
  const { source, titleId, userFolder, buildHash } = options;
  const dest = titleDataDir(saveStoreDir(userFolder, buildHash), titleId);
  const backups = path.join(userFolder, "SaveBackups");
  const staging = path.join(backups, `.import-${randomUUID()}`);
  const staged = path.join(staging, "data");
  try {
    let files: number;
    const info = await stat(source);
    if (info.isFile()) {
      if (info.size > MAX_BYTES + HEADER_SIZE) {
        throw new Error("This file is not a Wii save (data.bin).");
      }
      const save = readDataBin(await readFile(source));
      if (save.titleId !== titleId.toLowerCase()) {
        throw new Error(`This save is for another game (${titleGameId(save.titleId)}), not ${titleGameId(titleId)}.`);
      }
      files = await writeEntries(save.entries, staged);
    } else {
      const folder = await findNandSave(source, titleId);
      files = (await copySaveTree(folder, staged)).files;
    }
    if (files === 0) {
      throw new Error("This save has no files in it.");
    }

    let backup: string | null = null;
    const current = await summarizeSave(dest);
    if (current.files > 0) {
      backup = path.join(backups, buildHash.slice(0, 16).toLowerCase(), stamp(options.now ?? new Date()), "data");
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
    return { save: await summarizeSave(dest), backup };
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
