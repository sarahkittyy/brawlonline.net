import { mkdir, mkdtemp, readFile, rm, writeFile } from "node:fs/promises";
import os from "os";
import path from "path";
import { afterEach, beforeEach, describe, expect, it } from "vitest";

import type { ExtractFn } from "./game_assets_manager";
import { GameAssetsManager } from "./game_assets_manager";
import { resolveAssetPath } from "./protocol";
import type { GameAssetManifest } from "./types";
import { GameAssetsStatus } from "./types";

const manifest = (disc: string): GameAssetManifest => ({
  schemaVersion: 1,
  extractedAt: "2026-10-06T00:00:00.000Z",
  sources: { disc },
  textures: {},
  fonts: {},
  stocks: {},
  missing: [],
});

describe("game assets", () => {
  let dir: string;
  beforeEach(async () => {
    dir = await mkdtemp(path.join(os.tmpdir(), "ppo-assets-"));
  });
  afterEach(async () => {
    await rm(dir, { recursive: true, force: true });
  });

  it("serves only files inside the cache", () => {
    const root = path.join(dir, "theme");
    expect(resolveAssetPath(root, "game-asset://cache/textures/button.png")).toBe(
      path.join(root, "textures", "button.png"),
    );
    expect(resolveAssetPath(root, "game-asset://cache/../secret.txt")).toBe(path.join(root, "secret.txt"));
    expect(resolveAssetPath(root, "game-asset://cache/%2e%2e%2fsecret.txt")).toBeNull();
    expect(resolveAssetPath(root, "game-asset://other/x.png")).toBeNull();
  });

  it("keeps the fallback look until a disc is set, then extracts once and reuses the cache", async () => {
    const cacheDir = path.join(dir, "theme");
    const discFolder = path.join(dir, "files");
    await mkdir(discFolder);
    let disc: string | null = null;
    let extractions = 0;
    const extract: ExtractFn = async (opts) => {
      extractions += 1;
      const m = manifest(opts.disc.path);
      await writeFile(path.join(opts.cacheDir, "manifest.json"), JSON.stringify(m));
      return m;
    };
    const resolve = async () => (disc ? { disc: { kind: "folder" as const, path: disc } } : null);

    const manager = new GameAssetsManager(cacheDir, resolve, extract);
    expect((await manager.refresh()).status).toBe(GameAssetsStatus.NONE);
    expect(extractions).toBe(0);

    disc = discFolder;
    const ready = await manager.refresh();
    expect(ready.status).toBe(GameAssetsStatus.READY);
    expect(ready.manifest?.sources.disc).toBe(discFolder);
    expect(extractions).toBe(1);

    // A new process with the same sources loads the cached manifest.
    const again = await new GameAssetsManager(cacheDir, resolve, extract).refresh();
    expect(again.status).toBe(GameAssetsStatus.READY);
    expect(extractions).toBe(1);
    expect(JSON.parse(await readFile(path.join(cacheDir, "sources.json"), "utf8")).disc.path).toBe(discFolder);

    await manager.refresh({ force: true });
    expect(extractions).toBe(2);
  });

  it("reports extraction failures and falls back", async () => {
    const extract: ExtractFn = async () => {
      throw new Error("DolphinTool not found");
    };
    const manager = new GameAssetsManager(
      path.join(dir, "theme"),
      async () => ({ disc: { kind: "folder", path: dir } }),
      extract,
    );
    const state = await manager.refresh();
    expect(state.status).toBe(GameAssetsStatus.ERROR);
    expect(state.manifest).toBeNull();
    expect(state.error).toContain("DolphinTool");
  });
});
