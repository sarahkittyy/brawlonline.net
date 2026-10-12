/**
 * What the room hand-off (room_handoff.ts) takes instead of a room or connect code to have the game
 * create a room, or go to the Unranked or Ranked character select (Home's Play unranked / Play
 * ranked). Its own file without Node imports, so the renderer can use it.
 */
export const CREATE_ROOM = "create";
export const QUEUE_UNRANKED = "unranked";
export const QUEUE_RANKED = "ranked";

export type QueueTarget = typeof QUEUE_UNRANKED | typeof QUEUE_RANKED;

export function isQueueTarget(code: string): code is QueueTarget {
  return code === QUEUE_UNRANKED || code === QUEUE_RANKED;
}
