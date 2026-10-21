import { describe, expect, it } from "vitest";

import {
  BRAWL_NTSC_U_DISC,
  describeGameData,
  describeGameDataProblem,
  discContainer,
  parseGameData,
  parseToolProgress,
} from "./game_data";

// DolphinTool gamedata on the redump Rev 2 disc's WBFS (scrubbed: 8088 unused blocks).
const REV2_WBFS =
  '{"data_offset":260046848,"game_data":"e6ae2404f96cff10be1c4dda8559001c3a7dfd34","game_id":"RSBE01","h3_ok":true,' +
  '"platform":"wii","revision":2,"tmd_sha1":"4f952258cd18278be7f9f3fa66efed82c0342898","unused_block_errors":8088,' +
  '"used_block_errors":0}';

describe("game data", () => {
  it("reads DolphinTool gamedata", () => {
    expect(parseGameData(REV2_WBFS)).toEqual({
      gameId: "RSBE01",
      revision: 2,
      platform: "wii",
      dataOffset: 0xf800000,
      tmdSha1: "4f952258cd18278be7f9f3fa66efed82c0342898",
      h3Ok: true,
      usedBlockErrors: 0,
      unusedBlockErrors: 8088,
      gameData: "e6ae2404f96cff10be1c4dda8559001c3a7dfd34",
    });
    const gc = parseGameData('{"game_data":null,"game_id":"GALE01","platform":"gamecube","revision":2}');
    expect(gc).toMatchObject({ platform: "gamecube", gameData: null, dataOffset: null, usedBlockErrors: null });
    expect(() => parseGameData("Error: Unable to open disc image")).toThrow(/not a GameCube or Wii disc/);
    expect(() => parseGameData('{"game_id":""}')).toThrow(/not a GameCube or Wii disc/);
  });

  it("says why a disc has no game data", () => {
    const damaged = parseGameData(
      REV2_WBFS.replace('"used_block_errors":0', '"used_block_errors":64').replace(
        /"game_data":"[0-9a-f]+"/,
        '"game_data":null',
      ),
    );
    expect(describeGameDataProblem(damaged)).toBe(
      "64 blocks of the game's data fail their hash check (damaged or modified)",
    );
    expect(describeGameData(damaged)).toBe(
      "game ID RSBE01, revision 2, game partition at 0xf800000, no game data (64 blocks of the game's data fail " +
        "their hash check (damaged or modified)), 8088 unused blocks scrubbed",
    );
    expect(describeGameData(parseGameData(REV2_WBFS))).toBe(
      "game ID RSBE01, revision 2, game partition at 0xf800000, game data e6ae2404f96cff10be1c4dda8559001c3a7dfd34, " +
        "8088 unused blocks scrubbed",
    );
  });

  it("reads progress lines", () => {
    expect(parseToolProgress("progress 100 8511160320")).toEqual({ done: 100, total: 8511160320 });
    expect(parseToolProgress("Error: x")).toBeNull();
  });

  it("knows Dolphin's compressed formats from their first bytes, and a plain image from its game ID", () => {
    expect(discContainer(Buffer.from("WBFS\0\0\0\0", "latin1"))).toBe("WBFS");
    expect(discContainer(Buffer.from("RVZ\x01\0\0\0\0", "latin1"))).toBe("RVZ");
    expect(discContainer(Buffer.from("WIA\x01\0\0\0\0", "latin1"))).toBe("WIA");
    expect(discContainer(Buffer.from("CISO\0\0\0\0", "latin1"))).toBe("CISO");
    expect(discContainer(Buffer.from([0x01, 0xc0, 0x0b, 0xb1, 0, 0, 0, 0]))).toBe("GCZ");
    expect(discContainer(Buffer.from("RSBE01\0\x02", "latin1"))).toBeNull();
    expect(discContainer(Buffer.alloc(0))).toBeNull();
  });

  it("names NTSC-U Brawl Rev 1 and Rev 2 as one disc, the same in every launcher", () => {
    expect(BRAWL_NTSC_U_DISC).toMatch(/^[0-9a-f]{40}$/);
    expect(BRAWL_NTSC_U_DISC).toBe("a56ffa9437ddf80f4e4fa9c9bfddc7094d216483");
  });
});
