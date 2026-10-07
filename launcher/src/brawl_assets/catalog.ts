// Declarative list of the Brawl / Project+ assets the launcher theme uses.
//
// Every entry names real game data: the archive (disc path and/or P+ SD path), the nested archive entry and the
// texture name, with candidates tried in order (P+ SD first, then the disc). Paths and names were found by surveying
// the NTSC-U Rev 1 disc and the Project+ SD card (`cli.ts survey` lists an archive's textures and fonts).
//
// Menu textures in Brawl are mostly greyscale (I4/IA4) masks that the game tints through material colours, so the
// PNGs are white/grey with alpha. The theme is expected to tint them in CSS (e.g. mask-image + background-color, or
// filters) rather than redraw them.
//
// SPDX-License-Identifier: GPL-3.0-or-later

export type Slice = { top: number; right: number; bottom: number; left: number };

export type TextureCandidate = {
  origin: "disc" | "sd";
  /** Path relative to the disc's DATA/files (origin "disc") or to /Project+/pf/ (origin "sd"). */
  file: string;
  /** Slash-separated path of the BRRES inside the archive ("" for a bare .brres). */
  entry: string;
  texture: string;
  /** Expected size; used to disambiguate textures that share a name. */
  width?: number;
  height?: number;
};

export type TextureRole = {
  role: string;
  description: string;
  optional?: boolean;
  candidates: TextureCandidate[];
  /** Nine-slice insets in source pixels, for CSS border-image. */
  slice?: Slice;
};

export type FontRole = {
  role: string;
  description: string;
  family: string;
  weightClass?: number;
  optional?: boolean;
  candidates: { origin: "disc" | "sd"; file: string; entry: string }[];
};

const MENUMAIN_DISC = "menu2/mu_menumain_en.pac";
const MENUMAIN_SD = "menu2/mu_menumain.pac";
const SELCHAR_DISC = "menu2/sc_selcharacter_en.pac";
const SELCHAR_SD = "menu2/sc_selcharacter.pac";

function both(file: { disc: string; sd: string }, entry: string, texture: string, width?: number, height?: number) {
  return [
    { origin: "sd" as const, file: file.sd, entry, texture, width, height },
    { origin: "disc" as const, file: file.disc, entry, texture, width, height },
  ];
}

const MENUMAIN = { disc: MENUMAIN_DISC, sd: MENUMAIN_SD };
const SELCHAR = { disc: SELCHAR_DISC, sd: SELCHAR_SD };

export const TEXTURE_ROLES: TextureRole[] = [
  {
    role: "background",
    description:
      "Project+ menu background gradient (stage select backdrop, RGBA8 4x1024, teal to dark to blue); stretch to cover. " +
      "Brawl's own menu backdrop is a 3D scene with no single texture, so there is no disc fallback.",
    optional: true,
    candidates: [
      { origin: "sd", file: "menu2/sc_selmap.pac", entry: "MiscData[20]", texture: "bg_gradient", width: 4 },
    ],
  },
  {
    role: "backgroundTile",
    description: "Project+ stage-select grid tile (I4 32x32), repeatable overlay for the background.",
    optional: true,
    candidates: [{ origin: "sd", file: "menu2/sc_selmap.pac", entry: "MiscData[20]", texture: "bg_grid", width: 32 }],
  },
  {
    role: "button",
    description: "Common menu key/button plate: rounded square, black rim, bevelled grey face (MenCmn00, IA4 48x48).",
    candidates: [
      ...both(MENUMAIN, "MiscData[4]", "MenCmn00", 48, 48),
      ...both(SELCHAR, "MiscData[60]", "MenCmn00", 48, 48),
    ],
    slice: { top: 16, right: 16, bottom: 16, left: 16 },
  },
  {
    role: "buttonSelected",
    description:
      "Highlighted name-entry key: white outer rim, black ring, light grey fill (MenSelchrEntryW01b, IA4 40x32).",
    candidates: both(MENUMAIN, "MiscData[13]/TextureData[9]", "MenSelchrEntryW01b", 40, 32),
    slice: { top: 7, right: 7, bottom: 7, left: 7 },
  },
  {
    role: "panel",
    description: "Name-entry key panel: white outer rim, black ring, dark grey fill (MenSelchrEntryW01, IA4 32x24).",
    candidates: [
      ...both(MENUMAIN, "MiscData[2]", "MenSelchrEntryW01", 32, 24),
      ...both(SELCHAR, "MiscData[30]", "MenSelchrEntryW01", 32, 24),
    ],
    slice: { top: 7, right: 7, bottom: 7, left: 7 },
  },
  {
    role: "cursor",
    description: 'Player selection marker "P1" tag from the stage/character select (MenSelmapCursorPly.1, IA4 32x24).',
    optional: true,
    candidates: both(MENUMAIN, "MiscData[0]", "MenSelmapCursorPly.1", 32, 24),
  },
];

export const FONT_ROLES: FontRole[] = [
  {
    role: "menuFont",
    description: "Brawl's main UI font (latin, 34x40 cells, 996 characters), used for menu text and descriptions.",
    family: "BrawlMenu",
    candidates: [
      { origin: "sd", file: "system/common3.pac", entry: "MiscData[9]" },
      { origin: "disc", file: "system/font/font_latin1.arc", entry: "" },
      { origin: "disc", file: "system/common3_en.pac", entry: "MiscData[9]" },
    ],
  },
  {
    role: "titleFont",
    description: "Brawl's heavy display font (32x32 cells, latin + kana, 427 characters).",
    family: "BrawlTitle",
    weightClass: 700,
    optional: true,
    candidates: [
      { origin: "sd", file: "system/common2.pac", entry: "MiscData[9]" },
      { origin: "disc", file: "system/font/font_hira.brfnt", entry: "" },
      { origin: "disc", file: "system/common2_en.pac", entry: "MiscData[9]" },
    ],
  },
  {
    role: "smallFont",
    description: "Brawl's compact UI font (20x24 cells, 739 characters).",
    family: "BrawlSmall",
    optional: true,
    candidates: [
      { origin: "sd", file: "system/common2.pac", entry: "MiscData[10]" },
      { origin: "disc", file: "system/font/font_melee.brfnt", entry: "" },
      { origin: "disc", file: "system/common2_en.pac", entry: "MiscData[10]" },
    ],
  },
];

