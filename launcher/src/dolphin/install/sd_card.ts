// Installs our game plugin (PPOnline.rel) into a launcher-managed copy of the user's P+ SD card,
// the way `tools/sdcard/patch_sd.py SRC DST --plugin PPOnline.rel` does. The user's own card and
// the template it was seeded from are only ever read.
//
// SPDX-License-Identifier: GPL-3.0-or-later

import { FatImage } from "@brawl_assets/fat32";
import { Fat32Volume } from "@brawl_assets/fat32_writer";
import { createHash } from "crypto";
import fs from "fs";
import path from "path";

import type { PluginLocation } from "./paths";
import { PLUGIN_SD_PATH } from "./paths";

export const PATCHED_SD_IMAGE_NAME = "sd.raw";
export const PATCHED_SD_MANIFEST_NAME = "manifest.json";

export type PluginBinary = {
  data: Buffer;
  sha256: string;
  /** Where it was read from. */
  source: string;
};

export type PluginManifest = { file?: string; size?: number; sha256: string; version?: string };

export type PatchedSdManifest = {
  version: 1;
  source: { path: string; size: number; mtimeMs: number };
  plugin: { sha256: string; size: number; sdPath: string; from: string };
  updatedAt: string;
};

/**
 * - `copied`: the image was (re)created from the source card and patched.
 * - `replaced`: the plugin changed and was replaced inside the existing copy.
 * - `repaired`: the plugin in the copy did not match the manifest and was rewritten.
 * - `unchanged`: nothing was written.
 */
export type SdInstallAction = "copied" | "replaced" | "repaired" | "unchanged";

export type SdInstallResult = {
  image: string;
  manifest: string;
  action: SdInstallAction;
};

export type SdInstallOptions = {
  /** The user's P+ card (`<netplay User>/Wii/sd.raw`). Read only. */
  sourceImage: string;
  /** Folder of the managed copy (`patchedSdCardFolder(userData)`). */
  outputDir: string;
  plugin: PluginBinary;
  /** Path of the plugin on the card. */
  sdPath?: string;
  /** Files or folders that must never be written (the source card, the User template). */
  protectedPaths?: string[];
  log?: (message: string) => void;
};

export class SdCardError extends Error {}

export function sha256(data: Buffer): string {
  return createHash("sha256").update(data).digest("hex");
}

/**
 * Reads the plugin binary. When the location has a manifest (packaged builds), the binary must
 * match its sha256 (and size, if given).
 */
export async function loadPlugin(location: PluginLocation): Promise<PluginBinary> {
  let data: Buffer;
  try {
    data = await fs.promises.readFile(location.binary);
  } catch (err) {
    throw new SdCardError(
      `The online game plugin was not found at ${location.binary}` +
        (location.manifest ? ". Reinstall the launcher." : ". Build game-code/PPOnline or set PPO_PLUGIN_PATH."),
    );
  }
  const digest = sha256(data);
  if (location.manifest) {
    let manifest: PluginManifest;
    try {
      manifest = JSON.parse(await fs.promises.readFile(location.manifest, "utf8"));
    } catch (err) {
      throw new SdCardError(`The online game plugin's manifest (${location.manifest}) is missing or unreadable.`);
    }
    if (
      typeof manifest.sha256 !== "string" ||
      manifest.sha256.toLowerCase() !== digest ||
      (manifest.size !== undefined && manifest.size !== data.length)
    ) {
      throw new SdCardError(
        `The online game plugin (${location.binary}) does not match its manifest. Reinstall the launcher.`,
      );
    }
  }
  return { data, sha256: digest, source: location.binary };
}

function samePathOrInside(target: string, protectedPath: string): boolean {
  const norm = (p: string) => {
    const r = path.resolve(p);
    return process.platform === "win32" || process.platform === "darwin" ? r.toLowerCase() : r;
  };
  const t = norm(target);
  const p = norm(protectedPath);
  return t === p || t.startsWith(p.endsWith(path.sep) ? p : p + path.sep);
}

