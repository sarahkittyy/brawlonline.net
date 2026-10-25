// Deletes a file or folder tree, trying again for a while when something holds a file in it.
//
// SPDX-License-Identifier: GPL-3.0-or-later

import fs from "fs";

/** The errors a file another program holds gives on Windows (and a folder still emptying). */
const RETRY_CODES = new Set(["EBUSY", "EPERM", "EACCES", "ENOTEMPTY", "EMFILE", "ENFILE"]);

/**
 * `fs.promises.rm(p, { recursive: true, force: true })`, tried again up to `tries` times in all
 * with a linear backoff (`delayMs`, 2 * `delayMs`, ...) while it fails with an error a held file
 * gives. The last error is thrown.
 *
 * Never Node's own `maxRetries`: its rimraf retries in every folder of the tree, and a folder's
 * retry deletes its children again with their own retries, so the wait for one held file is
 * multiplied by `maxRetries + 1` per folder above it. With 10 retries of 200 ms a held file four
 * folders down took hours (bug report #148: Play said "The game files are being moved" for good
 * after a move, while the old folder was being deleted).
 */
export async function removeTree(
  p: string,
  {
    tries = 10,
    delayMs = 200,
    rm = fs.promises.rm,
  }: {
    tries?: number;
    delayMs?: number;
    /** Test hook. */
    rm?: (p: string, options: fs.RmOptions) => Promise<void>;
  } = {},
): Promise<void> {
  for (let attempt = 1; ; attempt++) {
    try {
      await rm(p, { recursive: true, force: true });
      return;
    } catch (err) {
      if (attempt >= tries || !RETRY_CODES.has((err as NodeJS.ErrnoException).code ?? "")) {
        throw err;
      }
      await new Promise((resolve) => setTimeout(resolve, attempt * delayMs));
    }
  }
}
