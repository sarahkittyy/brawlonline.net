// File copy and hashing with byte progress, for the slow first-run steps (the 2 GB SD card copy,
// the P+ download check, the ISO check), so the Play button can show how far along they are.
//
// SPDX-License-Identifier: GPL-3.0-or-later

import { createHash } from "crypto";
import fs from "fs";

/** Called with the bytes done so far and the total. `done` never decreases and ends at `total`. */
export type ByteProgress = (done: number, total: number) => void;

export const COPY_CHUNK_SIZE = 8 * 1024 * 1024;

export type CopyResult = "cloned" | "copied";

export type CopyOptions = {
  onProgress?: ByteProgress;
  chunkSize?: number;
  /**
   * Flush the copy to disk every this many bytes, and at the end. Without it a large copy runs
   * at page-cache speed and the real disk write happens later in one long, silent `fsync` (6 s
   * for the 2 GB SD card here); with it the progress follows the disk.
   */
  syncEvery?: number;
  /**
   * The copy-on-write clone (`copyFile` with `COPYFILE_FICLONE_FORCE`). Injectable for tests. It
   * fails where the file system cannot clone (NTFS, ext4, and always on Windows, where libuv does
   * not implement it); the chunked copy is used then.
   */
  clone?: (src: string, dst: string) => Promise<void>;
};

const forceClone = (src: string, dst: string) => fs.promises.copyFile(src, dst, fs.constants.COPYFILE_FICLONE_FORCE);

/**
 * Copies `src` to `dst` (replacing it). A copy-on-write clone is tried first: it is instant on
 * ReFS, APFS, Btrfs and XFS. Otherwise the file is copied in chunks, reporting progress after each
 * one. On failure `dst` may be left partly written: callers copy to a temporary name and rename.
 * An exception thrown by `onProgress` stops the copy (and is rethrown).
 */
export async function copyFileWithProgress(src: string, dst: string, options: CopyOptions = {}): Promise<CopyResult> {
  const { onProgress, chunkSize = COPY_CHUNK_SIZE, clone = forceClone } = options;
  const stat = await fs.promises.stat(src);
  try {
    await clone(src, dst);
    onProgress?.(stat.size, stat.size);
    return "cloned";
  } catch {
    // Not supported here: copy the bytes.
  }
  await chunkedCopy(src, dst, stat, chunkSize, onProgress, options.syncEvery ?? 0);
  return "copied";
}

async function chunkedCopy(
  src: string,
  dst: string,
  stat: fs.Stats,
  chunkSize: number,
  onProgress: ByteProgress | undefined,
  syncEvery: number,
): Promise<void> {
  const total = stat.size;
  const input = await fs.promises.open(src, "r");
  let output: fs.promises.FileHandle | null = null;
  // The next chunk is read while the current one is written.
  let pending: Promise<{ bytesRead: number; buffer: Buffer }> | null = null;
  try {
    output = await fs.promises.open(dst, "w", stat.mode & 0o777);
    const buffers = [Buffer.allocUnsafe(chunkSize), Buffer.allocUnsafe(chunkSize)];
    let which = 0;
    let done = 0;
    let unsynced = 0;
    onProgress?.(0, total);
    pending = input.read(buffers[which], 0, chunkSize, 0);
    for (;;) {
      const { bytesRead, buffer }: { bytesRead: number; buffer: Buffer } = await pending;
      pending = null;
      if (bytesRead === 0) {
        break;
      }
      which ^= 1;
      pending = input.read(buffers[which], 0, chunkSize, done + bytesRead);
      let written = 0;
      while (written < bytesRead) {
        const r = await output.write(buffer, written, bytesRead - written, done + written);
        written += r.bytesWritten;
      }
      done += bytesRead;
      unsynced += bytesRead;
      if (syncEvery > 0 && (unsynced >= syncEvery || done >= total)) {
        // Before reporting, so 100 % means the copy is on disk.
        await output.datasync();
        unsynced = 0;
      }
      // A file that grew while it was copied: the total grows with it, so progress stays <= 100 %.
      onProgress?.(done, Math.max(total, done));
    }
    if (syncEvery > 0 && unsynced > 0) {
      await output.datasync();
    }
    if (done < total) {
      // It shrank: what was copied is all there is.
      onProgress?.(done, done);
    }
  } finally {
    // Never close the input while a read is still in flight.
    await pending?.catch(() => undefined);
    await output?.close().catch(() => undefined);
    await input.close().catch(() => undefined);
  }
}

/** Hashes a file (`md5`, `sha256`, …) and reports the bytes read. */
export async function hashFileWithProgress(
  file: string,
  algorithm: string,
  onProgress?: ByteProgress,
  chunkSize = 4 * 1024 * 1024,
): Promise<string> {
  const hash = createHash(algorithm);
  const { size } = await fs.promises.stat(file);
  let done = 0;
  onProgress?.(0, size);
  const input = fs.createReadStream(file, { highWaterMark: chunkSize });
  for await (const chunk of input) {
    hash.update(chunk as Buffer);
    done += (chunk as Buffer).length;
    onProgress?.(done, Math.max(size, done));
  }
  if (done < size) {
    onProgress?.(done, done);
  }
  return hash.digest("hex");
}

/**
 * Wraps a progress callback so it fires at most every `intervalMs`, plus always at 0 and at the
 * end (`done === total`), so a fast step does not flood the IPC channel and a UI never misses
 * the last value.
 */
export function throttleProgress(onProgress: ByteProgress, intervalMs = 100, now = () => Date.now()): ByteProgress {
  let last = -Infinity;
  return (done, total) => {
    const t = now();
    if (done === 0 || done >= total || t - last >= intervalMs) {
      last = t;
      onProgress(done, total);
    }
  };
}