async function sameFile(a: string, b: string): Promise<boolean> {
  try {
    const [sa, sb] = await Promise.all([fs.promises.stat(a, { bigint: true }), fs.promises.stat(b, { bigint: true })]);
    return sa.ino !== BigInt(0) && sa.ino === sb.ino && sa.dev === sb.dev;
  } catch {
    return false;
  }
}

/** Throws if `target` is (or is inside) a protected path, or is the same file as one. */
export async function assertNotProtected(target: string, protectedPaths: string[]): Promise<void> {
  for (const p of protectedPaths) {
    if (samePathOrInside(target, p) || (await sameFile(target, p))) {
      throw new SdCardError(`Refusing to modify ${target}: it is the user's original SD card or template (${p}).`);
    }
  }
}

/** Copies a file, cloning it (copy-on-write) where the file system supports it. */
export async function fastCopy(src: string, dst: string): Promise<void> {
  try {
    await fs.promises.copyFile(src, dst, fs.constants.COPYFILE_FICLONE);
  } catch (err) {
    if ((err as NodeJS.ErrnoException).code === "ENOSPC") {
      throw err;
    }
    await fs.promises.copyFile(src, dst);
  }
}

/** sha256 of a file on the card, or null if it is missing or the card cannot be read. */
export async function readFileHashFromImage(image: string, sdPath: string): Promise<string | null> {
  let fat: FatImage | null = null;
  try {
    fat = await FatImage.open(image);
    const entry = await fat.stat(sdPath);
    if (!entry || entry.isDirectory) {
      return null;
    }
    return sha256(await fat.readFile(sdPath));
  } catch {
    return null;
  } finally {
    await fat?.close().catch(() => undefined);
  }
}

/**
 * Writes `data` to `sdPath` inside `image`, then verifies: the file reads back identical through
 * the independent reader (`FatImage`), and the FAT check finds no new problem.
 */
export async function patchImage(
  image: string,
  data: Buffer,
  sdPath: string,
  opts: { protectedPaths?: string[]; removePaths?: string[]; log?: (m: string) => void } = {},
): Promise<void> {
  await assertNotProtected(image, opts.protectedPaths ?? []);
  const vol = await Fat32Volume.open(image, { writable: true });
  let before: string[];
  let after: string[];
  try {
    before = await vol.check();
    if (before.length > 0) {
      opts.log?.(`SD card ${image} already had FAT problems before patching: ${before.slice(0, 5).join("; ")}`);
    }
    for (const p of opts.removePaths ?? []) {
      await vol.delete(p);
    }
    await vol.writeFile(sdPath, data);
    await vol.flush();
    after = await vol.check();
  } finally {
    await vol.close();
  }
  const known = new Set(before);
  const added = after.filter((p) => !known.has(p));
  if (added.length > 0) {
    throw new SdCardError(`FAT check failed after writing ${sdPath}: ${added.slice(0, 5).join("; ")}`);
  }
  const readBack = await readFileHashFromImage(image, sdPath);
  if (readBack !== sha256(data)) {
    throw new SdCardError(`${sdPath} does not read back identical from ${image}`);
  }
}

async function readManifest(file: string): Promise<PatchedSdManifest | null> {
  try {
    const m = JSON.parse(await fs.promises.readFile(file, "utf8"));
    if (m && m.version === 1 && m.source && m.plugin && typeof m.plugin.sha256 === "string") {
      return m as PatchedSdManifest;
    }
  } catch {
    // missing or unreadable: start over
  }
  return null;
}

async function writeJsonAtomic(file: string, value: unknown): Promise<void> {
  const tmp = `${file}.tmp`;
  await fs.promises.writeFile(tmp, JSON.stringify(value, null, 2) + "\n");
  await fs.promises.rename(tmp, file);
}

async function exists(p: string): Promise<boolean> {
  try {
    await fs.promises.access(p);
    return true;
  } catch {
    return false;
  }
}

/**
 * Makes sure `<outputDir>/sd.raw` is the user's card with the current plugin installed.
 * Idempotent: when nothing changed only the plugin is read back (~20 KB) and nothing is written.
 *
 * - copy missing, manifest missing, or source changed (path, size or mtime) → fresh copy + patch;
 * - plugin changed → the plugin is replaced inside the existing copy (keeps what the game wrote);
 * - otherwise the plugin is read back and re-written only if its sha256 does not match.
 * If patching the existing copy fails, the copy is recreated from the source once.
 * The manifest is written last, after the image verified.
 */
