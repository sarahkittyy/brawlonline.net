/**
 * What the room hand-off (room_handoff.ts) takes instead of a room or connect code to have the game
 * create a room, or go to the Unranked, Ranked or 2v2 character select (Home's Play unranked / Play
 * ranked / Play 2v2), and the room codes' check. Its own file without Node imports, so the renderer can use it.
 */
export const CREATE_ROOM = "create";
export const QUEUE_UNRANKED = "unranked";
export const QUEUE_RANKED = "ranked";
/** The 2v2 queue (docs/design/2v2.md): the game opens its 2V2 character select (the player's party). */
export const QUEUE_TWOS = "twos";

export type QueueTarget = typeof QUEUE_UNRANKED | typeof QUEUE_RANKED | typeof QUEUE_TWOS;

export function isQueueTarget(code: string): code is QueueTarget {
  return code === QUEUE_UNRANKED || code === QUEUE_RANKED || code === QUEUE_TWOS;
}

/** Room codes: 4 letters without vowels (and without Y). */
export function isRoomCode(code: string): boolean {
  return /^[BCDFGHJKLMNPQRSTVWXZ]{4}$/.test(code);
}
