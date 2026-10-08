import { describe, expect, it } from "vitest";

import { BufferReader, FatImage } from "./fat32";
import { SdSource } from "./sources";
import type { Node } from "./testing/fat_image";
import { buildFat, pattern } from "./testing/fat_image";

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
