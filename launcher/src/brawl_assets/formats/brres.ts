// NW4R BRRES resource archives, plus TEX0 (texture) and PLT0 (palette) sub-files.
//
// Layout ported from BrawlLib (BrawlCrate, GPL-3.0): BrawlLib/SSBB/Types/BRES.cs, TEX0.cs, PLT0.cs,
// BrawlLib/SSBB/ResourceNodes/BRESNode.cs, Graphics/TEX0Node.cs, Graphics/PLT0Node.cs.
// https://github.com/soopercool101/BrawlCrate
//
// SPDX-License-Identifier: GPL-3.0-or-later

import { decodeGx, decodePalette, GX_FORMAT_NAMES, isPaletted } from "./gx";

export type BrresFile = {
  /** Folder name, e.g. "Textures(NW4R)", "Palettes(NW4R)", "3DModels(NW4R)". */
  folder: string;
  name: string;
  /** 4-char tag of the sub-file, e.g. "TEX0", "PLT0", "MDL0". */
  tag: string;
  data: Buffer;
};

export type Brres = { files: BrresFile[] };

export function isBrres(buf: Uint8Array): boolean {
  return buf.length >= 0x10 && buf[0] === 0x62 && buf[1] === 0x72 && buf[2] === 0x65 && buf[3] === 0x73;
}

function readName(buf: Buffer, off: number): string {
  if (off <= 0 || off >= buf.length) {
    return "";
  }
  const end = buf.indexOf(0, off);
  return buf.toString("latin1", off, end < 0 ? buf.length : end);
}

type GroupEntry = { name: string; dataOffset: number };

/** Reads an NW4R resource index group (skipping its reference/root entry). Offsets are made absolute. */
export function readResourceGroup(buf: Buffer, groupOff: number): GroupEntry[] {
  const count = buf.readInt32BE(groupOff + 4);
  const out: GroupEntry[] = [];
  for (let i = 1; i <= count; i++) {
    const e = groupOff + 8 + i * 16;
    const nameOff = buf.readInt32BE(e + 8);
    const dataOff = buf.readInt32BE(e + 12);
    out.push({ name: readName(buf, groupOff + nameOff), dataOffset: groupOff + dataOff });
  }
  return out;
}

export function parseBrres(buf: Buffer): Brres {
  if (!isBrres(buf)) {
    throw new Error("Not a BRRES file");
  }
  const rootOff = buf.readUInt16BE(0x0c);
  if (buf.toString("latin1", rootOff, rootOff + 4) !== "root") {
    throw new Error("BRRES: missing root section");
  }
  const files: BrresFile[] = [];
  for (const folder of readResourceGroup(buf, rootOff + 8)) {
    for (const f of readResourceGroup(buf, folder.dataOffset)) {
      const tag = buf.toString("latin1", f.dataOffset, f.dataOffset + 4);
      const size = buf.readUInt32BE(f.dataOffset + 4);
      files.push({
        folder: folder.name,
        name: f.name,
        tag,
        data: buf.subarray(f.dataOffset, Math.min(buf.length, f.dataOffset + size)),
      });
    }
  }
  return { files };
}

export type Tex0Info = {
  name: string;
  width: number;
  height: number;
  format: number;
  formatName: string;
  mipCount: number;
  hasPalette: boolean;
  /** Raw pixel data of mip level 0. */
  pixels: Buffer;
};

export function parseTex0(tex: Buffer, fallbackName = ""): Tex0Info {
  if (tex.toString("latin1", 0, 4) !== "TEX0") {
    throw new Error("Not a TEX0");
  }
  const dataOff = tex.readInt32BE(0x10);
  const nameOff = tex.readInt32BE(0x14);
  const hasPalette = tex.readInt32BE(0x18) !== 0;
  const width = tex.readUInt16BE(0x1c);
  const height = tex.readUInt16BE(0x1e);
  const format = tex.readUInt32BE(0x20);
  const mipCount = tex.readInt32BE(0x24);
  let name = fallbackName;
  if (nameOff > 0 && nameOff < tex.length) {
    name = readName(tex, nameOff) || fallbackName;
  }
  return {
    name,
    width,
    height,
    format,
    formatName: GX_FORMAT_NAMES[format] ?? `fmt${format}`,
    mipCount,
    hasPalette: hasPalette || isPaletted(format),
    pixels: tex.subarray(dataOff),
  };
}

