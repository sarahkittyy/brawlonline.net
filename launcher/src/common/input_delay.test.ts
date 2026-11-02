import { describe, expect, it } from "vitest";

import {
  INPUT_DELAY_AGGRESSIVE,
  INPUT_DELAY_NORMAL,
  INPUT_DELAY_SMOOTH,
  INPUT_DELAY_ZERO_FRAMES,
  inputDelayFrames,
  inputDelayFromFrames,
  isInputDelayProfile,
  normalizeInputDelay,
} from "./input_delay";

describe("normalizeInputDelay", () => {
  it("keeps the frames, from the settings file or Dolphin.ini", () => {
    expect([-3, 1, 2, 3, 4, 8].map(normalizeInputDelay)).toEqual([-3, 1, 2, 3, 4, 8]);
    expect(["-3", "1", "2", "3", "4", "8"].map(normalizeInputDelay)).toEqual([-3, 1, 2, 3, 4, 8]);
  });

  it("keeps the profiles", () => {
    expect([-1, 0, -2].map(normalizeInputDelay)).toEqual([
      INPUT_DELAY_AGGRESSIVE,
      INPUT_DELAY_NORMAL,
      INPUT_DELAY_SMOOTH,
    ]);
    expect(["-1", "0", "-2"].map(normalizeInputDelay)).toEqual([-1, 0, -2]);
  });

  it("makes anything else Normal", () => {
    for (const v of [9, -4, 2.5, "", "auto", "1.5", null, undefined, NaN]) {
      expect(normalizeInputDelay(v)).toBe(INPUT_DELAY_NORMAL);
    }
  });
});

describe("isInputDelayProfile", () => {
  it("tells Automatic's profiles from Fixed's frames", () => {
    expect([-1, 0, -2].map(isInputDelayProfile)).toEqual([true, true, true]);
    expect([-3, 1, 4, 8].map(isInputDelayProfile)).toEqual([false, false, false, false]);
  });
});

describe("inputDelayFrames", () => {
  it("stores 0 frames as -3, as 0 is Normal", () => {
    expect(inputDelayFromFrames(0)).toBe(INPUT_DELAY_ZERO_FRAMES);
    expect([0, 1, 2, 8].map(inputDelayFromFrames).map(inputDelayFrames)).toEqual([0, 1, 2, 8]);
    expect([-1, 0, -2].map(inputDelayFrames)).toEqual([null, null, null]);
  });
});
