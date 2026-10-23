import { mkdir, mkdtemp, readdir, readFile, rm, writeFile } from "fs/promises";
import os from "os";
import path from "path";
import { afterEach, beforeEach, describe, expect, it } from "vitest";

import {
  DENTRY_SIZE,
  GC_BLOCK_SIZE,
  gciFileName,
  gcSaveDir,
  importGcSave,
  readCardImage,
  readGci,
  readGcSaves,
  summarizeGcSave,
} from "./gc_save";

const HASH = "14e34197d0329e2e" + "0".repeat(48);

type Save = { game: string; maker?: string; name: string; modified?: number; blocks: number; fill: number };

/** A save's directory entry as a GameCube card keeps it (Dolphin's DEntry). */
function entry(save: Save, firstBlock: number): Buffer {
  const e = Buffer.alloc(DENTRY_SIZE, 0);
  e.write(save.game, 0, "latin1");
  e.write(save.maker ?? "01", 4, "latin1");
  e[6] = 0xff;
  e.write(save.name, 8, "latin1");
  e.writeUInt32BE(save.modified ?? 1000, 0x28);
  e.writeUInt16BE(firstBlock, 0x36);
  e.writeUInt16BE(save.blocks, 0x38);
  e.writeUInt16BE(0xffff, 0x3a);
  return e;
}

const block = (fill: number, i: number) => Buffer.alloc(GC_BLOCK_SIZE, (fill + i) & 0xff);

function gci(save: Save): Buffer {
  return Buffer.concat([entry(save, 5), ...Array.from({ length: save.blocks }, (_, i) => block(save.fill, i))]);
}

/**
 * A 59-block card (64 blocks, 512 KB) with `saves` placed at `places` (each save's blocks, in
 * order, linked through the block map). The directory and block map are written to both copies;
 * `stale` puts an older, empty directory in the copy with the higher counter's twin.
 */
function card(saves: Save[], places: number[][], opts: { backupNewer?: boolean } = {}): Buffer {
  const c = Buffer.alloc(64 * GC_BLOCK_SIZE, 0);
  const dir = Buffer.alloc(GC_BLOCK_SIZE, 0xff);
  const bat = Buffer.alloc(GC_BLOCK_SIZE, 0);
  saves.forEach((save, i) => {
    entry(save, places[i][0]).copy(dir, i * DENTRY_SIZE);
    places[i].forEach((b, j) => {
      block(save.fill, j).copy(c, b * GC_BLOCK_SIZE);
      bat.writeUInt16BE(j + 1 < places[i].length ? places[i][j + 1] : 0xffff, 0xa + (b - 5) * 2);
    });
  });
  const emptyDir = Buffer.alloc(GC_BLOCK_SIZE, 0xff);
  // The update counters: the newer copy wins.
  dir.writeInt16BE(5, 0x1ffa);
  emptyDir.writeInt16BE(4, 0x1ffa);
  bat.writeInt16BE(5, 0x4);
  const [d1, d2] = opts.backupNewer ? [emptyDir, dir] : [dir, emptyDir];
  d1.copy(c, 1 * GC_BLOCK_SIZE);
  d2.copy(c, 2 * GC_BLOCK_SIZE);
  bat.copy(c, 3 * GC_BLOCK_SIZE);
  bat.copy(c, 4 * GC_BLOCK_SIZE);
  return c;
}

const MELEE: Save = { game: "GALE", name: "SuperSmashBros0110290334", blocks: 3, fill: 0x10 };
const SMB2: Save = { game: "GM2E", maker: "8P", name: "super_monkey_ball_2.dat", blocks: 2, fill: 0x40 };

