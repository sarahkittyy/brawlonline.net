import { describe, expect, it } from "vitest";

import { formatClock, formatEta } from "./queue_eta";

describe("formatEta", () => {
  it("shows the median as m:ss", () => {
    expect(formatEta(28)).toBe("0:28");
    expect(formatEta(249)).toBe("4:09");
  });

  it("reads 10+ min when there is no ETA or it is 10 minutes or more", () => {
    expect(formatEta(null)).toBe("10+ min");
    expect(formatEta(600)).toBe("10+ min");
  });

  it("never shows a negative clock", () => {
    expect(formatClock(-3)).toBe("0:00");
  });
});
