import { createHash } from "crypto";
import { mkdtemp, rm, writeFile } from "fs/promises";
import os from "os";
import path from "path";
import { describe, expect, it } from "vitest";

import { BRAWL_NTSC_U_DISC } from "../game_data";
import {
  discContentSha1,
  discContentSha1WithProgress,
  discIdentity,
  isBrawlGameId,
  isPlainDiscImage,
  parseHeaderJson,
  readDiscInfo,
} from "./disc_info";
import { defaultController } from "./types";

describe("disc info", () => {
  it("reads DolphinTool's header: a Wii disc has a title ID", () => {
    const wii = parseHeaderJson(
      '{"country":"USA","game_id":"SMNE01","internal_name":"New SUPER MARIO BROS. Wii","region":"NTSC-U","revision":2,"title_id":281476374285893}',
    );
    expect(wii).toEqual({
      gameId: "SMNE01",
      title: "New SUPER MARIO BROS. Wii",
      revision: 2,
      platform: "wii",
      titleId: "00010000534d4e45",
    });
    const gc = parseHeaderJson('{"game_id":"GALE01","internal_name":"Super Smash Bros Melee","revision":2}');
    expect(gc.platform).toBe("gamecube");
    expect(gc.titleId).toBeNull();
  });

  it("refuses what is not a disc", () => {
    expect(() => parseHeaderJson("Error: Unable to open input file")).toThrow(/not a GameCube or Wii disc/);
    expect(() => parseHeaderJson('{"game_id":"../x"}')).toThrow(/not a GameCube or Wii disc/);
  });

  it("runs DolphinTool with the disc as one argument", async () => {
    const calls: string[][] = [];
    const exec = async (file: string, args: string[]) => {
      calls.push([file, ...args]);
      return args[0] === "header"
        ? '{"game_id":"RSBE01","internal_name":"SUPER SMASH BROS. BRAWL","revision":2,"title_id":1}'
        : "9AC7242A5FEEBE1D0A6E2775C21F482C471B2240\n";
    };
    const disc = "C:/Games/My Games (USA)/Brawl.rvz";
    expect((await readDiscInfo("tool", disc, exec)).gameId).toBe("RSBE01");
    expect(await discContentSha1("tool", disc, exec)).toBe("9ac7242a5feebe1d0a6e2775c21f482c471b2240");
    expect(calls).toEqual([
      ["tool", "header", "-i", disc, "-j"],
      ["tool", "verify", "-u", expect.any(String), "-i", disc, "-a", "sha1"],
    ]);
    await expect(discContentSha1("tool", disc, async () => "Error: no hash")).rejects.toThrow(/Could not check/);
  });

  it("hashes a plain disc image itself, with progress, and leaves other formats to DolphinTool", async () => {
    const dir = await mkdtemp(path.join(os.tmpdir(), "mods-disc-"));
    try {
      // A Wii disc's header: its magic at 0x18; a GameCube disc's at 0x1C.
      const wii = Buffer.alloc(0x10000, 7);
      wii.writeUInt32BE(0x5d1c9ea3, 0x18);
      const gc = Buffer.alloc(0x400, 3);
      gc.writeUInt32BE(0xc2339f3d, 0x1c);
      // An RVZ starts with its own header ("RVZ\x01"), the disc's header further in.
      const rvz = Buffer.concat([Buffer.from("RVZ\x01", "latin1"), Buffer.alloc(0x400)]);
      const files = {
        wii: path.join(dir, "Brawl.iso"),
        gc: path.join(dir, "Melee.gcm"),
        rvz: path.join(dir, "Brawl.rvz"),
        split: path.join(dir, "Brawl.part0.iso"),
        tiny: path.join(dir, "tiny.iso"),
      };
      await writeFile(files.wii, wii);
      await writeFile(files.gc, gc);
      await writeFile(files.rvz, rvz);
      await writeFile(files.split, wii);
      await writeFile(files.tiny, "short");
      expect(await isPlainDiscImage(files.wii)).toBe(true);
      expect(await isPlainDiscImage(files.gc)).toBe(true);
      expect(await isPlainDiscImage(files.rvz)).toBe(false);
      expect(await isPlainDiscImage(files.split)).toBe(false);
      expect(await isPlainDiscImage(files.tiny)).toBe(false);

      const calls: string[][] = [];
      const exec = async (_file: string, args: string[]) => {
        calls.push(args);
        return "0E95949AC585F357E79FCD34B20670B5DCA97AC2\n";
      };
      const seen: number[] = [];
      const plain = await discContentSha1WithProgress("tool", files.wii, (done) => seen.push(done), exec);
      expect(plain).toBe(createHash("sha1").update(wii).digest("hex"));
      expect(seen[seen.length - 1]).toBe(wii.length);
      expect(calls).toEqual([]);
      expect(await discContentSha1WithProgress("tool", files.rvz, undefined, exec)).toBe(
        "0e95949ac585f357e79fcd34b20670b5dca97ac2",
      );
      expect(calls).toEqual([["verify", "-u", expect.any(String), "-i", files.rvz, "-a", "sha1"]]);
    } finally {
      await rm(dir, { recursive: true, force: true });
    }
  });

  it("names a disc by its game data: Brawl Rev 1 and Rev 2 as one, a disc without game data by its contents", async () => {
    const data =
      (gameData: string | null, platform: "wii" | "gamecube" = "wii") =>
      async () => ({
        gameId: "RSBE01",
        revision: 1,
        platform,
        dataOffset: 0xf800000,
        tmdSha1: null,
        h3Ok: true,
        usedBlockErrors: 0,
        unusedBlockErrors: 0,
        gameData,
      });
    const contents = async () => "c".repeat(40);
    const rev1 = await discIdentity(
      "tool",
      "rev1.iso",
      undefined,
      data("0d79fc54ef81e223b65eb8d4b1854643fc677554"),
      contents,
    );
    const rev2 = await discIdentity(
      "tool",
      "rev2.wbfs",
      undefined,
      data("e6ae2404f96cff10be1c4dda8559001c3a7dfd34"),
      contents,
    );
    expect(rev1).toBe(BRAWL_NTSC_U_DISC);
    expect(rev2).toBe(BRAWL_NTSC_U_DISC);
    expect(await discIdentity("tool", "nsmbw.rvz", undefined, data("8".repeat(40)), contents)).toBe("8".repeat(40));
    // Damaged or modified, or a GameCube disc: its contents, as before.
    expect(await discIdentity("tool", "damaged.wbfs", undefined, data(null), contents)).toBe("c".repeat(40));
    expect(await discIdentity("tool", "melee.iso", undefined, data(null, "gamecube"), contents)).toBe("c".repeat(40));
  });

  it("knows what controller a game is made for", () => {
    expect(defaultController({ gameId: "SMNE01", platform: "wii" })).toBe("wiimote");
    expect(defaultController({ gameId: "RSBE01", platform: "wii" })).toBe("gc");
    expect(defaultController({ gameId: "GALE01", platform: "gamecube" })).toBe("gc");
    expect(isBrawlGameId("RSBP01")).toBe(true);
  });
});
