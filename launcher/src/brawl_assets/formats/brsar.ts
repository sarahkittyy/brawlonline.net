// NW4R sound archives (BRSAR, magic "RSAR"): the SYMB (names), INFO (sounds, files, groups) and FILE blocks.
//
// A sound entry names a file (an RWSD for wave sounds, an RSEQ for sequences) and, for wave sounds, the index of its
// entry inside that RWSD. A file is stored inside one or more groups: each group has a header area (the files'
// headers back to back) and a wave data area (the files' sample data), both at absolute offsets in the archive.
//
// Layout ported from BrawlLib (BrawlCrate, GPL-3.0): BrawlLib/SSBB/Types/Audio/RSAR.cs,
// BrawlLib/SSBB/ResourceNodes/RSAR/*. https://github.com/soopercool101/BrawlCrate
//
// SPDX-License-Identifier: GPL-3.0-or-later

export const enum SoundType {
  Sequence = 1,
  Stream = 2,
  Wave = 3,
}

export const SOUND_TYPE_NAMES: Record<number, string> = { 1: "SEQ", 2: "STRM", 3: "WAVE" };

export type BrsarSound = {
  id: number;
  /** SYMB name (e.g. "snd_se_..."), or `sound_<id>` when the archive has no symbols. */
  name: string;
  fileId: number;
  playerId: number;
  volume: number;
  soundType: number;
  /** Wave sounds: entry index inside the RWSD. Sequences: the label/data id. -1 otherwise. */
  index: number;
};

export type BrsarFile = {
  id: number;
  headerLength: number;
  dataLength: number;
  /** External file path (streams), relative to the sound folder. */
  externalName: string | null;
  /** Where the file is stored: (group, item index) pairs. */
  positions: { groupId: number; index: number }[];
};

export type BrsarGroupItem = {
  fileId: number;
  /** Relative to the group's header offset. */
  headerOffset: number;
  headerLength: number;
  /** Relative to the group's wave data offset. */
  dataOffset: number;
  dataLength: number;
};

export type BrsarGroup = {
  id: number;
  name: string | null;
  /** Absolute offsets into the archive. */
  headerOffset: number;
  headerLength: number;
  waveDataOffset: number;
  waveDataLength: number;
  items: BrsarGroupItem[];
};

export type Brsar = {
  version: number;
  sounds: BrsarSound[];
  files: BrsarFile[];
  groups: BrsarGroup[];
  strings: string[];
};

/** The bytes of one file of the archive: its header (RWSD/RSEQ/RBNK) and its sample data. */
export type BrsarFileData = { header: Buffer; waveData: Buffer; groupId: number };

export function isBrsar(buf: Uint8Array): boolean {
  return buf.length >= 0x28 && buf[0] === 0x52 && buf[1] === 0x53 && buf[2] === 0x41 && buf[3] === 0x52;
}

function check(cond: boolean, msg: string): asserts cond {
  if (!cond) {
    throw new Error(`BRSAR: ${msg}`);
  }
}

function cString(buf: Buffer, off: number): string {
  const end = buf.indexOf(0, off);
  return buf.toString("latin1", off, end < 0 ? buf.length : end);
}

