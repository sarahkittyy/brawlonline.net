import { extractBrawlAssets } from "@brawl_assets/extract";

import type { ExtractFn } from "./game_assets_manager";
import type { GameAssetManifest } from "./types";

/** Runs the Brawl/P+ asset extractor (src/brawl_assets) for the launcher theme. */
export const extractGameAssets: ExtractFn = async (opts) => {
  const manifest = await extractBrawlAssets(opts);
  return manifest as GameAssetManifest;
};
