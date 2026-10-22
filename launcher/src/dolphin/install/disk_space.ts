// Free-space checks before the set-up's big writes (the bundled Dolphin, Project+'s 1.9 GB
// download and 2.1 GB of files from it, the patched SD card copy) and readable errors when a disk
// fills up anyway (bug report #91). Messages: @common/disk_space.
//
// SPDX-License-Identifier: GPL-3.0-or-later

import { diskSpaceMessage } from "@common/disk_space";
import fs from "fs";
import path from "path";

/** Kept free on top of what a step writes: Windows and other programs need room too. */
export const DISK_SPACE_MARGIN = 200 * 1024 * 1024;

/** Free bytes for this user on the disk of `dir`, or null when they cannot be read. */
export type FreeBytes = (dir: string) => Promise<number | null>;

export class DiskSpaceError extends Error {
  constructor(message: string) {
    super(message);
    this.name = "DiskSpaceError";
  }
}

/** Free bytes on the disk of `dir` (its nearest existing folder), or null when unknown. */
export const freeDiskBytes: FreeBytes = async (dir) => {
  let at = path.resolve(dir);
  for (;;) {
    try {
      const st = await fs.promises.statfs(at);
      return Number(st.bavail) * Number(st.bsize);
    } catch (err) {
      const parent = path.dirname(at);
      if ((err as NodeJS.ErrnoException).code !== "ENOENT" || parent === at) {
        return null;
      }
      at = parent;
    }
  }
};

/** How the message names the disk of `dir`: its drive ("C:") on Windows, else the folder's disk. */
export function diskPlace(dir: string, platform: NodeJS.Platform = process.platform): string {
  if (platform === "win32") {
    const root = path.win32.parse(path.win32.resolve(dir)).root.replace(/[\\/]+$/, "");
    if (/^[A-Za-z]:$/.test(root)) {
      return root.toUpperCase();
    }
  }
  return `the disk of ${dir}`;
}

/** Whether `err` is the disk running full (ENOSPC; Windows' disk-full errors map to it). */
export function isDiskFull(err: unknown): boolean {
  if (err instanceof DiskSpaceError) {
    return true;
  }
  const e = err as NodeJS.ErrnoException | null;
  return e?.code === "ENOSPC" || (e instanceof Error && /\bENOSPC\b/.test(e.message));
}

/**
 * Throws a DiskSpaceError when the disk of `dir` has less than `needBytes` (plus the margin) free.
 * Nothing is checked when the free space cannot be read: the write then fails or not by itself.
 */
export async function ensureFreeSpace(
  dir: string,
  needBytes: number,
  what: string,
  freeBytes: FreeBytes = freeDiskBytes,
): Promise<void> {
  if (needBytes <= 0) {
    return;
  }
  const free = await freeBytes(dir);
  const need = needBytes + DISK_SPACE_MARGIN;
  if (free != null && free < need) {
    throw new DiskSpaceError(diskSpaceMessage({ what, place: diskPlace(dir), needBytes: need, freeBytes: free }));
  }
}

/**
 * `err` as the player should read it: a disk-full error (from a write under `dir`, or the error's
 * own path) becomes a DiskSpaceError naming the drive and its free space, `needBytes` (plus the
 * margin) when known. Other errors are returned as they are.
 */
export async function asDiskSpaceError(
  err: unknown,
  dir: string,
  what: string,
  { needBytes = null, freeBytes = freeDiskBytes }: { needBytes?: number | null; freeBytes?: FreeBytes } = {},
): Promise<unknown> {
  if (err instanceof DiskSpaceError || !isDiskFull(err)) {
    return err;
  }
  const at = (err as NodeJS.ErrnoException).path ?? dir;
  const free = await freeBytes(path.dirname(at));
  return new DiskSpaceError(
    diskSpaceMessage({
      what,
      place: diskPlace(at),
      needBytes: needBytes == null ? null : needBytes + DISK_SPACE_MARGIN,
      freeBytes: free,
    }),
  );
}
