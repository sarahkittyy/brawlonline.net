import { IsoValidity } from "@common/types";
import type { GameData, GameDataFn } from "@dolphin/game_data";
import { readGameData } from "@dolphin/game_data";
import { mkdtemp, rm, stat, writeFile } from "node:fs/promises";
import os from "os";
import path from "path";
import { afterEach, beforeEach, describe, expect, it } from "vitest";

import { checkIsoHeader, md5ToValidity, verifyIso, verifyIsoCached, verifyIsoDetailed } from "./verify_iso";

function header(id: string, revision: number): Buffer {
  const b = Buffer.alloc(64);
  b.write(id, 0, "latin1");
  b.writeUInt8(revision, 7);
  return b;
}

describe("verifyIso", () => {
  let dir: string;
  beforeEach(async () => {
    dir = await mkdtemp(path.join(os.tmpdir(), "ppo-iso-"));
  });
  afterEach(async () => {
    await rm(dir, { recursive: true, force: true });
  });

  it("accepts exactly the NTSC-U Brawl Rev 1 and Rev 2 MD5s", () => {
    expect(md5ToValidity("d18726e6dfdc8bdbdad540b561051087")).toBe(IsoValidity.VALID);
    expect(md5ToValidity("52CE7160CED2505AD5E397477D0EA4FE")).toBe(IsoValidity.VALID);
    expect(md5ToValidity("00000000000000000000000000000000")).toBe(IsoValidity.INVALID);
  });

  it("rejects other games and revisions from the header without hashing", async () => {
    const melee = path.join(dir, "melee.iso");
    await writeFile(melee, header("GALE01", 2));
    expect(await checkIsoHeader(melee)).toBe(false);
    const rev0 = path.join(dir, "rev0.iso");
    await writeFile(rev0, header("RSBE01", 0));
    expect(await checkIsoHeader(rev0)).toBe(false);
    const rev2 = path.join(dir, "rev2.iso");
    await writeFile(rev2, header("RSBE01", 2));
    expect(await checkIsoHeader(rev2)).toBe(true);
  });

  it("rejects a Brawl header whose contents do not match, never answering unknown", async () => {
    const fake = path.join(dir, "modded.iso");
    await writeFile(fake, header("RSBE01", 1));
    expect(await verifyIso(fake)).toBe(IsoValidity.INVALID);
    const cache = path.join(dir, "cache.json");
    const first = await verifyIsoCached(fake, cache);
    expect(first.valid).toBe(IsoValidity.INVALID);
    expect(first.cached).toBe(false);
    const again = await verifyIsoCached(fake, cache);
    expect(again.valid).toBe(IsoValidity.INVALID);
    expect(again.cached).toBe(true);
    expect(again.detail).toContain(first.detail);
  });

  it("says why an image was rejected", async () => {
    const melee = path.join(dir, "melee.iso");
    await writeFile(melee, header("GALE01", 2));
    const wrongGame = await verifyIsoDetailed(melee);
    expect(wrongGame.valid).toBe(IsoValidity.INVALID);
    expect(wrongGame.detail).toBe(
      '64 bytes, header [47 41 4c 45 30 31 00 02] (game ID "GALE01", revision 2) is not RSBE01 revision 1 or 2',
    );

    const fake = path.join(dir, "modded.iso");
    await writeFile(fake, header("RSBE01", 1));
    const wrongHash = await verifyIsoDetailed(fake);
    expect(wrongHash.valid).toBe(IsoValidity.INVALID);
    expect(wrongHash.detail).toMatch(
      /^64 bytes, header \[52 53 42 45 30 31 00 01\] .* MD5 [0-9a-f]{32} is not an accepted image$/,
    );

    const empty = path.join(dir, "empty.iso");
    await writeFile(empty, Buffer.alloc(0));
    expect((await verifyIsoDetailed(empty)).detail).toBe(
      '0 bytes, header [] (game ID "") is not RSBE01 revision 1 or 2',
    );
  });

  const gameData =
    (over: Partial<GameData> = {}): GameDataFn =>
    async (_discPath, onProgress) => {
      onProgress?.(8511160320, 8511160320);
      return {
        gameId: "RSBE01",
        revision: 2,
        platform: "wii",
        dataOffset: 0xf800000,
        tmdSha1: "4f952258cd18278be7f9f3fa66efed82c0342898",
        h3Ok: true,
        usedBlockErrors: 0,
        unusedBlockErrors: 8088,
        gameData: "e6ae2404f96cff10be1c4dda8559001c3a7dfd34",
        ...over,
      };
    };

  it("accepts any image of an accepted disc by its game data: a WBFS, a scrubbed ISO", async () => {
    const wbfs = path.join(dir, "RSBE01.wbfs");
    await writeFile(wbfs, Buffer.concat([Buffer.from("WBFS", "latin1"), Buffer.alloc(60)]));
    const seen: number[] = [];
    const result = await verifyIsoDetailed(wbfs, (done) => seen.push(done), gameData());
    expect(result).toEqual({
      valid: IsoValidity.VALID,
      detail:
        "64 bytes, WBFS, game ID RSBE01, revision 2, game partition at 0xf800000, game data " +
        "e6ae2404f96cff10be1c4dda8559001c3a7dfd34, 8088 unused blocks scrubbed (NTSC-U Rev 2)",
    });
    expect(seen).toEqual([8511160320]);
    const scrubbed = path.join(dir, "scrubbed.iso");
    await writeFile(scrubbed, header("RSBE01", 1));
    const rev1 = await verifyIsoDetailed(
      scrubbed,
      undefined,
      gameData({ revision: 1, gameData: "0d79fc54ef81e223b65eb8d4b1854643fc677554" }),
    );
    expect(rev1.valid).toBe(IsoValidity.VALID);
    expect(rev1.detail).toMatch(/^64 bytes, plain image, .* \(NTSC-U Rev 1\)$/);
  });

  it("rejects an image whose game data is damaged, modified or another disc's", async () => {
    const wbfs = path.join(dir, "damaged.wbfs");
    await writeFile(wbfs, Buffer.concat([Buffer.from("WBFS", "latin1"), Buffer.alloc(60)]));
    const damaged = await verifyIsoDetailed(wbfs, undefined, gameData({ usedBlockErrors: 64, gameData: null }));
    expect(damaged.valid).toBe(IsoValidity.INVALID);
    expect(damaged.detail).toContain("64 blocks of the game's data fail their hash check");
    const pal = await verifyIsoDetailed(wbfs, undefined, gameData({ gameId: "RSBP01", gameData: "a".repeat(40) }));
    expect(pal.valid).toBe(IsoValidity.INVALID);
    expect(pal.detail).toMatch(/game ID RSBP01, .*: not an accepted Brawl disc$/);
  });

  it("checks a plain image's MD5 when DolphinTool fails, and needs DolphinTool for any other format", async () => {
    const fake = path.join(dir, "modded.iso");
    await writeFile(fake, header("RSBE01", 1));
    const failing: GameDataFn = async () => {
      throw new Error("DolphinTool.exe: spawn EPERM");
    };
    const plain = await verifyIsoDetailed(fake, undefined, failing);
    expect(plain.valid).toBe(IsoValidity.INVALID);
    expect(plain.detail).toMatch(
      /MD5 [0-9a-f]{32} is not an accepted image; DolphinTool failed \(DolphinTool.exe: spawn EPERM\), MD5 instead$/,
    );
    const rvz = path.join(dir, "brawl.rvz");
    await writeFile(rvz, Buffer.concat([Buffer.from("RVZ\x01", "latin1"), Buffer.alloc(60)]));
    await expect(verifyIsoDetailed(rvz, undefined, failing)).rejects.toThrow(/spawn EPERM/);
    expect(await verifyIsoDetailed(rvz)).toEqual({
      valid: IsoValidity.INVALID,
      detail: "64 bytes, RVZ: DolphinTool is needed to read it",
    });
  });

  it("checks again an image a launcher before game data checks rejected", async () => {
    const wbfs = path.join(dir, "brawl.wbfs");
    await writeFile(wbfs, Buffer.concat([Buffer.from("WBFS", "latin1"), Buffer.alloc(60)]));
    const s = await stat(wbfs);
    const cache = path.join(dir, "cache.json");
    await writeFile(
      cache,
      JSON.stringify({ [wbfs]: { size: s.size, mtimeMs: s.mtimeMs, valid: IsoValidity.INVALID, detail: "old" } }),
    );
    const result = await verifyIsoCached(wbfs, cache, undefined, gameData());
    expect(result.cached).toBe(false);
    expect(result.valid).toBe(IsoValidity.VALID);
    expect((await verifyIsoCached(wbfs, cache, undefined, gameData())).cached).toBe(true);
  });

  it("checks again a v2 rejection, not a v2 acceptance", async () => {
    const wbfs = path.join(dir, "RSBE01.wbfs");
    await writeFile(wbfs, Buffer.concat([Buffer.from("WBFS", "latin1"), Buffer.alloc(60)]));
    const rvz = path.join(dir, "brawl.rvz");
    await writeFile(rvz, Buffer.concat([Buffer.from("RVZ\x01", "latin1"), Buffer.alloc(60)]));
    const cache = path.join(dir, "cache.json");
    const entry = async (p: string, valid: IsoValidity) => {
      const s = await stat(p);
      return { v: 2, size: s.size, mtimeMs: s.mtimeMs, valid, detail: "v2" };
    };
    await writeFile(
      cache,
      JSON.stringify({ [wbfs]: await entry(wbfs, IsoValidity.INVALID), [rvz]: await entry(rvz, IsoValidity.VALID) }),
    );
    const rechecked = await verifyIsoCached(wbfs, cache, undefined, gameData());
    expect(rechecked.cached).toBe(false);
    expect(rechecked.valid).toBe(IsoValidity.VALID);
    const kept = await verifyIsoCached(rvz, cache, undefined, gameData());
    expect(kept.cached).toBe(true);
    expect(kept.valid).toBe(IsoValidity.VALID);
  });

  // Hashing an 8.5 GB image takes a while: opt in with PPO_TEST_REAL_ISO=<path to a Brawl ISO>.
  it.skipIf(!process.env.PPO_TEST_REAL_ISO)(
    "accepts a real Brawl image",
    async () => {
      const result = await verifyIsoDetailed(process.env.PPO_TEST_REAL_ISO as string);
      expect(result.valid).toBe(IsoValidity.VALID);
      expect(result.detail).toMatch(/^8511160320 bytes, MD5 [0-9a-f]{32} \(NTSC-U Rev [12]\)$/);
    },
    600000,
  );

  // With DolphinTool: PPO_TEST_DOLPHIN_TOOL=<DolphinTool> PPO_TEST_REAL_DISCS=<image>;<image>;...
  // (any formats of accepted discs).
  it.skipIf(!process.env.PPO_TEST_DOLPHIN_TOOL || !process.env.PPO_TEST_REAL_DISCS)(
    "accepts real Brawl images in any format by their game data",
    async () => {
      const tool = process.env.PPO_TEST_DOLPHIN_TOOL as string;
      for (const disc of (process.env.PPO_TEST_REAL_DISCS as string).split(";")) {
        const result = await verifyIsoDetailed(disc, undefined, (p, onProgress) => readGameData(tool, p, onProgress));
        expect(result.valid, `${disc}: ${result.detail}`).toBe(IsoValidity.VALID);
      }
    },
    1800000,
  );
});
