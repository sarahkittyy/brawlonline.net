import { createCipheriv, createHash } from "crypto";
import { mkdir, mkdtemp, readdir, readFile, rm, utimes, writeFile } from "fs/promises";
import os from "os";
import path from "path";
import { afterEach, beforeEach, describe, expect, it } from "vitest";

import {
  escapeNandName,
  findDolphinSaveNear,
  findNandSave,
  findOwnDolphinSave,
  importSave,
  KEEP_BACKUPS,
  moveSaveStore,
  ownDolphinUserFolders,
  readDataBin,
  sameSave,
  saveStoreDir,
  titleDataDir,
  titleGameId,
  titleIdHex,
} from "./save_import";

const SD_KEY = Buffer.from("ab01b9d8e1622b08afbad84dbfc2a55d", "hex");
const SD_IV = Buffer.from("216712e6aa1f689f95c5a22324dc6a98", "hex");
const BLANKER = Buffer.from("0e65378199be4517ab06ec22451a5793", "hex");
const NSMBW = "00010000534d4e45";
const MKW = "00010004524d4345";
const HASH = "14e34197d0329e2e" + "0".repeat(48);

const encrypt = (data: Buffer, iv: Buffer) => {
  const c = createCipheriv("aes-128-cbc", SD_KEY, iv).setAutoPadding(false);
  return Buffer.concat([c.update(data), c.final()]);
};

type BinFile = { name: string; data?: Buffer };

/** A data.bin as Dolphin's WiiSave::Export writes it (DataBinStorage::WriteHeader, WriteFiles):
 * the header encrypted with the SD key, the Bk header, each file's header and its data (padded to
 * 64 bytes, encrypted with the file header's IV: zeros, as Dolphin leaves it), the signatures. */
function makeDataBin(titleId: string, banner: Buffer, files: BinFile[]): Buffer {
  const header = Buffer.alloc(0xf0c0);
  header.writeBigUInt64BE(BigInt("0x" + titleId), 0);
  header.writeUInt32BE(banner.length, 8);
  header[12] = 0x34;
  BLANKER.copy(header, 14);
  banner.copy(header, 32);
  createHash("md5").update(header).digest().copy(header, 14);
  const body: Buffer[] = [];
  for (const f of files) {
    const fh = Buffer.alloc(0x80);
    fh.writeUInt32BE(0x03adf17e, 0);
    fh[8] = 0x3c;
    fh[10] = f.data ? 1 : 2;
    fh.write(f.name, 11, "latin1");
    if (f.data) {
      fh.writeUInt32BE(f.data.length, 4);
      const padded = Buffer.alloc(Math.ceil(f.data.length / 64) * 64);
      f.data.copy(padded);
      body.push(fh, encrypt(padded, Buffer.alloc(16)));
    } else {
      body.push(fh);
    }
  }
  const sizeOfFiles = body.reduce((n, b) => n + b.length, 0);
  const bk = Buffer.alloc(0x80);
  bk.writeUInt32BE(0x70, 0);
  bk.writeUInt32BE(0x426b0001, 4);
  bk.writeUInt32BE(0x0403ac68, 8);
  bk.writeUInt32BE(files.length, 12);
  bk.writeUInt32BE(sizeOfFiles, 16);
  bk.writeUInt32BE(sizeOfFiles + 0x3c0, 28);
  bk.writeBigUInt64BE(BigInt("0x" + titleId), 0x60);
  return Buffer.concat([encrypt(header, SD_IV), bk, ...body, Buffer.alloc(0x3c0, 0xaa)]);
}

const BANNER = Buffer.alloc(0x72a0, 7);
const SAVE_DATA = Buffer.from(Array.from({ length: 2000 }, (_, i) => i % 251));

async function listTree(dir: string): Promise<string[]> {
  const out: string[] = [];
  const walk = async (d: string, rel: string) => {
    for (const e of await readdir(d, { withFileTypes: true })) {
      const r = rel ? `${rel}/${e.name}` : e.name;
      if (e.isDirectory()) {
        out.push(`${r}/`);
        await walk(path.join(d, e.name), r);
      } else {
        out.push(r);
      }
    }
  };
  await walk(dir, "");
  return out.sort();
}

