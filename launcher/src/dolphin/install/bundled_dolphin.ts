// Our Dolphin build ships inside the launcher package (`<resources>/dolphin`, electron-builder
// extraResources) and updates with it. Like Slippi, which installs its Dolphin download into
// `<userData>/netplay`, the launcher copies the bundled build there on first start and after
// each launcher update, so Dolphin's Sys folder is writable (the Brawl save template goes there)
// and the build outlives the read-only AppImage mount and the NSIS reinstall of an update.
//
// The bundle carries `dolphin.json` ({version, executable, entries, files, size}), written by
// .github/scripts/dolphin-manifest.sh. The installed copy keeps it, so a matching version means
// nothing to do, as long as every file of the bundle is still there with its size: an install
// that lost a file afterwards (prod 2026-10-13: Sys/Rollback/gp-v22.json gone, so every session
// that player started failed) is installed again.
//
// SPDX-License-Identifier: GPL-3.0-or-later

import fs from "fs";
import path from "path";

export const BUNDLED_DOLPHIN_DIR = "dolphin";
export const DOLPHIN_MANIFEST_NAME = "dolphin.json";
/** What one file counts for in the install progress, on top of its size. */
export const PROGRESS_BYTES_PER_FILE = 128 * 1024;

/** Entries of the install folder that belong to the user or the launcher, never replaced. */
export const PROTECTED_ENTRIES = new Set([
  "User",
  "pponline-sd",
  "pplus",
  // Older Project+ releases' files and cards for replays (pplus_versions.ts).
  "pplus-versions",
  "downloads",
  DOLPHIN_MANIFEST_NAME,
]);
/** Where an install moves the old build before deleting it (see moveAside). */
export const OLD_DOLPHIN_DIR = ".old-dolphin";

export type DolphinBundleManifest = {
  version: string;
  /** The Dolphin executable, relative to the bundle (e.g. `Dolphin.exe`, `usr/bin/project-plus-dolphin`). */
  executable: string;
  /** Top-level entries of the bundle. */
  entries: string[];
  files: number;
  size: number;
};

export async function readDolphinManifest(dir: string): Promise<DolphinBundleManifest | null> {
  try {
    const m = JSON.parse(await fs.promises.readFile(path.join(dir, DOLPHIN_MANIFEST_NAME), "utf8"));
    if (typeof m.version !== "string" || typeof m.executable !== "string" || !Array.isArray(m.entries)) {
      return null;
    }
    return m as DolphinBundleManifest;
  } catch {
    return null;
  }
}

/**
 * The bundled Dolphin to install: `PPO_BUNDLED_DOLPHIN` if set, else `<resources>/dolphin` in a
 * packaged launcher, else none (development uses the build next to the repository).
 */
export function bundledDolphinSource(ctx: {
  env: Record<string, string | undefined>;
  isPackaged: boolean;
  resourcesPath: string;
}): string | null {
  const fromEnv = ctx.env.PPO_BUNDLED_DOLPHIN;
  if (fromEnv && fromEnv.trim() !== "") {
    return fromEnv.trim();
  }
  return ctx.isPackaged ? path.join(ctx.resourcesPath, BUNDLED_DOLPHIN_DIR) : null;
}

function checkEntryName(name: string) {
  if (
    !name ||
    name.includes("/") ||
    name.includes("\\") ||
    name === "." ||
    name === ".." ||
    name === OLD_DOLPHIN_DIR ||
    PROTECTED_ENTRIES.has(name)
  ) {
    throw new Error(`Bad entry in the bundled Dolphin's manifest: ${name}`);
  }
}

/**
 * The first file of the bundle (relative path) that `destDir` lacks or holds with another size,
 * or null when every one is in place. Symbolic links only need to exist.
 */
export async function firstMissingFile(sourceDir: string, destDir: string, entries: string[]): Promise<string | null> {
  const check = async (rel: string): Promise<string | null> => {
    const st = await fs.promises.lstat(path.join(sourceDir, rel));
    const got = await fs.promises.lstat(path.join(destDir, rel)).catch(() => null);
    if (!got) {
      return rel;
    }
    if (st.isDirectory()) {
      if (!got.isDirectory()) {
        return rel;
      }
      for (const child of await fs.promises.readdir(path.join(sourceDir, rel))) {
        const missing = await check(path.join(rel, child));
        if (missing) {
          return missing;
        }
      }
    } else if (!st.isSymbolicLink() && (!got.isFile() || got.size !== st.size)) {
      return rel;
    }
    return null;
  };
  for (const name of entries) {
    const missing = await check(name);
    if (missing) {
      return missing;
    }
  }
  return null;
}

/**
 * Takes an old entry of the build out of the way of the new one. On Windows, a file that another
 * program has open (an antivirus scan, an overlay or recorder) is only marked for deletion: its name
 * stays taken until that program lets go, so copying the new file fails and so does every later
 * install, with EPERM even on lstat (prod 2026-10-13: "Dolphin stops working after every update"
 * until the player restarted the PC). A running executable cannot be deleted at all. Both can be
 * renamed, so the entry moves into the old-build folder first and is deleted from there.
 */
