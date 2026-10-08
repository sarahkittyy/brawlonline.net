import { assetUrl, useGameAssets } from "@/styles/game_theme";

/** A 1x1 transparent image, for when no stock icon is available (no placeholder art). */
const EMPTY_IMAGE = "data:image/gif;base64,R0lGODlhAQABAIAAAAAAAP///yH5BAEAAAAALAAAAAABAAEAAAIBRAA7";

/**
 * Character IDs in our replays -> stock icon keys of the extracted asset manifest.
 * Filled in when our replay format is defined (PPLUS_PORTING.md, "Replays").
 */
export const characterStockKeys: Record<number, string> = {};

/**
 * Stock icon for a character, from the stock icons extracted from the user's own
 * Brawl disc / P+ SD card. Slippi bundles Melee stock icons; we cannot bundle game art.
 */
export const getCharacterIcon = (characterId: number | undefined, _characterColor: number | undefined = 0): string => {
  const manifest = useGameAssets.getState().state?.manifest;
  const key = characterId != null ? characterStockKeys[characterId] : undefined;
  const stock = key && manifest ? manifest.stocks[key] : undefined;
  return stock ? assetUrl(stock.file) : EMPTY_IMAGE;
};

/** Stage images: none yet (Slippi bundles Melee stage art; ours would also have to come from the disc). */
export const getStageImage = (_stageId: number): string => {
  return "";
};

export const toOrdinal = (i: number): string => {
  const j = i % 10,
    k = i % 100;
  if (j === 1 && k !== 11) {
    return i + "st";
  }
  if (j === 2 && k !== 12) {
    return i + "nd";
  }
  if (j === 3 && k !== 13) {
    return i + "rd";
  }
  return i + "th";
};

// Converts number of bytes into a human readable format.
// Based on code available from:
// https://coderrocketfuel.com/article/get-the-total-size-of-all-files-in-a-directory-using-node-js
export const humanReadableBytes = (bytes: number): string => {
  const sizes = ["bytes", "KB", "MB", "GB", "TB"];
  if (bytes > 0) {
    const i = Math.floor(Math.log(bytes) / Math.log(1024));
    return `${(bytes / Math.pow(1024, i)).toFixed(1)} ${sizes[i]}`;
  }

  return `0 ${sizes[0]}`;
};
