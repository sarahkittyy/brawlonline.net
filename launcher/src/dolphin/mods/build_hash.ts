/**
 * A Brawl Mods build's hash (docs/design/brawl-mods.md §4): SHA-256 over the SHA-256 of each part
 * the game reads, so two players with the same hash run the same machine:
 *
 *   "brawl-mods-build-v1\n" + "disc:<sha256>\n" + "launcher:<sha256 or ->\n" + "sd:<sha256 or ->\n"
 *
 * The disc and the SD card are hashed as raw images, not by their files: the game reads their
 * sectors (the SD card's FAT included), so two images with the same files in another layout put
 * other bytes in RAM and the machines would diverge. Hashing a disc takes a while (8.5 GB): a
 * build is hashed again only when a part's size or modification time changed (`hashKey`).
 */

import { createHash } from "crypto";
import { stat } from "fs/promises";
import type { ByteProgress } from "utils/copy_file";
import { hashFileWithProgress } from "utils/copy_file";

import type { ModBuild } from "./types";

export const BUILD_HASH_PREFIX = "brawl-mods-build-v1\n";

type Part = { label: "disc" | "launcher" | "sd"; path: string | null };

function partsOf(build: Pick<ModBuild, "discPath" | "launcherPath" | "sdPath">): Part[] {
  return [
    { label: "disc", path: build.discPath },
    { label: "launcher", path: build.launcherPath },
    { label: "sd", path: build.sdPath },
  ];
}

/** "<size>:<mtimeMs>" of each part ("-" for none): when it changes, the build is hashed again. */
export async function buildHashKey(build: Pick<ModBuild, "discPath" | "launcherPath" | "sdPath">): Promise<string> {
  const keys: string[] = [];
  for (const part of partsOf(build)) {
    if (!part.path) {
      keys.push("-");
      continue;
    }
    const s = await stat(part.path);
    keys.push(`${s.size}:${Math.floor(s.mtimeMs)}`);
  }
  return keys.join("|");
}

/** Combines the parts' hashes (hex, or null for a part the build does not have). */
export function combineBuildHash(parts: { disc: string; launcher: string | null; sd: string | null }): string {
  const text = `${BUILD_HASH_PREFIX}disc:${parts.disc}\nlauncher:${parts.launcher ?? "-"}\nsd:${parts.sd ?? "-"}\n`;
  return createHash("sha256").update(text, "utf8").digest("hex");
}

/** The build's hash, reading every part; `onProgress` gets the bytes done of all parts. */
export async function computeBuildHash(
  build: Pick<ModBuild, "discPath" | "launcherPath" | "sdPath">,
  onProgress?: ByteProgress,
): Promise<string> {
  const parts = partsOf(build);
  const sizes: number[] = [];
  for (const part of parts) {
    sizes.push(part.path ? (await stat(part.path)).size : 0);
  }
  const total = sizes.reduce((a, b) => a + b, 0);
  let before = 0;
  const hashes: Record<Part["label"], string | null> = { disc: null, launcher: null, sd: null };
  for (let i = 0; i < parts.length; ++i) {
    const part = parts[i];
    if (part.path) {
      const offset = before;
      hashes[part.label] = await hashFileWithProgress(part.path, "sha256", (done) =>
        onProgress?.(offset + done, total),
      );
    }
    before += sizes[i];
  }
  onProgress?.(total, total);
  if (!hashes.disc) {
    throw new Error("A build needs a disc image.");
  }
  return combineBuildHash({ disc: hashes.disc, launcher: hashes.launcher, sd: hashes.sd });
}

/** The first 8 hex digits, as the tab shows a build (the full hash on hover). */
export function shortHash(hash: string): string {
  return hash.slice(0, 8);
}
