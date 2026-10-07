// Read-only FAT32 (and FAT16) reader for disk images such as Dolphin's virtual SD card (sd.raw).
// Supports MBR-partitioned and partitionless ("superfloppy") images and VFAT long file names.
// Reads go straight to the file through a FileHandle; nothing but small FAT windows is cached.
//
// SPDX-License-Identifier: GPL-3.0-or-later

import fs from "fs";

export type FatDirEntry = {
  name: string;
  shortName: string;
  isDirectory: boolean;
  size: number;
  firstCluster: number;
};

export interface ByteReader {
  read(position: number, length: number): Promise<Buffer>;
  close(): Promise<void>;
}

class FileReader implements ByteReader {
  constructor(private readonly fh: fs.promises.FileHandle) {}
  async read(position: number, length: number): Promise<Buffer> {
    const buf = Buffer.alloc(length);
    let done = 0;
    while (done < length) {
      const { bytesRead } = await this.fh.read(buf, done, length - done, position + done);
      if (bytesRead === 0) {
        throw new Error(`FAT: read past end of image at ${position + done}`);
      }
      done += bytesRead;
    }
    return buf;
  }
  close() {
    return this.fh.close();
  }
}

/** In-memory reader (used by tests). */
export class BufferReader implements ByteReader {
  constructor(private readonly buf: Buffer) {}
  async read(position: number, length: number): Promise<Buffer> {
    if (position + length > this.buf.length) {
      throw new Error("FAT: read past end of image");
    }
    return this.buf.subarray(position, position + length);
  }
  async close() {
    // nothing to release
  }
}

const FAT_WINDOW = 64 * 1024;

export class FatImage {
  private readonly fatCache = new Map<number, Buffer>();
  private readonly dirCache = new Map<string, FatDirEntry[]>();

  private constructor(
    private readonly reader: ByteReader,
    readonly fatType: 16 | 32,
    /** Byte offset of the FAT volume inside the image (0 for a partitionless image). */
    readonly partStart: number,
    private readonly bytesPerSector: number,
    private readonly sectorsPerCluster: number,
    private readonly fatStart: number,
    private readonly rootDirStart: number,
    private readonly rootDirEntries: number,
    private readonly dataStart: number,
    private readonly rootCluster: number,
    private readonly clusterCount: number,
    private readonly fatBytes: number,
  ) {}

  static async open(path: string): Promise<FatImage> {
    const fh = await fs.promises.open(path, "r");
    try {
      return await FatImage.fromReader(new FileReader(fh));
    } catch (err) {
      await fh.close();
      throw err;
    }
  }

  static async fromReader(reader: ByteReader): Promise<FatImage> {
    const s0 = await reader.read(0, 512);
    let partStart = 0;
    if (!looksLikeBpb(s0)) {
      if (s0[510] !== 0x55 || s0[511] !== 0xaa) {
        throw new Error("FAT: no MBR or boot sector signature");
      }
      let found = -1;
      for (let i = 0; i < 4; i++) {
        const e = 0x1be + i * 16;
        const type = s0[e + 4];
        if ([0x01, 0x04, 0x06, 0x0b, 0x0c, 0x0e].includes(type)) {
          found = s0.readUInt32LE(e + 8);
          break;
        }
      }
      if (found < 0) {
        throw new Error("FAT: no FAT partition in MBR");
      }
      partStart = found * 512;
    }
    const bs = partStart === 0 ? s0 : await reader.read(partStart, 512);
    if (!looksLikeBpb(bs)) {
      throw new Error("FAT: invalid boot sector");
    }
    const bps = bs.readUInt16LE(11);
    const spc = bs[13];
    const reserved = bs.readUInt16LE(14);
    const numFats = bs[16];
    const rootEntries = bs.readUInt16LE(17);
    const totSec = bs.readUInt16LE(19) || bs.readUInt32LE(32);
    const fatSz = bs.readUInt16LE(22) || bs.readUInt32LE(36);
    const rootDirSectors = Math.ceil((rootEntries * 32) / bps);
    const firstData = reserved + numFats * fatSz + rootDirSectors;
    const clusters = Math.floor((totSec - firstData) / spc);
    if (clusters < 4085) {
      throw new Error("FAT12 volumes are not supported");
    }
    const fatType = clusters < 65525 ? 16 : 32;
    return new FatImage(
      reader,
      fatType,
      partStart,
      bps,
      spc,
      partStart + reserved * bps,
      partStart + (reserved + numFats * fatSz) * bps,
      rootEntries,
      partStart + firstData * bps,
      fatType === 32 ? bs.readUInt32LE(44) : 0,
      clusters,
      fatSz * bps,
    );
  }

