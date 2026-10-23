/**
 * The online input delay setting (`inputDelay`, Dolphin.ini `[Online] InputDelay`): the frames
 * between a player's button press and the game using it, for that player's own inputs, as Slippi's
 * delay frames.
 *
 * A profile (0 Normal, the default; -1 Aggressive; -2 Smooth) has Dolphin pick the frames from the
 * ping at the start of each game (`Gprb::Session::ProfileInputDelay`), and in an Other Games room
 * each time someone joins (`Machine::PickInputDelay`). Each player plays their own profile: a
 * player's delay smooths their own screen only.
 * A number of frames (1-4) applies in Direct and rooms only; Ranked and Unranked play Normal for it.
 */
export const INPUT_DELAY_NORMAL = 0;
export const INPUT_DELAY_AGGRESSIVE = -1;
export const INPUT_DELAY_SMOOTH = -2;

/** The profiles, as the setting lists them: least delay first. */
export const INPUT_DELAY_PROFILES: readonly number[] = [INPUT_DELAY_AGGRESSIVE, INPUT_DELAY_NORMAL, INPUT_DELAY_SMOOTH];

/** The numbers of frames a player can pick (Direct and rooms). Dolphin itself takes 1-9. */
export const INPUT_DELAY_CHOICES: readonly number[] = [1, 2, 3, 4];

/** A stored or ini value as the launcher uses it: a profile or one of the choices, else Normal. */
export function normalizeInputDelay(value: unknown): number {
  const n = typeof value === "string" ? Number(value.trim() === "" ? NaN : value) : value;
  return typeof n === "number" && (INPUT_DELAY_PROFILES.includes(n) || INPUT_DELAY_CHOICES.includes(n))
    ? n
    : INPUT_DELAY_NORMAL;
}