export function parseBrsar(buf: Buffer): Brsar {
  check(isBrsar(buf), "not an RSAR file");
  check(buf.readUInt16BE(4) === 0xfeff, "not big-endian");
  const version = buf.readUInt16BE(6);
  const symbOffset = buf.readUInt32BE(0x10);
  const infoOffset = buf.readUInt32BE(0x18);
  check(infoOffset > 0 && infoOffset + 0x38 <= buf.length, "INFO block out of range");
  check(buf.toString("latin1", infoOffset, infoOffset + 4) === "INFO", "missing INFO block");

  // SYMB: string table (offsets relative to the block + 8).
  const strings: string[] = [];
  if (symbOffset > 0 && buf.toString("latin1", symbOffset, symbOffset + 4) === "SYMB") {
    const base = symbOffset + 8;
    const table = base + buf.readUInt32BE(base);
    const count = buf.readUInt32BE(table);
    check(table + 4 + count * 4 <= buf.length, "SYMB string table out of range");
    for (let i = 0; i < count; i++) {
      strings.push(cString(buf, base + buf.readUInt32BE(table + 4 + i * 4)));
    }
  }
  const str = (id: number) => (id >= 0 && id < strings.length ? strings[id] : null);

  // INFO: "ruint" references are 8 bytes (u8 isOffset, u8 dataType, u16 pad, u32 offset), relative to INFO + 8.
  const base = infoOffset + 8;
  const ref = (off: number) => ({ dataType: buf[off + 1], offset: buf.readUInt32BE(off + 4), isOffset: buf[off] });
  const at = (off: number) => base + ref(off).offset;
  /** A RuintList: u32 count followed by references. Returns the absolute addresses of the entries. */
  const list = (addr: number): number[] => {
    const n = buf.readUInt32BE(addr);
    check(addr + 4 + n * 8 <= buf.length, "INFO list out of range");
    const out: number[] = [];
    for (let i = 0; i < n; i++) {
      out.push(base + buf.readUInt32BE(addr + 4 + i * 8 + 4));
    }
    return out;
  };

  const sounds: BrsarSound[] = list(at(base + 0x00)).map((e, id) => {
    const stringId = buf.readInt32BE(e);
    const soundType = buf[e + 0x16];
    const detail = ref(e + 0x18);
    const index = detail.offset || detail.isOffset ? buf.readInt32BE(base + detail.offset) : -1;
    return {
      id,
      name: str(stringId) ?? `sound_${id}`,
      fileId: buf.readInt32BE(e + 4),
      playerId: buf.readInt32BE(e + 8),
      volume: buf[e + 0x14],
      soundType: soundType || detail.dataType,
      index,
    };
  });

  const files: BrsarFile[] = list(at(base + 0x18)).map((e, id) => {
    const ext = ref(e + 0x0c);
    const pos = ref(e + 0x14);
    const positions =
      pos.offset || pos.isOffset
        ? list(base + pos.offset).map((p) => ({ groupId: buf.readInt32BE(p), index: buf.readInt32BE(p + 4) }))
        : [];
    return {
      id,
      headerLength: buf.readUInt32BE(e),
      dataLength: buf.readUInt32BE(e + 4),
      externalName: ext.offset ? cString(buf, base + ext.offset) : null,
      positions,
    };
  });

  const groups: BrsarGroup[] = list(at(base + 0x20)).map((e, id) => {
    const items = ref(e + 0x20);
    return {
      id,
      name: str(buf.readInt32BE(e)),
      headerOffset: buf.readUInt32BE(e + 0x10),
      headerLength: buf.readUInt32BE(e + 0x14),
      waveDataOffset: buf.readUInt32BE(e + 0x18),
      waveDataLength: buf.readUInt32BE(e + 0x1c),
      items:
        items.offset || items.isOffset
          ? list(base + items.offset).map((i) => ({
              fileId: buf.readInt32BE(i),
              headerOffset: buf.readUInt32BE(i + 4),
              headerLength: buf.readUInt32BE(i + 8),
              dataOffset: buf.readUInt32BE(i + 0x0c),
              dataLength: buf.readUInt32BE(i + 0x10),
            }))
          : [],
    };
  });

  return { version, sounds, files, groups, strings };
}

export function findSound(brsar: Brsar, nameOrId: string | number): BrsarSound | null {
  if (typeof nameOrId === "number" || /^\d+$/.test(nameOrId)) {
    return brsar.sounds[Number(nameOrId)] ?? null;
  }
  return brsar.sounds.find((s) => s.name === nameOrId) ?? null;
}

/** The groups that store a file, in the archive's order. */
export function fileGroups(brsar: Brsar, fileId: number): BrsarGroup[] {
  const file = brsar.files[fileId];
  return (file?.positions ?? []).map((p) => brsar.groups[p.groupId]).filter((g): g is BrsarGroup => !!g);
}

/** Reads a file's header and sample data out of the archive (from the first group that holds it). */
export function readBrsarFile(buf: Buffer, brsar: Brsar, fileId: number): BrsarFileData | null {
  const file = brsar.files[fileId];
  if (!file) {
    return null;
  }
  for (const pos of file.positions) {
    const group = brsar.groups[pos.groupId];
    const item = group?.items[pos.index];
    if (!item || item.fileId !== fileId) {
      continue;
    }
    const h = group.headerOffset + item.headerOffset;
    const d = group.waveDataOffset + item.dataOffset;
    if (h + item.headerLength > buf.length || d + item.dataLength > buf.length) {
      throw new Error(`BRSAR: file ${fileId} in group ${group.id} is out of range`);
    }
    return {
      header: buf.subarray(h, h + item.headerLength),
      waveData: buf.subarray(d, d + item.dataLength),
      groupId: group.id,
    };
  }
  return null;
}

