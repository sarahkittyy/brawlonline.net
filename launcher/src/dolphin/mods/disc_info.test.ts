import { describe, expect, it } from "vitest";

import { discContentSha1, isBrawlGameId, parseHeaderJson, readDiscInfo } from "./disc_info";
import { controllerOf, defaultController } from "./types";

describe("disc info", () => {
  it("reads DolphinTool's header: a Wii disc has a title ID", () => {
    const wii = parseHeaderJson(
      '{"country":"USA","game_id":"SMNE01","internal_name":"New SUPER MARIO BROS. Wii","region":"NTSC-U","revision":2,"title_id":281476374285893}',
    );
    expect(wii).toEqual({ gameId: "SMNE01", title: "New SUPER MARIO BROS. Wii", revision: 2, platform: "wii" });
    const gc = parseHeaderJson('{"game_id":"GALE01","internal_name":"Super Smash Bros Melee","revision":2}');
    expect(gc.platform).toBe("gamecube");
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
      ["tool", "verify", "-i", disc, "-a", "sha1"],
    ]);
    await expect(discContentSha1("tool", disc, async () => "Error: no hash")).rejects.toThrow(/Could not check/);
  });

  it("picks the controller a game plays with", () => {
    expect(defaultController({ gameId: "SMNE01", platform: "wii", launcherPath: null })).toBe("wiimote");
    expect(defaultController({ gameId: "RSBE01", platform: "wii", launcherPath: "minus.dol" })).toBe("gc");
    expect(defaultController({ gameId: "GALE01", platform: "gamecube", launcherPath: null })).toBe("gc");
    expect(isBrawlGameId("RSBP01")).toBe(true);
    const build = {
      id: "1",
      name: "x",
      discPath: "d",
      launcherPath: null,
      sdPath: null,
      gameId: "SMNE01",
      platform: "wii" as const,
      hash: null,
      hashKey: null,
    };
    expect(controllerOf(build)).toBe("wiimote");
    expect(controllerOf({ ...build, controller: "gc" })).toBe("gc");
  });
});
