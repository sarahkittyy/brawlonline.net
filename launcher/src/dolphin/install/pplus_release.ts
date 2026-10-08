// Gets Project+'s files from P+'s own official release at setup, instead of shipping them or
// copying a local P+ install: the launcher never distributes Brawl or P+ data.
//
// What we take from the pinned release zip (nothing else is extracted):
//   user/Wii/sd.raw                             -> <User>/Wii/sd.raw (P+'s virtual SD card)
//   user/Launcher/Project+ Netplay Launcher.dol -> <User>/Launcher/
//   user/Launcher/Project+ Offline Launcher.dol -> <User>/Launcher/
//   Sys/NetplaySave/**                          -> <store>/NetplaySave/ (the Brawl save template that
//                                                  Dolphin's WiiRoot.cpp copies into the netplay NAND;
//                                                  copied into Dolphin's Sys folder by ensureNetplaySave)
//
// The zip is checked against the pinned size and sha256 before anything is extracted, and every
// entry's CRC is checked while it is extracted. Files already in the User folder are never
// overwritten: the SD card holds what the game wrote to it.
//
// SPDX-License-Identifier: GPL-3.0-or-later

import fs from "fs";
import { async as AsyncStreamZip } from "node-stream-zip";
import path from "path";
import { pipeline } from "stream/promises";
import type { ByteProgress } from "utils/copy_file";
import { hashFileWithProgress } from "utils/copy_file";
import { download as httpDownload } from "utils/download";

export type PPlusRelease = {
  /** P+'s release tag. */
  version: string;
  url: string;
  size: number;
  sha256: string;
};

/**
 * The P+ release we take the files from. P+'s own updater reads
 * https://api.github.com/repos/Project-Plus-Development-Team/Project-Plus-Dolphin/releases/latest;
 * we pin one release instead, so every launcher gets the same verified files. The Windows zip is
 * used on every OS: it is a plain zip, and the files we take from it are not OS specific (the
 * Linux and macOS assets wrap the same files in an AppImage/Flatpak/app bundle).
 *
 * To move to a newer P+ release: take the asset's URL, size and sha256 ("digest") from that API,
 * check that the entries below still exist, and bump `version` (installed users then download the
 * new DOLs and save template; their SD card is kept).
 */
export const PPLUS_RELEASE: PPlusRelease = {
  version: "v3.2.0",
  url: "https://github.com/Project-Plus-Development-Team/Project-Plus-Dolphin/releases/download/v3.2.0/Project%2B.v3.2.Netplay.Windows.zip",
  size: 1858812257,
  sha256: "7121a0bc482e2a10bb84c693b23e4eb4d592cb8e51166089ca564e332d27b2d6",
};

export const PPLUS_SD_CARD_ENTRY = "user/Wii/sd.raw";
export const PPLUS_LAUNCHER_DOL_ENTRIES = [
  "user/Launcher/Project+ Netplay Launcher.dol",
  "user/Launcher/Project+ Offline Launcher.dol",
];
export const PPLUS_NETPLAY_SAVE_PREFIX = "Sys/NetplaySave/";

/** Name of the save template folder in Dolphin's Sys folder (Dolphin's NETPLAY_SAVE_DIR). */
export const NETPLAY_SAVE_DIR = "NetplaySave";
const MARKER_NAME = "pplus.json";

export type PPlusTarget = {
  /** The netplay Dolphin's User folder. */
  userFolder: string;
  /**
   * Launcher-owned folder for what does not belong in the User folder: `NetplaySave/` and the
   * `pplus.json` marker (`<userData>/netplay/pplus`).
   */
  storeDir: string;
  /** Where the release zip is downloaded to (deleted after extraction). */
  downloadDir: string;
};

export type PPlusMarker = { version: string; sha256: string; installedAt: string };

/** `download`: bytes received; `verify`: bytes of the zip hashed; `extract`: bytes written. */
export type PPlusProgress = { phase: "download" | "verify" | "extract"; current: number; total: number };

type Downloader = (options: {
  url: string;
  destinationFile: string;
  overwrite?: boolean;
  onProgress?: (progress: { transferredBytes: number; totalBytes: number }) => void;
}) => Promise<void>;

const exists = (p: string) =>
  fs.promises.access(p).then(
    () => true,
    () => false,
  );

export function launcherDolPath(userFolder: string, entry: string): string {
  return path.join(userFolder, "Launcher", path.posix.basename(entry));
}

export function sdCardPath(userFolder: string): string {
  return path.join(userFolder, "Wii", "sd.raw");
}

