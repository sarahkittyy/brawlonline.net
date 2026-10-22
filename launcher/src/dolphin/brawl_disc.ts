// NTSC-U Brawl's discs as game_data.ts names them, apart from it: the renderer's room matching
// (mods/room_match.ts, mods/known_builds.ts) needs them and cannot bundle game_data's Node modules.

/** NTSC-U Super Smash Bros. Brawl's game data, Rev 1 and Rev 2 (the redump dumps, read with
 * DolphinTool gamedata; any other format of them has the same). */
export const BRAWL_NTSC_U_GAME_DATA: ReadonlyMap<string, string> = new Map([
  ["0d79fc54ef81e223b65eb8d4b1854643fc677554", "NTSC-U Rev 1"],
  ["e6ae2404f96cff10be1c4dda8559001c3a7dfd34", "NTSC-U Rev 2"],
]);

/**
 * NTSC-U Brawl Rev 1 and Rev 2 as one disc, as Other Games rooms compare discs: the two revisions
 * have the same game files and their main.dol differs in one word, the disc revision the game
 * checks at boot. A whole-machine session's checksums leave out the bytes that revision leaves in
 * RAM (Dolphin's MachineSession, BRAWL_DISC_REVISION), so Rev 1 and Rev 2 players play together.
 * The SHA-1 of "disc: NTSC-U Brawl, Rev 1 or Rev 2" (game_data.test.ts checks it).
 */
export const BRAWL_NTSC_U_DISC = "a56ffa9437ddf80f4e4fa9c9bfddc7094d216483";
