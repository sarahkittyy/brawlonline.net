/**
 * Which of the player's games plays a room, part by part (docs/design/brawl-mods.md §6.5). A room
 * shows its disc's hash and its SD card's: a different disc keeps the player out (a disc cannot be
 * sent), a different SD card does not (Dolphin gets the host's before the join, with the player's
 * consent). A room from an older launcher has no parts: then only a game with its exact build hash
 * plays it. A room of the disc alone (no launcher, no card) is played with the disc of any of the
 * player's games on it (`discOnly`).
 */

import { BRAWL_NTSC_U_DISC } from "../brawl_disc";
import type { ModBuild, ModRoomBuild } from "./types";

/** How one part of the player's game compares with the room's. */
export type PartMatch =
  /** The same. */
  | "same"
  /** Different, and fixable: an SD card is synced before the join. */
  | "differs"
  /** Different, and the room cannot be joined with it (the disc; a card on one side only). */
  | "wrong";

export type RoomMatch = {
  /** The player's game that plays the room (or the closest: the one to fix), if any. */
  build: ModBuild | null;
  /** The room can be joined with `build` (an SD card that differs is synced first). */
  joinable: boolean;
  disc: PartMatch;
  /** null: the room's game has no SD card, nor has `build`. */
  sd: PartMatch | null;
  /** The room says its parts (a launcher from before them does not). */
  hasParts: boolean;
  /** The room plays the disc alone, and the player has it only in `build`, with a launcher or an
   * SD card: joined with that disc alone (the launcher adds it as its own game, `ModsManager`). */
  discOnly?: boolean;
};

/** The build hash of a disc played alone, no launcher and no SD card (`combineBuildHash`), by the
 * disc's part: the main process's, for a room to tell it plays the plain disc. */
export type DiscAloneHash = (disc: string) => string | undefined;

const hasCard = (sd: string | undefined | null) => !!sd;

/** The player's game for a room: the exact build first, then the same disc with the same card,
 * then the same disc with a card that differs, then the same disc at all, then the same game
 * (another revision, another dump: the one to fix). */
export function matchRoom(room: ModRoomBuild, builds: ModBuild[], discAlone?: DiscAloneHash): RoomMatch {
  const exact = builds.find((b) => b.hash === room.hash) ?? null;
  if (!room.disc) {
    return {
      build: exact,
      joinable: !!exact,
      disc: exact ? "same" : "wrong",
      sd: null,
      hasParts: false,
    };
  }
  const roomCard = hasCard(room.sd);
  // Brawl Online's own P+ (`builtIn`, every player has it) after the player's own games: a REX
  // room plays their REX with its card's few differences, not P+ with all of REX's.
  const onDisc = builds.filter((b) => b.parts?.disc === room.disc);
  const sameDisc = [...onDisc.filter((b) => !b.builtIn), ...onDisc.filter((b) => b.builtIn)];
  const cardOf = (b: ModBuild) => (b.sdPath ? b.parts?.sdCard ?? null : null);
  // The exact build (its launcher too) before any other game on the disc with a card: that one
  // can be another release of the mod, its whole card to download (bug #28, 2026-10-16: RUDY#60's
  // REX room asked BUDS#979 for 8032 MB, his REX 0 FINAL BUILD's card; his 0½ Hotfix 1, the
  // room's build, was 20 MB off).
  const best =
    sameDisc.find((b) => roomCard && cardOf(b) === room.sd) ??
    sameDisc.find((b) => b.hash === room.hash) ??
    sameDisc.find((b) => roomCard === !!b.sdPath) ??
    sameDisc[0] ??
    null;
  // A room of the plain disc, and the player's games on it all have a card or a launcher (Brawl
  // Online's P+ is on every player's Brawl disc): its disc alone plays the room, no card to turn
  // off by hand (Sarah's board, 2026-10-30: "if there's no sd card, can't we just turn it off?").
  if (best && !roomCard && best.hash !== room.hash && discAlone?.(room.disc) === room.hash) {
    return { build: best, joinable: true, disc: "same", sd: null, hasParts: true, discOnly: true };
  }
  if (best) {
    const card = !!best.sdPath;
    let sd: PartMatch | null = null;
    if (roomCard && card) {
      sd = cardOf(best) === room.sd ? "same" : "differs";
    } else if (roomCard !== card) {
      sd = "wrong";
    }
    return { build: best, joinable: sd !== "wrong", disc: "same", sd, hasParts: true };
  }
  // No game on that disc: the closest one is the one to fix (the exact build, then the same game
  // ID with an SD card as the room's: a REX room shows the player's REX, not their plain Brawl
  // disc as "add an SD card").
  const gameId = room.discLabel?.split(" ")[0];
  const ofGame = gameId ? builds.filter((b) => b.gameId === gameId) : [];
  const sameGame = [...ofGame.filter((b) => !b.builtIn), ...ofGame.filter((b) => b.builtIn)];
  const near = exact ?? sameGame.find((b) => roomCard === !!b.sdPath) ?? sameGame[0] ?? null;
  let sd: PartMatch | null = null;
  if (near) {
    if (roomCard && near.sdPath) {
      sd = cardOf(near) === room.sd ? "same" : "differs";
    } else if (roomCard !== !!near.sdPath) {
      sd = "wrong";
    }
  }
  return { build: near, joinable: false, disc: "wrong", sd, hasParts: true };
}

/** "SMNE01 Rev 2": what a disc is, as rooms name it. NTSC-U Brawl's Rev 1 and Rev 2 are one disc
 * in rooms (`disc_info.ts` discIdentity): "RSBE01". */
export function discLabel(build: Pick<ModBuild, "gameId" | "revision" | "parts">): string {
  if (!build.gameId) {
    return "";
  }
  if (build.parts?.disc === BRAWL_NTSC_U_DISC) {
    return build.gameId;
  }
  return `${build.gameId} Rev ${build.revision ?? 0}`;
}