export type Plt0Info = { name: string; format: number; count: number; data: Buffer };

export function parsePlt0(plt: Buffer, fallbackName = ""): Plt0Info {
  if (plt.toString("latin1", 0, 4) !== "PLT0") {
    throw new Error("Not a PLT0");
  }
  const dataOff = plt.readInt32BE(0x10);
  const nameOff = plt.readInt32BE(0x14);
  const format = plt.readUInt32BE(0x18);
  const count = plt.readUInt16BE(0x1c);
  let name = fallbackName;
  if (nameOff > 0 && nameOff < plt.length) {
    name = readName(plt, nameOff) || fallbackName;
  }
  return { name, format, count, data: plt.subarray(dataOff, dataOff + count * 2) };
}

export type DecodedImage = { width: number; height: number; rgba: Uint8Array };

/** Decodes a TEX0 (mip 0) to RGBA8, using the given palette for colour-indexed formats. */
export function decodeTex0(tex: Tex0Info, palette?: Plt0Info): DecodedImage {
  let pal: Uint8Array | undefined;
  if (isPaletted(tex.format)) {
    if (!palette) {
      throw new Error(`TEX0 ${tex.name}: palette required for ${tex.formatName}`);
    }
    pal = decodePalette(palette.data, palette.format, palette.count);
  }
  return { width: tex.width, height: tex.height, rgba: decodeGx(tex.format, tex.pixels, tex.width, tex.height, pal) };
}

export type BrresTexture = { tex: Tex0Info; palette?: Plt0Info };

/** Lists every TEX0 of a BRRES, paired with its same-named PLT0 when the format is colour-indexed. */
export function listBrresTextures(brres: Brres): BrresTexture[] {
  const palettes = new Map<string, Plt0Info>();
  for (const f of brres.files) {
    if (f.tag === "PLT0") {
      palettes.set(f.name, parsePlt0(f.data, f.name));
    }
  }
  const out: BrresTexture[] = [];
  for (const f of brres.files) {
    if (f.tag !== "TEX0") {
      continue;
    }
    const tex = parseTex0(f.data, f.name);
    tex.name = f.name; // the group name is authoritative
    out.push({ tex, palette: isPaletted(tex.format) ? palettes.get(f.name) : undefined });
  }
  return out;
}

// ---------------------------------------------------------------------------------------------------------------
// Builder used by tests to create synthetic BRRES files (TEX0 / PLT0 only).

function buildGroup(names: string[], dataOffsetsFromGroup: number[], nameOffsetsFromGroup: number[]): Buffer {
  const g = Buffer.alloc(8 + (names.length + 1) * 16);
  g.writeInt32BE(g.length, 0);
  g.writeInt32BE(names.length, 4);
  g.writeUInt16BE(0xffff, 8); // reference entry
  names.forEach((_, i) => {
    const e = 8 + (i + 1) * 16;
    g.writeUInt16BE(0, e);
    g.writeInt32BE(nameOffsetsFromGroup[i], e + 8);
    g.writeInt32BE(dataOffsetsFromGroup[i], e + 12);
  });
  return g;
}

export function buildTex0(format: number, width: number, height: number, pixels: Uint8Array): Buffer {
  const h = Buffer.alloc(0x40);
  h.write("TEX0", 0, "latin1");
  h.writeInt32BE(0x40 + pixels.length, 4);
  h.writeInt32BE(3, 8);
  h.writeInt32BE(0x40, 0x10);
  h.writeInt32BE(0, 0x14);
  h.writeInt32BE(isPaletted(format) ? 1 : 0, 0x18);
  h.writeUInt16BE(width, 0x1c);
  h.writeUInt16BE(height, 0x1e);
  h.writeUInt32BE(format, 0x20);
  h.writeInt32BE(1, 0x24);
  return Buffer.concat([h, Buffer.from(pixels)]);
}