describe("GameCube saves", () => {
  let dir: string;
  beforeEach(async () => {
    dir = await mkdtemp(path.join(os.tmpdir(), "gc-save-"));
  });
  afterEach(async () => {
    await rm(dir, { recursive: true, force: true });
  });

  it("reads a .gci and refuses what is not one", () => {
    const save = readGci(gci(MELEE));
    expect(save).toMatchObject({ gameCode: "GALE", maker: "01", fileName: "SuperSmashBros0110290334" });
    expect(save.data.length).toBe(DENTRY_SIZE + 3 * GC_BLOCK_SIZE);
    expect(() => readGci(gci(MELEE).subarray(0, DENTRY_SIZE + GC_BLOCK_SIZE))).toThrow(/not a GameCube save/);
    expect(() => readGci(Buffer.alloc(100))).toThrow(/not a GameCube save/);
  });

  it("names a save as Dolphin's GCI folders do", () => {
    expect(gciFileName({ maker: "01", gameCode: "GALE", fileName: "SuperSmashBros0110290334" })).toBe(
      "01-GALE-SuperSmashBros0110290334.gci",
    );
    expect(gciFileName({ maker: "8P", gameCode: "GM2E", fileName: "a/b:c" })).toBe("8P-GM2E-a__2f__b__3a__c.gci");
  });

  it("takes every save off a card image, following the block map", () => {
    const saves = readCardImage(
      card(
        [MELEE, SMB2],
        [
          [9, 5, 30],
          [6, 7],
        ],
      ),
    );
    expect(saves.map((s) => s.gameCode)).toEqual(["GALE", "GM2E"]);
    // The blocks in the map's order, after the card's entry: the same bytes as Dolphin's export.
    expect(saves[0].data.subarray(DENTRY_SIZE)).toEqual(gci(MELEE).subarray(DENTRY_SIZE));
    expect(saves[1].data.subarray(DENTRY_SIZE)).toEqual(gci(SMB2).subarray(DENTRY_SIZE));
    expect(readGci(saves[0].data).fileName).toBe(MELEE.name);
  });

  it("reads the newer directory of the two", () => {
    expect(readCardImage(card([MELEE], [[5, 6, 7]], { backupNewer: true })).map((s) => s.gameCode)).toEqual(["GALE"]);
  });

  it("skips a save whose chain is broken", () => {
    const c = card([MELEE], [[5, 6, 7]]);
    // Block 6 now ends the save one block early.
    c.writeUInt16BE(0xffff, 3 * GC_BLOCK_SIZE + 0xa + (6 - 5) * 2);
    c.writeUInt16BE(0xffff, 4 * GC_BLOCK_SIZE + 0xa + (6 - 5) * 2);
    expect(readCardImage(c)).toEqual([]);
  });

  it("finds the game's saves in a Dolphin User folder, the newest of each", async () => {
    const user = path.join(dir, "Dolphin Emulator");
    const cardA = path.join(user, "GC", "USA", "Card A");
    await mkdir(cardA, { recursive: true });
    await writeFile(path.join(cardA, "01-GALE-SuperSmashBros0110290334.gci"), gci({ ...MELEE, modified: 2000 }));
    await writeFile(path.join(cardA, "8P-GM2E-super_monkey_ball_2.dat.gci"), gci(SMB2));
    // The raw card has an older copy of the Melee save.
    await writeFile(path.join(user, "GC", "MemoryCardA.USA.raw"), card([{ ...MELEE, fill: 0x77 }], [[5, 6, 7]]));
    const saves = await readGcSaves(user, "GALE01");
    expect(saves).toHaveLength(1);
    expect(saves[0].modified).toBe(2000);
    expect(saves[0].data).toEqual(gci({ ...MELEE, modified: 2000 }));
    // A GCI folder itself, and its region folder.
    expect(await readGcSaves(cardA, "GM2E8P")).toHaveLength(1);
    expect(await readGcSaves(path.join(user, "GC", "USA"), "GM2E8P")).toHaveLength(1);
  });

  it("says when the save is another game's", async () => {
    const file = path.join(dir, "smb2.gci");
    await writeFile(file, gci(SMB2));
    await expect(readGcSaves(file, "GALE01")).rejects.toThrow("This save is for another game (GM2E8P), not GALE01.");
    // The same game code under another maker: a hack's save.
    await expect(readGcSaves(file, "GM2EBJ")).rejects.toThrow("This save is for another game (GM2E8P), not GM2EBJ.");
    const raw = path.join(dir, "card.raw");
    await writeFile(raw, card([SMB2], [[5, 6]]));
    await expect(readGcSaves(raw, "GALE01")).rejects.toThrow("This memory card has no save of GALE01 (it has GM2E8P).");
    await expect(readGcSaves(dir, "GALE01")).rejects.toThrow(/There's no save of GALE01 in this folder/);
  });

  it("imports into the build's store, the saves there before kept aside", async () => {
    const user = path.join(dir, "netplay");
    const store = gcSaveDir(user, HASH);
    expect(store).toBe(path.join(user, "Saves", "14e34197d0329e2e", "GC"));
    await mkdir(store, { recursive: true });
    await writeFile(path.join(store, "01-GALE-old.gci"), gci({ ...MELEE, name: "old" }));
    await writeFile(path.join(store, "01-GALE-old.gci.deleted"), "x");
    expect((await summarizeGcSave(store)).files).toBe(1);

    const raw = path.join(dir, "MemoryCardA.USA.raw");
    await writeFile(
      raw,
      card(
        [MELEE, SMB2],
        [
          [5, 6, 7],
          [8, 9],
        ],
      ),
    );
    const result = await importGcSave({
      source: raw,
      gameId: "GALE01",
      userFolder: user,
      buildHash: HASH,
      now: new Date("2026-10-23T03:00:00Z"),
    });
    expect(await readdir(store)).toEqual(["01-GALE-SuperSmashBros0110290334.gci"]);
    expect(await readFile(path.join(store, "01-GALE-SuperSmashBros0110290334.gci"))).toEqual(gci(MELEE));
    expect(result.save.files).toBe(1);
    expect(result.backup).toBe(path.join(user, "SaveBackups", "14e34197d0329e2e", "20261023-030000-000", "GC"));
    expect((await readdir(result.backup!)).sort()).toEqual(["01-GALE-old.gci", "01-GALE-old.gci.deleted"]);
    // Nothing left of the import's staging.
    expect((await readdir(path.join(user, "SaveBackups"))).filter((n) => n.startsWith("."))).toEqual([]);
  });

  it("leaves the store as it was when the source has no save of the game", async () => {
    const user = path.join(dir, "netplay");
    const store = gcSaveDir(user, HASH);
    await mkdir(store, { recursive: true });
    await writeFile(path.join(store, "01-GALE-old.gci"), gci({ ...MELEE, name: "old" }));
    const file = path.join(dir, "smb2.gci");
    await writeFile(file, gci(SMB2));
    await expect(importGcSave({ source: file, gameId: "GALE01", userFolder: user, buildHash: HASH })).rejects.toThrow(
      /another game/,
    );
    expect(await readdir(store)).toEqual(["01-GALE-old.gci"]);
  });
});
