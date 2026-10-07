// Integration tests against the real game files on the development machine. Skipped when they are absent.
// Override the locations with BRAWL_DISC_FOLDER, BRAWL_ISO, DOLPHIN_TOOL and PPLUS_SD_RAW.

import fs from "fs";
import os from "os";
import path from "path";
import { afterAll, describe, expect, it } from "vitest";

import { extractBrawlAssets } from "./extract";
import { FatImage } from "./fat32";
import { readTtf } from "./font/ttf";
import { decodeTex0 } from "./formats/brres";
import { findFonts, findTextures } from "./formats/container";
import { parseRfnt } from "./formats/rfnt";
import { decodePng } from "./png";
import { DiscFolderSource, DiscIsoSource } from "./sources";

const DISC = process.env.BRAWL_DISC_FOLDER ?? "D:/code/pm_rollback/game/rev1-extract/DATA/files";
const ISO = process.env.BRAWL_ISO ?? "D:/code/pm_rollback/game/SSBB_NTSC.iso";
const TOOL = process.env.DOLPHIN_TOOL ?? "D:/code/pm_rollback/dolphin/build/release/x64/Binaries/DolphinTool.exe";
const SD = process.env.PPLUS_SD_RAW ?? "D:/code/pm_rollback/run/template-user/Wii/sd.raw";

const hasDisc = fs.existsSync(path.join(DISC, "menu2", "mu_menumain_en.pac"));
const hasIso = fs.existsSync(ISO) && fs.existsSync(TOOL);
const hasSd = fs.existsSync(SD);

const tmpRoot = fs.mkdtempSync(path.join(os.tmpdir(), "brawl-integration-"));
afterAll(() => fs.rmSync(tmpRoot, { recursive: true, force: true }));

async function discFile(rel: string) {
  const src = await DiscFolderSource.open(DISC);
  return (await src.read(rel))!;
}

function stats(rgba: Uint8Array) {
  const alphas = new Set<number>();
  const colours = new Set<number>();
  for (let i = 0; i < rgba.length; i += 4) {
    alphas.add(rgba[i + 3]);
    colours.add((rgba[i] << 16) | (rgba[i + 1] << 8) | rgba[i + 2]);
  }
  return { alphas: alphas.size, colours: colours.size, hasTransparent: alphas.has(0), hasOpaque: alphas.has(255) };
}

describe.skipIf(!hasDisc)("real disc files", () => {
  it("decodes every texture in the title, character select and common archives", async () => {
    const counts: Record<string, number> = {};
    for (const rel of ["menu2/sc_title_en.pac", "menu2/sc_selcharacter_en.pac", "system/common3_en.pac"]) {
      for (const t of findTextures(await discFile(rel))) {
        const img = decodeTex0(t.tex, t.palette);
        expect(img.rgba.length).toBe(t.tex.width * t.tex.height * 4);
        counts[t.tex.formatName] = (counts[t.tex.formatName] ?? 0) + 1;
      }
    }
    for (const f of ["I4", "I8", "IA4", "IA8", "RGB565", "RGB5A3", "RGBA8", "C4", "C8", "CMPR"]) {
      expect(counts[f] ?? 0, f).toBeGreaterThan(0);
    }
  }, 60_000);

  it("decodes CMPR and RGB5A3 character art with real alpha and colour", async () => {
    const tex = findTextures(await discFile("menu2/sc_title_en.pac"));
    for (const name of ["bil_mario", "bil_donkey"]) {
      const t = tex.find((x) => x.tex.name === name)!;
      const s = stats(decodeTex0(t.tex, t.palette).rgba);
      expect(s.hasTransparent && s.hasOpaque, name).toBe(true);
      expect(s.colours, name).toBeGreaterThan(500);
    }
  });

  it("parses the system fonts", async () => {
    const latin = findFonts(await discFile("system/font/font_latin1.arc"))[0];
    const f = parseRfnt(latin.data);
    expect([f.cellWidth, f.cellHeight, f.baseline, f.sheetFormatName]).toEqual([34, 40, 35, "I4"]);
    expect(f.codeToGlyph.get(0x41)).toBe(33);
    expect(f.widths.get(33)).toEqual({ left: 1, glyphWidth: 25, charWidth: 26 });
  });
});

describe.skipIf(!hasSd)("real P+ SD card image", () => {
  it("reads the Project+ tree from sd.raw", async () => {
    const img = await FatImage.open(SD);
    try {
      expect(img.fatType).toBe(32);
      const pf = (await img.readdir("/Project+/pf")).map((e) => e.name);
      expect(pf).toEqual(expect.arrayContaining(["menu2", "system", "info2"]));
      const stock = await img.readFile("/Project+/pf/menu/common/StockFaceTex.brres");
      expect(stock.toString("latin1", 0, 4)).toBe("bres");
    } finally {
      await img.close();
    }
  });
});

describe.skipIf(!hasDisc || !hasSd)("full extraction (disc folder + sd.raw)", () => {
  it("produces every role and the P+ roster", async () => {
    const cacheDir = path.join(tmpRoot, "full");
    const m = await extractBrawlAssets({ disc: { kind: "folder", path: DISC }, sdRawPath: SD, cacheDir });
    expect(m.missing).toEqual([]);
    expect(Object.keys(m.textures).sort()).toEqual(
      ["background", "backgroundTile", "button", "buttonSelected", "cursor", "panel"].sort(),
    );
    expect(Object.keys(m.fonts).sort()).toEqual(["menuFont", "smallFont", "titleFont"]);
    for (const key of ["mario", "roy", "mewtwo", "knuckles", "wario_man", "sonic"]) {
      expect(m.stocks[key]?.origin, key).toBe("sd");
    }
    expect(m.stocks.pokemon_trainer).toBeUndefined();
    const button = decodePng(fs.readFileSync(path.join(cacheDir, m.textures.button.file)));
    expect([button.width, button.height]).toEqual([48, 48]);
    const ttf = readTtf(fs.readFileSync(path.join(cacheDir, m.fonts.menuFont.file)));
    expect(ttf.advance(ttf.glyphFor(0x41))).toBe(26 * 64);
    expect(ttf.contours(ttf.glyphFor(0x6f)).length).toBe(2); // "o" has a hole
  }, 120_000);
});

describe.skipIf(!hasIso)("ISO through DolphinTool", () => {
  it("extracts a single file from the disc image", async () => {
    const src = await DiscIsoSource.open({ isoPath: ISO, dolphinToolPath: TOOL });
    try {
      const font = await src.read("system/font/font_hira.brfnt");
      expect(font!.toString("latin1", 0, 4)).toBe("RFNT");
      expect(await src.read("system/font/does_not_exist.brfnt")).toBeNull();
    } finally {
      await src.close();
    }
  }, 120_000);
});
