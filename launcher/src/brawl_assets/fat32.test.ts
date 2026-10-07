import { describe, expect, it } from "vitest";

import { BufferReader, FatImage } from "./fat32";
import { SdSource } from "./sources";

type Node = {
  name: string;
  children?: Node[];
  data?: Buffer;
  fragment?: boolean;
  lowerCase?: boolean;
  deleted?: boolean;
};

const BPS = 512;

/** Builds a small FAT16/FAT32 image (1 sector per cluster) for tests. */
function buildFat(opts: { fat: 16 | 32; partitioned: boolean; tree: Node[] }): Buffer {
  const fat32 = opts.fat === 32;
  const reserved = fat32 ? 32 : 1;
  const clusters = fat32 ? 66000 : 5000;
  const entBytes = fat32 ? 4 : 2;
  const fatSz = Math.ceil(((clusters + 2) * entBytes) / BPS);
  const rootEntries = fat32 ? 0 : 512;
  const rootDirSectors = (rootEntries * 32) / BPS;
  const totSec = reserved + 2 * fatSz + rootDirSectors + clusters;
  const partStart = opts.partitioned ? 8 : 0;
  const img = Buffer.alloc((partStart + totSec) * BPS);
  const base = partStart * BPS;
  const fatOff = base + reserved * BPS;
  const rootOff = fatOff + 2 * fatSz * BPS;
  const dataOff = rootOff + rootDirSectors * BPS;
  const fatSet = (c: number, v: number) =>
    fat32 ? img.writeUInt32LE(v >>> 0, fatOff + c * 4) : img.writeUInt16LE(v & 0xffff, fatOff + c * 2);
  const eoc = fat32 ? 0x0fffffff : 0xffff;
  fatSet(0, fat32 ? 0x0ffffff8 : 0xfff8);
  fatSet(1, eoc);
  let nextCluster = 2;
  const alloc = (bytes: number, fragment = false): number[] => {
    const n = Math.max(1, Math.ceil(bytes / BPS));
    const list = Array.from({ length: n }, () => nextCluster++);
    const order = fragment && n > 2 ? [list[n - 1], ...list.slice(0, n - 1)] : list;
    order.forEach((c, i) => fatSet(c, i + 1 < order.length ? order[i + 1] : eoc));
    return order;
  };
  const writeChain = (chain: number[], data: Buffer) => {
    chain.forEach((c, i) => data.subarray(i * BPS, (i + 1) * BPS).copy(img, dataOff + (c - 2) * BPS));
  };
  let tilde = 1;
  const shortName = (n: Node): { raw: Buffer; needsLfn: boolean; caseFlags: number } => {
    const upper = n.name.toUpperCase();
    const m = /^([A-Z0-9_]{1,8})(?:\.([A-Z0-9_]{1,3}))?$/.exec(upper);
    const raw = Buffer.alloc(11, 0x20);
    if (m && (n.name === upper || n.lowerCase)) {
      raw.write(m[1], 0, "latin1");
      raw.write(m[2] ?? "", 8, "latin1");
      return { raw, needsLfn: false, caseFlags: n.lowerCase ? 0x18 : 0 };
    }
    const [b, e = ""] = upper.replace(/[^A-Z0-9_.]/g, "").split(/\.(?=[^.]*$)/);
    raw.write(`${b.replace(/\./g, "").slice(0, 6)}~${tilde++}`, 0, "latin1");
    raw.write(e.slice(0, 3), 8, "latin1");
    return { raw, needsLfn: true, caseFlags: 0 };
  };
  const checksum = (raw: Buffer) => {
    let s = 0;
    for (let i = 0; i < 11; i++) {
      s = ((((s & 1) << 7) | (s >> 1)) + raw[i]) & 0xff;
    }
    return s;
  };
  const dirEntries = (nodes: Node[], self: number, parent: number): Buffer[] => {
    const out: Buffer[] = [];
    if (self >= 0) {
      for (const [nm, cl] of [
        [".", self],
        ["..", parent],
      ] as [string, number][]) {
        const e = Buffer.alloc(32, 0);
        Buffer.alloc(11, 0x20).copy(e);
        e.write(nm, 0, "latin1");
        e[11] = 0x10;
        e.writeUInt16LE(cl >> 16, 20);
        e.writeUInt16LE(cl & 0xffff, 26);
        out.push(e);
      }
    }
    for (const n of nodes) {
      const sn = shortName(n);
      if (sn.needsLfn) {
        const chars = [...Buffer.from(n.name, "utf16le")];
        const units: number[] = [];
        for (let i = 0; i < chars.length; i += 2) {
          units.push(chars[i] | (chars[i + 1] << 8));
        }
        const count = Math.ceil((units.length + 1) / 13);
        const padded = [...units, 0, ...new Array(count * 13).fill(0xffff)].slice(0, count * 13);
        for (let k = count; k >= 1; k--) {
          const e = Buffer.alloc(32);
          e[0] = k | (k === count ? 0x40 : 0);
          e[11] = 0x0f;
          e[13] = checksum(sn.raw);
          const part = padded.slice((k - 1) * 13, k * 13);
          part.slice(0, 5).forEach((u, i) => e.writeUInt16LE(u, 1 + i * 2));
          part.slice(5, 11).forEach((u, i) => e.writeUInt16LE(u, 14 + i * 2));
          part.slice(11, 13).forEach((u, i) => e.writeUInt16LE(u, 28 + i * 2));
          out.push(e);
        }
      }
      const e = Buffer.alloc(32);
      sn.raw.copy(e);
      if (n.deleted) {
        e[0] = 0xe5;
      }
      e[11] = n.children ? 0x10 : 0x20;
      e[12] = sn.caseFlags;
      (n as any)._entry = e;
      out.push(e);
    }
    return out;
  };
  const writeDir = (nodes: Node[], self: number, parent: number, chainOrRoot: number[] | "root") => {
    // Children first (so their clusters are known), then this directory's entries.
    const entries = dirEntries(nodes, self, parent);
    for (const n of nodes) {
      const e = (n as any)._entry as Buffer;
      if (n.children) {
        const childEntries = n.children.length * 3 + 2;
        const chain = alloc(childEntries * 32);
        e.writeUInt16LE(chain[0] >> 16, 20);
        e.writeUInt16LE(chain[0] & 0xffff, 26);
        writeDir(n.children, chain[0], self < 0 ? 0 : self, chain);
      } else if (n.data) {
        const chain = n.data.length ? alloc(n.data.length, n.fragment) : [0];
        if (n.data.length) {
          writeChain(chain, n.data);
        }
        e.writeUInt16LE(chain[0] >> 16, 20);
        e.writeUInt16LE(chain[0] & 0xffff, 26);
        e.writeUInt32LE(n.data.length, 28);
      }
    }
    const buf = Buffer.concat(entries);
    if (chainOrRoot === "root") {
      buf.copy(img, rootOff);
    } else {
      const needed = Math.ceil(buf.length / BPS);
      if (needed > chainOrRoot.length) {
        // Extend the chain for large directories.
        const extra = alloc((needed - chainOrRoot.length) * BPS);
        fatSet(chainOrRoot[chainOrRoot.length - 1], extra[0]);
        chainOrRoot.push(...extra);
      }
      writeChain(chainOrRoot, buf);
    }
  };
  if (fat32) {
    const rootChain = alloc(BPS);
    writeDir(opts.tree, -1, 0, rootChain);
  } else {
    writeDir(opts.tree, -1, 0, "root");
  }
  // Boot sector.
  const bs = img.subarray(base, base + BPS);
  bs[0] = 0xeb;
  bs[1] = 0x58;
  bs[2] = 0x90;
  bs.write("MSWIN4.1", 3, "latin1");
  bs.writeUInt16LE(BPS, 11);
  bs[13] = 1;
  bs.writeUInt16LE(reserved, 14);
  bs[16] = 2;
  bs.writeUInt16LE(rootEntries, 17);
  bs.writeUInt16LE(totSec < 0x10000 ? totSec : 0, 19);
  bs[21] = 0xf8;
  bs.writeUInt16LE(fat32 ? 0 : fatSz, 22);
  bs.writeUInt32LE(partStart, 28);
  bs.writeUInt32LE(totSec >= 0x10000 ? totSec : 0, 32);
  if (fat32) {
    bs.writeUInt32LE(fatSz, 36);
    bs.writeUInt32LE(2, 44);
  }
  bs[510] = 0x55;
  bs[511] = 0xaa;
  if (opts.partitioned) {
    const e = 0x1be;
    img[e + 4] = fat32 ? 0x0c : 0x06;
    img.writeUInt32LE(partStart, e + 8);
    img.writeUInt32LE(totSec, e + 12);
    img[510] = 0x55;
    img[511] = 0xaa;
  }
  return img;
}

