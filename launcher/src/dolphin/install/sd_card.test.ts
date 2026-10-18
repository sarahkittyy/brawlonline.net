import { FatImage } from "@brawl_assets/fat32";
import { Fat32Volume } from "@brawl_assets/fat32_writer";
import { makeEmptyFat32Image, pattern } from "@brawl_assets/testing/fat_image";
import fs from "fs";
import os from "os";
import path from "path";
import { afterAll, beforeAll, beforeEach, describe, expect, it } from "vitest";

import { PLUGIN_SD_PATH } from "./paths";
import type { PluginBinary } from "./sd_card";
import {
  installPluginOnSdCard,
  loadPlugin,
  PPLUS_REPLAY_DIR,
  pruneGameReplays,
  readFileHashFromImage,
  SdCardInUseError,
  sha256,
} from "./sd_card";

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

  it("shares a copy in use as it is, without reading it back (bug report #17)", async () => {
    const p = plugin(11);
    const first = await installPluginOnSdCard(opts(p));
    // What the read-back would take for a broken plugin: in use, it is left alone.
    await writeOnCard(first.image, PLUGIN_SD_PATH, pattern(500, 99));
    const imgStat = await fs.promises.stat(first.image);
    const manStat = await fs.promises.stat(first.manifest);

    const res = await installPluginOnSdCard({ ...opts(p), inUse: true });
    expect(res.action).toBe("unchanged");
    expect(res.image).toBe(first.image);
    expect((await fs.promises.stat(first.image)).mtimeMs).toBe(imgStat.mtimeMs);
    expect((await fs.promises.stat(first.manifest)).mtimeMs).toBe(manStat.mtimeMs);
  });

  it("refuses a copy in use that needs a change, and leaves it and its manifest", async () => {
    const first = await installPluginOnSdCard(opts(plugin(12)));
    const imgHash = await fileHash(first.image);
    const manifest = await fs.promises.readFile(first.manifest, "utf8");

    await expect(installPluginOnSdCard({ ...opts(plugin(13)), inUse: true })).rejects.toBeInstanceOf(SdCardInUseError);
    const later = new Date(Date.now() + 5000);
    await fs.promises.utimes(source, later, later);
    await expect(installPluginOnSdCard({ ...opts(plugin(12)), inUse: true })).rejects.toThrow(/source card changed/);
    expect(await fileHash(first.image)).toBe(imgHash);
    expect(await fs.promises.readFile(first.manifest, "utf8")).toBe(manifest);
  });

  it("refuses to make a first copy while another Dolphin runs", async () => {
    await expect(installPluginOnSdCard({ ...opts(plugin(14)), inUse: true })).rejects.toThrow(/no copy/);
    expect(fs.existsSync(path.join(out, "sd.raw"))).toBe(false);
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

  it("reports the copy's progress on the first Play only", async () => {
    const p = plugin(11);
    const { size } = await fs.promises.stat(source);
    const calls: [number, number][] = [];
    const first = await installPluginOnSdCard({
      ...opts(p),
      onProgress: (d, t) => calls.push([d, t]),
      clone: async () => {
        throw Object.assign(new Error("no reflinks here"), { code: "ENOTSUP" });
      },
    });
    expect(first.action).toBe("copied");
    expect(calls.length).toBeGreaterThan(2);
    calls.forEach(([d, t], i) => {
      expect(t).toBe(size);
      expect(d).toBeLessThanOrEqual(t);
      if (i > 0) {
        expect(d).toBeGreaterThanOrEqual(calls[i - 1][0]);
      }
    });
    expect(calls[0]).toEqual([0, size]);
    expect(calls[calls.length - 1]).toEqual([size, size]);

    // Nothing to copy later: no progress (the Play button never changes).
    calls.length = 0;
    expect((await installPluginOnSdCard({ ...opts(p), onProgress: (d, t) => calls.push([d, t]) })).action).toBe(
      "unchanged",
    );
    expect(
      (await installPluginOnSdCard({ ...opts(plugin(12)), onProgress: (d, t) => calls.push([d, t]) })).action,
    ).toBe("replaced");
    expect(calls).toEqual([]);
  });

  it("takes the clone when the file system has one", async () => {
    const p = plugin(13);
    const cloned: string[] = [];
    const res = await installPluginOnSdCard({
      ...opts(p),
      clone: async (s, d) => {
        cloned.push(d);
        await fs.promises.copyFile(s, d);
      },
    });
    expect(res.action).toBe("copied");
    expect(cloned).toEqual([`${res.image}.partial`]);
    expect(logs.some((m) => /SD card cloned in/.test(m))).toBe(true);
    expect(await readFileHashFromImage(res.image, PLUGIN_SD_PATH)).toBe(p.sha256);
  });

  it("leaves nothing that looks done when the copy is interrupted, and copies again next time", async () => {
    const p = plugin(14);
    const srcHash = await fileHash(source);
    let calls = 0;
    const interrupted = installPluginOnSdCard({
      ...opts(p),
      clone: async () => {
        throw new Error("no clone");
      },
      onProgress: () => {
        // The launcher closing mid-copy, as seen from the copy.
        if (++calls === 3) {
          throw new Error("closed");
        }
      },
    });
    await expect(interrupted).rejects.toThrow("closed");
    expect(fs.existsSync(path.join(out, "sd.raw"))).toBe(false);
    expect(fs.existsSync(path.join(out, "sd.raw.partial"))).toBe(false);
    expect(fs.existsSync(path.join(out, "manifest.json"))).toBe(false);
    expect(await fileHash(source)).toBe(srcHash);

    // A partial file left by a killed launcher (no clean-up ran) is replaced too.
    await fs.promises.writeFile(path.join(out, "sd.raw.partial"), "half a card");
    const res = await installPluginOnSdCard(opts(p));
    expect(res.action).toBe("copied");
    expect(await readFileHashFromImage(res.image, PLUGIN_SD_PATH)).toBe(p.sha256);
    expect(fs.existsSync(path.join(out, "sd.raw.partial"))).toBe(false);
  });

  it("fails clearly without a source card", async () => {
    await expect(
      installPluginOnSdCard({ ...opts(plugin(1)), sourceImage: path.join(dir, "missing.raw") }),
    ).rejects.toThrow(/SD card was not found/);
  });

  // Bug report #48: P+'s autosaved replays filled the copy, and the game hung on the results screen.
  it("deletes P+'s oldest replays from a copy that is nearly full before Play", async () => {
    const p = plugin(7);
    const first = await installPluginOnSdCard(opts(p));
    const names = replayNames(40);
    for (const name of names) {
      await writeOnCard(first.image, `${PPLUS_REPLAY_DIR}/${name}`, pattern(30_000, 3));
    }
    await fillCard(first.image, 200_000);
    const prune = { lowFreeBytes: 400_000, targetFreeBytes: 600_000 };

    const res = await installPluginOnSdCard({ ...opts(p), prune });
    expect(res.action).toBe("unchanged");
    const left = await replaysOn(res.image);
    expect(left.length).toBeGreaterThan(0);
    expect(left.length).toBeLessThan(names.length);
    expect(left).toEqual(names.slice(names.length - left.length)); // the newest are kept
    expect(await freeBytes(res.image)).toBeGreaterThanOrEqual(prune.targetFreeBytes);
    expect(await readFileHashFromImage(res.image, PLUGIN_SD_PATH)).toBe(p.sha256);
    expect(logs.some((m) => /deleted \d+ of P\+'s oldest replays/.test(m))).toBe(true);
    await checkCard(res.image);

    // Enough space now: the next Play deletes nothing.
    const again = await installPluginOnSdCard({ ...opts(p), prune });
    expect(again.action).toBe("unchanged");
    expect(await replaysOn(again.image)).toEqual(left);
  });

  it("prunes a fresh copy of a nearly full card before patching it", async () => {
    for (const name of replayNames(20)) {
      await writeOnCard(source, `${PPLUS_REPLAY_DIR}/${name}`, pattern(30_000, 4));
    }
    await fillCard(source, 100_000);
    const srcHash = await fileHash(source);
    const p = plugin(8, 150_000); // more than the source card has free

    const res = await installPluginOnSdCard({ ...opts(p), prune: { lowFreeBytes: 400_000, targetFreeBytes: 600_000 } });
    expect(res.action).toBe("copied");
    expect(await readFileHashFromImage(res.image, PLUGIN_SD_PATH)).toBe(p.sha256);
    expect((await replaysOn(res.image)).length).toBeLessThan(20);
    expect(await fileHash(source)).toBe(srcHash); // the player's own card keeps its replays
    await checkCard(res.image);
  });
});

const replayNames = (n: number) =>
  Array.from({ length: n }, (_, i) => {
    const day = 11 + Math.floor(i / 10);
    const minute = String(i % 10).padStart(2, "0");
    return `rp_2610${day}_23${minute}_05.bin`;
  });

async function replaysOn(image: string): Promise<string[]> {
  const vol = await Fat32Volume.open(image);
  try {
    const dir = await vol.lookup(PPLUS_REPLAY_DIR);
    return dir
      ? (await vol.listDir(dir.firstCluster))
          .filter((e) => !e.isDirectory && e.name.startsWith("rp_"))
          .map((e) => e.name)
          .sort()
      : [];
  } finally {
    await vol.close();
  }
}

async function freeBytes(image: string): Promise<number> {
  const vol = await Fat32Volume.open(image);
  try {
    return vol.freeClusters() * vol.clusterSize;
  } finally {
    await vol.close();
  }
}

/** Writes a filler file so that about `leave` bytes stay free. */
async function fillCard(image: string, leave: number) {
  const free = await freeBytes(image);
  await writeOnCard(image, "/Project+/pf/filler.bin", Buffer.alloc(Math.max(0, free - leave - 4096), 1));
}

async function checkCard(image: string) {
  const vol = await Fat32Volume.open(image);
  try {
    expect(await vol.check()).toEqual([]);
  } finally {
    await vol.close();
  }
}

describe("pruneGameReplays", () => {
  let image: string;

  beforeEach(async () => {
    image = path.join(root, `prune-${n++}.raw`);
    await makeEmptyFat32Image(image);
    await writeOnCard(image, `${PPLUS_REPLAY_DIR}/Folder needed for autosave replays.txt`, Buffer.alloc(0));
    await writeOnCard(image, "/Project+/pf/plugins/AsyncRSP.rel", pattern(7080, 1));
  });

  it("only reads a card with enough free space", async () => {
    await writeOnCard(image, `${PPLUS_REPLAY_DIR}/rp_261011_2300_05.bin`, pattern(30_000, 2));
    const hash = await fileHash(image);
    const res = await pruneGameReplays(image, { lowFreeBytes: 1_000_000, targetFreeBytes: 2_000_000 });
    expect(res.deleted).toBe(0);
    expect(await fileHash(image)).toBe(hash);
  });

  it("deletes only P+'s replays, oldest first, and keeps the rest of the card", async () => {
    const names = replayNames(30);
    for (const name of [...names].reverse()) {
      await writeOnCard(image, `${PPLUS_REPLAY_DIR}/${name}`, pattern(20_000, 5));
    }
    await writeOnCard(image, `${PPLUS_REPLAY_DIR}/my replay.bin`, pattern(1000, 6));
    await fillCard(image, 50_000);

    const res = await pruneGameReplays(image, { lowFreeBytes: 100_000, targetFreeBytes: 250_000 });
    expect(res.freeBytes).toBeGreaterThanOrEqual(250_000);
    const left = await replaysOn(image);
    expect(left).toEqual(names.slice(res.deleted));
    expect(res.deleted).toBeGreaterThanOrEqual(10);
    expect(await readOnCard(image, `${PPLUS_REPLAY_DIR}/Folder needed for autosave replays.txt`)).not.toBeNull();
    expect(await readOnCard(image, `${PPLUS_REPLAY_DIR}/my replay.bin`)).not.toBeNull();
    expect(await readOnCard(image, "/Project+/pf/plugins/AsyncRSP.rel")).not.toBeNull();
    await checkCard(image);
    // The independent reader agrees.
    const fat = await FatImage.open(image);
    try {
      expect(await fat.exists(`${PPLUS_REPLAY_DIR}/${names[0]}`)).toBe(false);
      expect(await fat.exists(`${PPLUS_REPLAY_DIR}/${names[names.length - 1]}`)).toBe(true);
    } finally {
      await fat.close();
    }
  });

  it("deletes every replay when that is still not enough, and stops there", async () => {
    for (const name of replayNames(5)) {
      await writeOnCard(image, `${PPLUS_REPLAY_DIR}/${name}`, pattern(10_000, 7));
    }
    await fillCard(image, 20_000);
    const res = await pruneGameReplays(image, { lowFreeBytes: 1_000_000, targetFreeBytes: 2_000_000 });
    expect(res.deleted).toBe(5);
    expect(res.freeBytes).toBeLessThan(2_000_000);
    expect(await replaysOn(image)).toEqual([]);
    await checkCard(image);
  });

  it("does nothing on a card without a replay folder", async () => {
    const bare = path.join(root, `prune-bare-${n++}.raw`);
    await makeEmptyFat32Image(bare);
    await fillCard(bare, 20_000);
    const res = await pruneGameReplays(bare, { lowFreeBytes: 1_000_000, targetFreeBytes: 2_000_000 });
    expect(res.deleted).toBe(0);
    await checkCard(bare);
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
