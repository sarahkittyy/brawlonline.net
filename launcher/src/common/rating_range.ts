/**
 * "Unranked Rating Range" (`unrankedRatingRange`, Dolphin.ini `[Online] UnrankedRatingRange`):
 * Unranked pairs the player only with placed players whose Ranked rating is within this many
 * points of theirs, however long they search (server/README.md, Unranked queue). 0: off. A player
 * who has not placed in Ranked yet searches as if it were off.
 */
export const RATING_RANGE_OFF = 0;

/** The ranges a player can pick, in rating points (Off first). */
export const RATING_RANGE_CHOICES: readonly number[] = [RATING_RANGE_OFF, 100, 200, 300];

/** A stored value as the launcher uses it: one of the choices, else off. */
export function normalizeRatingRange(value: unknown): number {
  const n = typeof value === "string" ? Number.parseInt(value, 10) : value;
  return typeof n === "number" && RATING_RANGE_CHOICES.includes(n) ? n : RATING_RANGE_OFF;
}
