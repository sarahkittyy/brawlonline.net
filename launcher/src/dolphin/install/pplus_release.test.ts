import { Fat32Volume } from "@brawl_assets/fat32_writer";
import { makeEmptyFat32Image } from "@brawl_assets/testing/fat_image";
import { formatBytes } from "@common/disk_space";
import crypto from "crypto";
import fs from "fs";
import http from "http";
import type { AddressInfo } from "net";
import os from "os";
import path from "path";
import { afterAll, beforeAll, beforeEach, describe, expect, it } from "vitest";
import zlib from "zlib";

import { DISK_SPACE_MARGIN, DiskSpaceError } from "./disk_space";
import type { PPlusProgress, PPlusRelease, PPlusTarget } from "./pplus_release";
import {
  ensureNetplaySave,
  installProjectPlusFiles,
  missingProjectPlusFiles,
  notProjectPlusCard,
  PPLUS_RELEASE,
  readMarker,
  sha256File,
  verifyReleaseZip,
} from "./pplus_release";

// ---- a minimal zip writer (deflate, CRC-32), so the tests need no real P+ release ----------

const CRC_TABLE = (() => {
  const t = new Uint32Array(256);
  for (let n = 0; n < 256; n++) {
    let c = n;
    for (let k = 0; k < 8; k++) {
      c = c & 1 ? 0xedb88320 ^ (c >>> 1) : c >>> 1;
    }
    t[n] = c >>> 0;
  }
  return t;
})();

function crc32(buf: Buffer): number {
  let c = 0xffffffff;
  for (const b of buf) {
    c = CRC_TABLE[(c ^ b) & 0xff] ^ (c >>> 8);
  }
  return (c ^ 0xffffffff) >>> 0;
}

function makeZip(files: Record<string, Buffer>, opts: { badCrcFor?: string } = {}): Buffer {
  const locals: Buffer[] = [];
  const centrals: Buffer[] = [];
  let offset = 0;
  for (const [name, data] of Object.entries(files)) {
    const nameBuf = Buffer.from(name, "utf8");
    const comp = zlib.deflateRawSync(data);
    let crc = crc32(data);
    if (opts.badCrcFor === name) {
      crc = (crc ^ 1) >>> 0;
    }
    const local = Buffer.alloc(30);
    local.writeUInt32LE(0x04034b50, 0);
    local.writeUInt16LE(20, 4);
    local.writeUInt16LE(0x0800, 6); // UTF-8 names
    local.writeUInt16LE(8, 8); // deflate
    local.writeUInt32LE(crc, 14);
    local.writeUInt32LE(comp.length, 18);
    local.writeUInt32LE(data.length, 22);
    local.writeUInt16LE(nameBuf.length, 26);
    locals.push(local, nameBuf, comp);
    const central = Buffer.alloc(46);
    central.writeUInt32LE(0x02014b50, 0);
    central.writeUInt16LE(20, 4);
    central.writeUInt16LE(20, 6);
    central.writeUInt16LE(0x0800, 8);
    central.writeUInt16LE(8, 10);
    central.writeUInt32LE(crc, 16);
    central.writeUInt32LE(comp.length, 20);
    central.writeUInt32LE(data.length, 24);
    central.writeUInt16LE(nameBuf.length, 28);
    central.writeUInt32LE(offset, 42);
    centrals.push(central, nameBuf);
    offset += local.length + nameBuf.length + comp.length;
  }
  const cd = Buffer.concat(centrals);
  const end = Buffer.alloc(22);
  end.writeUInt32LE(0x06054b50, 0);
  end.writeUInt16LE(Object.keys(files).length, 8);
  end.writeUInt16LE(Object.keys(files).length, 10);
  end.writeUInt32LE(cd.length, 12);
  end.writeUInt32LE(offset, 16);
  return Buffer.concat([...locals, cd, end]);
}

const filled = (size: number, seed: number) => {
  const b = Buffer.alloc(size);
  for (let i = 0; i < size; i++) {
    b[i] = (i * 31 + seed * 7 + (i >> 8)) & 0xff;
  }
  return b;
};

