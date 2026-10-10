import { createReadStream, createWriteStream } from "node:fs";
import { readdir, readFile, rename, rm, stat } from "node:fs/promises";
import path from "path";
import { ZipFile } from "yazl";

/** The most of any one file that goes in (its end): logs from builds before Dolphin capped them can be huge. */
export const MAX_ARCHIVED_FILE_BYTES = 50 * 1024 * 1024;

/** Dolphin config files worth sending. Not RetroAchievements.ini (an API token) or Online/user.json (the play key). */
const DOLPHIN_CONFIG_FILES = ["Dolphin.ini", "GFX.ini", "Logger.ini", "GCPadNew.ini"];

export type ArchiveFile = {
  /** Path inside the zip, with forward slashes. */
  name: string;
  /** A file on disk, or the text to store. */
  source: { path: string } | { text: string };
};

/** The files of `folder` (not its subfolders) under `zipFolder`. A missing folder adds nothing. */
export async function folderFiles(folder: string, zipFolder: string): Promise<ArchiveFile[]> {
  const entries = await readdir(folder, { withFileTypes: true }).catch(() => []);
  return entries
    .filter((e) => e.isFile())
    .map((e) => ({ name: `${zipFolder}/${e.name}`, source: { path: path.join(folder, e.name) } }));
}

/** A Dolphin User folder's logs and the config files that say how it was set up, under `zipFolder`. */
export async function dolphinUserFiles(userFolder: string, zipFolder: string): Promise<ArchiveFile[]> {
  const files = await folderFiles(path.join(userFolder, "Logs"), `${zipFolder}/Logs`);
  for (const name of DOLPHIN_CONFIG_FILES) {
    const file = path.join(userFolder, "Config", name);
    const text = await readFile(file, "utf-8").catch(() => null);
    if (text !== null) {
      files.push({
        name: `${zipFolder}/Config/${name}`,
        source: { text: name === "Dolphin.ini" ? redactDolphinIni(text) : text },
      });
    }
  }
  return files;
}

/** Dolphin.ini without the analytics ID (`[Analytics] ID`), which identifies the player's machine. */
export function redactDolphinIni(text: string): string {
  let section = "";
  return text
    .split(/(\r?\n)/)
    .map((line) => {
      const header = line.match(/^\s*\[([^\]]*)\]/);
      if (header) {
        section = header[1];
      } else if (section === "Analytics" && /^\s*ID\s*=/.test(line)) {
        return "ID = (removed)";
      }
      return line;
    })
    .join("");
}

/**
 * Writes `files` into a zip at `dest`. A file that is still being written (Dolphin's log during a
 * match) goes in as it was when the archive started, and only its last `maxFileBytes`.
 * Files that are gone by then are left out. The zip is written next to `dest` and renamed, so a
 * failed archive leaves nothing behind.
 */
export async function writeLogsArchive(
  dest: string,
  files: ArchiveFile[],
  maxFileBytes = MAX_ARCHIVED_FILE_BYTES,
): Promise<void> {
  const zip = new ZipFile();
  for (const file of files) {
    if ("text" in file.source) {
      zip.addBuffer(Buffer.from(file.source.text, "utf-8"), file.name);
      continue;
    }
    const stats = await stat(file.source.path).catch(() => null);
    if (!stats?.isFile()) {
      continue;
    }
    if (stats.size === 0) {
      zip.addBuffer(Buffer.alloc(0), file.name, { mtime: stats.mtime });
      continue;
    }
    const start = Math.max(0, stats.size - maxFileBytes);
    const size = stats.size - start;
    const filePath = file.source.path;
    zip.addReadStreamLazy(file.name, { size, mtime: stats.mtime }, (cb) =>
      cb(null, createReadStream(filePath, { start, end: stats.size - 1 })),
    );
  }
  zip.end();

  const partial = `${dest}.partial`;
  try {
    await new Promise<void>((resolve, reject) => {
      const out = createWriteStream(partial);
      // Settle once the file is closed, so a failed one can be removed (Windows keeps open files).
      let failure: unknown = null;
      const fail = (err: unknown) => {
        failure ??= err;
        out.destroy();
      };
      zip.on("error", fail);
      out.on("error", fail);
      out.on("close", () => (failure ? reject(failure) : resolve()));
      zip.outputStream.pipe(out);
    });
    await rename(partial, dest);
  } catch (err) {
    await rm(partial, { force: true });
    throw err;
  }
}
