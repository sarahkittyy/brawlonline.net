import { describe, expect, it } from "vitest";

import { getLauncherUpdateMode } from "./launcher_update";
import { MAC_DOWNLOAD_URL, MAC_SELF_UPDATE } from "./product";

describe("getLauncherUpdateMode", () => {
  it("installs updates in place on Windows and Linux, whatever the macOS flag", () => {
    for (const macSelfUpdate of [false, true]) {
      expect(getLauncherUpdateMode("win32", macSelfUpdate)).toBe("install");
      expect(getLauncherUpdateMode("linux", macSelfUpdate)).toBe("install");
    }
  });

  it("offers the download on macOS until the build can update itself", () => {
    expect(getLauncherUpdateMode("darwin", false)).toBe("download");
    expect(getLauncherUpdateMode("darwin", true)).toBe("install");
  });

  it("downloads on macOS with the current build (ad-hoc signed)", () => {
    expect(MAC_SELF_UPDATE).toBe(false);
    expect(getLauncherUpdateMode("darwin")).toBe("download");
    expect(getLauncherUpdateMode("win32")).toBe("install");
    expect(getLauncherUpdateMode("linux")).toBe("install");
  });

  it("links the website's macOS download", () => {
    expect(MAC_DOWNLOAD_URL).toBe("https://brawlonline.net/downloads/BrawlOnline.dmg");
  });
});