export function buildPlt0(format: number, entries: number[]): Buffer {
  const h = Buffer.alloc(0x40);
  h.write("PLT0", 0, "latin1");
  const data = Buffer.alloc(Math.ceil((entries.length * 2) / 32) * 32);
  entries.forEach((v, i) => data.writeUInt16BE(v, i * 2));
  h.writeInt32BE(0x40 + data.length, 4);
  h.writeInt32BE(1, 8);
  h.writeInt32BE(0x40, 0x10);
  h.writeUInt32BE(format, 0x18);
  h.writeUInt16BE(entries.length, 0x1c);
  return Buffer.concat([h, data]);
}

/** Builds a BRRES with a "Textures(NW4R)" folder and optionally "Palettes(NW4R)". */
export function buildBrres(
  textures: { name: string; tex0: Buffer }[],
  palettes: { name: string; plt0: Buffer }[] = [],
) {
  const folders: { name: string; items: { name: string; data: Buffer }[] }[] = [
    { name: "Textures(NW4R)", items: textures.map((t) => ({ name: t.name, data: t.tex0 })) },
  ];
  if (palettes.length) {
    folders.push({ name: "Palettes(NW4R)", items: palettes.map((p) => ({ name: p.name, data: p.plt0 })) });
  }
  // Layout: header(0x10) | root tag+size(8) | root group | folder groups | sub-files | string table
  const rootOff = 0x10;
  const rootGroupOff = rootOff + 8;
  const rootGroupSize = 8 + (folders.length + 1) * 16;
  const folderGroupOffs: number[] = [];
  let cur = rootGroupOff + rootGroupSize;
  for (const f of folders) {
    folderGroupOffs.push(cur);
    cur += 8 + (f.items.length + 1) * 16;
  }
  cur = Math.ceil(cur / 0x20) * 0x20;
  const fileOffs: number[][] = folders.map((f) =>
    f.items.map((it) => {
      const o = cur;
      cur = Math.ceil((cur + it.data.length) / 0x20) * 0x20;
      return o;
    }),
  );
  // String table: [u32 len][chars\0] padded to 4
  const strings: Buffer[] = [];
  const strOff = new Map<string, number>();
  const addStr = (s: string) => {
    if (strOff.has(s)) {
      return strOff.get(s)!;
    }
    const b = Buffer.alloc(Math.ceil((4 + s.length + 1) / 4) * 4);
    b.writeUInt32BE(s.length, 0);
    b.write(s, 4, "latin1");
    const off = cur + strings.reduce((a, x) => a + x.length, 0) + 4;
    strings.push(b);
    strOff.set(s, off);
    return off;
  };
  folders.forEach((f) => {
    addStr(f.name);
    f.items.forEach((it) => addStr(it.name));
  });
  const total = cur + strings.reduce((a, x) => a + x.length, 0);
  const out = Buffer.alloc(total);
  out.write("bres", 0, "latin1");
  out.writeUInt16BE(0xfeff, 4);
  out.writeUInt32BE(total, 8);
  out.writeUInt16BE(rootOff, 0x0c);
  out.writeUInt16BE(1 + folders.reduce((a, f) => a + f.items.length, 0), 0x0e);
  out.write("root", rootOff, "latin1");
  out.writeInt32BE(rootGroupSize + 8 + folderGroupOffs.length, rootOff + 4);
  buildGroup(
    folders.map((f) => f.name),
    folderGroupOffs.map((o) => o - rootGroupOff),
    folders.map((f) => strOff.get(f.name)! - rootGroupOff),
  ).copy(out, rootGroupOff);
  folders.forEach((f, fi) => {
    const go = folderGroupOffs[fi];
    buildGroup(
      f.items.map((it) => it.name),
      fileOffs[fi].map((o) => o - go),
      f.items.map((it) => strOff.get(it.name)! - go),
    ).copy(out, go);
    f.items.forEach((it, ii) => {
      const o = fileOffs[fi][ii];
      it.data.copy(out, o);
      out.writeInt32BE(-o, o + 0x0c);
      out.writeInt32BE(strOff.get(it.name)! - o, o + 0x14);
    });
  });
  let so = cur;
  for (const s of strings) {
    s.copy(out, so);
    so += s.length;
  }
  return out;
}
