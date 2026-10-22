/**
 * "Matchmaking Ping Limit" (`pingLimit`, Dolphin.ini `[Online] MatchmakingPingLimit`): the highest
 * estimated ping, in ms, Unranked and Ranked pair the player at, however long they search
 * (server/README.md, Ping limit).
 */
export const PING_LIMIT_DEFAULT = 150;

/** The limits a player can pick, in ms. */
export const PING_LIMIT_CHOICES: readonly number[] = [100, 150, 200];

/** A stored value as the launcher uses it: one of the choices, else the default. */
export function normalizePingLimit(value: unknown): number {
  const n = typeof value === "string" ? Number.parseInt(value, 10) : value;
  return typeof n === "number" && PING_LIMIT_CHOICES.includes(n) ? n : PING_LIMIT_DEFAULT;
}
