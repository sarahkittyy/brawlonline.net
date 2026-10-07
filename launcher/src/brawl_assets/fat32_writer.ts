// FAT32 reader/writer for disk images such as Dolphin's virtual SD card (sd.raw).
//
// The write side of `tools/sdcard/fat32.py` ported to TypeScript (same allocation and naming
// rules): partitionless ("superfloppy") images, which Dolphin creates, and MBR images with a
// FAT32 partition; VFAT long names; create directories, add, replace and delete files; both FAT
// copies and the FSInfo free count are updated on flush. `check()` is `patch_sd.py --check`.
//
// The image itself is never loaded: all access is positioned reads and writes on a FileHandle.
// Only the first FAT (4 MB for P+'s 2 GB card) is held in memory while the volume is open.
//
// SPDX-License-Identifier: GPL-3.0-or-later

import fs from "fs";

const ATTR_VOLUME = 0x08;
const ATTR_DIR = 0x10;
const ATTR_ARCHIVE = 0x20;
const ATTR_LFN = 0x0f;
const EOC = 0x0fffffff;
const FREE = 0;
const DIR_ENTRY = 32;

export class FatError extends Error {}

export type FatWriterEntry = {
  /** Long name if present, else the 8.3 name (with NT lower-case flags applied). */
  name: string;
  /** Raw 8.3 name, "NAME.EXT". */
  shortName: string;
  attr: number;
  firstCluster: number;
  size: number;
  isDirectory: boolean;
  /** Absolute image offsets of every 32-byte slot of this entry (LFN slots, then the 8.3 slot). */
  slotOffsets: number[];
};

const SHORT_OK = new Set("ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789$%'-_@~`!(){}^#&");
// Characters that may not appear in a VFAT long name (besides control characters). Slashes and
// backslashes never reach a name: paths are split on both.
const LONG_NAME_INVALID_CHARS = new Set('"*:<>?|');
const isInvalidLongName = (name: string) =>
  [...name].some((ch) => ch.charCodeAt(0) < 0x20 || ch.charCodeAt(0) === 0x7f || LONG_NAME_INVALID_CHARS.has(ch));

function lfnChecksum(short11: Buffer): number {
  let s = 0;
  for (let i = 0; i < 11; i++) {
    s = ((((s & 1) << 7) | (s >> 1)) + short11[i]) & 0xff;
  }
  return s;
}

function splitPath(p: string): string[] {
  return p.replace(/\\/g, "/").split("/").filter(Boolean);
}

function looksLikeFat32Bpb(s: Buffer): boolean {
  if (s[0] !== 0xeb && s[0] !== 0xe9) {
    return false;
  }
  const bps = s.readUInt16LE(11);
  const spc = s[13];
  return (
    [512, 1024, 2048, 4096].includes(bps) &&
    spc > 0 &&
    (spc & (spc - 1)) === 0 &&
    s[16] >= 1 &&
    s[16] <= 2 &&
    s.readUInt16LE(22) === 0 && // FAT32: 16-bit FAT size is zero
    s.readUInt32LE(36) !== 0
  );
}

/** FAT date/time words for a JS date (local time, 2 s resolution). */
function fatTimestamp(d: Date): { date: number; time: number } {
  const year = Math.min(Math.max(d.getFullYear(), 1980), 2107);
  return {
    date: ((year - 1980) << 9) | ((d.getMonth() + 1) << 5) | d.getDate(),
    time: (d.getHours() << 11) | (d.getMinutes() << 5) | Math.floor(d.getSeconds() / 2),
  };
}

export class Fat32Volume {
  private readonly dirtyFatSectors = new Set<number>();
  private freeHint = 2;
  private unsynced = false;

  private constructor(
    private readonly fh: fs.promises.FileHandle,
    readonly writable: boolean,
    /** Byte offset of the volume in the image (0 for a partitionless image). */
    readonly base: number,
    readonly bytesPerSector: number,
    readonly sectorsPerCluster: number,
    private readonly numFats: number,
    private readonly fatSectors: number,
    private readonly fatOffset: number,
    private readonly dataOffset: number,
    readonly rootCluster: number,
    private readonly fsInfoSector: number,
    readonly clusterCount: number,
    /** In-memory copy of the first FAT (entries 0 .. clusterCount + 1). */
    private readonly fat: Uint32Array,
  ) {}

