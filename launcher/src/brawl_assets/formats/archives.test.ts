import { describe, expect, it } from "vitest";

import { buildArc, isArc, parseArc } from "./arc";
import { buildBrres, buildPlt0, buildTex0, decodeTex0, listBrresTextures, parseBrres } from "./brres";
import { findTextures, walk } from "./container";
import { GxFormat, PaletteFormat } from "./gx";
import { compressLz77 } from "./lz77";
import { buildU8, isU8, parseU8 } from "./u8";

function sampleBrres() {
  const i8 = new Uint8Array(32).map((_, i) => i * 8);
  const c4 = new Uint8Array(32);
  c4[0] = 0x10;
  return buildBrres(
    [
      { name: "TexI8", tex0: buildTex0(GxFormat.I8, 8, 4, i8) },
      { name: "TexC4", tex0: buildTex0(GxFormat.C4, 8, 8, c4) },
    ],
    [{ name: "TexC4", plt0: buildPlt0(PaletteFormat.RGB565, [0xf800, 0x001f]) }],
  );
}

describe("BRRES / TEX0 / PLT0", () => {
  it("enumerates textures and pairs palettes by name", () => {
    const brres = parseBrres(sampleBrres());
    expect(brres.files.map((f) => `${f.folder}/${f.name}:${f.tag}`)).toEqual([
      "Textures(NW4R)/TexI8:TEX0",
      "Textures(NW4R)/TexC4:TEX0",
      "Palettes(NW4R)/TexC4:PLT0",
    ]);
    const tex = listBrresTextures(brres);
    expect(tex.map((t) => [t.tex.name, t.tex.formatName, t.tex.width, t.tex.height, !!t.palette])).toEqual([
      ["TexI8", "I8", 8, 4, false],
      ["TexC4", "C4", 8, 8, true],
    ]);
    const i8 = decodeTex0(tex[0].tex);
    expect(Array.from(i8.rgba.subarray(4, 8))).toEqual([8, 8, 8, 8]);
    const c4 = decodeTex0(tex[1].tex, tex[1].palette);
    expect(Array.from(c4.rgba.subarray(0, 8))).toEqual([0, 0, 255, 255, 255, 0, 0, 255]);
  });

  it("reads the TEX0 name embedded in the sub-file", () => {
    const tex = listBrresTextures(parseBrres(sampleBrres()))[1].tex;
    expect(tex.name).toBe("TexC4");
    expect(tex.mipCount).toBe(1);
  });
});

describe("ARC", () => {
  it("parses entries with stable type/index names and 0x20 alignment", () => {
    const arc = buildArc("sc_test", [
      { type: 1, fileIndex: 0, data: Buffer.from("hello") },
      { type: 2, fileIndex: 3, data: Buffer.alloc(40, 1) },
      { type: 1, fileIndex: 0, data: Buffer.from("dup") },
    ]);
    expect(isArc(arc)).toBe(true);
    const parsed = parseArc(arc);
    expect(parsed.name).toBe("sc_test");
    expect(parsed.entries.map((e) => e.name)).toEqual(["MiscData[0]", "ModelData[3]", "MiscData[0]#1"]);
    expect(parsed.entries[0].data.toString()).toBe("hello");
    expect(parsed.entries[1].data.length).toBe(40);
    expect(parsed.entries[2].data.toString()).toBe("dup");
  });

  it("rejects truncated archives", () => {
    const arc = buildArc("x", [{ type: 1, fileIndex: 0, data: Buffer.alloc(64) }]);
    expect(() => parseArc(arc.subarray(0, 0x50))).toThrow();
  });
});

describe("U8", () => {
  it("round-trips nested directories", () => {
    const u8 = buildU8([
      { path: "a.bin", data: Buffer.from("A") },
      { path: "dir/b.bin", data: Buffer.from("BB") },
      { path: "dir/sub/c.bin", data: Buffer.from("CCC") },
      { path: "z.bin", data: Buffer.from("Z") },
    ]);
    expect(isU8(u8)).toBe(true);
    const files = parseU8(u8);
    expect(files.map((f) => `${f.path}=${f.data.toString()}`).sort()).toEqual([
      "a.bin=A",
      "dir/b.bin=BB",
      "dir/sub/c.bin=CCC",
      "z.bin=Z",
    ]);
  });
});

describe("container walk", () => {
  it("finds textures through LZ77-compressed ARC, nested ARC and U8 layers", () => {
    const brres = sampleBrres();
    const inner = buildArc("inner", [{ type: 3, fileIndex: 2, data: compressLz77(brres, true) }]);
    const u8 = buildU8([{ path: "deep/tex.brres", data: brres }]);
    const outer = buildArc("outer", [
      { type: 1, fileIndex: 0, data: Buffer.from("not a container") },
      { type: 1, fileIndex: 13, data: inner },
      { type: 1, fileIndex: 14, data: u8 },
    ]);
    const file = compressLz77(outer);
    const kinds = [...walk(file)].map((n) => `${n.path || "<root>"}:${n.kind}${n.compressed ? "(lz)" : ""}`);
    expect(kinds).toEqual([
      "<root>:arc(lz)",
      "MiscData[0]:data",
      "MiscData[13]:arc",
      "MiscData[13]/TextureData[2]:brres(lz)",
      "MiscData[14]:u8",
      "MiscData[14]/deep/tex.brres:brres",
    ]);
    const tex = findTextures(file).map((t) => t.path);
    expect(tex).toEqual([
      "MiscData[13]/TextureData[2]/Textures(NW4R)/TexI8",
      "MiscData[13]/TextureData[2]/Textures(NW4R)/TexC4",
      "MiscData[14]/deep/tex.brres/Textures(NW4R)/TexI8",
      "MiscData[14]/deep/tex.brres/Textures(NW4R)/TexC4",
    ]);
  });
});