describe("save import", () => {
  let tmp: string;
  let user: string;
  beforeEach(async () => {
    tmp = await mkdtemp(path.join(os.tmpdir(), "save-import-"));
    user = path.join(tmp, "netplay", "User");
    await mkdir(user, { recursive: true });
  });
  afterEach(async () => {
    await rm(tmp, { recursive: true, force: true });
  });
  const storeData = (titleId = NSMBW) => titleDataDir(saveStoreDir(user, HASH), titleId);

  it("names the store as Dolphin does: the build hash's first 16 digits, the title's data folder", () => {
    expect(saveStoreDir(user, HASH)).toBe(path.join(user, "Saves", "14e34197d0329e2e"));
    expect(titleDataDir("S", MKW)).toBe(path.join("S", "title", "00010004", "524d4345", "data"));
    expect(titleIdHex(281476374285893)).toBe(NSMBW);
    expect(titleGameId(NSMBW)).toBe("SMNE");
    expect(titleGameId("0001000100000002")).toBe("0001000100000002");
  });

  it("moves a build's save to its new hash, unless that one has a save already", async () => {
    const NEW = "f".repeat(64);
    await mkdir(storeData(), { recursive: true });
    await writeFile(path.join(storeData(), "save.bin"), "progress");
    expect(await moveSaveStore(user, HASH, NEW)).toBe(true);
    expect(await readFile(path.join(titleDataDir(saveStoreDir(user, NEW), NSMBW), "save.bin"), "utf8")).toBe(
      "progress",
    );
    // Nothing left to move; and a store the new hash has is never overwritten.
    expect(await moveSaveStore(user, HASH, NEW)).toBe(false);
    await mkdir(storeData(), { recursive: true });
    expect(await moveSaveStore(user, HASH, NEW)).toBe(false);
    expect(await moveSaveStore(user, NEW, NEW)).toBe(false);
  });

  it("escapes NAND names as Dolphin's host file system does", () => {
    expect(escapeNandName("wiiking2.sav")).toBe("wiiking2.sav");
    expect(escapeNandName("a:b?")).toBe("a__3a__b__3f__");
    expect(escapeNandName("x__y")).toBe("x__5f____5f__y");
    expect(escapeNandName("..")).toBe("__2e____2e__");
  });

  it("reads a data.bin: its title, the banner and every file and folder", () => {
    const bin = makeDataBin(NSMBW, BANNER, [
      { name: "wiiking2.sav", data: SAVE_DATA },
      { name: "sub" },
      { name: "sub/x.bin", data: Buffer.from("hello") },
    ]);
    const save = readDataBin(bin);
    expect(save.titleId).toBe(NSMBW);
    expect(save.entries.map((e) => [e.path, e.data?.length ?? null])).toEqual([
      ["banner.bin", 0x72a0],
      ["wiiking2.sav", 2000],
      ["sub", null],
      ["sub/x.bin", 5],
    ]);
    expect(save.entries[0].data!.equals(BANNER)).toBe(true);
    expect(save.entries[1].data!.equals(SAVE_DATA)).toBe(true);
    expect(save.entries[3].data!.toString()).toBe("hello");
  });

  it("refuses what is not a save, a damaged one, and file names that leave the save", () => {
    expect(() => readDataBin(Buffer.alloc(0x10000, 1))).toThrow(/not a Wii save/);
    const bin = makeDataBin(NSMBW, BANNER, [{ name: "a.sav", data: SAVE_DATA }]);
    const flipped = Buffer.from(bin);
    flipped[100] ^= 1;
    expect(() => readDataBin(flipped)).toThrow(/damaged|not a Wii save/);
    expect(() => readDataBin(bin.subarray(0, bin.length - 0x3c0 - 64))).toThrow(/cut short/);
    expect(() => readDataBin(makeDataBin(NSMBW, BANNER, [{ name: "../evil", data: SAVE_DATA }]))).toThrow(/bad name/);
    expect(() => readDataBin(makeDataBin(NSMBW, BANNER, [{ name: "a//b", data: SAVE_DATA }]))).toThrow(/bad name/);
  });

  it("imports a data.bin into a store with no save yet", async () => {
    const binPath = path.join(tmp, "data.bin");
    const files = [
      { name: "wiiking2.sav", data: SAVE_DATA },
      { name: "odd:name", data: Buffer.from("x") },
    ];
    await writeFile(binPath, makeDataBin(NSMBW, BANNER, files));
    const result = await importSave({ source: binPath, titleId: NSMBW, userFolder: user, buildHash: HASH });
    expect(result.backup).toBeNull();
    expect(result.save).toMatchObject({ folder: storeData(), files: 3, bytes: 0x72a0 + 2000 + 1 });
    expect(await listTree(storeData())).toEqual(["banner.bin", "odd__3a__name", "wiiking2.sav"]);
    expect((await readFile(path.join(storeData(), "wiiking2.sav"))).equals(SAVE_DATA)).toBe(true);
    // Nothing is left behind next to the backups.
    expect(await readdir(path.join(user, "SaveBackups"))).toEqual([]);
  });

  it("refuses another game's data.bin and leaves the store as it was", async () => {
    await mkdir(storeData(), { recursive: true });
    await writeFile(path.join(storeData(), "wiiking2.sav"), "mine");
    const binPath = path.join(tmp, "data.bin");
    await writeFile(binPath, makeDataBin(MKW, BANNER, [{ name: "rksys.dat", data: SAVE_DATA }]));
    await expect(importSave({ source: binPath, titleId: NSMBW, userFolder: user, buildHash: HASH })).rejects.toThrow(
      "This save is for another game (RMCE), not SMNE.",
    );
    expect(await listTree(storeData())).toEqual(["wiiking2.sav"]);
  });

  it("imports from a Dolphin User folder, the save it replaces kept as a backup", async () => {
    const dolphin = path.join(tmp, "Dolphin Emulator");
    const nandSave = path.join(dolphin, "Wii", "title", "00010000", "534d4e45", "data");
    await mkdir(nandSave, { recursive: true });
    await writeFile(path.join(nandSave, "banner.bin"), BANNER);
    await writeFile(path.join(nandSave, "wiiking2.sav"), SAVE_DATA);
    // The store's save from a room: another file too, which must not stay.
    await mkdir(storeData(), { recursive: true });
    await writeFile(path.join(storeData(), "wiiking2.sav"), "fresh");
    await writeFile(path.join(storeData(), "stale.bin"), "old");

    const now = new Date("2026-10-19T02:30:00.123Z");
    const result = await importSave({ source: dolphin, titleId: NSMBW, userFolder: user, buildHash: HASH, now });
    expect(await listTree(storeData())).toEqual(["banner.bin", "wiiking2.sav"]);
    expect((await readFile(path.join(storeData(), "wiiking2.sav"))).equals(SAVE_DATA)).toBe(true);
    expect(result.backup).toBe(path.join(user, "SaveBackups", "14e34197d0329e2e", "20261019-023000-123", "data"));
    expect(await listTree(result.backup!)).toEqual(["stale.bin", "wiiking2.sav"]);
    // The player's Dolphin folder is only read.
    expect(await listTree(nandSave)).toEqual(["banner.bin", "wiiking2.sav"]);
  });

  it("keeps the newest backups only", async () => {
    const src = path.join(tmp, "src", "data");
    await mkdir(src, { recursive: true });
    await writeFile(path.join(src, "a.sav"), "new");
    for (let i = 0; i < KEEP_BACKUPS + 2; i++) {
      await mkdir(storeData(), { recursive: true });
      await writeFile(path.join(storeData(), "a.sav"), `v${i}`);
      const now = new Date(Date.UTC(2026, 9, 19, 0, 0, i));
      await importSave({ source: src, titleId: NSMBW, userFolder: user, buildHash: HASH, now });
    }
    const kept = (await readdir(path.join(user, "SaveBackups", "14e34197d0329e2e"))).sort();
    expect(kept).toHaveLength(KEEP_BACKUPS);
    expect(kept[0]).toBe("20261019-000002-000");
  });

  it("finds the save in a User folder, a Wii folder, the title's folder or its data folder", async () => {
    const wii = path.join(tmp, "User", "Wii");
    const data = path.join(wii, "title", "00010000", "534d4e45", "data");
    await mkdir(data, { recursive: true });
    for (const picked of [path.join(tmp, "User"), wii, path.dirname(data), data, tmp]) {
      expect(await findNandSave(picked, NSMBW)).toBe(data);
    }
    const other = path.join(wii, "title", "00010004", "524d4345", "data");
    await mkdir(other, { recursive: true });
    await expect(findNandSave(other, NSMBW)).rejects.toThrow("the save of another game (RMCE), not SMNE");
    await expect(findNandSave(path.join(tmp, "nothing"), "000100045a5a4345")).rejects.toThrow(
      "Wii/title/00010004/5a5a4345/data",
    );
  });

  it("finds the save of the Dolphin a mod's launcher or SD card is in (bug report #144)", async () => {
    const BRAWL = "0001000052534245";
    const root = path.join(tmp, "Cobalt 1.33b");
    const launcher = path.join(root, "User", "Launcher", "Project+ Netplay Launcher.dol");
    const sd = path.join(root, "User", "Wii", "sd.raw");
    const data = path.join(root, "User", "Wii", "title", "00010000", "52534245", "data");
    await mkdir(path.dirname(launcher), { recursive: true });
    await mkdir(data, { recursive: true });
    // An empty save folder is no save.
    expect(await findDolphinSaveNear([launcher, sd], BRAWL)).toBeNull();
    await writeFile(path.join(data, "autosv0.bin"), "rules");
    expect(await findDolphinSaveNear([launcher], BRAWL)).toBe(data);
    expect(await findDolphinSaveNear([null, sd], BRAWL)).toBe(data);
    // Another game's save, or none near the files, finds nothing.
    expect(await findDolphinSaveNear([launcher, sd], NSMBW)).toBeNull();
    expect(await findDolphinSaveNear([path.join(tmp, "elsewhere", "a", "b", "launcher.dol")], BRAWL)).toBeNull();
  });

  it("knows where the player's own Dolphin keeps its User folder", () => {
    expect(ownDolphinUserFolders("win32", "/home/me", { APPDATA: "/roaming" })[0]).toBe(
      path.join("/roaming", "Dolphin Emulator"),
    );
    expect(ownDolphinUserFolders("win32", "/home/me", {})).toContain(
      path.join("/home/me", "Documents", "Dolphin Emulator"),
    );
    expect(ownDolphinUserFolders("darwin", "/Users/me", {})).toEqual([
      path.join("/Users/me", "Library", "Application Support", "Dolphin"),
    ]);
    expect(ownDolphinUserFolders("linux", "/home/me", { XDG_DATA_HOME: "/xdg" })[0]).toBe(
      path.join("/xdg", "dolphin-emu"),
    );
    expect(ownDolphinUserFolders("linux", "/home/me", {})).toContain(
      path.join("/home/me", ".var", "app", "org.DolphinEmu.dolphin-emu", "data", "dolphin-emu"),
    );
  });

  it("finds the newest save of the title in the player's own Dolphins (bug report #193)", async () => {
    const save = async (root: string, text: string, when: Date) => {
      const data = path.join(tmp, root, "Wii", "title", "00010000", "534d4e45", "data");
      await mkdir(data, { recursive: true });
      await writeFile(path.join(data, "wiiking2.sav"), text);
      await utimes(path.join(data, "wiiking2.sav"), when, when);
      return data;
    };
    const old = await save("Documents", "old", new Date("2020-01-01"));
    const recent = await save("AppData", "recent", new Date("2026-01-01"));
    const folders = ["AppData", "Documents", "none"].map((f) => path.join(tmp, f));
    expect((await findOwnDolphinSave(folders, NSMBW))?.folder).toBe(recent);
    expect(await findOwnDolphinSave(folders, MKW)).toBeNull();
    expect(await sameSave(old, recent)).toBe(false);
    await writeFile(path.join(old, "wiiking2.sav"), "recent");
    expect(await sameSave(old, recent)).toBe(true);
    expect(await sameSave(path.join(tmp, "none"), path.join(tmp, "none2"))).toBe(false);
  });

  it("refuses an empty save and a folder too big to be one, and leaves the store as it was", async () => {
    await mkdir(storeData(), { recursive: true });
    await writeFile(path.join(storeData(), "wiiking2.sav"), "mine");
    const empty = path.join(tmp, "empty", "data");
    await mkdir(empty, { recursive: true });
    await expect(importSave({ source: empty, titleId: NSMBW, userFolder: user, buildHash: HASH })).rejects.toThrow(
      "no files",
    );
    const big = path.join(tmp, "big", "data");
    await mkdir(big, { recursive: true });
    await Promise.all(Array.from({ length: 1025 }, (_, i) => writeFile(path.join(big, `f${i}`), "")));
    await expect(importSave({ source: big, titleId: NSMBW, userFolder: user, buildHash: HASH })).rejects.toThrow(
      "too big",
    );
    expect(await listTree(storeData())).toEqual(["wiiking2.sav"]);
    expect(await readdir(path.join(user, "SaveBackups"))).toEqual([]);
  });
});
