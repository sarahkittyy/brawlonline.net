import { describe, expect, it } from "vitest";

import { DISC_IMAGE_EXTENSIONS, findNativeFile, getIsoFileKind, ISO_STEP_EXTENSIONS } from "./iso_file";

describe("getIsoFileKind", () => {
  it("verifies disc images in any format Dolphin reads, whatever the case of the extension", () => {
    expect(getIsoFileKind("D:\\games\\SSBB_NTSC.iso")).toBe("iso");
    expect(getIsoFileKind("/Users/me/Brawl.ISO")).toBe("iso");
    expect(getIsoFileKind("/home/me/Super Smash Bros. Brawl (USA) (Rev 2).iso")).toBe("iso");
    expect(getIsoFileKind("/Users/me/Brawl.rvz")).toBe("iso");
    expect(getIsoFileKind("C:\\Brawl.WBFS")).toBe("iso");
  });

  it("explains archives", () => {
    expect(getIsoFileKind("C:\\Brawl.7z")).toBe("7z");
    expect(getIsoFileKind("/Users/me/Brawl.7Z")).toBe("7z");
  });

  it("lets Select pick every disc image and the archives it explains", () => {
    // Electron's dialog filters take extensions without the dot.
    expect(DISC_IMAGE_EXTENSIONS).toEqual(["iso", "rvz", "wbfs", "wia", "gcz", "ciso"]);
    expect(ISO_STEP_EXTENSIONS.map((ext) => getIsoFileKind(`Brawl.${ext}`))).toEqual([
      "iso",
      "iso",
      "iso",
      "iso",
      "iso",
      "iso",
      "7z",
    ]);
  });
});

describe("findNativeFile", () => {
  const iso = { name: "SSBB_NTSC.iso", size: 4_699_979_776, lastModified: 1700000000000 };

  it("finds the native twin of a dropped file", () => {
    const native = { ...iso, native: true };
    const other = { ...iso, name: "other.iso", native: true };
    expect(findNativeFile(iso, [other, native])).toBe(native);
  });

  it("finds nothing for a file that was not dropped", () => {
    expect(findNativeFile(iso, null)).toBeUndefined();
    expect(findNativeFile(iso, [])).toBeUndefined();
    expect(findNativeFile(iso, [{ ...iso, size: 1 }])).toBeUndefined();
  });
});
