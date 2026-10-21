// Gets Project+'s files from P+'s own official release at setup, instead of shipping them or
// copying a local P+ install: the launcher never distributes Brawl or P+ data.
//
// What we take from the pinned release zip (nothing else is extracted):
//   user/Wii/sd.raw                             -> <User>/Wii/sd.raw (P+'s virtual SD card)
//   user/Launcher/Project+ Netplay Launcher.dol -> <User>/Launcher/
//   user/Launcher/Project+ Offline Launcher.dol -> <User>/Launcher/
//   Sys/NetplaySave/**                          -> <store>/NetplaySave/ (the Brawl save template that
//                                                  Dolphin's WiiRoot.cpp copies into the netplay NAND;
//                                                  copied into <User>/NetplaySave by ensureNetplaySave)
//
// The zip is checked against the pinned size and sha256 before anything is extracted, and every
// entry's CRC is checked while it is extracted. Files already in the User folder are never
// overwritten: the SD card holds what the game wrote to it. A card on the previous P+ release is
// updated instead (pplus_card_update.ts: only the files P+ changed), from the new release's card,
// which is extracted to the download folder for that and deleted afterwards.
//
// SPDX-License-Identifier: GPL-3.0-or-later

import fs from "fs";
import { async as AsyncStreamZip } from "node-stream-zip";
import path from "path";
import { pipeline } from "stream/promises";
import type { ByteProgress } from "utils/copy_file";
import { hashFileWithProgress } from "utils/copy_file";
import { download as httpDownload } from "utils/download";

import { cardRelease, cardUpdateTo, updateProjectPlusCard } from "./pplus_card_update";

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
 * new DOLs and save template; their SD card is kept). When the release's user/Wii/sd.raw changed,
 * add a card update for it (pplus_card_update.ts), or installed users keep the old game files.
 */
export const PPLUS_RELEASE: PPlusRelease = {
  version: "v3.2.1",
  url: "https://github.com/Project-Plus-Development-Team/Project-Plus-Dolphin/releases/download/v3.2.1/Project%2B.v3.2.1.Netplay.Windows.zip",
  size: 1854400373,
  sha256: "f55fb3307c6dfdbc62a85e76df846093d1a9ca1c5b1c8712a677e8675a0520ec",
};

export const PPLUS_SD_CARD_ENTRY = "user/Wii/sd.raw";
export const PPLUS_LAUNCHER_DOL_ENTRIES = [
  "user/Launcher/Project+ Netplay Launcher.dol",
  "user/Launcher/Project+ Offline Launcher.dol",
];
export const PPLUS_NETPLAY_SAVE_PREFIX = "Sys/NetplaySave/";

/** Name of the save template folder in Dolphin's User (or, older builds, Sys) folder: Dolphin's NETPLAY_SAVE_DIR. */
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
  /** Before a card update writes: the old release's files are kept for its replays (pplus_versions.ts). */
  beforeCardUpdate?: (card: string, paths: string[]) => Promise<unknown>;
};

export type PPlusMarker = { version: string; sha256: string; installedAt: string };

/**
 * `download`: bytes received; `verify`: bytes of the zip hashed; `extract`: bytes written; `update`:
 * an existing SD card brought to the release (bytes copied and written).
 */
export type PPlusProgress = { phase: "download" | "verify" | "extract" | "update"; current: number; total: number };