  static async open(imagePath: string, opts: { writable?: boolean } = {}): Promise<Fat32Volume> {
    const writable = opts.writable ?? false;
    const fh = await fs.promises.open(imagePath, writable ? "r+" : "r");
    try {
      return await Fat32Volume.fromHandle(fh, writable);
    } catch (err) {
      await fh.close();
      throw err;
    }
  }

  private static async fromHandle(fh: fs.promises.FileHandle, writable: boolean): Promise<Fat32Volume> {
    const read = (pos: number, len: number) => readExactly(fh, pos, len);
    const s0 = await read(0, 512);
    let base = -1;
    if (looksLikeFat32Bpb(s0)) {
      base = 0;
    } else if (s0[510] === 0x55 && s0[511] === 0xaa) {
      for (let i = 0; i < 4; i++) {
        const e = 0x1be + i * 16;
        const type = s0[e + 4];
        const lba = s0.readUInt32LE(e + 8);
        if (type !== 0 && lba !== 0 && [0x0b, 0x0c, 0x1b, 0x1c].includes(type)) {
          base = lba * 512;
          break;
        }
      }
    }
    if (base < 0) {
      throw new FatError("no FAT32 volume found");
    }
    const bs = base === 0 ? s0 : await read(base, 512);
    if (!looksLikeFat32Bpb(bs)) {
      throw new FatError("not a FAT32 volume");
    }
    const bps = bs.readUInt16LE(11);
    const spc = bs[13];
    const reserved = bs.readUInt16LE(14);
    const numFats = bs[16];
    const totalSectors = bs.readUInt32LE(32) || bs.readUInt16LE(19);
    const fatSectors = bs.readUInt32LE(36);
    const rootCluster = bs.readUInt32LE(44);
    const fsInfoSector = bs.readUInt16LE(48);
    const fatOffset = base + reserved * bps;
    const dataOffset = fatOffset + numFats * fatSectors * bps;
    const clusterCount = Math.floor((totalSectors - (reserved + numFats * fatSectors)) / spc);
    if (clusterCount < 65525) {
      throw new FatError("not a FAT32 volume (too few clusters)");
    }
    const fatBytes = await read(fatOffset, fatSectors * bps);
    const n = Math.min(clusterCount + 2, Math.floor(fatBytes.length / 4));
    const fat = new Uint32Array(n);
    for (let i = 0; i < n; i++) {
      fat[i] = fatBytes.readUInt32LE(i * 4);
    }
    return new Fat32Volume(
      fh,
      writable,
      base,
      bps,
      spc,
      numFats,
      fatSectors,
      fatOffset,
      dataOffset,
      rootCluster,
      fsInfoSector,
      clusterCount,
      fat,
    );
  }

  get clusterSize(): number {
    return this.bytesPerSector * this.sectorsPerCluster;
  }

  /** Flushes pending FAT changes (when writable) and closes the image. */
  async close(): Promise<void> {
    try {
      if (this.writable) {
        await this.flush();
      }
    } finally {
      await this.fh.close();
    }
  }

  // ------------------------------------------------------------------ low level

  private read(pos: number, len: number): Promise<Buffer> {
    return readExactly(this.fh, pos, len);
  }

  private async write(pos: number, data: Buffer): Promise<void> {
    if (!this.writable) {
      throw new FatError("image opened read-only");
    }
    let done = 0;
    while (done < data.length) {
      const { bytesWritten } = await this.fh.write(data, done, data.length - done, pos + done);
      done += bytesWritten;
    }
    this.unsynced = true;
  }

  private clusterOffset(c: number): number {
    return this.dataOffset + (c - 2) * this.clusterSize;
  }

  private get limit(): number {
    return Math.min(this.fat.length, this.clusterCount + 2);
  }

  private next(c: number): number {
    return this.fat[c] & 0x0fffffff;
  }

