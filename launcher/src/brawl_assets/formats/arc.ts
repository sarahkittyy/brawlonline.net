// Brawl "ARC" archives (.pac / .pcs): magic "ARC\0", 0x40-byte header, then 0x20-byte entry headers each
// followed by the entry data, aligned to 0x20.
//
// Layout ported from BrawlLib (BrawlCrate, GPL-3.0): BrawlLib/SSBB/Types/ARC.cs,
// BrawlLib/SSBB/ResourceNodes/Archives/ARCNode.cs. https://github.com/soopercool101/BrawlCrate
//
// SPDX-License-Identifier: GPL-3.0-or-later

export const ARC_FILE_TYPES = [
  "None",
  "MiscData",
  "ModelData",
  "TextureData",
  "AnimationData",
  "SceneData",
  "Type6",
  "GroupedArchive",
  "EffectData",
] as const;

export type ArcEntry = {
  /** Stable name: `${type}[${fileIndex}]`, with `#n` appended for duplicates. */
  name: string;
  type: number;
  typeName: string;
  fileIndex: number;
  groupIndex: number;
  redirectIndex: number;
  /** Raw (possibly compressed) entry data. */
  data: Buffer;
};

export type ArcArchive = { name: string; entries: ArcEntry[] };

export function isArc(buf: Uint8Array): boolean {
  return buf.length >= 0x40 && buf[0] === 0x41 && buf[1] === 0x52 && buf[2] === 0x43 && buf[3] === 0;
}

export function parseArc(buf: Buffer): ArcArchive {
  if (!isArc(buf)) {
    throw new Error("Not a Brawl ARC archive");
  }
  const numFiles = buf.readUInt16BE(6);
  const nameEnd = buf.indexOf(0, 0x10);
  const name = buf.toString("latin1", 0x10, nameEnd < 0 || nameEnd > 0x40 ? 0x40 : nameEnd);
  const entries: ArcEntry[] = [];
  const seen = new Map<string, number>();
  let off = 0x40;
  for (let i = 0; i < numFiles; i++) {
    if (off + 0x20 > buf.length) {
      throw new Error(`ARC ${name}: entry ${i} header out of range`);
    }
    const type = buf.readInt16BE(off);
    const fileIndex = buf.readInt16BE(off + 2);
    const size = buf.readInt32BE(off + 4);
    const groupIndex = buf[off + 8];
    const redirectIndex = buf.readInt16BE(off + 10);
    const dataStart = off + 0x20;
    if (size < 0 || dataStart + size > buf.length) {
      throw new Error(`ARC ${name}: entry ${i} data out of range`);
    }
    const typeName = ARC_FILE_TYPES[type] ?? `Type${type}`;
    let entryName = `${typeName}[${fileIndex}]`;
    const dup = seen.get(entryName) ?? 0;
    seen.set(entryName, dup + 1);
    if (dup > 0) {
      entryName += `#${dup}`;
    }
    entries.push({
      name: entryName,
      type,
      typeName,
      fileIndex,
      groupIndex,
      redirectIndex,
      data: buf.subarray(dataStart, dataStart + size),
    });
    off = align(dataStart + size, 0x20);
  }
  return { name, entries };
}

/** Builds an ARC archive (used by tests to make synthetic fixtures). */
export function buildArc(name: string, files: { type: number; fileIndex: number; data: Uint8Array }[]): Buffer {
  const parts: Buffer[] = [];
  const header = Buffer.alloc(0x40);
  header.write("ARC\0", 0, "latin1");
  header.writeUInt16BE(0x0101, 4);
  header.writeUInt16BE(files.length, 6);
  header.write(name.slice(0, 47), 0x10, "latin1");
  parts.push(header);
  for (const f of files) {
    const eh = Buffer.alloc(0x20);
    eh.writeInt16BE(f.type, 0);
    eh.writeInt16BE(f.fileIndex, 2);
    eh.writeInt32BE(f.data.length, 4);
    eh.writeInt16BE(-1, 10);
    parts.push(eh, Buffer.from(f.data));
    const pad = align(f.data.length, 0x20) - f.data.length;
    if (pad) {
      parts.push(Buffer.alloc(pad));
    }
  }
  return Buffer.concat(parts);
}

function align(v: number, a: number): number {
  return Math.ceil(v / a) * a;
}
