import { PPLUS_STAGES, stageIconTexture } from "@common/stages";
import { describe, expect, it } from "vitest";

import { buildArc } from "./formats/arc";
import { buildBrres, buildTex0 } from "./formats/brres";
import { GxFormat } from "./formats/gx";
import { decodePng } from "./png";
import type { GameFileSource } from "./sources";
import { extractStageIcons, STAGE_ICON_ARCHIVE } from "./stages";

const sdWith = (files: Record<string, Buffer>): GameFileSource => ({
  kind: "sd",
  label: "fake-sd",
  read: async (p) => files[p] ?? null,
  close: async () => undefined,
});

describe("stage select icons", () => {
  it("names the textures after the icon", () => {
    expect(stageIconTexture(1)).toBe("MenSelmapIcon.01");
    expect(stageIconTexture(98)).toBe("MenSelmapIcon.98");
  });

  it("writes one PNG per stage found in P+'s sc_selmap.pac, keyed by stage id", async () => {
    const tex = (w: number, h: number, v: number) => buildTex0(GxFormat.I8, w, h, new Uint8Array(w * h).fill(v));
    const brres = buildBrres([
      { name: "MenSelmapIcon.00", tex0: tex(4, 4, 1) },
      { name: "MenSelmapIcon.01", tex0: tex(8, 8, 0x40) },
      // Yoshi's Story (stage id 0x2a) has icon 51.
      { name: "MenSelmapIcon.51", tex0: tex(8, 8, 0x80) },
    ]);
    const written: Record<string, Buffer> = {};
    const files = { [STAGE_ICON_ARCHIVE]: buildArc("sc_selmap", [{ type: 1, fileIndex: 80, data: brres }]) };
    const { stages, missing } = await extractStageIcons(sdWith(files), async (rel, data) => {
      written[rel] = data;
    });
    expect(stages).toEqual({
      "1": { file: "stages/1.png", name: "Battlefield" },
      "42": { file: "stages/42.png", name: "Yoshi's Story" },
    });
    expect(missing).toHaveLength(PPLUS_STAGES.length - 2);
    expect(missing).toContain("stage:2");
    const png = decodePng(written["stages/42.png"]);
    expect([png.width, png.height, png.rgba[0]]).toEqual([8, 8, 0x80]);
  });

  it("reports the icons missing without an SD card", async () => {
    expect(await extractStageIcons(null, async () => undefined)).toEqual({ stages: {}, missing: ["stages"] });
    expect(await extractStageIcons(sdWith({}), async () => undefined)).toEqual({ stages: {}, missing: ["stages"] });
  });
});
