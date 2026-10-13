import { assetUrl, useGameAssets } from "@/styles/game_theme";

/** A 1x1 transparent image, for when no stock icon is available (no placeholder art). */
const EMPTY_IMAGE = "data:image/gif;base64,R0lGODlhAQABAIAAAAAAAP///yH5BAEAAAAALAAAAAABAAEAAAIBRAA7";

/**
 * Character IDs in our replays (Brawl's gmCharacterKind, as the replay's INFO and the online
 * lock-ins carry them) -> stock icon keys of the extracted asset manifest (brawl_assets/catalog.ts).
 */
export const characterStockKeys: Record<number, string> = {
  0x00: "mario",
  0x01: "donkey_kong",
  0x02: "link",
  0x03: "samus",
  0x04: "zero_suit_samus",
  0x05: "yoshi",
  0x06: "kirby",
  0x07: "fox",
  0x08: "pikachu",
  0x09: "luigi",
  0x0a: "captain_falcon",
  0x0b: "ness",
  0x0c: "bowser",
  0x0d: "peach",
  0x0e: "zelda",
  0x0f: "sheik",
  0x10: "ice_climbers",
  0x11: "ice_climbers",
  0x13: "marth",
  0x14: "mr_game_and_watch",
  0x15: "falco",
  0x16: "ganondorf",
  0x17: "wario",
  0x18: "meta_knight",
  0x19: "pit",
  0x1a: "olimar",
  0x1b: "lucas",
  0x1c: "diddy_kong",
  0x1e: "charizard",
  0x20: "squirtle",
  0x22: "ivysaur",
  0x23: "king_dedede",
  0x24: "lucario",
  0x25: "ike",
  0x26: "rob",
  0x27: "jigglypuff",
  0x28: "toon_link",
  0x29: "wolf",
  0x2a: "snake",
  0x2b: "sonic",
  0x2c: "giga_bowser",
  0x2d: "wario_man",
  0x32: "roy",
  0x33: "mewtwo",
  0x35: "knuckles",
};

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