  /** The clusters of a chain, in order. Throws on loops and out-of-range links. */
  chain(first: number): number[] {
    const out: number[] = [];
    const seen = new Set<number>();
    let c = first;
    while (c >= 2 && c < 0x0ffffff8) {
      if (seen.has(c) || c >= this.limit) {
        throw new FatError(`bad cluster chain at 0x${c.toString(16)}`);
      }
      seen.add(c);
      out.push(c);
      c = this.next(c);
    }
    return out;
  }

  /** Groups clusters into runs of consecutive clusters (one read or write each). */
  private static runs(clusters: number[]): { start: number; count: number; index: number }[] {
    const out: { start: number; count: number; index: number }[] = [];
    clusters.forEach((c, i) => {
      const last = out[out.length - 1];
      if (last && last.start + last.count === c) {
        last.count++;
      } else {
        out.push({ start: c, count: 1, index: i });
      }
    });
    return out;
  }

  private async readClusters(clusters: number[]): Promise<Buffer> {
    const cs = this.clusterSize;
    const out = Buffer.alloc(clusters.length * cs);
    for (const r of Fat32Volume.runs(clusters)) {
      (await this.read(this.clusterOffset(r.start), r.count * cs)).copy(out, r.index * cs);
    }
    return out;
  }

  /** Writes `data` over the clusters (zero-padding the last one). */
  private async writeClusters(clusters: number[], data: Buffer): Promise<void> {
    const cs = this.clusterSize;
    for (const r of Fat32Volume.runs(clusters)) {
      const chunk = Buffer.alloc(r.count * cs);
      data.subarray(r.index * cs, (r.index + r.count) * cs).copy(chunk);
      await this.write(this.clusterOffset(r.start), chunk);
    }
  }

  // ------------------------------------------------------------------ reading

  private async dirSlots(first: number): Promise<{ buf: Buffer; offsetOf: (i: number) => number }> {
    const clusters = this.chain(first);
    const buf = await this.readClusters(clusters);
    const cs = this.clusterSize;
    return { buf, offsetOf: (i) => this.clusterOffset(clusters[Math.floor(i / cs)]) + (i % cs) };
  }

  async listDir(first: number): Promise<FatWriterEntry[]> {
    const { buf, offsetOf } = await this.dirSlots(first);
    const out: FatWriterEntry[] = [];
    let lfnParts = new Map<number, string>();
    let lfnOffsets: number[] = [];
    let lfnSum = -1;
    const reset = () => {
      lfnParts = new Map();
      lfnOffsets = [];
      lfnSum = -1;
    };
    for (let i = 0; i + DIR_ENTRY <= buf.length; i += DIR_ENTRY) {
      const e = buf.subarray(i, i + DIR_ENTRY);
      if (e[0] === 0x00) {
        break;
      }
      if (e[0] === 0xe5) {
        reset();
        continue;
      }
      const attr = e[11];
      if (attr === ATTR_LFN) {
        if (e[0] & 0x40) {
          reset();
          lfnSum = e[13];
        }
        const raw = Buffer.concat([e.subarray(1, 11), e.subarray(14, 26), e.subarray(28, 32)]);
        let s = "";
        for (let k = 0; k < 13; k++) {
          const u = raw.readUInt16LE(k * 2);
          if (u === 0x0000) {
            break;
          }
          if (u !== 0xffff) {
            s += String.fromCharCode(u);
          }
        }
        lfnParts.set(e[0] & 0x1f, s);
        lfnOffsets.push(offsetOf(i));
        continue;
      }
      if (attr & ATTR_VOLUME) {
        reset();
        continue;
      }
      let nm = e.toString("latin1", 0, 8).trimEnd();
      let ext = e.toString("latin1", 8, 11).trimEnd();
      if (e[0] === 0x05) {
        nm = "\xe5" + nm.slice(1);
      }
      const shortName = nm + (ext ? "." + ext : "");
      if (e[12] & 0x08) {
        nm = nm.toLowerCase();
      }
      if (e[12] & 0x10) {
        ext = ext.toLowerCase();
      }
      const display = nm + (ext ? "." + ext : "");
      let long: string | null = null;
      if (lfnParts.size > 0 && lfnSum === lfnChecksum(e.subarray(0, 11))) {
        long = [...lfnParts.keys()]
          .sort((a, b) => a - b)
          .map((k) => lfnParts.get(k))
          .join("");
      }
      const entry: FatWriterEntry = {
        name: long ?? display,
        shortName,
        attr,
        firstCluster: ((e.readUInt16LE(20) << 16) | e.readUInt16LE(26)) >>> 0,
        size: e.readUInt32LE(28),
        isDirectory: (attr & ATTR_DIR) !== 0,
        slotOffsets: [...(long !== null ? lfnOffsets : []), offsetOf(i)],
      };
      reset();
      if (shortName === "." || shortName === "..") {
        continue;
      }
      out.push(entry);
    }
    return out;
  }

