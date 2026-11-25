import { describe, expect, it } from "vitest";

import { isLauncherUpdate } from "./changelog_version";

describe("isLauncherUpdate", () => {
  it("is an update when another version ran last", () => {
    expect(isLauncherUpdate("0.2.8", "0.2.9", false)).toBe(true);
    // A downgrade too: the changelog's own dates decide what it shows.
    expect(isLauncherUpdate("0.3.0", "0.2.9", false)).toBe(true);
  });

  it("is not one at a start of the same version", () => {
    expect(isLauncherUpdate("0.2.9", "0.2.9", false)).toBe(false);
  });

  it("counts the first start after the release with changelogs as an update", () => {
    expect(isLauncherUpdate(undefined, "0.2.9", false)).toBe(true);
  });

  it("shows none on a new install", () => {
    expect(isLauncherUpdate(undefined, "0.2.9", true)).toBe(false);
  });
});
