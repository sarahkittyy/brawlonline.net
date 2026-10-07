/**
 * The launcher's look comes from the user's own copy of Brawl and the P+ SD
 * card: menu frames, panels, fonts and stock icons are extracted at first run
 * into a local cache (never shipped, never committed). See src/brawl_assets for
 * the decoders and the asset catalog, and PPLUS_PORTING.md for the rationale.
 */

/** URL scheme the main process serves the asset cache on. */
export const GAME_ASSET_SCHEME = "game-asset";
export const GAME_ASSET_BASE_URL = `${GAME_ASSET_SCHEME}://cache/`;

export type NineSlice = { top: number; right: number; bottom: number; left: number };

/** Mirrors `AssetManifest` from src/brawl_assets (kept structural so the renderer needs no Node code). */
export type GameAssetManifest = {
  schemaVersion: number;
  extractedAt: string;
  sources: { disc: string; sd?: string };
  textures: Record<
    string,
    {
      file: string;
      width: number;
      height: number;
      origin: string;
      archive: string;
      entry: string;
      texture?: string;
      format?: string;
      /** Greyscale mask that Brawl tints at runtime (the theme tints it too). */
      greyscale?: boolean;
      slice?: NineSlice;
    }
  >;
  fonts: Record<string, { file: string; family: string; sheets: string[]; metrics: string; origin: string }>;
  stocks: Record<string, { file: string; origin: string }>;
  missing: string[];
  warnings?: string[];
};

export const enum GameAssetsStatus {
  /** No disc chosen yet, or nothing extracted: the plain fallback look. */
  NONE = "NONE",
  EXTRACTING = "EXTRACTING",
  READY = "READY",
  ERROR = "ERROR",
}

export type GameAssetsState = {
  status: GameAssetsStatus;
  manifest: GameAssetManifest | null;
  progress?: { done: number; total: number; label: string };
  error?: string;
};

export interface GameAssetsApi {
  getState(): Promise<GameAssetsState>;
  /** Re-extract from the current disc and SD card. */
  extract(): Promise<GameAssetsState>;
  onStateChange(handle: (state: GameAssetsState) => void): () => void;
}