export function netplaySaveStore(storeDir: string): string {
  return path.join(storeDir, NETPLAY_SAVE_DIR);
}

export async function readMarker(storeDir: string): Promise<PPlusMarker | null> {
  try {
    return JSON.parse(await fs.promises.readFile(path.join(storeDir, MARKER_NAME), "utf8"));
  } catch {
    return null;
  }
}

/** The P+ files that are missing for `release` (empty when nothing has to be downloaded). */
export async function missingProjectPlusFiles(target: PPlusTarget, release = PPLUS_RELEASE): Promise<string[]> {
  const missing: string[] = [];
  if (!(await exists(sdCardPath(target.userFolder)))) {
    missing.push("Wii/sd.raw");
  }
  for (const entry of PPLUS_LAUNCHER_DOL_ENTRIES) {
    if (!(await exists(launcherDolPath(target.userFolder, entry)))) {
      missing.push(`Launcher/${path.posix.basename(entry)}`);
    }
  }
  const marker = await readMarker(target.storeDir);
  if (!marker || marker.version !== release.version || !(await exists(netplaySaveStore(target.storeDir)))) {
    missing.push(NETPLAY_SAVE_DIR);
  }
  return missing;
}

export async function sha256File(file: string, onProgress?: ByteProgress): Promise<string> {
  return hashFileWithProgress(file, "sha256", onProgress);
}

/**
 * Checks a downloaded release zip against the pinned size and sha256; throws if it differs.
 * Hashing the 1.9 GB zip takes several seconds, so it reports progress.
 */
export async function verifyReleaseZip(file: string, release: PPlusRelease, onProgress?: ByteProgress): Promise<void> {
  const { size } = await fs.promises.stat(file);
  if (size !== release.size) {
    throw new Error(`The Project+ ${release.version} download has ${size} bytes, expected ${release.size}.`);
  }
  const sha = await sha256File(file, onProgress);
  if (sha !== release.sha256) {
    throw new Error(`The Project+ ${release.version} download is corrupt (sha256 ${sha}, expected ${release.sha256}).`);
  }
}

/**
 * Downloads the pinned P+ release (unless everything is in place already), verifies it and
 * extracts the files we use. Returns "present" when nothing had to be done.
 */
export async function installProjectPlusFiles({
  target,
  release = PPLUS_RELEASE,
  onProgress,
  log = () => undefined,
  downloadFile = httpDownload,
}: {
  target: PPlusTarget;
  release?: PPlusRelease;
  onProgress?: (progress: PPlusProgress) => void;
  log?: (message: string) => void;
  downloadFile?: Downloader;
}): Promise<"present" | "installed"> {
  const missing = await missingProjectPlusFiles(target, release);
  if (missing.length === 0) {
    return "present";
  }
  log(`Project+ files missing (${missing.join(", ")}); getting them from Project+ ${release.version}`);

  await fs.promises.mkdir(target.downloadDir, { recursive: true });
  const zipPath = path.join(target.downloadDir, `pplus-${release.version}.zip`);
  const partPath = `${zipPath}.part`;

  const verifyProgress: ByteProgress = (current, total) => onProgress?.({ phase: "verify", current, total });

  // A zip left by an interrupted extraction is reused if it still verifies.
  let haveZip = false;
  if (await exists(zipPath)) {
    try {
      await verifyReleaseZip(zipPath, release, verifyProgress);
      haveZip = true;
      log(`Reusing the verified download ${zipPath}`);
    } catch (err) {
      log(`Discarding ${zipPath}: ${err instanceof Error ? err.message : String(err)}`);
      await fs.promises.rm(zipPath, { force: true });
    }
  }
  if (!haveZip) {
    log(`Downloading ${release.url}`);
    await fs.promises.rm(partPath, { force: true });
    await downloadFile({
      url: release.url,
      destinationFile: partPath,
      overwrite: true,
      onProgress: ({ transferredBytes, totalBytes }) =>
        onProgress?.({ phase: "download", current: transferredBytes, total: totalBytes || release.size }),
    });
    try {
      await verifyReleaseZip(partPath, release, verifyProgress);
    } catch (err) {
      await fs.promises.rm(partPath, { force: true });
      throw err;
    }
    await fs.promises.rename(partPath, zipPath);
    log(`Verified ${zipPath} (sha256 ${release.sha256})`);
  }

  await extractProjectPlusFiles(zipPath, target, release, onProgress, log);
  await fs.promises.rm(zipPath, { force: true });
  return "installed";
}