  private rootEntry(): FatWriterEntry {
    return {
      name: "/",
      shortName: "/",
      attr: ATTR_DIR,
      firstCluster: this.rootCluster,
      size: 0,
      isDirectory: true,
      slotOffsets: [],
    };
  }

  /** Looks up a path (case-insensitive, long or 8.3 names). */
  async lookup(p: string): Promise<FatWriterEntry | null> {
    let cur = this.rootEntry();
    for (const part of splitPath(p)) {
      if (!cur.isDirectory) {
        return null;
      }
      const want = part.toLowerCase();
      const hit = (await this.listDir(cur.firstCluster)).find(
        (e) => e.name.toLowerCase() === want || e.shortName.toLowerCase() === want,
      );
      if (!hit) {
        return null;
      }
      cur = hit;
    }
    return cur;
  }

  async readFile(p: string): Promise<Buffer> {
    const e = await this.lookup(p);
    if (!e || e.isDirectory) {
      throw new FatError(`no such file: ${p}`);
    }
    if (e.size === 0) {
      return Buffer.alloc(0);
    }
    return (await this.readClusters(this.chain(e.firstCluster))).subarray(0, e.size);
  }

  // ------------------------------------------------------------------ allocation

  private setFat(c: number, v: number) {
    this.fat[c] = ((this.fat[c] & 0xf0000000) | (v & 0x0fffffff)) >>> 0;
    this.dirtyFatSectors.add(Math.floor(c / (this.bytesPerSector / 4)));
    if (v === FREE && c < this.freeHint) {
      this.freeHint = c;
    }
  }

  /** Allocates `n` clusters (lowest free first) and links them into a chain ending in EOC. */
  private alloc(n: number): number[] {
    const out: number[] = [];
    const limit = this.limit;
    let c = this.freeHint;
    while (out.length < n) {
      while (c < limit && this.next(c) !== FREE) {
        c++;
      }
      if (c >= limit) {
        throw new FatError("the SD card image is full");
      }
      out.push(c);
      c++;
    }
    for (let i = 0; i + 1 < out.length; i++) {
      this.setFat(out[i], out[i + 1]);
    }
    this.setFat(out[out.length - 1], EOC);
    this.freeHint = c;
    return out;
  }

  private freeChain(first: number) {
    if (first < 2) {
      return;
    }
    for (const c of this.chain(first)) {
      this.setFat(c, FREE);
    }
  }

  /** Free clusters according to the in-memory FAT. */
  freeClusters(): number {
    let free = 0;
    for (let c = 2; c < this.limit; c++) {
      if (this.next(c) === FREE) {
        free++;
      }
    }
    return free;
  }

  /** Writes changed FAT sectors to every FAT copy, updates FSInfo and syncs the file. */
  async flush(): Promise<void> {
    if (this.dirtyFatSectors.size > 0) {
      await this.writeFats();
    }
    if (this.unsynced) {
      await this.fh.sync();
      this.unsynced = false;
    }
  }

