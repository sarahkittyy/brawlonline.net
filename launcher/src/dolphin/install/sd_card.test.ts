import { FatImage } from "@brawl_assets/fat32";
import { Fat32Volume } from "@brawl_assets/fat32_writer";
import { makeEmptyFat32Image, pattern } from "@brawl_assets/testing/fat_image";
import fs from "fs";
import os from "os";
import path from "path";
import { afterAll, beforeAll, beforeEach, describe, expect, it } from "vitest";

import { PLUGIN_SD_PATH } from "./paths";
import type { PluginBinary } from "./sd_card";
import { installPluginOnSdCard, loadPlugin, readFileHashFromImage, sha256 } from "./sd_card";

let root: string;
let dir: string;
let n = 0;

beforeAll(async () => {
  root = await fs.promises.mkdtemp(path.join(os.tmpdir(), "sd-install-"));
});

afterAll(async () => {
  await fs.promises.rm(root, { recursive: true, force: true });
});

const plugin = (seed: number, size = 18_000): PluginBinary => {
  const data = pattern(size, seed);
  return { data, sha256: sha256(data), source: `plugin-${seed}.rel` };
};

async function fileHash(file: string) {
  return sha256(await fs.promises.readFile(file));
}

async function writeOnCard(image: string, sdPath: string, data: Buffer) {
  const vol = await Fat32Volume.open(image, { writable: true });
  await vol.writeFile(sdPath, data);
  await vol.close();
}

async function readOnCard(image: string, sdPath: string): Promise<Buffer | null> {
  const fat = await FatImage.open(image);
  try {
    return (await fat.exists(sdPath)) ? await fat.readFile(sdPath) : null;
  } finally {
    await fat.close();
  }
}