function pattern(n: number, seed: number) {
  const b = Buffer.alloc(n);
  let s = seed;
  for (let i = 0; i < n; i++) {
    s = (s * 1664525 + 1013904223) >>> 0;
    b[i] = s >>> 24;
  }
  return b;
}

const BIG = pattern(1300, 7); // three clusters, stored out of order
const SMALL = Buffer.from("small file");

function tree(): Node[] {
  return [
    {
      name: "Project+",
      children: [
        {
          name: "pf",
          children: [
            {
              name: "menu2",
              children: [
                { name: "old_file.pac", data: Buffer.from("deleted"), deleted: true },
                { name: "mu_menumain.pac", data: BIG, fragment: true },
                { name: "A.BIN", data: SMALL },
                { name: "empty.dat", data: Buffer.alloc(0) },
              ],
            },
          ],
        },
      ],
    },
    { name: "readme.txt", data: Buffer.from("lower"), lowerCase: true },
    ...Array.from({ length: 20 }, (_, i) => ({ name: `file number ${i}.dat`, data: Buffer.from(`#${i}`) })),
  ];
}

describe.each([
  { fat: 32 as const, partitioned: true },
  { fat: 32 as const, partitioned: false },
  { fat: 16 as const, partitioned: true },
])("FAT$fat image (partitioned: $partitioned)", (cfg) => {
  const img = buildFat({ ...cfg, tree: tree() });

  it("detects the volume and lists directories with long names", async () => {
    const fs = await FatImage.fromReader(new BufferReader(img));
    expect(fs.fatType).toBe(cfg.fat);
    const root = await fs.readdir("/");
    expect(root.length).toBe(22);
    expect(root[0]).toMatchObject({ name: "Project+", isDirectory: true });
    expect(root[1]).toMatchObject({ name: "readme.txt", isDirectory: false, size: 5 });
    expect(root[21].name).toBe("file number 19.dat");
    const menu2 = await fs.readdir("/Project+/pf/menu2");
    expect(menu2.map((e) => e.name)).toEqual(["mu_menumain.pac", "A.BIN", "empty.dat"]);
  });

  it("reads files by path (case-insensitive, short names, fragmented chains)", async () => {
    const fs = await FatImage.fromReader(new BufferReader(img));
    expect((await fs.readFile("/Project+/pf/menu2/mu_menumain.pac")).equals(BIG)).toBe(true);
    expect((await fs.readFile("project+/PF/Menu2/MU_MENUMAIN.PAC")).equals(BIG)).toBe(true);
    const short = (await fs.readdir("/Project+/pf/menu2"))[0].shortName;
    expect(short).toMatch(/~/);
    expect((await fs.readFile(`/Project+/pf/menu2/${short}`)).equals(BIG)).toBe(true);
    expect((await fs.readFile("/Project+/pf/menu2/a.bin")).toString()).toBe("small file");
    expect((await fs.readFile("/Project+/pf/menu2/empty.dat")).length).toBe(0);
    expect((await fs.readFile("/readme.txt")).toString()).toBe("lower");
    expect((await fs.readFile("/file number 17.dat")).toString()).toBe("#17");
    expect(await fs.stat("/Project+/pf/menu2/old_file.pac")).toBeNull();
    expect(await fs.exists("/nope/x")).toBe(false);
    await expect(fs.readFile("/Project+/pf")).rejects.toThrow();
    await expect(fs.readFile("/missing.bin")).rejects.toThrow();
  });

  it("serves as an SD source rooted at /Project+/pf/", async () => {
    const sd = SdSource.fromFat(await FatImage.fromReader(new BufferReader(img)));
    expect((await sd.read("menu2/mu_menumain.pac"))!.equals(BIG)).toBe(true);
    expect(await sd.read("menu2/missing.pac")).toBeNull();
    expect(await sd.read("menu2")).toBeNull();
    await sd.close();
  });
});

describe("FAT error handling", () => {
  it("rejects images without a FAT volume", async () => {
    await expect(FatImage.fromReader(new BufferReader(Buffer.alloc(4096)))).rejects.toThrow();
  });
});