  private async writeFats(): Promise<void> {
    const perSector = this.bytesPerSector / 4;
    const sectors = [...this.dirtyFatSectors].sort((a, b) => a - b);
    for (const s of sectors) {
      const data = Buffer.alloc(this.bytesPerSector);
      for (let i = 0; i < perSector; i++) {
        const c = s * perSector + i;
        if (c < this.fat.length) {
          data.writeUInt32LE(this.fat[c], i * 4);
        }
      }
      for (let k = 0; k < this.numFats; k++) {
        await this.write(this.fatOffset + k * this.fatSectors * this.bytesPerSector + s * this.bytesPerSector, data);
      }
    }
    this.dirtyFatSectors.clear();
    if (this.fsInfoSector && this.fsInfoSector !== 0xffff) {
      const off = this.base + this.fsInfoSector * this.bytesPerSector;
      const info = await this.read(off, 512);
      if (info.toString("latin1", 0, 4) === "RRaA" && info.toString("latin1", 0x1e4, 0x1e8) === "rrAa") {
        let nextFree = 0xffffffff;
        for (let c = 2; c < this.limit; c++) {
          if (this.next(c) === FREE) {
            nextFree = c;
            break;
          }
        }
        info.writeUInt32LE(this.freeClusters(), 0x1e8);
        info.writeUInt32LE(nextFree >>> 0, 0x1ec);
        await this.write(off, info);
      }
    }
  }

  // ------------------------------------------------------------------ writing

  /** An 11-byte 8.3 name for `name` in the directory, and whether a long name is needed. */
  private async shortNameFor(dirCluster: number, name: string): Promise<{ short: Buffer; needLfn: boolean }> {
    const existing = new Set((await this.listDir(dirCluster)).map((e) => e.shortName.toUpperCase()));
    const dot = name.lastIndexOf(".");
    let base = dot > 0 ? name.slice(0, dot) : name;
    let ext = dot > 0 ? name.slice(dot + 1) : "";
    if (!base) {
      base = name;
      ext = "";
    }
    const clean = (s: string) =>
      [...s.toUpperCase()].filter((ch) => ch.charCodeAt(0) < 128 && SHORT_OK.has(ch)).join("");
    const exact = base.length <= 8 && ext.length <= 3 && clean(base) === base && clean(ext) === ext;
    if (exact && !existing.has(base + (ext ? "." + ext : ""))) {
      return { short: Buffer.from(base.padEnd(8) + ext.padEnd(3), "latin1"), needLfn: false };
    }
    const cb = clean(base) || "FILE";
    const ce = clean(ext).slice(0, 3);
    for (let i = 1; i < 100000; i++) {
      const tail = `~${i}`;
      const stem = cb.slice(0, 8 - tail.length) + tail;
      if (!existing.has(stem + (ce ? "." + ce : ""))) {
        return { short: Buffer.from(stem.padEnd(8) + ce.padEnd(3), "latin1"), needLfn: true };
      }
    }
    throw new FatError("no free short name");
  }

  /** Offsets of `need` consecutive free slots in a directory, growing the directory if needed. */
  private async freeSlots(dirCluster: number, need: number): Promise<number[]> {
    const { buf, offsetOf } = await this.dirSlots(dirCluster);
    let run: number[] = [];
    for (let i = 0; i + DIR_ENTRY <= buf.length; i += DIR_ENTRY) {
      if (buf[i] === 0x00 || buf[i] === 0xe5) {
        run.push(offsetOf(i));
        if (run.length === need) {
          return run;
        }
      } else {
        run = [];
      }
    }
    const perCluster = this.clusterSize / DIR_ENTRY;
    const n = Math.ceil((need - run.length) / perCluster);
    const chain = this.chain(dirCluster);
    const added = this.alloc(n);
    this.setFat(chain[chain.length - 1], added[0]);
    await this.writeClusters(added, Buffer.alloc(0));
    for (const c of added) {
      for (let i = 0; i < this.clusterSize && run.length < need; i += DIR_ENTRY) {
        run.push(this.clusterOffset(c) + i);
      }
    }
    return run;
  }