async function moveAside(from: string, to: string, log: (message: string) => void): Promise<void> {
  try {
    await fs.promises.mkdir(path.dirname(to), { recursive: true });
    await fs.promises.rename(from, to);
    return;
  } catch (err) {
    if ((err as NodeJS.ErrnoException).code === "ENOENT") {
      return;
    }
    log(`Could not move ${from} aside (${(err as Error).message}): deleting it in place`);
  }
  try {
    // A DolphinTool run that is ending gets about 10 s (linear backoff) before the install gives up.
    await fs.promises.rm(from, { recursive: true, force: true, maxRetries: 10, retryDelay: 200 });
  } catch (err) {
    const code = (err as NodeJS.ErrnoException).code;
    if (code === "EPERM" || code === "EBUSY" || code === "EACCES") {
      throw new Error(
        `Could not replace ${from}: another program still has the old Dolphin open (Dolphin itself, ` +
          `an antivirus scan, or an overlay or recorder). Close it or restart your computer, then ` +
          `start the launcher again. (${(err as Error).message})`,
      );
    }
    throw err;
  }
}

/** Deletes the old builds moved aside; what another program still holds goes on a later start. */
async function removeOldBuilds(destDir: string, log: (message: string) => void): Promise<void> {
  await fs.promises.rm(path.join(destDir, OLD_DOLPHIN_DIR), { recursive: true, force: true }).catch((err) => {
    log(`Some of the old Dolphin is still in use and stays until a later start: ${(err as Error).message}`);
  });
}

/**
 * Installs (or updates) the bundled Dolphin from `sourceDir` into `destDir`. Entries listed by
 * the previously installed manifest and by the new one are replaced; the User folder, the
 * patched SD card and other launcher data in `destDir` are left alone.
 */
export async function installBundledDolphin({
  sourceDir,
  destDir,
  onProgress,
  log = () => undefined,
}: {
  sourceDir: string;
  destDir: string;
  onProgress?: (current: number, total: number) => void;
  log?: (message: string) => void;
}): Promise<{ action: "unchanged" | "installed"; manifest: DolphinBundleManifest }> {
  const manifest = await readDolphinManifest(sourceDir);
  if (!manifest) {
    throw new Error(
      `The bundled Dolphin (${sourceDir}) has no valid ${DOLPHIN_MANIFEST_NAME}. Reinstall the launcher.`,
    );
  }
  manifest.entries.forEach(checkEntryName);
  await removeOldBuilds(destDir, log);
  const installed = await readDolphinManifest(destDir);
  if (
    installed &&
    installed.version === manifest.version &&
    installed.size === manifest.size &&
    (await fs.promises.access(path.join(destDir, manifest.executable)).then(
      () => true,
      () => false,
    ))
  ) {
    const missing = await firstMissingFile(sourceDir, destDir, manifest.entries);
    if (!missing) {
      return { action: "unchanged", manifest };
    }
    log(`The installed Dolphin ${manifest.version} lacks ${missing} (or has another size): installing it again`);
  }
  log(`Installing Dolphin ${manifest.version} from ${sourceDir} into ${destDir} (was ${installed?.version ?? "none"})`);

  await fs.promises.mkdir(destDir, { recursive: true });
  // The manifest goes first (removed) and last (written): an interrupted install is redone.
  await fs.promises.rm(path.join(destDir, DOLPHIN_MANIFEST_NAME), { force: true });
  const old = new Set([...(installed?.entries ?? []), ...manifest.entries]);
  const trash = path.join(destDir, OLD_DOLPHIN_DIR, String(Date.now()));
  for (const name of old) {
    if (
      !PROTECTED_ENTRIES.has(name) &&
      name !== OLD_DOLPHIN_DIR &&
      !name.includes("/") &&
      !name.includes("\\") &&
      name !== "." &&
      name !== ".."
    ) {
      await moveAside(path.join(destDir, name), path.join(trash, name), log);
    }
  }
  await removeOldBuilds(destDir, log);

  // Progress counts each file as its bytes plus a fixed cost: most of the bundle's ~1000 files are
  // small, and creating them takes longer than copying their bytes (by bytes alone the last 7 %
  // took most of the time).
  const total = Math.max(1, (manifest.size || 0) + (manifest.files || 0) * PROGRESS_BYTES_PER_FILE);
  let done = 0;
  onProgress?.(0, total);
  const copy = async (from: string, to: string) => {
    const st = await fs.promises.lstat(from);
    if (st.isDirectory()) {
      await fs.promises.mkdir(to, { recursive: true });
      for (const child of await fs.promises.readdir(from)) {
        await copy(path.join(from, child), path.join(to, child));
      }
    } else if (st.isSymbolicLink()) {
      await fs.promises.symlink(await fs.promises.readlink(from), to);
    } else {
      await fs.promises.copyFile(from, to);
      await fs.promises.chmod(to, st.mode & 0o777);
      done += st.size + PROGRESS_BYTES_PER_FILE;
      onProgress?.(Math.min(done, total), total);
    }
  };
  for (const name of manifest.entries) {
    await copy(path.join(sourceDir, name), path.join(destDir, name));
  }
  if (done < total) {
    onProgress?.(total, total);
  }
  await fs.promises.writeFile(path.join(destDir, DOLPHIN_MANIFEST_NAME), JSON.stringify(manifest, null, 2) + "\n");
  log(`Dolphin ${manifest.version} installed (${manifest.files} files)`);
  return { action: "installed", manifest };
}
