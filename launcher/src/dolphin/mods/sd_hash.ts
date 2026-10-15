/**
 * An SD card's root as rooms compare cards (docs/design/brawl-mods.md §6.5), from `DolphinTool
 * sdhash`: BLAKE2b-256 of the card's manifest (a hash per 64 KiB chunk; the chunks no file uses
 * count as zero, so leftover deleted data never makes two cards differ). DolphinTool keeps the
 * chunks' hashes in the netplay User folder's cache (`-u`), where a room's Dolphin finds them:
 * a card is read once, not again when it hosts.
 */

import { spawn } from "child_process";
import path from "path";
import type { ByteProgress } from "utils/copy_file";

/** An 8 GB card on a slow USB drive. */
const SD_HASH_TIMEOUT_MS = 30 * 60_000;

export type SdHashFn = (
  toolPath: string,
  sdPath: string,
  userDir: string,
  onProgress?: ByteProgress,
) => Promise<string>;

/** Parses DolphinTool's "progress <done> <total>" lines (stderr). */
export function parseSdHashProgress(line: string): { done: number; total: number } | null {
  const m = /^progress (\d+) (\d+)$/.exec(line.trim());
  return m ? { done: Number(m[1]), total: Number(m[2]) } : null;
}

export const sdCardRoot: SdHashFn = (toolPath, sdPath, userDir, onProgress) =>
  new Promise((resolve, reject) => {
    const child = spawn(toolPath, ["sdhash", "-i", sdPath, "-u", userDir, "-p"], { windowsHide: true });
    let out = "";
    let err = "";
    let pending = "";
    const timer = setTimeout(() => {
      child.kill();
      reject(new Error("Checking the SD card took too long."));
    }, SD_HASH_TIMEOUT_MS);
    child.stdout.on("data", (d: Buffer) => {
      out += d.toString();
    });
    child.stderr.on("data", (d: Buffer) => {
      pending += d.toString();
      const lines = pending.split(/\r?\n/);
      pending = lines.pop() ?? "";
      for (const line of lines) {
        const p = parseSdHashProgress(line);
        if (p) {
          onProgress?.(p.done, p.total);
        } else if (line.trim()) {
          err += `${line.trim()}\n`;
        }
      }
    });
    child.on("error", (e) => {
      clearTimeout(timer);
      reject(e);
    });
    child.on("close", (code) => {
      clearTimeout(timer);
      const root = out.trim().toLowerCase();
      if (code === 0 && /^[0-9a-f]{64}$/.test(root)) {
        resolve(root);
      } else {
        const detail = err.trim() || out.trim() || `exit code ${code}`;
        reject(new Error(`${path.basename(toolPath)}: could not check the SD card: ${detail.slice(0, 200)}`));
      }
    });
  });
