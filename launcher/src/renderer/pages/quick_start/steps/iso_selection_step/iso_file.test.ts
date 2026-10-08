import { describe, expect, it } from "vitest";

import { findNativeFile, getIsoFileKind, ISO_STEP_EXTENSIONS } from "./iso_file";

describe("getIsoFileKind", () => {
  it("verifies disc images, whatever the case of the extension", () => {
    expect(getIsoFileKind("D:\\games\\SSBB_NTSC.iso")).toBe("iso");
    expect(getIsoFileKind("/Users/me/Brawl.ISO")).toBe("iso");
    expect(getIsoFileKind("/home/me/Super Smash Bros. Brawl (USA) (Rev 2).iso")).toBe("iso");
  });

  it("explains archives and compressed images", () => {
    expect(getIsoFileKind("C:\\Brawl.7z")).toBe("7z");
    expect(getIsoFileKind("/Users/me/Brawl.7Z")).toBe("7z");
    expect(getIsoFileKind("/Users/me/Brawl.rvz")).toBe("compressed");
    expect(getIsoFileKind("C:\\Brawl.WBFS")).toBe("compressed");
  });

  it("lets Select pick the ISO and every file it has a message for", () => {
    // Electron's dialog filters take extensions without the dot.
    expect(ISO_STEP_EXTENSIONS.map((ext) => getIsoFileKind(`Brawl.${ext}`))).toEqual([
      "iso",
      "compressed",
      "compressed",
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
