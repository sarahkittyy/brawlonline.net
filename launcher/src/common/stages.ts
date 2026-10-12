/**
 * P+ v3.2's stages: the ids a ruleset lists (`StageLists`, the P+ stage ids its stage select and
 * stage file loader key on, docs/game-code.md section 11), their names and their stage select icons.
 *
 * Read off P+ itself (2026-10-12): the netplay stage switch preset (`pf/stage/switch/SwitchFF.rss`,
 * harness/ppharness/brawl.py SWITCHFF_PAGES_AND_SLOTS) lists each stage select page's stages in
 * screen order, each a slot of {stage id, cosmetic}. The icon is `MenSelmapIcon.<icon>` in P+'s
 * `menu2/sc_selmap.pac` and the name `MenSelmapFrontStname.<icon>` (an image; the names below were
 * read off it). The cosmetic byte is not the icon number for P+'s added stages (35-49 are icons
 * 80-94, 61-64 are 95-98), so the icon is listed here, checked against the stage select's pages.
 *
 * In stage select order: page 1 (21 stages: the 15 legal ones and 6 more), page 2 (21), page 3 (19).
 */

export type StageInfo = {
  /** The P+ stage id (a ruleset's `stages`). */
  id: number;
  name: string;
  /** `MenSelmapIcon.<icon>` in P+'s menu2/sc_selmap.pac. */
  icon: number;
};

const STAGES: [number, string, number][] = [
  // Page 1
  [0x2a, "Yoshi's Story", 51],
  [0x06, "Bowser's Castle", 6],
  [0x19, "Castle Siege", 21],
  [0x43, "Sky Sanctuary Zone", 83],
  [0x47, "Golden Temple", 87],
  [0x49, "Ceres Space Colony", 89],
  [0x1d, "Distant Planet", 23],
  [0x0d, "Yoshi's Island", 13],
  [0x1c, "Wario Land", 22],
  [0x1f, "Fountain of Dreams", 25],
  [0x02, "Final Destination", 2],
  [0x05, "Metal Cavern", 5],
  [0x2d, "Dream Land", 54],
  [0x0c, "Frigate Husk", 12],
  [0x09, "Temple of Time", 9],
  [0x23, "Green Hill Zone", 29],
  [0x21, "Smashville", 27],
  [0x01, "Battlefield", 1],
  [0x2e, "Pokémon Stadium 2", 59],
  [0x03, "Delfino's Secret", 3],
  [0x04, "Luigi's Mansion", 4],
  // Page 2
  [0x2f, "Peach's Castle", 55],
  [0x07, "Kongo Jungle", 7],
  [0x08, "Rumble Falls", 8],
  [0x33, "Hyrule Castle", 10],
  [0x29, "Hyrule Temple", 50],
  [0x0e, "Halberd", 14],
  [0x32, "Planet Zebes", 58],
  [0x14, "Saffron City", 16],
  [0x15, "Spear Pillar", 17],
  [0x30, "Corneria", 56],
  [0x13, "Lylat Cruise", 15],
  [0x2c, "Onett", 53],
  [0x20, "Fourside", 26],
  [0x17, "Infinite Glacier", 19],
  [0x31, "Big Blue", 57],
  [0x16, "Port Town Aero Dive", 18],
  [0x18, "Flat Zone 2", 20],
  [0x1e, "Skyworld", 24],
  [0x22, "Shadow Moses Island", 28],
  [0x24, "PictoChat", 30],
  [0x37, "Training Room", 60],
  // Page 3
  [0x45, "Dinosaur Land", 85],
  [0x41, "Mario Circuit", 81],
  [0x4c, "Mushroom Kingdom", 92],
  [0x4f, "Rainbow Cruise", 95],
  [0x46, "Minecart Madness", 86],
  [0x2b, "Jungle Japes", 52],
  [0x4a, "Cookie Country", 90],
  [0x42, "Clock Town", 82],
  [0x51, "Pirate Ship", 97],
  [0x4d, "WarioWare, Inc.", 93],
  [0x50, "Poké Floats", 96],
  [0x48, "Bell Tower", 88],
  [0x0b, "Norfair", 11],
  [0x52, "Venom", 98],
  [0x25, "Hanenbow", 31],
  [0x4b, "Venus Lighthouse", 91],
  [0x40, "Dracula's Castle", 80],
  [0x44, "Dead Line", 84],
  [0x4e, "Subspace", 94],
];

/** Every stage of P+'s stage select, in its order. */
export const PPLUS_STAGES: readonly StageInfo[] = STAGES.map(([id, name, icon]) => ({ id, name, icon }));

const BY_ID = new Map(PPLUS_STAGES.map((s) => [s.id, s]));

export function stageInfo(id: number): StageInfo | undefined {
  return BY_ID.get(id);
}

/** The stage's icon texture in P+'s menu2/sc_selmap.pac. */
export function stageIconTexture(icon: number): string {
  return `MenSelmapIcon.${String(icon).padStart(2, "0")}`;
}