// ---------------------------------------------------------------------------------------------------------------
// Stock icons.
//
// StockFaceTex.brres holds one TEX0 per character costume. The character number ("stock id") is the same on the disc
// and in P+, but the naming differs:
//   disc (StockFaceTex_en.brres): "InfStc.<id*10 + costume>" padded to 3 digits, costumes 1..9 (C4 + PLT0)
//   P+   (StockFaceTex.brres):    "InfStc.<id*50 + costume>" padded to 4 digits (BrawlEx style, C8 + PLT0)
// The id -> character table below was established by viewing every icon.

export type StockCharacter = { key: string; name: string; id: number; pplusOnly?: boolean; discOnly?: boolean };

export const STOCK_CHARACTERS: StockCharacter[] = [
  { key: "mario", name: "Mario", id: 0 },
  { key: "donkey_kong", name: "Donkey Kong", id: 1 },
  { key: "link", name: "Link", id: 2 },
  { key: "samus", name: "Samus", id: 3 },
  { key: "yoshi", name: "Yoshi", id: 4 },
  { key: "kirby", name: "Kirby", id: 5 },
  { key: "fox", name: "Fox", id: 6 },
  { key: "pikachu", name: "Pikachu", id: 7 },
  { key: "luigi", name: "Luigi", id: 8 },
  { key: "captain_falcon", name: "Captain Falcon", id: 9 },
  { key: "ness", name: "Ness", id: 10 },
  { key: "bowser", name: "Bowser", id: 11 },
  { key: "peach", name: "Peach", id: 12 },
  { key: "zelda", name: "Zelda", id: 13 },
  { key: "sheik", name: "Sheik", id: 14 },
  { key: "ice_climbers", name: "Ice Climbers", id: 15 },
  { key: "marth", name: "Marth", id: 16 },
  { key: "mr_game_and_watch", name: "Mr. Game & Watch", id: 17 },
  { key: "falco", name: "Falco", id: 18 },
  { key: "ganondorf", name: "Ganondorf", id: 19 },
  { key: "meta_knight", name: "Meta Knight", id: 21 },
  { key: "pit", name: "Pit", id: 22 },
  { key: "zero_suit_samus", name: "Zero Suit Samus", id: 23 },
  { key: "olimar", name: "Olimar", id: 24 },
  { key: "lucas", name: "Lucas", id: 25 },
  { key: "diddy_kong", name: "Diddy Kong", id: 26 },
  // Slot 27 is Pokemon Trainer on the disc and Mewtwo in Project+.
  { key: "pokemon_trainer", name: "Pokemon Trainer", id: 27, discOnly: true },
  { key: "mewtwo", name: "Mewtwo", id: 27, pplusOnly: true },
  { key: "charizard", name: "Charizard", id: 28 },
  { key: "squirtle", name: "Squirtle", id: 29 },
  { key: "ivysaur", name: "Ivysaur", id: 30 },
  { key: "king_dedede", name: "King Dedede", id: 31 },
  { key: "lucario", name: "Lucario", id: 32 },
  { key: "ike", name: "Ike", id: 33 },
  { key: "rob", name: "R.O.B.", id: 34 },
  { key: "jigglypuff", name: "Jigglypuff", id: 36 },
  { key: "wario", name: "Wario", id: 37 },
  { key: "roy", name: "Roy", id: 39, pplusOnly: true },
  { key: "toon_link", name: "Toon Link", id: 40 },
  { key: "knuckles", name: "Knuckles", id: 42, pplusOnly: true },
  { key: "wolf", name: "Wolf", id: 43 },
  { key: "giga_bowser", name: "Giga Bowser", id: 44, pplusOnly: true },
  { key: "snake", name: "Snake", id: 45 },
  { key: "sonic", name: "Sonic", id: 46 },
  { key: "wario_man", name: "Wario-Man", id: 66, pplusOnly: true },
];

export const STOCK_FILES = {
  sd: { file: "menu/common/StockFaceTex.brres", entry: "" },
  disc: { file: "menu/common/StockFaceTex_en.brres", entry: "" },
};

export function stockTextureName(origin: "disc" | "sd", id: number, costume = 1): string {
  return origin === "sd"
    ? `InfStc.${String(id * 50 + costume).padStart(4, "0")}`
    : `InfStc.${String(id * 10 + costume).padStart(3, "0")}`;
}

/** Parses a stock texture name back to (id, costume) for the given naming scheme. */
export function parseStockTextureName(origin: "disc" | "sd", name: string): { id: number; costume: number } | null {
  const m = /^InfStc\.(\d+)$/.exec(name);
  if (!m) {
    return null;
  }
  const n = Number(m[1]);
  if (origin === "sd") {
    if (m[1].length !== 4 || n % 50 === 0) {
      return null;
    }
    return { id: Math.floor(n / 50), costume: n % 50 };
  }
  if (m[1].length !== 3 || n % 10 === 0) {
    return null;
  }
  return { id: Math.floor(n / 10), costume: n % 10 };
}
