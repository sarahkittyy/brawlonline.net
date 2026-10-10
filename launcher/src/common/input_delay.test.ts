import { describe, expect, it } from "vitest";

import { INPUT_DELAY_AUTO, normalizeInputDelay } from "./input_delay";

describe("normalizeInputDelay", () => {
  it("keeps the choices, from the settings file or Dolphin.ini", () => {
    expect([2, 3, 4].map(normalizeInputDelay)).toEqual([2, 3, 4]);
    expect(["2", "3", "4"].map(normalizeInputDelay)).toEqual([2, 3, 4]);
  });

  it("makes anything else automatic", () => {
    for (const v of [0, "0", 1, 5, 9, -1, 2.5, "", "auto", null, undefined, NaN]) {
      expect(normalizeInputDelay(v)).toBe(INPUT_DELAY_AUTO);
    }
  });
});