  get clusterSize() {
    return this.bytesPerSector * this.sectorsPerCluster;
  }

  async close() {
    await this.reader.close();
  }

  private async fatEntry(cluster: number): Promise<number> {
    const bytes = this.fatType === 32 ? 4 : 2;
    const off = cluster * bytes;
    const win = Math.floor(off / FAT_WINDOW);
    let buf = this.fatCache.get(win);
    if (!buf) {
      const start = win * FAT_WINDOW;
      buf = await this.reader.read(this.fatStart + start, Math.min(FAT_WINDOW, this.fatBytes - start));
      if (this.fatCache.size > 256) {
        this.fatCache.clear();
      }
      this.fatCache.set(win, buf);
    }
    const o = off - win * FAT_WINDOW;
    if (o + bytes > buf.length) {
      throw new Error(`FAT: cluster ${cluster} beyond the FAT`);
    }
    return this.fatType === 32 ? buf.readUInt32LE(o) & 0x0fffffff : buf.readUInt16LE(o);
  }

  private isEnd(v: number) {
    return this.fatType === 32 ? v >= 0x0ffffff8 : v >= 0xfff8;
  }

  /** Follows a cluster chain, returning runs of contiguous clusters. */
  private async chain(first: number): Promise<{ start: number; count: number }[]> {
    const runs: { start: number; count: number }[] = [];
    let c = first;
    let guard = 0;
    while (c >= 2 && !this.isEnd(c) && c < this.clusterCount + 2) {
      const last = runs[runs.length - 1];
      if (last && last.start + last.count === c) {
        last.count++;
      } else {
        runs.push({ start: c, count: 1 });
      }
      c = await this.fatEntry(c);
      if (++guard > this.clusterCount) {
        throw new Error("FAT: cluster chain loop");
      }
    }
    return runs;
  }

  private clusterOffset(c: number) {
    return this.dataStart + (c - 2) * this.clusterSize;
  }

  private async readChain(first: number, size?: number): Promise<Buffer> {
    const runs = await this.chain(first);
    const parts: Buffer[] = [];
    let remaining = size ?? Number.POSITIVE_INFINITY;
    for (const r of runs) {
      if (remaining <= 0) {
        break;
      }
      const len = Math.min(r.count * this.clusterSize, remaining);
      parts.push(await this.reader.read(this.clusterOffset(r.start), len));
      remaining -= len;
    }
    const out = Buffer.concat(parts);
    if (size !== undefined && out.length < size) {
      throw new Error("FAT: file is shorter than its directory entry");
    }
    return out;
  }

  private async readDirRaw(cluster: number): Promise<Buffer> {
    if (cluster === 0 && this.fatType === 16) {
      return this.reader.read(this.rootDirStart, this.rootDirEntries * 32);
    }
    return this.readChain(cluster === 0 ? this.rootCluster : cluster);
  }

  /** Lists a directory by path ("/" or "" for the root). Matching is case-insensitive. */
  async readdir(dirPath: string): Promise<FatDirEntry[]> {
    const key = normalize(dirPath).toLowerCase();
    const cached = this.dirCache.get(key);
    if (cached) {
      return cached;
    }
    let entries: FatDirEntry[];
    if (key === "") {
      entries = parseDir(await this.readDirRaw(0));
    } else {
      const e = await this.stat(dirPath);
      if (!e || !e.isDirectory) {
        throw new Error(`FAT: not a directory: ${dirPath}`);
      }
      entries = parseDir(await this.readDirRaw(e.firstCluster));
    }
    this.dirCache.set(key, entries);
    return entries;
  }