/** Extracts the files we use from a verified release zip (see the top of this file). */
export async function extractProjectPlusFiles(
  zipPath: string,
  target: PPlusTarget,
  release: PPlusRelease,
  onProgress?: (progress: PPlusProgress) => void,
  log: (message: string) => void = () => undefined,
): Promise<void> {
  const zip = new AsyncStreamZip({ file: zipPath, storeEntries: true });
  try {
    const entries = await zip.entries();
    const need = [PPLUS_SD_CARD_ENTRY, ...PPLUS_LAUNCHER_DOL_ENTRIES];
    for (const name of need) {
      if (!entries[name] || entries[name].isDirectory) {
        throw new Error(`The Project+ ${release.version} release has no ${name}.`);
      }
    }
    const saveEntries = Object.values(entries).filter(
      (e) => !e.isDirectory && e.name.startsWith(PPLUS_NETPLAY_SAVE_PREFIX),
    );
    if (saveEntries.length === 0) {
      throw new Error(`The Project+ ${release.version} release has no ${PPLUS_NETPLAY_SAVE_PREFIX}.`);
    }

    // Plan: only what is missing in the User folder; the save template always (it is ours).
    const jobs: { entry: string; dest: string }[] = [];
    const sd = sdCardPath(target.userFolder);
    if (!(await exists(sd))) {
      jobs.push({ entry: PPLUS_SD_CARD_ENTRY, dest: sd });
    }
    for (const entry of PPLUS_LAUNCHER_DOL_ENTRIES) {
      const dest = launcherDolPath(target.userFolder, entry);
      if (!(await exists(dest))) {
        jobs.push({ entry, dest });
      }
    }
    const storeNew = path.join(target.storeDir, `${NETPLAY_SAVE_DIR}.new`);
    await fs.promises.rm(storeNew, { recursive: true, force: true });
    for (const e of saveEntries) {
      const rel = e.name.slice(PPLUS_NETPLAY_SAVE_PREFIX.length);
      const parts = rel.split("/");
      if (rel === "" || parts.some((p) => p === "" || p === "." || p === "..") || path.isAbsolute(rel)) {
        throw new Error(`Unexpected entry in the Project+ release: ${e.name}`);
      }
      jobs.push({ entry: e.name, dest: path.join(storeNew, ...parts) });
    }

    const total = jobs.reduce((sum, j) => sum + entries[j.entry].size, 0);
    let done = 0;
    onProgress?.({ phase: "extract", current: 0, total });
    for (const job of jobs) {
      await fs.promises.mkdir(path.dirname(job.dest), { recursive: true });
      const partial = `${job.dest}.partial`;
      const input = await zip.stream(job.entry);
      input.on("data", (chunk: Buffer) => {
        done += chunk.length;
        onProgress?.({ phase: "extract", current: done, total });
      });
      try {
        await pipeline(input, fs.createWriteStream(partial));
        const { size } = await fs.promises.stat(partial);
        if (size !== entries[job.entry].size) {
          throw new Error(`${job.entry}: extracted ${size} bytes, expected ${entries[job.entry].size}`);
        }
      } catch (err) {
        await fs.promises.rm(partial, { force: true });
        throw err;
      }
      await fs.promises.rename(partial, job.dest);
      if (!job.entry.startsWith(PPLUS_NETPLAY_SAVE_PREFIX)) {
        log(`Extracted ${job.entry} -> ${job.dest}`);
      }
    }

    const store = netplaySaveStore(target.storeDir);
    await fs.promises.rm(store, { recursive: true, force: true });
    await fs.promises.rename(storeNew, store);
    log(`Extracted ${saveEntries.length} files of ${PPLUS_NETPLAY_SAVE_PREFIX} -> ${store}`);

    const marker: PPlusMarker = {
      version: release.version,
      sha256: release.sha256,
      installedAt: new Date().toISOString(),
    };
    await fs.promises.writeFile(path.join(target.storeDir, MARKER_NAME), JSON.stringify(marker, null, 2) + "\n");
  } finally {
    await zip.close();
  }
}

/**
 * Puts the Brawl save template into Dolphin's Sys folder (`<Sys>/NetplaySave`), where WiiRoot.cpp
 * looks for it, if it is not there. Returns false when we have no template to copy yet.
 */
export async function ensureNetplaySave(storeDir: string, sysFolder: string): Promise<boolean> {
  const dest = path.join(sysFolder, NETPLAY_SAVE_DIR);
  if (await exists(dest)) {
    return true;
  }
  const store = netplaySaveStore(storeDir);
  if (!(await exists(store)) || !(await exists(sysFolder))) {
    return false;
  }
  await fs.promises.cp(store, dest, { recursive: true, force: false });
  return true;
}
