import fs from "fs";
import os from "os";
import path from "path";
import { afterAll, beforeAll, beforeEach, describe, expect, it } from "vitest";

import type { DolphinBundleManifest } from "./bundled_dolphin";
import { bundledDolphinSource, installBundledDolphin, readDolphinManifest } from "./bundled_dolphin";

let root: string;
let n = 0;
let dest: string;

beforeAll(async () => {
  root = await fs.promises.mkdtemp(path.join(os.tmpdir(), "bundled-dolphin-"));
});

afterAll(async () => {
  await fs.promises.rm(root, { recursive: true, force: true });
});

beforeEach(() => {
  n += 1;
  dest = path.join(root, `user-${n}`, "netplay");
});

/** Writes a bundle like .github/scripts/dolphin-manifest.sh makes. */
async function makeBundle(version: string, files: Record<string, string>): Promise<string> {
  const dir = path.join(root, `bundle-${n}-${version}`);
  let size = 0;
  for (const [rel, content] of Object.entries(files)) {
    const p = path.join(dir, ...rel.split("/"));
    await fs.promises.mkdir(path.dirname(p), { recursive: true });
    await fs.promises.writeFile(p, content);
    size += Buffer.byteLength(content);
  }
  const entries = [...new Set(Object.keys(files).map((f) => f.split("/")[0]))].sort();
  const manifest: DolphinBundleManifest = {
    version,
    executable: "Dolphin.exe",
    entries,
    files: Object.keys(files).length,
    size,
  };
  await fs.promises.writeFile(path.join(dir, "dolphin.json"), JSON.stringify(manifest));
  return dir;
}

const read = (...p: string[]) => fs.promises.readFile(path.join(dest, ...p), "utf8");

describe("installBundledDolphin", () => {
  it("installs the bundle and its manifest, then does nothing for the same version", async () => {
    const src = await makeBundle("0.1.1", { "Dolphin.exe": "exe-1", "Sys/GameSettings/RSBE01.ini": "ini-1" });
    const progress: [number, number][] = [];
    const first = await installBundledDolphin({
      sourceDir: src,
      destDir: dest,
      onProgress: (c, t) => progress.push([c, t]),
    });
    expect(first.action).toBe("installed");
    expect(await read("Dolphin.exe")).toBe("exe-1");
    expect(await read("Sys", "GameSettings", "RSBE01.ini")).toBe("ini-1");
    expect((await readDolphinManifest(dest))?.version).toBe("0.1.1");
    expect(progress[progress.length - 1]).toEqual([10, 10]);

    progress.length = 0;
    const second = await installBundledDolphin({
      sourceDir: src,
      destDir: dest,
      onProgress: (c, t) => progress.push([c, t]),
    });
    expect(second.action).toBe("unchanged");
    expect(progress).toEqual([]);
  });

  it("updates to a new version, removing old files but keeping the user's data", async () => {
    const v1 = await makeBundle("0.1.1", { "Dolphin.exe": "exe-1", "Sys/old.txt": "old", "Old.dll": "dll" });
    await installBundledDolphin({ sourceDir: v1, destDir: dest });
    // What the user and the launcher keep in the install folder.
    await fs.promises.mkdir(path.join(dest, "User", "Wii"), { recursive: true });
    await fs.promises.writeFile(path.join(dest, "User", "Wii", "sd.raw"), "card");
    await fs.promises.mkdir(path.join(dest, "pplus", "NetplaySave"), { recursive: true });
    await fs.promises.mkdir(path.join(dest, "pponline-sd"), { recursive: true });
    await fs.promises.writeFile(path.join(dest, "pponline-sd", "sd.raw"), "patched");
    await fs.promises.mkdir(path.join(dest, "Sys", "NetplaySave"), { recursive: true });

    const v2 = await makeBundle("0.1.2", { "Dolphin.exe": "exe-2", "Sys/new.txt": "new" });
    const result = await installBundledDolphin({ sourceDir: v2, destDir: dest });
    expect(result.action).toBe("installed");
    expect(await read("Dolphin.exe")).toBe("exe-2");
    expect(await read("Sys", "new.txt")).toBe("new");
    expect(fs.existsSync(path.join(dest, "Sys", "old.txt"))).toBe(false);
    expect(fs.existsSync(path.join(dest, "Old.dll"))).toBe(false);
    // Sys was replaced, so the save template has to be copied again (ensureNetplaySave).
    expect(fs.existsSync(path.join(dest, "Sys", "NetplaySave"))).toBe(false);
    expect(await read("User", "Wii", "sd.raw")).toBe("card");
    expect(await read("pponline-sd", "sd.raw")).toBe("patched");
    expect(fs.existsSync(path.join(dest, "pplus", "NetplaySave"))).toBe(true);
    expect((await readDolphinManifest(dest))?.version).toBe("0.1.2");
  });

  it("redoes an interrupted install (no manifest written)", async () => {
    const src = await makeBundle("0.1.3", { "Dolphin.exe": "exe-3" });
    await fs.promises.mkdir(dest, { recursive: true });
    await fs.promises.writeFile(path.join(dest, "Dolphin.exe"), "half");
    expect((await installBundledDolphin({ sourceDir: src, destDir: dest })).action).toBe("installed");
    expect(await read("Dolphin.exe")).toBe("exe-3");
  });

  it("refuses a bundle without a manifest or with an entry that would replace user data", async () => {
    const empty = path.join(root, `empty-${n}`);
    await fs.promises.mkdir(empty, { recursive: true });
    await expect(installBundledDolphin({ sourceDir: empty, destDir: dest })).rejects.toThrow(/Reinstall the launcher/);

    const bad = await makeBundle("0.1.4", { "Dolphin.exe": "x", "User/evil": "x" });
    await expect(installBundledDolphin({ sourceDir: bad, destDir: dest })).rejects.toThrow(/Bad entry/);
  });
});

describe("bundledDolphinSource", () => {
  it("uses resources/dolphin when packaged, PPO_BUNDLED_DOLPHIN if set, and nothing in development", () => {
    const resourcesPath = path.resolve("/app/resources");
    expect(bundledDolphinSource({ env: {}, isPackaged: true, resourcesPath })).toBe(
      path.join(resourcesPath, "dolphin"),
    );
    expect(bundledDolphinSource({ env: {}, isPackaged: false, resourcesPath })).toBeNull();
    expect(bundledDolphinSource({ env: { PPO_BUNDLED_DOLPHIN: " /b " }, isPackaged: false, resourcesPath })).toBe("/b");
  });
});
