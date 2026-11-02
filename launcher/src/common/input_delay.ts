/**
 * The online input delay setting (`inputDelay`, Dolphin.ini `[Online] InputDelay`): the frames
 * between a player's button press and the game using it, for that player's own inputs, as Slippi's
 * delay frames.
 *
 * A profile (0 Normal, the default; -1 Aggressive; -2 Smooth) has Dolphin pick the frames from the
 * ping at the start of each game (`Gprb::Session::ProfileInputDelay`), and in an Other Games room
 * each time someone joins (`Machine::PickInputDelay`). Each player plays their own profile: a
 * player's delay smooths their own screen only.
 * A number of frames (0-8, the setting's Automatic switch off) is played as it is, in every mode:
 * a player's delay costs only their own screen (until 2026-10-23 Direct and rooms only). 1-8 are
 * stored as they are; 0 frames as -3 (`INPUT_DELAY_ZERO_FRAMES`), 0 being Normal.
 */
export const INPUT_DELAY_NORMAL = 0;
export const INPUT_DELAY_AGGRESSIVE = -1;
export const INPUT_DELAY_SMOOTH = -2;
export const INPUT_DELAY_ZERO_FRAMES = -3;

/** The profiles, as the setting lists them: least delay first. */
export const INPUT_DELAY_PROFILES: readonly number[] = [INPUT_DELAY_AGGRESSIVE, INPUT_DELAY_NORMAL, INPUT_DELAY_SMOOTH];

/** The numbers of frames a player can pick, as frames (not as stored). Dolphin itself takes 0-9. */
export const INPUT_DELAY_FRAMES: readonly number[] = [0, 1, 2, 3, 4, 5, 6, 7, 8];

/** Whether a setting is a profile (Automatic) rather than a number of frames. */
export function isInputDelayProfile(inputDelay: number): boolean {
  return INPUT_DELAY_PROFILES.includes(inputDelay);
}

/** A setting of a number of frames as those frames (-3 is 0); a profile as null. */
export function inputDelayFrames(inputDelay: number): number | null {
  return inputDelay === INPUT_DELAY_ZERO_FRAMES ? 0 : inputDelay > 0 ? inputDelay : null;
}

/** A number of frames as the setting stores it (0 as -3). */
export function inputDelayFromFrames(frames: number): number {
  return frames === 0 ? INPUT_DELAY_ZERO_FRAMES : frames;
}

/** A stored or ini value as the launcher uses it: a profile or one of the frames, else Normal. */
export function normalizeInputDelay(value: unknown): number {
  const n = typeof value === "string" ? Number(value.trim() === "" ? NaN : value) : value;
  return typeof n === "number" &&
    (INPUT_DELAY_PROFILES.includes(n) || INPUT_DELAY_FRAMES.includes(inputDelayFrames(n) ?? NaN))
    ? n
    : INPUT_DELAY_NORMAL;
}
