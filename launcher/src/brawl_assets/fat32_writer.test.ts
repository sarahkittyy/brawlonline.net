import { createHash } from "crypto";
import fs from "fs";
import os from "os";
import path from "path";
import { afterAll, beforeAll, describe, expect, it } from "vitest";

import { FatImage } from "./fat32";
import { Fat32Volume, FatError } from "./fat32_writer";
import type { Node } from "./testing/fat_image";
import { buildFat, makeEmptyFat32Image, pattern } from "./testing/fat_image";

let tmp: string;
let counter = 0;

beforeAll(async () => {
  tmp = await fs.promises.mkdtemp(path.join(os.tmpdir(), "fat32-writer-"));
});

afterAll(async () => {
  await fs.promises.rm(tmp, { recursive: true, force: true });
});

const nextFile = () => path.join(tmp, `img-${counter++}.raw`);
const sha = (b: Buffer) => createHash("sha256").update(b).digest("hex");

async function fsInfoFree(file: string, partitioned: boolean): Promise<number> {
  const fh = await fs.promises.open(file, "r");
  try {
    const buf = Buffer.alloc(512);
    await fh.read(buf, 0, 512, (partitioned ? 2048 * 512 : 0) + 512);
    return buf.readUInt32LE(0x1e8);
  } finally {
    await fh.close();
  }
}

const PLUGIN = pattern(18_500, 3);
const LONG_NAME = "A long file name with spaces (and more).bin";

describe.each([
  { partitioned: false, sectorsPerCluster: 1 },
  { partitioned: true, sectorsPerCluster: 1 },
  { partitioned: false, sectorsPerCluster: 4 },
])("FAT32 writer (partitioned: $partitioned, $sectorsPerCluster sectors per cluster)", (cfg) => {
  it("creates nested directories and files with long names; the reader sees them byte-identical", async () => {
    const file = nextFile();
    await makeEmptyFat32Image(file, cfg);
    const vol = await Fat32Volume.open(file, { writable: true });
    await vol.writeFile("/Project+/pf/plugins/PPOnline.rel", PLUGIN);
    await vol.writeFile("/Project+/pf/plugins/" + LONG_NAME, Buffer.from("long"));
    await vol.writeFile("/SHORT.TXT", Buffer.from("short"));
    await vol.writeFile("/empty.dat", Buffer.alloc(0));
    // 13 and 26 UTF-16 units: long names without a terminator
    await vol.writeFile("/thirteen_char", Buffer.from("13"));
    await vol.writeFile("/twenty-six characters.binx", Buffer.from("26"));
    await vol.writeFile("/Unicodé ÿ.txt", Buffer.from("u"));
    await vol.flush();
    expect(await vol.check()).toEqual([]);
    const free = vol.freeClusters();
    await vol.close();

    expect(await fsInfoFree(file, cfg.partitioned)).toBe(free);

    const reader = await FatImage.open(file);
    try {
      expect((await reader.readFile("/Project+/pf/plugins/PPOnline.rel")).equals(PLUGIN)).toBe(true);
      expect((await reader.readFile(`/project+/PF/Plugins/${LONG_NAME.toUpperCase()}`)).toString()).toBe("long");
      expect((await reader.readFile("/short.txt")).toString()).toBe("short");
      expect((await reader.readFile("/empty.dat")).length).toBe(0);
      expect((await reader.readFile("/thirteen_char")).toString()).toBe("13");
      expect((await reader.readFile("/twenty-six characters.binx")).toString()).toBe("26");
      expect((await reader.readFile("/Unicodé ÿ.txt")).toString()).toBe("u");
      const root = await reader.readdir("/");
      expect(root.map((e) => e.name)).toEqual([
        "Project+",
        "SHORT.TXT",
        "empty.dat",
        "thirteen_char",
        "twenty-six characters.binx",
        "Unicodé ÿ.txt",
      ]);
      const plugins = await reader.readdir("/Project+/pf/plugins");
      expect(plugins.map((e) => e.name)).toEqual(["PPOnline.rel", LONG_NAME]);
      expect(plugins[0].shortName).toBe("PPONLI~1.REL");
    } finally {
      await reader.close();
    }
  });

  it("replaces an existing file without leaking clusters", async () => {
    const file = nextFile();
    await makeEmptyFat32Image(file, cfg);
    let vol = await Fat32Volume.open(file, { writable: true });
    const freeAtStart = vol.freeClusters();
    await vol.writeFile("/Project+/pf/plugins/PPOnline.rel", PLUGIN);
    await vol.close();

    vol = await Fat32Volume.open(file, { writable: true });
    const smaller = pattern(700, 9);
    await vol.writeFile("/Project+/pf/plugins/PPOnline.rel", smaller);
    await vol.flush();
    expect(await vol.check()).toEqual([]);
    const cs = vol.clusterSize;
    // three directories (Project+, pf, plugins) + the file
    expect(vol.freeClusters()).toBe(freeAtStart - 3 - Math.ceil(smaller.length / cs));
    expect((await vol.listDir((await vol.lookup("/Project+/pf/plugins"))!.firstCluster)).length).toBe(1);
    await vol.close();

    const reader = await FatImage.open(file);
    expect((await reader.readFile("/Project+/pf/plugins/PPOnline.rel")).equals(smaller)).toBe(true);
    await reader.close();
    expect(await fsInfoFree(file, cfg.partitioned)).toBe(freeAtStart - 3 - Math.ceil(smaller.length / cs));
  });

  it("grows a directory past one cluster", async () => {
    const file = nextFile();
    await makeEmptyFat32Image(file, cfg);
    const vol = await Fat32Volume.open(file, { writable: true });
    const names = Array.from({ length: 60 }, (_, i) => `replay file number ${i}.rep`);
    for (const n of names) {
      await vol.writeFile(`/Project+/rp/${n}`, Buffer.from(n));
    }
    await vol.flush();
    expect(await vol.check()).toEqual([]);
    await vol.close();
    const reader = await FatImage.open(file);
    expect((await reader.readdir("/Project+/rp")).map((e) => e.name)).toEqual(names);
    expect((await reader.readFile("/Project+/rp/replay file number 59.rep")).toString()).toBe(
      "replay file number 59.rep",
    );
    await reader.close();
  });
});