  private static dirEntry(short: Buffer, attr: number, first: number, size: number, when: Date): Buffer {
    const { date, time } = fatTimestamp(when);
    const e = Buffer.alloc(DIR_ENTRY);
    short.copy(e, 0, 0, 11);
    e[11] = attr;
    e.writeUInt16LE(time, 14); // created
    e.writeUInt16LE(date, 16);
    e.writeUInt16LE(date, 18); // accessed
    e.writeUInt16LE((first >>> 16) & 0xffff, 20);
    e.writeUInt16LE(time, 22); // written
    e.writeUInt16LE(date, 24);
    e.writeUInt16LE(first & 0xffff, 26);
    e.writeUInt32LE(size >>> 0, 28);
    return e;
  }

  private async addEntry(dirCluster: number, name: string, attr: number, first: number, size: number) {
    if (name.length === 0 || name.length > 255 || isInvalidLongName(name) || name === "." || name === "..") {
      throw new FatError(`invalid file name: ${JSON.stringify(name)}`);
    }
    const { short, needLfn } = await this.shortNameFor(dirCluster, name);
    const slots: Buffer[] = [];
    if (needLfn) {
      const units: number[] = [];
      for (let i = 0; i < name.length; i++) {
        units.push(name.charCodeAt(i));
      }
      if (units.length % 13 !== 0) {
        // A name of exactly 13*k units has no terminator.
        units.push(0x0000);
        while (units.length % 13 !== 0) {
          units.push(0xffff);
        }
      }
      const count = units.length / 13;
      const sum = lfnChecksum(short);
      for (let idx = count; idx >= 1; idx--) {
        const part = units.slice((idx - 1) * 13, idx * 13);
        const e = Buffer.alloc(DIR_ENTRY);
        e[0] = idx | (idx === count ? 0x40 : 0);
        part.slice(0, 5).forEach((u, k) => e.writeUInt16LE(u, 1 + k * 2));
        e[11] = ATTR_LFN;
        e[12] = 0;
        e[13] = sum;
        part.slice(5, 11).forEach((u, k) => e.writeUInt16LE(u, 14 + k * 2));
        part.slice(11, 13).forEach((u, k) => e.writeUInt16LE(u, 28 + k * 2));
        slots.push(e);
      }
    }
    slots.push(Fat32Volume.dirEntry(short, attr, first, size, new Date()));
    const offsets = await this.freeSlots(dirCluster, slots.length);
    for (let i = 0; i < slots.length; i++) {
      await this.write(offsets[i], slots[i]);
    }
  }

  private async parentOf(p: string): Promise<{ parentCluster: number; name: string }> {
    const parts = splitPath(p);
    if (parts.length === 0) {
      throw new FatError("empty path");
    }
    const parent = "/" + parts.slice(0, -1).join("/");
    const pe = await this.lookup(parent);
    if (!pe || !pe.isDirectory) {
      throw new FatError(`no such directory: ${parent}`);
    }
    return { parentCluster: pe.firstCluster, name: parts[parts.length - 1] };
  }

  /** Creates a directory (and its parents). Returns its first cluster. */
  async mkdir(p: string): Promise<number> {
    const existing = await this.lookup(p);
    if (existing) {
      if (!existing.isDirectory) {
        throw new FatError(`${p} exists and is a file`);
      }
      return existing.firstCluster;
    }
    const parts = splitPath(p);
    if (parts.length > 1) {
      await this.mkdir("/" + parts.slice(0, -1).join("/"));
    }
    const { parentCluster, name } = await this.parentOf(p);
    const [c] = this.alloc(1);
    const now = new Date();
    const buf = Buffer.alloc(this.clusterSize);
    Fat32Volume.dirEntry(Buffer.from(".          ", "latin1"), ATTR_DIR, c, 0, now).copy(buf, 0);
    const dotdot = parentCluster === this.rootCluster ? 0 : parentCluster;
    Fat32Volume.dirEntry(Buffer.from("..         ", "latin1"), ATTR_DIR, dotdot, 0, now).copy(buf, DIR_ENTRY);
    await this.write(this.clusterOffset(c), buf);
    await this.addEntry(parentCluster, name, ATTR_DIR, c, 0);
    return c;
  }