export async function installPluginOnSdCard(opts: SdInstallOptions): Promise<SdInstallResult> {
  const sdPath = opts.sdPath ?? PLUGIN_SD_PATH;
  const log = opts.log ?? (() => undefined);
  const image = path.join(opts.outputDir, PATCHED_SD_IMAGE_NAME);
  const manifestFile = path.join(opts.outputDir, PATCHED_SD_MANIFEST_NAME);
  const protectedPaths = [opts.sourceImage, ...(opts.protectedPaths ?? [])];
  const plugin = opts.plugin;

  let srcStat: fs.Stats;
  try {
    srcStat = await fs.promises.stat(opts.sourceImage);
  } catch {
    throw new SdCardError(`The Project+ SD card was not found at ${opts.sourceImage}.`);
  }
  await assertNotProtected(image, protectedPaths);
  await assertNotProtected(manifestFile, protectedPaths);
  await fs.promises.mkdir(opts.outputDir, { recursive: true });

  const manifest = await readManifest(manifestFile);
  const imageExists = await exists(image);
  const source = { path: path.resolve(opts.sourceImage), size: srcStat.size, mtimeMs: srcStat.mtimeMs };
  const sourceChanged =
    !manifest ||
    manifest.source.path !== source.path ||
    manifest.source.size !== source.size ||
    manifest.source.mtimeMs !== source.mtimeMs;

  const finish = async (action: SdInstallAction): Promise<SdInstallResult> => {
    if (action !== "unchanged" || !manifest) {
      const next: PatchedSdManifest = {
        version: 1,
        source,
        plugin: { sha256: plugin.sha256, size: plugin.data.length, sdPath, from: plugin.source },
        updatedAt: new Date().toISOString(),
      };
      await writeJsonAtomic(manifestFile, next);
    }
    return { image, manifest: manifestFile, action };
  };

  const freshCopy = async (): Promise<SdInstallResult> => {
    // Drop the manifest first: an interrupted copy or patch must never look up to date.
    await fs.promises.rm(manifestFile, { force: true });
    await fs.promises.rm(image, { force: true });
    const partial = `${image}.partial`;
    await fs.promises.rm(partial, { force: true });
    const started = Date.now();
    log(`Copying the SD card ${opts.sourceImage} to ${image}`);
    try {
      await fastCopy(opts.sourceImage, partial);
      await patchImage(partial, plugin.data, sdPath, { protectedPaths, log });
    } catch (err) {
      await fs.promises.rm(partial, { force: true }).catch(() => undefined);
      throw err;
    }
    await fs.promises.rename(partial, image);
    log(`SD card copied and patched in ${((Date.now() - started) / 1000).toFixed(1)} s`);
    return finish("copied");
  };

  if (!imageExists || sourceChanged) {
    return freshCopy();
  }

  const patchInPlace = async (action: "replaced" | "repaired"): Promise<SdInstallResult> => {
    await fs.promises.rm(manifestFile, { force: true });
    try {
      const removePaths = manifest && manifest.plugin.sdPath !== sdPath ? [manifest.plugin.sdPath] : [];
      await patchImage(image, plugin.data, sdPath, { protectedPaths, removePaths, log });
    } catch (err) {
      log(`Patching ${image} in place failed (${err instanceof Error ? err.message : String(err)}); recreating it`);
      return freshCopy();
    }
    log(`SD card plugin ${action}: ${sdPath} (${plugin.sha256.slice(0, 12)})`);
    return finish(action);
  };

  if (manifest!.plugin.sha256 !== plugin.sha256 || manifest!.plugin.sdPath !== sdPath) {
    return patchInPlace("replaced");
  }
  const onCard = await readFileHashFromImage(image, sdPath);
  if (onCard !== plugin.sha256) {
    log(`The plugin on ${image} does not match (${onCard ?? "missing"}); rewriting it`);
    return patchInPlace("repaired");
  }
  return finish("unchanged");
}
