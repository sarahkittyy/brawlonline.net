/**
 * A build's hash (docs/design/brawl-mods.md §4): SHA-256 over the hashes of each part the game
 * reads, so two players with the same hash run the same machine:
 *
 *   "brawl-mods-build-v2\n" + "disc:<sha1>\n" + "launcher:<sha256 or ->\n" + "sd:<sha256 or ->\n"
 *
 * The disc's is the SHA-1 of its contents (`disc_info.ts`: DolphinTool reads every format), so an
 * ISO and an RVZ of the same disc are the same build. The launcher and the SD card are hashed as
 * files; the SD card as a raw image, not by its files: the game reads its sectors (its FAT too),
 * so two images with the same files in another layout put other bytes in RAM. A build is hashed
 * again only when a part's size or modification time changed (`hashKey`).
 */

import { createHash } from "crypto";
import { stat } from "fs/promises";
import type { ByteProgress } from "utils/copy_file";
import { hashFileWithProgress } from "utils/copy_file";

import type { ModBuild, ModBuildParts } from "./types";

export const BUILD_HASH_PREFIX = "brawl-mods-build-v2\n";

type PathsOf = Pick<ModBuild, "discPath" | "launcherPath" | "sdPath">;

/** The SHA-1 of a disc's contents (`disc_info.ts`), reporting the disc's bytes done if it can. */
export type DiscDigest = (discPath: string, onProgress?: ByteProgress) => Promise<string>;

/** "<size>:<mtimeMs>" of each part ("-" for none): when it changes, the build is hashed again. */
export async function buildHashKey(build: PathsOf): Promise<string> {
  const keys: string[] = [];
  for (const p of [build.discPath, build.launcherPath, build.sdPath]) {
    if (!p) {
      keys.push("-");
      continue;
    }
    const s = await stat(p);
    keys.push(`${s.size}:${Math.floor(s.mtimeMs)}`);
  }
  return `v2|${keys.join("|")}`;
}

/** Combines the parts' hashes (hex, or null for a part the build does not have). */
export function combineBuildHash(parts: ModBuildParts): string {
  const text = `${BUILD_HASH_PREFIX}disc:${parts.disc}\nlauncher:${parts.launcher ?? "-"}\nsd:${parts.sd ?? "-"}\n`;
  return createHash("sha256").update(text, "utf8").digest("hex");
}

/**
 * Every part's hash; `onProgress` gets the bytes done of all parts (the disc's all at once when
 * the digest reports none: DolphinTool's).
 */
export async function computeBuildParts(
  build: PathsOf,
  discDigest: DiscDigest,
  onProgress?: ByteProgress,
): Promise<ModBuildParts> {
  const discSize = (await stat(build.discPath)).size;
  const launcherSize = build.launcherPath ? (await stat(build.launcherPath)).size : 0;
  const sdSize = build.sdPath ? (await stat(build.sdPath)).size : 0;
  const total = discSize + launcherSize + sdSize;
  onProgress?.(0, total);
  const disc = await discDigest(build.discPath, (done) => onProgress?.(Math.min(done, discSize), total));
  onProgress?.(discSize, total);
  const launcher = build.launcherPath
    ? await hashFileWithProgress(build.launcherPath, "sha256", (done) => onProgress?.(discSize + done, total))
    : null;
  const sd = build.sdPath
    ? await hashFileWithProgress(build.sdPath, "sha256", (done) => onProgress?.(discSize + launcherSize + done, total))
    : null;
  onProgress?.(total, total);
  return { disc, launcher, sd };
}

/** The first 8 hex digits, as the tab shows a build (the full hash on hover). */
export function shortHash(hash: string): string {
  return hash.slice(0, 8);
}
