import { describe, expect, it } from "vitest";

import { isBrawlOnlineDolphinVersion } from "./brawl_online_build";

describe("isBrawlOnlineDolphinVersion", () => {
  it("accepts our releases and local builds", () => {
    expect(isBrawlOnlineDolphinVersion("0.1.77")).toBe(true);
    expect(isBrawlOnlineDolphinVersion("Project+ Dolphin f61c06a1")).toBe(true);
  });

  it("rejects Project+'s, Slippi's and stock Dolphin builds", () => {
    expect(isBrawlOnlineDolphinVersion("3.2.0")).toBe(false);
    expect(isBrawlOnlineDolphinVersion("3.4.2")).toBe(false);
    expect(isBrawlOnlineDolphinVersion("Dolphin 2512")).toBe(false);
    expect(isBrawlOnlineDolphinVersion("Dolphin 5.0-21088")).toBe(false);
  });

  it("does not judge an unknown version", () => {
    expect(isBrawlOnlineDolphinVersion(undefined)).toBe(true);
    expect(isBrawlOnlineDolphinVersion("")).toBe(true);
  });
});
