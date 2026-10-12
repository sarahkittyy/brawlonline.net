import { extractBrawlAssets, extractMissingSounds, soundsOutdated } from "@brawl_assets/extract";

import type { ExtractFn, FillMissingFn } from "./game_assets_manager";
import type { GameAssetManifest } from "./types";

/** Runs the Brawl/P+ asset extractor (src/brawl_assets) for the launcher theme. */
export const extractGameAssets: ExtractFn = async (opts) => {
  const manifest = await extractBrawlAssets(opts);
  return manifest as GameAssetManifest;
};

/** Adds the sound effects to a cache extracted by a launcher from before they existed, or replaces outdated ones. */
export const fillMissingGameAssets: FillMissingFn = async (opts) => {
  const manifest = await extractMissingSounds(opts);
  return manifest as GameAssetManifest | null;
};

/** Whether a cached manifest needs fillMissingGameAssets: no sounds, or not the catalog's; no ITEM SWITCH icons. */
export const gameAssetsNeedFill = (manifest: GameAssetManifest) => soundsOutdated(manifest) || !manifest.items;