describe("FAT32 writer on a populated image", () => {
  const tree = (withDeleted: boolean): Node[] => [
    {
      name: "Project+",
      children: [
        {
          name: "pf",
          children: [
            {
              name: "menu2",
              children: [
                ...(withDeleted ? [{ name: "old_file.pac", data: Buffer.from("deleted"), deleted: true }] : []),
                { name: "mu_menumain.pac", data: pattern(1300, 7), fragment: true },
                { name: "A.BIN", data: Buffer.from("small file") },
              ],
            },
          ],
        },
      ],
    },
    ...Array.from({ length: 20 }, (_, i) => ({ name: `file number ${i}.dat`, data: Buffer.from(`#${i}`) })),
  ];

  it.each([false, true])("adds and replaces files next to existing ones (partitioned: %s)", async (partitioned) => {
    const file = nextFile();
    await fs.promises.writeFile(file, buildFat({ fat: 32, partitioned, tree: tree(false) }));
    const vol = await Fat32Volume.open(file, { writable: true });
    expect(await vol.check()).toEqual([]);
    const replacement = pattern(3000, 11);
    await vol.writeFile("/Project+/pf/menu2/mu_menumain.pac", replacement);
    await vol.writeFile("/Project+/pf/plugins/PPOnline.rel", PLUGIN);
    expect(await vol.delete("/file number 3.dat")).toBe(true);
    expect(await vol.delete("/missing.dat")).toBe(false);
    await vol.flush();
    expect(await vol.check()).toEqual([]);
    await vol.close();

    const reader = await FatImage.open(file);
    expect((await reader.readFile("/Project+/pf/menu2/mu_menumain.pac")).equals(replacement)).toBe(true);
    expect((await reader.readFile("/Project+/pf/menu2/a.bin")).toString()).toBe("small file");
    expect(sha(await reader.readFile("/Project+/pf/plugins/PPOnline.rel"))).toBe(sha(PLUGIN));
    expect(await reader.exists("/file number 3.dat")).toBe(false);
    expect((await reader.readFile("/file number 19.dat")).toString()).toBe("#19");
    await reader.close();
  });

  it("reports lost chains and shared clusters", async () => {
    const file = nextFile();
    // The builder allocates clusters for the deleted entry: a lost chain.
    await fs.promises.writeFile(file, buildFat({ fat: 32, partitioned: false, tree: tree(true) }));
    let vol = await Fat32Volume.open(file, { writable: true });
    expect(await vol.check()).toEqual(["1 allocated clusters are not reachable (lost chains)"]);
    // Point A.BIN at mu_menumain.pac's clusters.
    const big = (await vol.lookup("/Project+/pf/menu2/mu_menumain.pac"))!;
    const small = (await vol.lookup("/Project+/pf/menu2/A.BIN"))!;
    await vol.close();
    const fh = await fs.promises.open(file, "r+");
    const slot = small.slotOffsets[small.slotOffsets.length - 1];
    const b = Buffer.alloc(2);
    b.writeUInt16LE(big.firstCluster & 0xffff);
    await fh.write(b, 0, 2, slot + 26);
    await fh.close();
    vol = await Fat32Volume.open(file);
    const problems = await vol.check();
    await vol.close();
    expect(problems.some((p) => /A\.BIN: cluster 0x[0-9a-f]+ shared with/.test(p))).toBe(true);
    expect(problems.some((p) => /A\.BIN: chain 3 clusters, size needs 1/.test(p))).toBe(true);
  });
});

describe("FAT32 writer errors", () => {
  it("refuses to write a read-only volume and non-FAT32 images", async () => {
    const file = nextFile();
    await makeEmptyFat32Image(file);
    const vol = await Fat32Volume.open(file);
    await expect(vol.writeFile("/x.bin", Buffer.from("x"))).rejects.toThrow(FatError);
    await vol.close();

    const fat16 = nextFile();
    await fs.promises.writeFile(fat16, buildFat({ fat: 16, partitioned: false, tree: [] }));
    await expect(Fat32Volume.open(fat16)).rejects.toThrow(FatError);
    const zeros = nextFile();
    await fs.promises.writeFile(zeros, Buffer.alloc(4096));
    await expect(Fat32Volume.open(zeros)).rejects.toThrow(FatError);
  });

  it("rejects invalid names and reports a full card", async () => {
    const file = nextFile();
    await makeEmptyFat32Image(file, { clusters: 65530 });
    const vol = await Fat32Volume.open(file, { writable: true });
    await expect(vol.writeFile("/bad:name.txt", Buffer.from("x"))).rejects.toThrow(/invalid file name/);
    await expect(vol.writeFile("/dir/bad\u0001.txt", Buffer.from("x"))).rejects.toThrow(/invalid file name/);
    await expect(vol.writeFile("/huge.bin", Buffer.alloc(65530 * 512))).rejects.toThrow(/full/);
    await vol.close();
  });
});