  /** Deletes a file. Returns false if it did not exist. */
  async delete(p: string): Promise<boolean> {
    const e = await this.lookup(p);
    if (!e) {
      return false;
    }
    if (e.isDirectory) {
      throw new FatError("deleting directories is not supported");
    }
    this.freeChain(e.firstCluster);
    for (const off of e.slotOffsets) {
      await this.write(off, Buffer.from([0xe5]));
    }
    return true;
  }

  /** Adds or replaces a file, creating missing parent directories. Call flush() or close() after. */
  async writeFile(p: string, data: Buffer): Promise<void> {
    const parts = splitPath(p);
    if (parts.length > 1) {
      await this.mkdir("/" + parts.slice(0, -1).join("/"));
    }
    await this.delete(p);
    const { parentCluster, name } = await this.parentOf(p);
    let first = 0;
    if (data.length > 0) {
      const clusters = this.alloc(Math.ceil(data.length / this.clusterSize));
      await this.writeClusters(clusters, data);
      first = clusters[0];
    }
    await this.addEntry(parentCluster, name, ATTR_ARCHIVE, first, data.length);
  }

  // ------------------------------------------------------------------ checks

  /**
   * Consistency check (`patch_sd.py --check`): every reachable chain is valid, file chains match
   * their sizes, no cluster is shared, and no allocated cluster is unreachable (lost chains).
   * Returns the problems found (empty = clean). Uses the in-memory FAT, so call it after flush().
   */
  async check(): Promise<string[]> {
    const problems: string[] = [];
    const owner = new Map<number, string>();
    try {
      for (const c of this.chain(this.rootCluster)) {
        owner.set(c, "/");
      }
    } catch (err) {
      return [`/: ${err instanceof Error ? err.message : String(err)}`];
    }
    const stack: { path: string; cluster: number }[] = [{ path: "", cluster: this.rootCluster }];
    while (stack.length > 0) {
      const dir = stack.pop()!;
      let entries: FatWriterEntry[];
      try {
        entries = await this.listDir(dir.cluster);
      } catch (err) {
        problems.push(`${dir.path || "/"}: ${err instanceof Error ? err.message : String(err)}`);
        continue;
      }
      for (const e of entries) {
        const full = `${dir.path}/${e.name}`;
        if (e.firstCluster < 2) {
          if (!e.isDirectory && e.size > 0) {
            problems.push(`${full}: ${e.size} bytes but no clusters`);
          }
          continue;
        }
        let chain: number[];
        try {
          chain = this.chain(e.firstCluster);
        } catch (err) {
          problems.push(`${full}: ${err instanceof Error ? err.message : String(err)}`);
          continue;
        }
        if (!e.isDirectory) {
          const need = Math.ceil(e.size / this.clusterSize);
          if (chain.length !== need) {
            problems.push(`${full}: chain ${chain.length} clusters, size needs ${need}`);
          }
        }
        let shared = false;
        for (const c of chain) {
          if (this.next(c) === FREE) {
            problems.push(`${full}: chain links to free cluster 0x${c.toString(16)}`);
          }
          const prev = owner.get(c);
          if (prev !== undefined) {
            problems.push(`${full}: cluster 0x${c.toString(16)} shared with ${prev}`);
            shared = true;
          }
          owner.set(c, full);
        }
        if (e.isDirectory && !shared) {
          stack.push({ path: full, cluster: e.firstCluster });
        }
      }
    }
    let lost = 0;
    for (let c = 2; c < this.limit; c++) {
      if (this.next(c) !== FREE && !owner.has(c)) {
        lost++;
      }
    }
    if (lost > 0) {
      problems.push(`${lost} allocated clusters are not reachable (lost chains)`);
    }
    return problems;
  }
}

async function readExactly(fh: fs.promises.FileHandle, pos: number, len: number): Promise<Buffer> {
  const buf = Buffer.alloc(len);
  let done = 0;
  while (done < len) {
    const { bytesRead } = await fh.read(buf, done, len - done, pos + done);
    if (bytesRead === 0) {
      throw new FatError(`short read at 0x${(pos + done).toString(16)}`);
    }
    done += bytesRead;
  }
  return buf;
}
