import fs from "fs";
import os from "os";
import path from "path";
import { afterAll, describe, expect, it } from "vitest";

import { stockTextureName } from "./catalog";
import { extractBrawlAssets, readManifest, runExtraction } from "./extract";
import { readTtf } from "./font/ttf";
import { buildArc } from "./formats/arc";
import { buildBrres, buildPlt0, buildTex0 } from "./formats/brres";
import { GxFormat, PaletteFormat } from "./formats/gx";
import { compressLz77 } from "./formats/lz77";
import { buildRfnt } from "./formats/rfnt";
import { decodePng } from "./png";
import type { GameFileSource } from "./sources";
import { DiscFolderSource } from "./sources";

const tmpRoot = fs.mkdtempSync(path.join(os.tmpdir(), "brawl-extract-test-"));
afterAll(() => fs.rmSync(tmpRoot, { recursive: true, force: true }));

function write(p: string, data: Buffer) {
  fs.mkdirSync(path.dirname(p), { recursive: true });
  fs.writeFileSync(p, data);
}

/** A synthetic "disc" with just enough structure for the catalog: no Nintendo data involved. */
function makeSyntheticDisc(root: string) {
  const files = path.join(root, "DATA", "files");
  const ia4 = new Uint8Array((48 / 8) * (48 / 4) * 32).fill(0xf8);
  const button = buildBrres([{ name: "MenCmn00", tex0: buildTex0(GxFormat.IA4, 48, 48, ia4) }]);
  const menumain = buildArc("mu_menumain", [
    { type: 1, fileIndex: 0, data: Buffer.from("misc") },
    { type: 1, fileIndex: 4, data: compressLz77(button) },
  ]);
  write(path.join(files, "menu2", "mu_menumain_en.pac"), compressLz77(menumain));
  const sheet = new Uint8Array(192).fill(0xff);
  const font = buildRfnt({
    cellWidth: 6,
    cellHeight: 8,
    baseline: 7,
    ascent: 7,
    lineFeed: 9,
    sheetFormat: GxFormat.I4,
    sheetRow: 2,
    sheetLine: 2,
    sheetWidth: 16,
    sheetHeight: 24,
    sheets: [sheet.subarray(0, 192)],
    widths: [
      { left: 0, glyphWidth: 6, charWidth: 7 },
      { left: 0, glyphWidth: 0, charWidth: 3 },
    ],
    maps: [{ method: 0, begin: 0x41, end: 0x41, base: 0 }],
  });
  write(path.join(files, "system", "font", "font_latin1.arc"), compressLz77(font));
  const c4 = new Uint8Array(32 * 16).fill(0x11);
  const stocks = buildBrres(
    [
      { name: stockTextureName("disc", 0), tex0: buildTex0(GxFormat.C4, 32, 32, c4) },
      { name: stockTextureName("disc", 45), tex0: buildTex0(GxFormat.C4, 32, 32, c4) },
    ],
    [
      { name: stockTextureName("disc", 0), plt0: buildPlt0(PaletteFormat.RGB5A3, [0, 0xfc00]) },
      { name: stockTextureName("disc", 45), plt0: buildPlt0(PaletteFormat.RGB5A3, [0, 0x83e0]) },
    ],
  );
  write(path.join(files, "menu", "common", "StockFaceTex_en.brres"), stocks);
  return root;
}

describe("extractBrawlAssets (synthetic disc)", () => {
  it("extracts what exists, records what is missing, and writes a manifest", async () => {
    const disc = makeSyntheticDisc(path.join(tmpRoot, "disc"));
    const cacheDir = path.join(tmpRoot, "cache");
    const progress: number[] = [];
    const manifest = await extractBrawlAssets({
      disc: { kind: "folder", path: disc },
      cacheDir,
      onProgress: (done, total) => progress.push(done / total),
    });
    expect(progress[progress.length - 1]).toBe(1);
    expect(manifest.schemaVersion).toBe(1);
    expect(manifest.textures.button).toMatchObject({
      file: "textures/button.png",
      width: 48,
      height: 48,
      origin: "disc",
      archive: "menu2/mu_menumain_en.pac",
      entry: "MiscData[4]",
      texture: "MenCmn00",
      format: "IA4",
      greyscale: true,
      slice: { top: 16, right: 16, bottom: 16, left: 16 },
    });
    const png = decodePng(fs.readFileSync(path.join(cacheDir, "textures", "button.png")));
    expect(Array.from(png.rgba.subarray(0, 4))).toEqual([0x88, 0x88, 0x88, 0xff]);

    expect(manifest.fonts.menuFont).toMatchObject({ family: "BrawlMenu", origin: "disc", glyphCount: 2 });
    const ttf = readTtf(fs.readFileSync(path.join(cacheDir, manifest.fonts.menuFont.file)));
    expect(ttf.advance(ttf.glyphFor(0x41))).toBe(7 * 64);
    expect(fs.existsSync(path.join(cacheDir, manifest.fonts.menuFont.metrics))).toBe(true);
    expect(manifest.fonts.menuFont.sheets.every((s) => fs.existsSync(path.join(cacheDir, s)))).toBe(true);

    expect(Object.keys(manifest.stocks).sort()).toEqual(["mario", "snake"]);
    const mario = decodePng(fs.readFileSync(path.join(cacheDir, manifest.stocks.mario.file)));
    expect(Array.from(mario.rgba.subarray(0, 4))).toEqual([255, 0, 0, 255]);

    expect(manifest.missing).toEqual(
      expect.arrayContaining(["texture:background", "texture:panel", "font:titleFont", "stock:luigi"]),
    );
    expect(manifest.missing).not.toContain("stock:roy"); // P+-only characters are not expected without an SD card
    expect(await readManifest(cacheDir)).toEqual(manifest);
  });

  it("prefers the SD copy of a file when it contains the texture", async () => {
    const disc = await DiscFolderSource.open(makeSyntheticDisc(path.join(tmpRoot, "disc2")));
    const ia4 = new Uint8Array((48 / 8) * (48 / 4) * 32).fill(0xf3);
    const sdBrres = buildBrres([{ name: "MenCmn00", tex0: buildTex0(GxFormat.IA4, 48, 48, ia4) }]);
    // P+ reorders archives: the texture lives in a different entry on the SD.
    const sdArc = buildArc("mu_menumain", [{ type: 1, fileIndex: 7, data: sdBrres }]);
    const sd: GameFileSource = {
      kind: "sd",
      label: "fake-sd",
      read: async (p) => (p === "menu2/mu_menumain.pac" ? sdArc : null),
      close: async () => undefined,
    };
    const cacheDir = path.join(tmpRoot, "cache2");
    const manifest = await runExtraction(disc, sd, { cacheDir });
    expect(manifest.sources.sd).toBe("fake-sd");
    expect(manifest.textures.button).toMatchObject({
      origin: "sd",
      archive: "menu2/mu_menumain.pac",
      entry: "MiscData[7]",
    });
    const png = decodePng(fs.readFileSync(path.join(cacheDir, "textures", "button.png")));
    expect(Array.from(png.rgba.subarray(0, 4))).toEqual([0x33, 0x33, 0x33, 0xff]);
    // Fonts are not on this fake SD, so they come from the disc.
    expect(manifest.fonts.menuFont.origin).toBe("disc");
    expect(manifest.missing).toContain("stock:roy");
  });
});