const SD = filled(300_000, 1);
const NETPLAY_DOL = filled(4_000, 2);
const OFFLINE_DOL = filled(4_100, 3);
const SAVE_TMD = filled(520, 4);
const SAVE_DATA = filled(9_000, 5);

const releaseFiles = (): Record<string, Buffer> => ({
  "Dolphin.exe": filled(1000, 9),
  "Sys/NetplaySave/title/00010000/52534245/content/title.tmd": SAVE_TMD,
  "Sys/NetplaySave/title/00010000/52534245/data/advsv0.bin": SAVE_DATA,
  "user/Launcher/Project+ Netplay Launcher.dol": NETPLAY_DOL,
  "user/Launcher/Project+ Netplay Launcher.png": filled(100, 6),
  "user/Launcher/Project+ Offline Launcher.dol": OFFLINE_DOL,
  "user/Load/Textures/RSBE01/tex.png": filled(100, 7),
  "user/Wii/sd.raw": SD,
});

// ---- a local HTTP server standing in for GitHub's release download ----------------------------

let root: string;
let server: http.Server;
let baseUrl: string;
let served: Record<string, Buffer> = {};
let requests: string[] = [];
/** Each request's Range header ("" without one). */
let ranges: string[] = [];
/** Like a server without range support: always the whole file. */
let ignoreRange = false;
/** The next responses send this many bytes of the body and then drop the connection. */
let breakOffAfter: number[] = [];

beforeAll(async () => {
  root = await fs.promises.mkdtemp(path.join(os.tmpdir(), "pplus-release-"));
  server = http.createServer((req, res) => {
    requests.push(req.url ?? "");
    ranges.push(req.headers.range ?? "");
    const body = served[req.url ?? ""];
    if (!body) {
      res.writeHead(404).end("not found");
      return;
    }
    const range = /^bytes=(\d+)-$/.exec(req.headers.range ?? "");
    let part = body;
    if (range && !ignoreRange) {
      const from = Number(range[1]);
      if (from >= body.length) {
        res.writeHead(416, { "Content-Range": `bytes */${body.length}` }).end();
        return;
      }
      part = body.subarray(from);
      res.writeHead(206, {
        "Content-Type": "application/zip",
        "Content-Length": part.length,
        "Content-Range": `bytes ${from}-${body.length - 1}/${body.length}`,
      });
    } else {
      res.writeHead(200, { "Content-Type": "application/zip", "Content-Length": body.length });
    }
    const cut = breakOffAfter.shift();
    if (cut !== undefined) {
      res.write(part.subarray(0, cut), () => setTimeout(() => res.destroy(), 20));
      return;
    }
    res.end(part);
  });
  await new Promise<void>((resolve) => server.listen(0, "127.0.0.1", resolve));
  baseUrl = `http://127.0.0.1:${(server.address() as AddressInfo).port}`;
});

afterAll(async () => {
  await new Promise<void>((resolve) => server.close(() => resolve()));
  await fs.promises.rm(root, { recursive: true, force: true });
});

let n = 0;
let target: PPlusTarget;

beforeEach(async () => {
  n += 1;
  const dir = path.join(root, `case-${n}`);
  target = {
    userFolder: path.join(dir, "netplay", "User"),
    storeDir: path.join(dir, "netplay", "pplus"),
    downloadDir: path.join(dir, "netplay", "pplus", "downloads"),
  };
  served = {};
  requests = [];
  ranges = [];
  ignoreRange = false;
  breakOffAfter = [];
});

async function serve(zip: Buffer, name = `p${n}.zip`, overrides: Partial<PPlusRelease> = {}): Promise<PPlusRelease> {
  served[`/${name}`] = zip;
  const tmp = path.join(root, `zip-${n}-${name}`);
  await fs.promises.writeFile(tmp, zip);
  return {
    version: "v9.9.9",
    url: `${baseUrl}/${name}`,
    size: zip.length,
    sha256: await sha256File(tmp),
    ...overrides,
  };
}

const read = (p: string) => fs.promises.readFile(p);
const exists = (p: string) => fs.existsSync(p);

