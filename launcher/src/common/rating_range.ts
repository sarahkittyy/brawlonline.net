/**
 * "Unranked Rating Range" (`unrankedRatingRange`, Dolphin.ini `[Online] UnrankedRatingRange`):
 * Unranked pairs the player only with placed players whose Ranked rating is within this many
 * points of theirs, however long they search (server/README.md, Unranked queue). 0: off. A player
 * who has not placed in Ranked yet searches as if it were off.
 */
export const RATING_RANGE_OFF = 0;

/** The ranges the setting offers as quick picks, in rating points; any other whole number is a custom range. */
export const RATING_RANGE_PRESETS: readonly number[] = [100, 200, 300];

/** The largest range kept (more than any two ratings are apart). */
export const RATING_RANGE_MAX = 9999;

/** A stored or ini value as the launcher uses it: a whole number of points up to the max, else off. */
export function normalizeRatingRange(value: unknown): number {
  const n = typeof value === "string" ? Number(value.trim() === "" ? NaN : value) : value;
  if (typeof n !== "number" || !Number.isFinite(n) || n <= 0) {
    return RATING_RANGE_OFF;
  }
  return Math.min(Math.round(n), RATING_RANGE_MAX);
}