// ---------------------------------------------------------------------------------------------------------------
// Builder, for synthetic test fixtures.

export type BuildBrsarSound = {
  name: string;
  fileId: number;
  /** Entry index in the RWSD. */
  index: number;
  soundType?: number;
  volume?: number;
};

export type BuildBrsarGroup = { name: string; files: { fileId: number; header: Uint8Array; waveData: Uint8Array }[] };

/**
 * Builds a minimal RSAR: one SYMB string table (no search tries), INFO with sounds, files and groups, and a FILE
 * block holding each group's headers followed by its wave data. Every file must be in exactly one group.
 */
export function buildBrsar(sounds: BuildBrsarSound[], groups: BuildBrsarGroup[]): Buffer {
  const align = (v: number, a = 0x20) => Math.ceil(v / a) * a;
  const names = [...sounds.map((s) => s.name), ...groups.map((g) => g.name)];

  // SYMB
  const strOff: number[] = [];
  const strData: Buffer[] = [];
  let strPos = 0x14 + 4 + names.length * 4;
  for (const n of names) {
    strOff.push(strPos);
    const b = Buffer.from(`${n}\0`, "latin1");
    strData.push(b);
    strPos += b.length;
  }
  const symbBody = Buffer.alloc(align(8 + strPos) - 8);
  symbBody.writeUInt32BE(0x14, 0);
  symbBody.writeUInt32BE(names.length, 0x14);
  strOff.forEach((o, i) => symbBody.writeUInt32BE(o, 0x18 + i * 4));
  let p = 0x18 + names.length * 4;
  for (const b of strData) {
    b.copy(symbBody, p);
    p += b.length;
  }
  const symb = Buffer.concat([Buffer.from("SYMB", "latin1"), Buffer.alloc(4), symbBody]);
  symb.writeUInt32BE(symb.length, 4);

  // FILE block layout
  const fileCount = Math.max(-1, ...groups.flatMap((g) => g.files.map((f) => f.fileId))) + 1;
  type Placed = { groupId: number; index: number; item: BrsarGroupItem };
  const placed: (Placed | undefined)[] = new Array(fileCount);
  const groupLayout: { headerOffset: number; headerLength: number; dataOffset: number; dataLength: number }[] = [];
  const fileChunks: Buffer[] = [];
  let filePos = 0x20; // relative to the FILE block
  groups.forEach((g, gi) => {
    const headerStart = filePos;
    g.files.forEach((f, index) => {
      const h = Buffer.alloc(align(f.header.length));
      Buffer.from(f.header).copy(h);
      fileChunks.push(h);
      placed[f.fileId] = {
        groupId: gi,
        index,
        item: {
          fileId: f.fileId,
          headerOffset: filePos - headerStart,
          headerLength: h.length,
          dataOffset: 0,
          dataLength: 0,
        },
      };
      filePos += h.length;
    });
    const dataStart = filePos;
    g.files.forEach((f) => {
      const d = Buffer.alloc(align(f.waveData.length));
      Buffer.from(f.waveData).copy(d);
      fileChunks.push(d);
      const item = placed[f.fileId]!.item;
      item.dataOffset = filePos - dataStart;
      item.dataLength = d.length;
      filePos += d.length;
    });
    groupLayout.push({
      headerOffset: headerStart,
      headerLength: dataStart - headerStart,
      dataOffset: dataStart,
      dataLength: filePos - dataStart,
    });
  });

  // INFO (offsets relative to INFO + 8). Laid out as: collection (6 refs), then tables and entries appended.
  const chunks: Buffer[] = [];
  let infoPos = 0x30;
  const append = (b: Buffer) => {
    const off = infoPos;
    chunks.push(b);
    infoPos += b.length;
    return off;
  };
  const refBuf = (b: Buffer, at: number, dataType: number, offset: number) => {
    b[at] = 1;
    b[at + 1] = dataType;
    b.writeUInt32BE(offset, at + 4);
  };
  const appendList = (entries: number[]) => {
    const b = Buffer.alloc(4 + entries.length * 8);
    b.writeUInt32BE(entries.length, 0);
    entries.forEach((e, i) => refBuf(b, 4 + i * 8, 0, e));
    return append(b);
  };

  const soundEntries = sounds.map((s, i) => {
    const detail = Buffer.alloc(0x10);
    detail.writeInt32BE(s.index, 0);
    const detailOff = append(detail);
    const e = Buffer.alloc(0x2c);
    e.writeInt32BE(i, 0);
    e.writeInt32BE(s.fileId, 4);
    e[0x14] = s.volume ?? 0x7f;
    e[0x16] = s.soundType ?? SoundType.Wave;
    refBuf(e, 0x18, s.soundType ?? SoundType.Wave, detailOff);
    return append(e);
  });
  const fileEntries = Array.from({ length: fileCount }, (_, id) => {
    const pl = placed[id];
    const posEntries: number[] = [];
    if (pl) {
      const pe = Buffer.alloc(8);
      pe.writeInt32BE(pl.groupId, 0);
      pe.writeInt32BE(pl.index, 4);
      posEntries.push(append(pe));
    }
    const posList = appendList(posEntries);
    const e = Buffer.alloc(0x1c);
    e.writeUInt32BE(pl?.item.headerLength ?? 0, 0);
    e.writeUInt32BE(pl?.item.dataLength ?? 0, 4);
    e.writeInt32BE(-1, 8);
    refBuf(e, 0x14, 0, posList);
    return append(e);
  });
  // Group entries need absolute offsets, filled in once the INFO size is known.
  const groupEntryOffsets = groups.map((g, gi) => {
    const items = g.files.map((f) => {
      const it = placed[f.fileId]!.item;
      const b = Buffer.alloc(0x18);
      b.writeInt32BE(it.fileId, 0);
      b.writeUInt32BE(it.headerOffset, 4);
      b.writeUInt32BE(it.headerLength, 8);
      b.writeUInt32BE(it.dataOffset, 0x0c);
      b.writeUInt32BE(it.dataLength, 0x10);
      return append(b);
    });
    const itemList = appendList(items);
    const e = Buffer.alloc(0x28);
    e.writeInt32BE(sounds.length + gi, 0);
    e.writeInt32BE(-1, 4);
    refBuf(e, 0x20, 0, itemList);
    return { off: append(e), buf: e };
  });
  const soundList = appendList(soundEntries);
  const bankList = appendList([]);
  const playerList = appendList([]);
  const fileList = appendList(fileEntries);
  const groupList = appendList(groupEntryOffsets.map((g) => g.off));
  const footer = append(Buffer.alloc(0x14));
  const collection = Buffer.alloc(0x30);
  [soundList, bankList, playerList, fileList, groupList, footer].forEach((o, i) => refBuf(collection, i * 8, 0, o));

  const infoLen = align(8 + infoPos);
  const symbOffset = 0x40;
  const infoOffset = symbOffset + symb.length;
  const fileOffset = infoOffset + infoLen;
  for (let gi = 0; gi < groups.length; gi++) {
    const l = groupLayout[gi];
    const e = groupEntryOffsets[gi].buf;
    e.writeUInt32BE(fileOffset + l.headerOffset, 0x10);
    e.writeUInt32BE(l.headerLength, 0x14);
    e.writeUInt32BE(fileOffset + l.dataOffset, 0x18);
    e.writeUInt32BE(l.dataLength, 0x1c);
  }
  const infoBlock = Buffer.alloc(infoLen);
  infoBlock.write("INFO", 0, "latin1");
  infoBlock.writeUInt32BE(infoLen, 4);
  collection.copy(infoBlock, 8);
  let q = 8 + 0x30;
  for (const c of chunks) {
    c.copy(infoBlock, q);
    q += c.length;
  }

  const fileBlock = Buffer.concat([Buffer.alloc(0x20), ...fileChunks]);
  fileBlock.write("FILE", 0, "latin1");
  fileBlock.writeUInt32BE(fileBlock.length, 4);

  const header = Buffer.alloc(0x40);
  header.write("RSAR", 0, "latin1");
  header.writeUInt16BE(0xfeff, 4);
  header.writeUInt16BE(0x0104, 6);
  header.writeUInt32BE(fileOffset + fileBlock.length, 8);
  header.writeUInt16BE(0x40, 0x0c);
  header.writeUInt16BE(3, 0x0e);
  header.writeUInt32BE(symbOffset, 0x10);
  header.writeUInt32BE(symb.length, 0x14);
  header.writeUInt32BE(infoOffset, 0x18);
  header.writeUInt32BE(infoLen, 0x1c);
  header.writeUInt32BE(fileOffset, 0x20);
  header.writeUInt32BE(fileBlock.length, 0x24);
  return Buffer.concat([header, symb, infoBlock, fileBlock]);
}