describe("the pinned P+ release", () => {
  it("is P+'s official GitHub release asset with a sha256", () => {
    expect(PPLUS_RELEASE.url).toMatch(
      /^https:\/\/github\.com\/Project-Plus-Development-Team\/Project-Plus-Dolphin\/releases\/download\//,
    );
    expect(PPLUS_RELEASE.sha256).toMatch(/^[0-9a-f]{64}$/);
    expect(PPLUS_RELEASE.size).toBeGreaterThan(0);
  });
});

describe("installProjectPlusFiles", () => {
  it("downloads, verifies and extracts only the files we use", async () => {
    const release = await serve(makeZip(releaseFiles()));
    const progress: PPlusProgress[] = [];
    const logs: string[] = [];

    const result = await installProjectPlusFiles({
      target,
      release,
      onProgress: (p) => progress.push(p),
      log: (m) => logs.push(m),
    });

    expect(result).toBe("installed");
    expect(await read(path.join(target.userFolder, "Wii", "sd.raw"))).toEqual(SD);
    expect(await read(path.join(target.userFolder, "Launcher", "Project+ Netplay Launcher.dol"))).toEqual(NETPLAY_DOL);
    expect(await read(path.join(target.userFolder, "Launcher", "Project+ Offline Launcher.dol"))).toEqual(OFFLINE_DOL);
    const save = path.join(target.storeDir, "NetplaySave", "title", "00010000", "52534245");
    expect(await read(path.join(save, "content", "title.tmd"))).toEqual(SAVE_TMD);
    expect(await read(path.join(save, "data", "advsv0.bin"))).toEqual(SAVE_DATA);
    // Nothing else from the release: no textures, no PNGs, no Dolphin.
    expect(exists(path.join(target.userFolder, "Load"))).toBe(false);
    expect(exists(path.join(target.userFolder, "Launcher", "Project+ Netplay Launcher.png"))).toBe(false);
    expect(fs.readdirSync(target.userFolder).sort()).toEqual(["Launcher", "Wii"]);
    // The download is deleted after extraction; the marker records the release.
    expect(fs.readdirSync(target.downloadDir)).toEqual([]);
    expect(await readMarker(target.storeDir)).toMatchObject({ version: "v9.9.9", sha256: release.sha256 });

    const downloads = progress.filter((p) => p.phase === "download");
    expect(downloads.length).toBeGreaterThan(0);
    expect(downloads[downloads.length - 1]).toEqual({ phase: "download", current: release.size, total: release.size });
    const extracts = progress.filter((p) => p.phase === "extract");
    const extractTotal = SD.length + NETPLAY_DOL.length + OFFLINE_DOL.length + SAVE_TMD.length + SAVE_DATA.length;
    expect(extracts[extracts.length - 1]).toEqual({ phase: "extract", current: extractTotal, total: extractTotal });
    // The check of the download (sha256 over the whole zip) reports progress too.
    const verifies = progress.filter((p) => p.phase === "verify");
    expect(verifies[0]).toEqual({ phase: "verify", current: 0, total: release.size });
    expect(verifies[verifies.length - 1]).toEqual({ phase: "verify", current: release.size, total: release.size });
    // Phases come in order, each one monotonic and never past its total.
    const order = progress.map((p) => p.phase).filter((ph, i, all) => i === 0 || all[i - 1] !== ph);
    expect(order).toEqual(["download", "verify", "extract"]);
    for (const phase of order) {
      const values = progress.filter((p) => p.phase === phase);
      values.forEach((p, i) => {
        expect(p.current).toBeLessThanOrEqual(p.total);
        if (i > 0) {
          expect(p.current).toBeGreaterThanOrEqual(values[i - 1].current);
        }
      });
    }
    expect(await missingProjectPlusFiles(target, release)).toEqual([]);
  });

  it("does nothing when the files are in place", async () => {
    const release = await serve(makeZip(releaseFiles()));
    await installProjectPlusFiles({ target, release });
    requests = [];
    expect(await installProjectPlusFiles({ target, release })).toBe("present");
    expect(requests).toEqual([]);
  });

  it("refuses a download whose sha256 differs and extracts nothing", async () => {
    const release = await serve(makeZip(releaseFiles()), "tampered.zip", { sha256: "0".repeat(64) });
    await expect(installProjectPlusFiles({ target, release })).rejects.toThrow(
      /corrupt \(sha256 [0-9a-f]{64}, expected 0{64}\)/,
    );
    expect(exists(target.userFolder)).toBe(false);
    expect(exists(path.join(target.storeDir, "NetplaySave"))).toBe(false);
    expect(fs.readdirSync(target.downloadDir)).toEqual([]);
    expect(await readMarker(target.storeDir)).toBeNull();
  });

  it("refuses a download of the wrong size", async () => {
    const zip = makeZip(releaseFiles());
    const release = await serve(zip, "short.zip", { size: zip.length + 1 });
    await expect(installProjectPlusFiles({ target, release })).rejects.toThrow(/expected \d+/);
    expect(exists(target.userFolder)).toBe(false);
  });

  it("fails on an HTTP error", async () => {
    const release = await serve(makeZip(releaseFiles()));
    await expect(
      installProjectPlusFiles({ target, release: { ...release, url: `${baseUrl}/missing.zip` } }),
    ).rejects.toThrow(/HTTP 404/);
    expect(exists(target.userFolder)).toBe(false);
    // Nothing was received, so it is not tried again.
    expect(requests).toEqual(["/missing.zip"]);
  });

  it("fails when the release lacks a file we need", async () => {
    const files = releaseFiles();
    delete files["user/Launcher/Project+ Offline Launcher.dol"];
    const release = await serve(makeZip(files));
    await expect(installProjectPlusFiles({ target, release })).rejects.toThrow(
      /has no user\/Launcher\/Project\+ Offline/,
    );
    expect(exists(path.join(target.userFolder, "Wii", "sd.raw"))).toBe(false);
  });

  it("fails on a corrupt entry (CRC) and leaves no partial file", async () => {
    // The zip's sha256 is pinned to the bad zip, so only the per-entry CRC check can catch it.
    const release = await serve(makeZip(releaseFiles(), { badCrcFor: "user/Wii/sd.raw" }));
    await expect(installProjectPlusFiles({ target, release })).rejects.toThrow();
    expect(exists(path.join(target.userFolder, "Wii", "sd.raw"))).toBe(false);
    expect(exists(path.join(target.userFolder, "Wii", "sd.raw.partial"))).toBe(false);
  });

  it("never overwrites the user's SD card, and only adds what is missing", async () => {
    const mine = filled(1234, 42);
    await fs.promises.mkdir(path.join(target.userFolder, "Wii"), { recursive: true });
    await fs.promises.writeFile(path.join(target.userFolder, "Wii", "sd.raw"), mine);
    const release = await serve(makeZip(releaseFiles()));

    expect(await missingProjectPlusFiles(target, release)).toEqual([
      "Launcher/Project+ Netplay Launcher.dol",
      "Launcher/Project+ Offline Launcher.dol",
      "NetplaySave",
    ]);
    await installProjectPlusFiles({ target, release });
    expect(await read(path.join(target.userFolder, "Wii", "sd.raw"))).toEqual(mine);
    expect(await read(path.join(target.userFolder, "Launcher", "Project+ Netplay Launcher.dol"))).toEqual(NETPLAY_DOL);
  });

  it("replaces a blank SD card Dolphin made before the release came (bug report #157)", async () => {
    // Dolphin's own card (SDCardUtil.cpp): an empty FAT32 volume at <User>/Wii/sd.raw.
    const sd = path.join(target.userFolder, "Wii", "sd.raw");
    await fs.promises.mkdir(path.dirname(sd), { recursive: true });
    await makeEmptyFat32Image(sd);
    expect(await notProjectPlusCard(sd)).toBe("blank");
    const release = await serve(makeZip(releaseFiles()));

    expect(await missingProjectPlusFiles(target, release)).toContain("Wii/sd.raw (not a Project+ card)");
    await installProjectPlusFiles({ target, release });
    expect(await read(sd)).toEqual(SD);
    expect(fs.readdirSync(path.dirname(sd))).toEqual(["sd.raw"]);
  });

  it("keeps a card with files of its own but no Project+ next to the release's", async () => {
    const sd = path.join(target.userFolder, "Wii", "sd.raw");
    await fs.promises.mkdir(path.dirname(sd), { recursive: true });
    await makeEmptyFat32Image(sd);
    const vol = await Fat32Volume.open(sd, { writable: true });
    await vol.writeFile("/private/wii/app/RSBE/st/st_001.bin", filled(700, 8));
    await vol.close();
    const theirs = await sha256File(sd);
    expect(await notProjectPlusCard(sd)).toBe("other");
    const release = await serve(makeZip(releaseFiles()));

    await installProjectPlusFiles({ target, release });
    expect(await read(sd)).toEqual(SD);
    expect(await sha256File(path.join(path.dirname(sd), "sd (not Project+).raw"))).toBe(theirs);
  });

  it("takes a FAT card with a Project+ folder for Project+'s", async () => {
    const sd = path.join(target.userFolder, "Wii", "sd.raw");
    await fs.promises.mkdir(path.dirname(sd), { recursive: true });
    await makeEmptyFat32Image(sd);
    const vol = await Fat32Volume.open(sd, { writable: true });
    await vol.writeFile("/Project+/info.xml", Buffer.from("<version>3.2</version>"));
    await vol.close();
    const mine = await sha256File(sd);
    expect(await notProjectPlusCard(sd)).toBeNull();
    const release = await serve(makeZip(releaseFiles()));

    expect(await missingProjectPlusFiles(target, release)).not.toContainEqual(expect.stringMatching(/sd\.raw/));
    await installProjectPlusFiles({ target, release });
    expect(await sha256File(sd)).toBe(mine);
  });

  it("reuses a verified download left by an interrupted extraction", async () => {
    const zip = makeZip(releaseFiles());
    const release = await serve(zip);
    await fs.promises.mkdir(target.downloadDir, { recursive: true });
    await fs.promises.writeFile(path.join(target.downloadDir, `pplus-${release.version}.zip`), zip);
    const phases = new Set<string>();
    await installProjectPlusFiles({ target, release, onProgress: (p) => phases.add(p.phase) });
    expect(requests).toEqual([]);
    // The kept zip is checked again (with progress) before it is used; nothing is downloaded.
    expect([...phases]).toEqual(["verify", "extract"]);
    expect(await read(path.join(target.userFolder, "Wii", "sd.raw"))).toEqual(SD);
  });

  it("downloads again when a left-over download does not verify", async () => {
    const zip = makeZip(releaseFiles());
    const release = await serve(zip);
    await fs.promises.mkdir(target.downloadDir, { recursive: true });
    await fs.promises.writeFile(path.join(target.downloadDir, `pplus-${release.version}.zip`), zip.subarray(0, 100));
    await installProjectPlusFiles({ target, release });
    expect(requests).toEqual([`/p${n}.zip`]);
  });

  it("refuses before downloading when the disk lacks room for the download and its files", async () => {
    const release = await serve(makeZip(releaseFiles()), undefined, { unpackedSize: 3e9 });
    const asked: string[] = [];
    const freeBytes = async (dir: string) => {
      asked.push(dir);
      return 3e9;
    };
    const err = await installProjectPlusFiles({ target, release, freeBytes }).catch((e) => e);
    expect(err).toBeInstanceOf(DiskSpaceError);
    // The rest of the download + the unpacked files + the margin.
    expect(err.message).toContain(`${formatBytes(release.size + 3e9 + DISK_SPACE_MARGIN)} needed, 3.0 GB free`);
    expect(asked).toEqual([target.downloadDir]);
    expect(requests).toEqual([]);
  });

  it("checks the unpacked files' exact size before extracting a kept download", async () => {
    const zip = makeZip(releaseFiles());
    const release = await serve(zip);
    await fs.promises.mkdir(target.downloadDir, { recursive: true });
    await fs.promises.writeFile(path.join(target.downloadDir, `pplus-${release.version}.zip`), zip);
    const err = await installProjectPlusFiles({ target, release, freeBytes: async () => 1000 }).catch((e) => e);
    expect(err).toBeInstanceOf(DiskSpaceError);
    const extractTotal = SD.length + NETPLAY_DOL.length + OFFLINE_DOL.length + SAVE_TMD.length + SAVE_DATA.length;
    expect(err.message).toContain(`${formatBytes(extractTotal + DISK_SPACE_MARGIN)} needed, 1 KB free`);
    expect(exists(path.join(target.userFolder, "Wii", "sd.raw"))).toBe(false);
    // The verified download stays for the next try.
    expect(exists(path.join(target.downloadDir, `pplus-${release.version}.zip`))).toBe(true);
  });
});

