// Our Dolphin build ships inside the launcher package (`<resources>/dolphin`, electron-builder
// extraResources) and updates with it. Like Slippi, which installs its Dolphin download into
// `<userData>/netplay`, the launcher copies the bundled build there on first start and after
// each launcher update, so Dolphin's Sys folder is writable (the Brawl save template goes there)
// and the build outlives the read-only AppImage mount and the NSIS reinstall of an update.
//
// The bundle carries `dolphin.json` ({version, executable, entries, files, size}), written by
// .github/scripts/dolphin-manifest.sh. The installed copy keeps it, so a matching version means
// nothing to do.
//
// SPDX-License-Identifier: GPL-3.0-or-later

import fs from "fs";
import path from "path";

export const BUNDLED_DOLPHIN_DIR = "dolphin";
export const DOLPHIN_MANIFEST_NAME = "dolphin.json";

/** Entries of the install folder that belong to the user or the launcher, never replaced. */
export const PROTECTED_ENTRIES = new Set(["User", "pponline-sd", "pplus", "downloads", DOLPHIN_MANIFEST_NAME]);

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
    PROTECTED_ENTRIES.has(name)
  ) {
    throw new Error(`Bad entry in the bundled Dolphin's manifest: ${name}`);
  }
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
    return { action: "unchanged", manifest };
  }
  log(`Installing Dolphin ${manifest.version} from ${sourceDir} into ${destDir} (was ${installed?.version ?? "none"})`);

  await fs.promises.mkdir(destDir, { recursive: true });
  // The manifest goes first (removed) and last (written): an interrupted install is redone.
  await fs.promises.rm(path.join(destDir, DOLPHIN_MANIFEST_NAME), { force: true });
  const old = new Set([...(installed?.entries ?? []), ...manifest.entries]);
  for (const name of old) {
    if (!PROTECTED_ENTRIES.has(name) && !name.includes("/") && !name.includes("\\") && name !== "." && name !== "..") {
      await fs.promises.rm(path.join(destDir, name), { recursive: true, force: true });
    }
  }

  const total = manifest.size || 1;
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
      done += st.size;
      onProgress?.(Math.min(done, total), total);
    }
  };
  for (const name of manifest.entries) {
    await copy(path.join(sourceDir, name), path.join(destDir, name));
  }
  await fs.promises.writeFile(path.join(destDir, DOLPHIN_MANIFEST_NAME), JSON.stringify(manifest, null, 2) + "\n");
  log(`Dolphin ${manifest.version} installed (${manifest.files} files)`);
  return { action: "installed", manifest };
}
