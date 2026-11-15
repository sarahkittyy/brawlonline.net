/**
 * Joining a room by its code from anywhere a name is (a friend's JOIN, "Join their room" in a
 * player's menu; docs/design/friends.md), without each of those importing the game hand-off
 * (use_hand_off.ts loads the play button's stores). SocialRoot, which holds the hand-off, registers
 * it here while the chat is there.
 */
type Joiner = (code: string) => Promise<boolean>;

let joiner: Joiner | undefined;

export function setRoomJoiner(fn: Joiner | undefined) {
  joiner = fn;
}

/** Hands the code to the game (or starts it with it); false when nothing could take it. */
export function joinRoomByCode(code: string): Promise<boolean> {
  return joiner ? joiner(code) : Promise.resolve(false);
}