describe("installProjectPlusFiles after an interrupted download", () => {
  const partOf = (release: PPlusRelease) => path.join(target.downloadDir, `pplus-${release.version}.zip.part`);
  const fast = { retryDelayMs: 0, overlapBytes: 0 };
  // Incompressible, so the zip is large enough to break off in (the other entries deflate to a few KB).
  const bigZip = () => makeZip({ ...releaseFiles(), "Dolphin.exe": crypto.randomBytes(200_000) });

  it("continues the part a crashed launcher left", async () => {
    const zip = bigZip();
    const release = await serve(zip);
    await fs.promises.mkdir(target.downloadDir, { recursive: true });
    await fs.promises.writeFile(partOf(release), zip.subarray(0, 100_000));
    const progress: PPlusProgress[] = [];
    const logs: string[] = [];

    await installProjectPlusFiles({
      target,
      release,
      downloadOptions: fast,
      onProgress: (p) => progress.push(p),
      log: (m) => logs.push(m),
    });

    expect(ranges).toEqual(["bytes=100000-"]);
    expect(await read(path.join(target.userFolder, "Wii", "sd.raw"))).toEqual(SD);
    expect(fs.readdirSync(target.downloadDir)).toEqual([]);
    // Progress counts what the part already had.
    const downloads = progress.filter((p) => p.phase === "download");
    expect(downloads[0].current).toBeGreaterThan(100_000);
    expect(downloads[downloads.length - 1]).toEqual({ phase: "download", current: zip.length, total: zip.length });
    expect(logs).toContain(`Resuming the download of ${release.url} at 100000 of ${zip.length} bytes`);
  });

  it("fetches the part's last bytes again, in case the PC crashed while writing them", async () => {
    const zip = bigZip();
    const release = await serve(zip);
    await fs.promises.mkdir(target.downloadDir, { recursive: true });
    // A torn tail: the last 4 KB of the part never reached the disk.
    await fs.promises.writeFile(partOf(release), Buffer.concat([zip.subarray(0, 96_000), Buffer.alloc(4_000)]));

    await installProjectPlusFiles({ target, release, downloadOptions: { retryDelayMs: 0, overlapBytes: 8_000 } });

    expect(ranges).toEqual(["bytes=92000-"]);
    expect(await read(path.join(target.userFolder, "Wii", "sd.raw"))).toEqual(SD);
  });

  it("goes on where the connection broke off", async () => {
    const zip = bigZip();
    const release = await serve(zip);
    breakOffAfter = [50_000, 30_000];
    const logs: string[] = [];

    await installProjectPlusFiles({ target, release, downloadOptions: fast, log: (m) => logs.push(m) });

    expect(ranges).toEqual(["", "bytes=50000-", "bytes=80000-"]);
    expect(await read(path.join(target.userFolder, "Wii", "sd.raw"))).toEqual(SD);
    expect(logs.filter((m) => m.startsWith("The download broke off after"))).toHaveLength(2);
  });

  it("keeps the part when every attempt breaks off, and the next install continues it", async () => {
    const zip = bigZip();
    const release = await serve(zip);
    breakOffAfter = [10_000, 10_000];

    await expect(
      installProjectPlusFiles({ target, release, downloadOptions: { ...fast, attempts: 2 } }),
    ).rejects.toThrow();
    expect(fs.statSync(partOf(release)).size).toBe(20_000);
    expect(exists(target.userFolder)).toBe(false);

    await installProjectPlusFiles({ target, release, downloadOptions: fast });
    expect(ranges).toEqual(["", "bytes=10000-", "bytes=20000-"]);
    expect(await read(path.join(target.userFolder, "Wii", "sd.raw"))).toEqual(SD);
  });

  it("starts over when the server sends the whole file", async () => {
    const zip = bigZip();
    const release = await serve(zip);
    ignoreRange = true;
    await fs.promises.mkdir(target.downloadDir, { recursive: true });
    await fs.promises.writeFile(partOf(release), zip.subarray(0, 100_000));

    await installProjectPlusFiles({ target, release, downloadOptions: fast });

    expect(ranges).toEqual(["bytes=100000-"]);
    expect(await read(path.join(target.userFolder, "Wii", "sd.raw"))).toEqual(SD);
  });

  it("checks a complete part without downloading, and deletes one that does not verify", async () => {
    const zip = bigZip();
    const release = await serve(zip);
    await fs.promises.mkdir(target.downloadDir, { recursive: true });
    const bad = Buffer.from(zip);
    bad[10] ^= 1;
    await fs.promises.writeFile(partOf(release), bad);

    await expect(installProjectPlusFiles({ target, release, downloadOptions: fast })).rejects.toThrow(/corrupt/);
    expect(requests).toEqual([]);
    expect(exists(partOf(release))).toBe(false);

    // The next install downloads it whole.
    await installProjectPlusFiles({ target, release, downloadOptions: fast });
    expect(ranges).toEqual([""]);
  });

  it("downloads again when the part is longer than the release", async () => {
    const zip = bigZip();
    const release = await serve(zip);
    await fs.promises.mkdir(target.downloadDir, { recursive: true });
    await fs.promises.writeFile(partOf(release), Buffer.concat([zip, Buffer.alloc(10)]));

    await installProjectPlusFiles({ target, release, downloadOptions: fast });

    expect(ranges).toEqual([""]);
    expect(await read(path.join(target.userFolder, "Wii", "sd.raw"))).toEqual(SD);
  });

  it("starts over when the part reaches past the server's file (416)", async () => {
    const zip = bigZip();
    const release = await serve(zip);
    await fs.promises.mkdir(target.downloadDir, { recursive: true });
    await fs.promises.writeFile(partOf(release), zip.subarray(0, 1000));
    // The server now has less than the part.
    served[`/p${n}.zip`] = zip.subarray(0, 500);

    await expect(installProjectPlusFiles({ target, release, downloadOptions: fast })).rejects.toThrow(/expected/);
    expect(ranges).toEqual(["bytes=1000-", ""]);
  });
});

