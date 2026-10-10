/**
 * The online input delay setting (`inputDelay`, Dolphin.ini `[Online] InputDelay`): the frames
 * between a player's button press and the game using it, for that player's own inputs, as Slippi's
 * delay frames. 0 is automatic: Dolphin picks 1-4 frames from the ping at the start of each game
 * (`Gprb::Session::AutoInputDelay`).
 */
export const INPUT_DELAY_AUTO = 0;

/** The delays a player can pick, in frames. Dolphin itself takes 1-9. */
export const INPUT_DELAY_CHOICES: readonly number[] = [1, 2, 3, 4];

/** A stored or ini value as the launcher uses it: one of the choices, else automatic. */
export function normalizeInputDelay(value: unknown): number {
  const n = typeof value === "string" ? Number.parseInt(value, 10) : value;
  return typeof n === "number" && INPUT_DELAY_CHOICES.includes(n) ? n : INPUT_DELAY_AUTO;
}
