// Moves a freshly written folder into place, waiting out (or copying past) a program that holds a
// file in it.
//
// SPDX-License-Identifier: GPL-3.0-or-later

import fs from "fs";

import { removeTree } from "./remove_tree";

/** What Windows answers a rename with while another program has a file in the folder open. */
const HELD_CODES = new Set(["EPERM", "EACCES", "EBUSY"]);

export type MoveTreeOptions = {
  /** Renames in all, with a linear backoff (`delayMs`, 2 * `delayMs`, ...): ~22 s by default. */
  tries?: number;
  delayMs?: number;
  /** When the folder still can't be renamed: copy it to `to`, then delete `from` (what it can). */
  copyFallback?: boolean;
  log?: (message: string) => void;
  /** Test hooks. */
  rename?: (from: string, to: string) => Promise<void>;
  cp?: (from: string, to: string, options: fs.CopyOptions) => Promise<void>;
};

/**
 * `fs.promises.rename(from, to)` for a folder just written. On Windows a folder can't be renamed
 * while another program has a file in it open, and an antivirus scans the files of a fresh unpack
 * (a new .exe and its DLLs) right after they are written: the rename failed with "EPERM: operation
 * not permitted" and the download was thrown away (board card, EmissaryRecomp v0.7.5). Tried again
 * for a while; with `copyFallback`, the files are then copied, which a scan doesn't stop.
 */
export async function moveTree(from: string, to: string, opts: MoveTreeOptions = {}): Promise<void> {
  const { tries = 15, delayMs = 200, copyFallback = false, log = () => undefined } = opts;
  const rename = opts.rename ?? fs.promises.rename;
  const cp = opts.cp ?? fs.promises.cp;
  let lastErr: unknown;
  for (let attempt = 1; attempt <= tries; attempt++) {
    try {
      await rename(from, to);
      if (attempt > 1) {
        log(`Moved ${from} to ${to} on try ${attempt}`);
      }
      return;
    } catch (err) {
      lastErr = err;
      if (!HELD_CODES.has((err as NodeJS.ErrnoException).code ?? "")) {
        throw err;
      }
      if (attempt < tries) {
        await new Promise((resolve) => setTimeout(resolve, attempt * delayMs));
      }
    }
  }
  if (!copyFallback) {
    throw lastErr;
  }
  log(`Could not move ${from} to ${to} (${(lastErr as Error).message}): copying it instead`);
  try {
    await cp(from, to, { recursive: true, errorOnExist: true, force: false });
  } catch (err) {
    await removeTree(to).catch(() => undefined);
    throw err;
  }
  await removeTree(from).catch((err) => log(`Could not delete ${from} after copying it: ${(err as Error).message}`));
}