describe("installPluginOnSdCard", () => {
  let template: string;
  let source: string;
  let out: string;
  const logs: string[] = [];
  const opts = (p: PluginBinary) => ({
    sourceImage: source,
    outputDir: out,
    plugin: p,
    protectedPaths: [template],
    log: (m: string) => logs.push(m),
  });

  beforeEach(async () => {
    dir = path.join(root, `case-${n++}`);
    template = path.join(dir, "template-user");
    source = path.join(dir, "User", "Wii", "sd.raw");
    out = path.join(dir, "userData", "netplay", "pponline-sd");
    await fs.promises.mkdir(path.dirname(source), { recursive: true });
    await fs.promises.mkdir(template, { recursive: true });
    await makeEmptyFat32Image(source);
    // A card like P+'s: other plugins already there.
    await writeOnCard(source, "/Project+/pf/plugins/AsyncRSP.rel", pattern(7080, 1));
    logs.length = 0;
  });

  it("copies and patches once, then writes nothing while nothing changes", async () => {
    const srcStat = await fs.promises.stat(source);
    const srcHash = await fileHash(source);
    const p = plugin(42);

    const first = await installPluginOnSdCard(opts(p));
    expect(first.action).toBe("copied");
    expect(first.image).toBe(path.join(out, "sd.raw"));
    expect((await readOnCard(first.image, PLUGIN_SD_PATH))!.equals(p.data)).toBe(true);
    expect((await readOnCard(first.image, "/Project+/pf/plugins/AsyncRSP.rel"))!.length).toBe(7080);
    const manifest = JSON.parse(await fs.promises.readFile(first.manifest, "utf8"));
    expect(manifest).toMatchObject({
      version: 1,
      source: { path: path.resolve(source), size: srcStat.size, mtimeMs: srcStat.mtimeMs },
      plugin: { sha256: p.sha256, size: p.data.length, sdPath: PLUGIN_SD_PATH },
    });
    expect(fs.existsSync(`${first.image}.partial`)).toBe(false);

    const imgStat = await fs.promises.stat(first.image);
    const manStat = await fs.promises.stat(first.manifest);
    const imgHash = await fileHash(first.image);

    const second = await installPluginOnSdCard(opts(p));
    expect(second.action).toBe("unchanged");
    expect((await fs.promises.stat(first.image)).mtimeMs).toBe(imgStat.mtimeMs);
    expect((await fs.promises.stat(first.manifest)).mtimeMs).toBe(manStat.mtimeMs);
    expect(await fileHash(first.image)).toBe(imgHash);

    // The source card was never written.
    expect((await fs.promises.stat(source)).mtimeMs).toBe(srcStat.mtimeMs);
    expect(await fileHash(source)).toBe(srcHash);
    expect(await readOnCard(source, PLUGIN_SD_PATH)).toBeNull();
  });

  it("replaces only the plugin when it changes, keeping what the game wrote", async () => {
    const first = await installPluginOnSdCard(opts(plugin(1)));
    await writeOnCard(first.image, "/Project+/rp/game save.rep", Buffer.from("written by the game"));

    const p2 = plugin(2, 30_000);
    const res = await installPluginOnSdCard(opts(p2));
    expect(res.action).toBe("replaced");
    expect((await readOnCard(res.image, PLUGIN_SD_PATH))!.equals(p2.data)).toBe(true);
    expect((await readOnCard(res.image, "/Project+/rp/game save.rep"))!.toString()).toBe("written by the game");
    const manifest = JSON.parse(await fs.promises.readFile(res.manifest, "utf8"));
    expect(manifest.plugin.sha256).toBe(p2.sha256);
    expect((await installPluginOnSdCard(opts(p2))).action).toBe("unchanged");
  });

  it("re-copies when the source card changes", async () => {
    const p = plugin(5);
    const first = await installPluginOnSdCard(opts(p));
    await writeOnCard(first.image, "/Project+/rp/old.rep", Buffer.from("old copy"));

    await writeOnCard(source, "/Project+/pf/new in source.bin", Buffer.from("new"));
    const later = new Date(Date.now() + 5000);
    await fs.promises.utimes(source, later, later);

    const res = await installPluginOnSdCard(opts(p));
    expect(res.action).toBe("copied");
    expect((await readOnCard(res.image, "/Project+/pf/new in source.bin"))!.toString()).toBe("new");
    expect(await readOnCard(res.image, "/Project+/rp/old.rep")).toBeNull();
    expect((await readOnCard(res.image, PLUGIN_SD_PATH))!.equals(p.data)).toBe(true);
    expect(await readOnCard(source, PLUGIN_SD_PATH)).toBeNull();
  });

  it("detects and repairs a corrupted or missing plugin in the copy", async () => {
    const p = plugin(7);
    const first = await installPluginOnSdCard(opts(p));

    // Corrupt the plugin's data in place (directory entry and FAT untouched): find its first
    // bytes on disk and overwrite a few bytes after them.
    const fh = await fs.promises.open(first.image, "r+");
    const needle = p.data.subarray(0, 64);
    const chunk = Buffer.alloc(4 * 1024 * 1024);
    let pos = 0;
    let found = -1;
    for (;;) {
      const { bytesRead } = await fh.read(chunk, 0, chunk.length, pos);
      if (bytesRead === 0) {
        break;
      }
      const i = chunk.subarray(0, bytesRead).indexOf(needle);
      if (i >= 0) {
        found = pos + i;
        break;
      }
      pos += bytesRead - needle.length;
    }
    expect(found).toBeGreaterThan(0);
    await fh.write(Buffer.from("corrupt"), 0, 7, found + 100);
    await fh.close();
    expect(await readFileHashFromImage(first.image, PLUGIN_SD_PATH)).not.toBe(p.sha256);

    const repaired = await installPluginOnSdCard(opts(p));
    expect(repaired.action).toBe("repaired");
    expect(await readFileHashFromImage(first.image, PLUGIN_SD_PATH)).toBe(p.sha256);
    expect((await installPluginOnSdCard(opts(p))).action).toBe("unchanged");

    // Deleted from the copy (e.g. by hand): put back.
    const v2 = await Fat32Volume.open(first.image, { writable: true });
    await v2.delete(PLUGIN_SD_PATH);
    await v2.close();
    expect((await installPluginOnSdCard(opts(p))).action).toBe("repaired");
    expect(await readFileHashFromImage(first.image, PLUGIN_SD_PATH)).toBe(p.sha256);
  });

  it("recreates a copy that cannot be patched", async () => {
    const p = plugin(8);
    const first = await installPluginOnSdCard(opts(p));
    // Destroy the copy's boot sector.
    const fh = await fs.promises.open(first.image, "r+");
    await fh.write(Buffer.alloc(512), 0, 512, 0);
    await fh.close();
    const res = await installPluginOnSdCard(opts(p));
    expect(res.action).toBe("copied");
    expect(await readFileHashFromImage(res.image, PLUGIN_SD_PATH)).toBe(p.sha256);
  });

  it("never writes the source card or the template", async () => {
    const p = plugin(9);
    // Output folder = the source card's folder: the copy would be the user's card itself.
    await expect(installPluginOnSdCard({ ...opts(p), outputDir: path.dirname(source) })).rejects.toThrow(
      /Refusing to modify/,
    );
    await expect(
      installPluginOnSdCard({ ...opts(p), outputDir: path.join(template, "Wii", "pponline-sd") }),
    ).rejects.toThrow(/Refusing to modify/);
    expect(await readOnCard(source, PLUGIN_SD_PATH)).toBeNull();
    expect(fs.readdirSync(template)).toEqual([]);
  });

  it("fails clearly without a source card", async () => {
    await expect(
      installPluginOnSdCard({ ...opts(plugin(1)), sourceImage: path.join(dir, "missing.raw") }),
    ).rejects.toThrow(/SD card was not found/);
  });
});

describe("loadPlugin", () => {
  it("checks a shipped plugin against its manifest", async () => {
    const d = path.join(root, "plugin");
    await fs.promises.mkdir(d, { recursive: true });
    const bin = path.join(d, "PPOnline.rel");
    const man = path.join(d, "PPOnline.json");
    const data = pattern(1000, 4);
    await fs.promises.writeFile(bin, data);

    await fs.promises.writeFile(man, JSON.stringify({ file: "PPOnline.rel", size: 1000, sha256: sha256(data) }));
    const ok = await loadPlugin({ binary: bin, manifest: man });
    expect(ok.sha256).toBe(sha256(data));
    expect(ok.data.equals(data)).toBe(true);

    await fs.promises.writeFile(man, JSON.stringify({ sha256: "00".repeat(32) }));
    await expect(loadPlugin({ binary: bin, manifest: man })).rejects.toThrow(/does not match its manifest/);
    await fs.promises.rm(man);
    await expect(loadPlugin({ binary: bin, manifest: man })).rejects.toThrow(/manifest/);
    // Development: no manifest expected.
    expect((await loadPlugin({ binary: bin, manifest: null })).sha256).toBe(sha256(data));
    await expect(loadPlugin({ binary: path.join(d, "nope.rel"), manifest: null })).rejects.toThrow(/PPO_PLUGIN_PATH/);
  });
});
