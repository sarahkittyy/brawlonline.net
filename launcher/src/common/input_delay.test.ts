import { describe, expect, it } from "vitest";

import { INPUT_DELAY_AGGRESSIVE, INPUT_DELAY_NORMAL, INPUT_DELAY_SMOOTH, normalizeInputDelay } from "./input_delay";

describe("normalizeInputDelay", () => {
  it("keeps the profiles", () => {
    expect([-1, 0, -2].map(normalizeInputDelay)).toEqual([
      INPUT_DELAY_AGGRESSIVE,
      INPUT_DELAY_NORMAL,
      INPUT_DELAY_SMOOTH,
    ]);
    expect(["-1", "0", "-2"].map(normalizeInputDelay)).toEqual([-1, 0, -2]);
  });

  it("makes anything else Normal", () => {
    for (const v of [1, 2, 3, 4, "3", 5, 9, -3, 2.5, "", "auto", "1.5", null, undefined, NaN]) {
      expect(normalizeInputDelay(v)).toBe(INPUT_DELAY_NORMAL);
    }
  });
});