  async stat(path: string): Promise<FatDirEntry | null> {
    const parts = normalize(path).split("/").filter(Boolean);
    if (parts.length === 0) {
      return { name: "", shortName: "", isDirectory: true, size: 0, firstCluster: 0 };
    }
    const parent = parts.slice(0, -1).join("/");
    let list: FatDirEntry[];
    try {
      list = await this.readdir(parent);
    } catch {
      return null;
    }
    const want = parts[parts.length - 1].toLowerCase();
    return list.find((e) => e.name.toLowerCase() === want || e.shortName.toLowerCase() === want) ?? null;
  }

  async exists(path: string): Promise<boolean> {
    return (await this.stat(path)) !== null;
  }

  async readFile(path: string): Promise<Buffer> {
    const e = await this.stat(path);
    if (!e) {
      throw new Error(`FAT: no such file: ${path}`);
    }
    if (e.isDirectory) {
      throw new Error(`FAT: is a directory: ${path}`);
    }
    if (e.size === 0) {
      return Buffer.alloc(0);
    }
    return this.readChain(e.firstCluster, e.size);
  }
}

function normalize(p: string) {
  return p.replace(/\\/g, "/").replace(/^\/+|\/+$/g, "");
}

function looksLikeBpb(s: Buffer): boolean {
  if (s[0] !== 0xeb && s[0] !== 0xe9) {
    return false;
  }
  const bps = s.readUInt16LE(11);
  const spc = s[13];
  return [512, 1024, 2048, 4096].includes(bps) && spc > 0 && (spc & (spc - 1)) === 0 && s[16] >= 1 && s[16] <= 2;
}

function lfnChecksum(raw: Buffer, off: number): number {
  let sum = 0;
  for (let i = 0; i < 11; i++) {
    sum = (((sum & 1) << 7) | (sum >> 1)) + raw[off + i];
    sum &= 0xff;
  }
  return sum;
}

function parseDir(raw: Buffer): FatDirEntry[] {
  const out: FatDirEntry[] = [];
  let lfnParts: string[] = [];
  let lfnSum = -1;
  for (let off = 0; off + 32 <= raw.length; off += 32) {
    const first = raw[off];
    if (first === 0x00) {
      break;
    }
    if (first === 0xe5) {
      lfnParts = [];
      continue;
    }
    const attr = raw[off + 11];
    if (attr === 0x0f) {
      const seq = first & 0x3f;
      if (first & 0x40) {
        lfnParts = [];
      }
      let s = "";
      for (const [o, n] of [
        [1, 5],
        [14, 6],
        [28, 2],
      ]) {
        for (let i = 0; i < n; i++) {
          const ch = raw.readUInt16LE(off + o + i * 2);
          if (ch === 0x0000 || ch === 0xffff) {
            break;
          }
          s += String.fromCharCode(ch);
        }
      }
      lfnParts[seq - 1] = s;
      lfnSum = raw[off + 13];
      continue;
    }
    if (attr & 0x08) {
      lfnParts = [];
      continue; // volume label
    }
    const base = raw.toString("latin1", off, off + 8).trimEnd();
    const ext = raw.toString("latin1", off + 8, off + 11).trimEnd();
    const caseFlags = raw[off + 12];
    const b = caseFlags & 0x08 ? base.toLowerCase() : base;
    const e = caseFlags & 0x10 ? ext.toLowerCase() : ext;
    let shortName = ext ? `${b}.${e}` : b;
    if (first === 0x05) {
      shortName = "å" + shortName.slice(1);
    }
    let name = shortName;
    if (lfnParts.length && lfnSum === lfnChecksum(raw, off)) {
      name = lfnParts.join("");
    }
    lfnParts = [];
    if (shortName === "." || shortName === "..") {
      continue;
    }
    out.push({
      name,
      shortName,
      isDirectory: (attr & 0x10) !== 0,
      size: raw.readUInt32LE(off + 28),
      firstCluster: ((raw.readUInt16LE(off + 20) << 16) | raw.readUInt16LE(off + 26)) >>> 0,
    });
  }
  return out;
}