describe("verifyReleaseZip", () => {
  it("accepts the exact file and rejects any other", async () => {
    const zip = makeZip(releaseFiles());
    const release = await serve(zip);
    const file = path.join(root, `verify-${n}.zip`);
    await fs.promises.writeFile(file, zip);
    await expect(verifyReleaseZip(file, release)).resolves.toBeUndefined();
    const flipped = Buffer.from(zip);
    flipped[10] ^= 0xff;
    await fs.promises.writeFile(file, flipped);
    await expect(verifyReleaseZip(file, release)).rejects.toThrow(/corrupt/);
  });
});

describe("ensureNetplaySave", () => {
  it("copies the save template into Dolphin's User folder once", async () => {
    const release = await serve(makeZip(releaseFiles()));
    await installProjectPlusFiles({ target, release });
    const user = target.userFolder;

    expect(await ensureNetplaySave(target.storeDir, user)).toBe(true);
    const tmd = path.join(user, "NetplaySave", "title", "00010000", "52534245", "content", "title.tmd");
    expect(await read(tmd)).toEqual(SAVE_TMD);
    // An existing template is left as it is.
    await fs.promises.writeFile(tmd, "changed");
    expect(await ensureNetplaySave(target.storeDir, user)).toBe(true);
    expect((await read(tmd)).toString()).toBe("changed");
  });

  it("reports false when there is nothing to copy yet", async () => {
    expect(await ensureNetplaySave(target.storeDir, target.userFolder)).toBe(false);
    expect(fs.existsSync(path.join(target.userFolder, "NetplaySave"))).toBe(false);
  });
});
