import { createReadStream, createWriteStream } from "node:fs";
import { readdir, readFile, rename, rm, stat } from "node:fs/promises";
import path from "path";
import { ZipFile } from "yazl";

/** The most of any one file that goes in (its end): logs from builds before Dolphin capped them can be huge. */
export const MAX_ARCHIVED_FILE_BYTES = 50 * 1024 * 1024;

/** The same for a bug report's zip, which is uploaded (the server takes at most 32 MiB in all). */
export const BUG_REPORT_MAX_FILE_BYTES = 8 * 1024 * 1024;

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

/** EmissaryRecomp's crash dumps, written by the Brawl Online core in it into the netplay User folder's Logs. */
const EMISSARY_DUMP = /^recomp-\d+\.dmp$/;
/** How many of EmissaryRecomp's last runs a zip has (its run folders and its dumps). */
export const EMISSARY_RUNS_ARCHIVED = 3;

/** A Dolphin User folder's logs and the config files that say how it was set up, under `zipFolder`. */
export async function dolphinUserFiles(userFolder: string, zipFolder: string): Promise<ArchiveFile[]> {
  // EmissaryRecomp's dumps go in whole with its logs (emissaryFiles), never cut to their end.
  const files = (await folderFiles(path.join(userFolder, "Logs"), `${zipFolder}/Logs`)).filter(
    (f) => !EMISSARY_DUMP.test(path.posix.basename(f.name)),
  );
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

/** `count` newest entries of `entries`, by modification time. */
async function newest(folder: string, names: string[], count: number): Promise<string[]> {
  const dated = await Promise.all(
    names.map(async (name) => ({ name, at: (await stat(path.join(folder, name)).catch(() => null))?.mtimeMs ?? 0 })),
  );
  return dated
    .sort((a, b) => b.at - a.at)
    .slice(0, count)
    .map((d) => d.name);
}

/**
 * EmissaryRecomp's logs under `zipFolder`: its last runs' folders (`<UserData>/Logs/<run>/`:
 * console.log and what a crash left) and the crash dumps of the Brawl Online core in it
 * (`<netplay User>/Logs/recomp-<pid>.dmp`), the newest of each, the dumps only when they fit
 * whole (`maxDumpBytes`): a dump cut to its end can't be read.
 */
export async function emissaryFiles(
  userData: string,
  netplayUserFolder: string,
  zipFolder: string,
  maxDumpBytes: number,
): Promise<ArchiveFile[]> {
  const files: ArchiveFile[] = [];
  const logs = path.join(userData, "Logs");
  const runs = (await readdir(logs, { withFileTypes: true }).catch(() => [])).filter((e) => e.isDirectory());
  for (const run of await newest(
    logs,
    runs.map((e) => e.name),
    EMISSARY_RUNS_ARCHIVED,
  )) {
    files.push(...(await folderFiles(path.join(logs, run), `${zipFolder}/Logs/${run}`)));
  }
  const coreLogs = path.join(netplayUserFolder, "Logs");
  const dumps = (await readdir(coreLogs).catch(() => [] as string[])).filter((n) => EMISSARY_DUMP.test(n));
  for (const name of await newest(coreLogs, dumps, EMISSARY_RUNS_ARCHIVED)) {
    const size = (await stat(path.join(coreLogs, name)).catch(() => null))?.size ?? 0;
    if (size > 0 && size <= maxDumpBytes) {
      files.push({ name: `${zipFolder}/dumps/${name}`, source: { path: path.join(coreLogs, name) } });
    }
  }
  const config = await readFile(path.join(userData, "Config.toml"), "utf-8").catch(() => null);
  if (config !== null) {
    files.push({ name: `${zipFolder}/Config.toml`, source: { text: config } });
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
 * A zip of `files`. A file that is still being written (Dolphin's log during a match) goes in as
 * it was when the archive started, and only its last `maxFileBytes`. Files that are gone by then
 * are left out.
 */
async function logsZip(files: ArchiveFile[], maxFileBytes: number): Promise<ZipFile> {
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
  return zip;
}

/** Writes the zip of `files` (`logsZip`) to `dest`: next to it first, then renamed, so a failed archive leaves nothing behind. */
export async function writeLogsArchive(
  dest: string,
  files: ArchiveFile[],
  maxFileBytes = MAX_ARCHIVED_FILE_BYTES,
): Promise<void> {
  const zip = await logsZip(files, maxFileBytes);
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

/** The zip of `files` (`logsZip`) in memory, for uploading. */
export async function logsArchiveBuffer(files: ArchiveFile[], maxFileBytes: number): Promise<Uint8Array<ArrayBuffer>> {
  const zip = await logsZip(files, maxFileBytes);
  const chunks = await new Promise<Buffer[]>((resolve, reject) => {
    const out: Buffer[] = [];
    zip.on("error", reject);
    zip.outputStream.on("error", reject);
    zip.outputStream.on("data", (chunk: Buffer) => out.push(chunk));
    zip.outputStream.on("end", () => resolve(out));
  });
  const all = new Uint8Array(chunks.reduce((n, c) => n + c.length, 0));
  let offset = 0;
  for (const chunk of chunks) {
    all.set(chunk, offset);
    offset += chunk.length;
  }
  return all;
}