type Downloader = (options: {
  url: string;
  destinationFile: string;
  overwrite?: boolean;
  resume?: boolean;
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
  } else if (await cardNeedsUpdate(target, release)) {
    missing.push(`Wii/sd.raw (update to Project+ ${release.version})`);
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

/** The User folder's card is on the release before `release`, which changed the card's files. */
async function cardNeedsUpdate(target: PPlusTarget, release: PPlusRelease): Promise<boolean> {
  const update = cardUpdateTo(release.version);
  if (!update) {
    return false;
  }
  const marker = await readMarker(target.storeDir);
  if (marker?.version === release.version) {
    return false; // updated (or left alone as another release's card) when the marker was written
  }
  return (await cardRelease(sdCardPath(target.userFolder), update)) === "from";
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

/** How the release download recovers: see downloadRelease. */
export type PPlusDownloadOptions = {
  /** Attempts in one install while each one receives something (default 3). */
  attempts?: number;
  /** Wait before the next attempt (default 2 s). */
  retryDelayMs?: number;
  /** Bytes at the end of a kept part fetched again, in case the PC crashed while writing them (default 1 MiB). */
  overlapBytes?: number;
};

/**
 * Downloads the release zip to `partPath`. The part is kept when the download fails or the
 * launcher (or the PC) dies: the next attempt continues it, so 1.9 GB never start over. An attempt
 * that broke off after receiving something is tried again at once (a dropped connection); one that
 * received nothing fails (HTTP errors, no network). The sha256 check after it catches a bad part,
 * which is then deleted.
 */
async function downloadRelease(
  release: PPlusRelease,
  partPath: string,
  downloadFile: Downloader,
  onProgress: ((progress: PPlusProgress) => void) | undefined,
  log: (message: string) => void,
  { attempts = 3, retryDelayMs = 2000, overlapBytes = 1024 * 1024 }: PPlusDownloadOptions = {},
): Promise<void> {
  const partSize = () =>
    fs.promises.stat(partPath).then(
      (s) => s.size,
      () => 0,
    );
  for (let attempt = 1; ; attempt++) {
    const have = await partSize();
    if (have >= release.size) {
      if (have > release.size) {
        await fs.promises.rm(partPath, { force: true });
      } else {
        return;
      }
    } else if (have > 0) {
      const from = Math.max(0, have - overlapBytes);
      await fs.promises.truncate(partPath, from);
      log(`Resuming the download of ${release.url} at ${from} of ${release.size} bytes`);
    } else {
      log(`Downloading ${release.url}`);
    }
    const before = await partSize();
    try {
      await downloadFile({
        url: release.url,
        destinationFile: partPath,
        resume: true,
        onProgress: ({ transferredBytes, totalBytes }) =>
          onProgress?.({ phase: "download", current: transferredBytes, total: totalBytes || release.size }),
      });
      return;
    } catch (err) {
      const received = (await partSize()) - before;
      if (attempt >= attempts || received <= 0) {
        throw err;
      }
      log(
        `The download broke off after ${received} bytes (${
          err instanceof Error ? err.message : String(err)
        }); trying again`,
      );
      await new Promise((resolve) => setTimeout(resolve, retryDelayMs));
    }
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
  downloadOptions,
}: {
  target: PPlusTarget;
  release?: PPlusRelease;
  onProgress?: (progress: PPlusProgress) => void;
  log?: (message: string) => void;
  downloadFile?: Downloader;
  downloadOptions?: PPlusDownloadOptions;
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
    await downloadRelease(release, partPath, downloadFile, onProgress, log, downloadOptions);
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

    // Plan: only what is missing in the User folder; the save template always (it is ours). A card
    // on the previous release gets the new release's card next to the zip, to update it from.
    const jobs: { entry: string; dest: string }[] = [];
    const sd = sdCardPath(target.userFolder);
    const cardUpdate = cardUpdateTo(release.version);
    const newCard = path.join(target.downloadDir, `sd-${release.version}.raw`);
    let updateCard = false;
    if (!(await exists(sd))) {
      jobs.push({ entry: PPLUS_SD_CARD_ENTRY, dest: sd });
    } else if (cardUpdate && (await cardRelease(sd, cardUpdate)) === "from") {
      jobs.push({ entry: PPLUS_SD_CARD_ENTRY, dest: newCard });
      updateCard = true;
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

    if (updateCard && cardUpdate) {
      try {
        await updateProjectPlusCard({
          card: sd,
          newCard,
          update: cardUpdate,
          onProgress: (current, total) => onProgress?.({ phase: "update", current, total }),
          log,
          beforeWrite: target.beforeCardUpdate,
        });
      } finally {
        await fs.promises.rm(newCard, { force: true });
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
 * Puts the Brawl save template into Dolphin's User folder (`<User>/NetplaySave`), where WiiRoot.cpp
 * looks for it first, if it is not there. Never into Dolphin's Sys folder: on macOS that is inside
 * the signed app bundle, and an added file breaks its signature ("Dolphin is damaged").
 * Returns false when we have no template to copy yet.
 */
export async function ensureNetplaySave(storeDir: string, userFolder: string): Promise<boolean> {
  const dest = path.join(userFolder, NETPLAY_SAVE_DIR);
  if (await exists(dest)) {
    return true;
  }
  const store = netplaySaveStore(storeDir);
  if (!(await exists(store))) {
    return false;
  }
  await fs.promises.cp(store, dest, { recursive: true, force: false });
  return true;
}
