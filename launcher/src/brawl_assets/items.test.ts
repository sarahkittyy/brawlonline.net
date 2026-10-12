import { describe, expect, it } from "vitest";

import { buildArc } from "./formats/arc";
import { buildBrres, buildTex0 } from "./formats/brres";
import { GxFormat } from "./formats/gx";
import { extractItemIcons, ITEM_ICON_ARCHIVE, itemIconTexture } from "./items";
import { decodePng } from "./png";
import type { GameFileSource } from "./sources";

const sdWith = (files: Record<string, Buffer>): GameFileSource => ({
  kind: "sd",
  label: "fake-sd",
  read: async (p) => files[p] ?? null,
  close: async () => undefined,
});

describe("item switch icons", () => {
  it("names the textures after the cell", () => {
    expect(itemIconTexture(0)).toBe("MenMainItem.01");
    expect(itemIconTexture(52)).toBe("MenMainItem.53");
  });

  it("writes one PNG per cell found in P+'s mu_menumain.pac", async () => {
    const tex = (v: number) => buildTex0(GxFormat.I8, 8, 4, new Uint8Array(8 * 4).fill(v));
    const brres = buildBrres([
      { name: "MenMainItem.00", tex0: tex(1) },
      { name: "MenMainItem.01", tex0: tex(0x40) },
      { name: "MenMainItem.50", tex0: tex(0x80) },
    ]);
    const files: Record<string, Buffer> = {};
    const written: Record<string, Buffer> = {};
    files[ITEM_ICON_ARCHIVE] = buildArc("mu_menumain", [{ type: 1, fileIndex: 14, data: brres }]);
    const { items, missing } = await extractItemIcons(sdWith(files), async (rel, data) => {
      written[rel] = data;
    });
    expect(items).toEqual({
      "0": { file: "items/0.png", name: "Smash Ball" },
      "49": { file: "items/49.png", name: "Stage Items" },
    });
    expect(missing).toHaveLength(51);
    expect(missing).toContain("item:1");
    const png = decodePng(written["items/49.png"]);
    expect([png.width, png.height, png.rgba[0]]).toEqual([8, 4, 0x80]);
  });

  it("reports the icons missing without an SD card", async () => {
    expect(await extractItemIcons(null, async () => undefined)).toEqual({ items: {}, missing: ["items"] });
    expect(await extractItemIcons(sdWith({}), async () => undefined)).toEqual({ items: {}, missing: ["items"] });
  });
});
