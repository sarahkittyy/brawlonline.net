/**
 * Which of the player's games plays a room, part by part (docs/design/brawl-mods.md §6.5). A room
 * shows its disc's hash and its SD card's: a different disc keeps the player out (a disc cannot be
 * sent: "make sure you're both on the same revision"), a different SD card does not (Dolphin gets
 * the host's before the join, with the player's consent). A room from an older launcher has no
 * parts: then only a game with its exact build hash plays it.
 */

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
};

const hasCard = (sd: string | undefined | null) => !!sd;

/** The player's game for a room: the exact build first, then the same disc with the same card,
 * then the same disc with a card that differs, then the same disc at all, then the same game
 * (another revision, another dump: the one to fix). */
export function matchRoom(room: ModRoomBuild, builds: ModBuild[]): RoomMatch {
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
  const sameDisc = builds.filter((b) => b.parts?.disc === room.disc);
  const cardOf = (b: ModBuild) => (b.sdPath ? b.parts?.sdCard ?? null : null);
  const best =
    sameDisc.find((b) => roomCard && cardOf(b) === room.sd) ??
    sameDisc.find((b) => roomCard === !!b.sdPath) ??
    sameDisc[0] ??
    null;
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
  // No game on that disc: the closest one is the one to fix (the same game ID, then the exact build).
  const gameId = room.discLabel?.split(" ")[0];
  const near = exact ?? (gameId ? builds.find((b) => b.gameId === gameId) : undefined) ?? null;
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

/** "RSBE01 Rev 1": what a disc is, as rooms name it. */
export function discLabel(build: Pick<ModBuild, "gameId" | "revision">): string {
  if (!build.gameId) {
    return "";
  }
  return `${build.gameId} Rev ${build.revision ?? 0}`;
}
