import { INACTIVE_TEST_MODE } from "@common/test_mode";
import path from "path";
import { describe, expect, it } from "vitest";

import { LOGGING_CONFIG_ARGS } from "../netplay_args";
import { buildModsDolphinArgs } from "./mods_args";

const userArgs = ["-u", path.resolve("/data/netplay/User")];
const disc = path.resolve("/games/Brawl.iso");
const dol = path.resolve("/games/Minus/MinusLauncher/minus.dol");
const sd = path.resolve("/games/Minus/sd.raw");
const hash = "3f9a1c07" + "0".repeat(52) + "beef";

describe("buildModsDolphinArgs", () => {
  it("boots the launcher with the build's SD card and disc, and opens the room", () => {
    const room = {
      host: true as const,
      build: { hash, name: "Brawl Minus" },
      public: false,
      controller: "gc" as const,
    };
    const args = buildModsDolphinArgs({
      userArgs,
      build: { discPath: disc, launcherPath: dol, sdPath: sd },
      room,
      testMode: INACTIVE_TEST_MODE,
    });
    expect(args).toEqual([
      ...userArgs,
      ...LOGGING_CONFIG_ARGS,
      "-C",
      `Dolphin.General.WiiSDCardPath=${sd}`,
      "-C",
      "Dolphin.Core.WiiSDCard=True",
      "-C",
      "Dolphin.Core.WiiSDCardEnableFolderSync=False",
      "-C",
      `Dolphin.Core.DefaultISO=${disc}`,
      "-e",
      dol,
      "--mods-room",
      JSON.stringify(room),
    ]);
  });

  it("boots the disc itself without an SD card for a vanilla build, and joins by code", () => {
    const room = { join: "KFQB", build: { hash, name: "Brawl" }, controller: "wiimote" as const };
    const args = buildModsDolphinArgs({
      userArgs,
      build: { discPath: disc, launcherPath: null, sdPath: null },
      room,
      testMode: INACTIVE_TEST_MODE,
    });
    expect(args).toContain("Dolphin.Core.WiiSDCard=False");
    expect(args.slice(-4)).toEqual(["-e", disc, "--mods-room", JSON.stringify(room)]);
    expect(JSON.parse(args[args.length - 1])).toEqual({
      join: "KFQB",
      build: { hash, name: "Brawl" },
      controller: "wiimote",
    });
  });

  it("tells Dolphin the player's answer to the SD card question, asked before it started", () => {
    const room = {
      join: "KFQB",
      build: { hash, name: "REX" },
      controller: "gc" as const,
      sd_sync: "overwrite" as const,
    };
    const args = buildModsDolphinArgs({
      userArgs,
      build: { discPath: disc, launcherPath: dol, sdPath: sd },
      room,
      testMode: INACTIVE_TEST_MODE,
    });
    expect(JSON.parse(args[args.length - 1]).sd_sync).toBe("overwrite");
  });

  it("refuses a path Dolphin would cut at its '='", () => {
    expect(() =>
      buildModsDolphinArgs({
        userArgs,
        build: { discPath: path.resolve("/games/a=b/Brawl.iso"), launcherPath: null, sdPath: null },
        room: { join: "KFQB", build: { hash, name: "" }, controller: "gc" },
        testMode: INACTIVE_TEST_MODE,
      }),
    ).toThrow(/cannot contain "="/);
  });

  it("loads the mod folder's HD textures with custom textures on", () => {
    const load = path.resolve("/games/Minus/User/Load");
    const args = buildModsDolphinArgs({
      userArgs,
      build: { discPath: disc, launcherPath: dol, sdPath: sd },
      textures: { loadPath: load },
      room: { join: "KFQB", build: { hash, name: "" }, controller: "gc" },
      testMode: INACTIVE_TEST_MODE,
    });
    const at = args.indexOf(`Dolphin.General.LoadPath=${load}`);
    expect(at).toBeGreaterThan(0);
    expect(args.slice(at - 1, at + 3)).toEqual([
      "-C",
      `Dolphin.General.LoadPath=${load}`,
      "-C",
      "GFX.Settings.HiresTextures=True",
    ]);
    expect(args.slice(-4)).toEqual(["-e", dol, "--mods-room", args[args.length - 1]]);
  });

  it("leaves the textures out, not the room, for a Load path with '='", () => {
    const args = buildModsDolphinArgs({
      userArgs,
      build: { discPath: disc, launcherPath: null, sdPath: null },
      textures: { loadPath: path.resolve("/games/a=b/User/Load") },
      room: { join: "KFQB", build: { hash, name: "" }, controller: "gc" },
      testMode: INACTIVE_TEST_MODE,
    });
    expect(args.some((a) => a.startsWith("Dolphin.General.LoadPath=") || a.startsWith("GFX."))).toBe(false);
  });

  it("sets no Load path or textures without a pack in the mod folder", () => {
    const args = buildModsDolphinArgs({
      userArgs,
      build: { discPath: disc, launcherPath: dol, sdPath: sd },
      room: { join: "KFQB", build: { hash, name: "" }, controller: "gc" },
      testMode: INACTIVE_TEST_MODE,
    });
    expect(args.some((a) => a.startsWith("Dolphin.General.LoadPath=") || a.startsWith("GFX."))).toBe(false);
  });

  it("puts the harness port last in test mode", () => {
    const args = buildModsDolphinArgs({
      userArgs,
      build: { discPath: disc, launcherPath: null, sdPath: null },
      room: { join: "KFQB", build: { hash, name: "" }, controller: "gc" },
      testMode: { active: true, harnessPort: 23456, dolphinExtraArgs: ["-v", "Null"] },
    });
    expect(args.slice(-4)).toEqual(["--harness-port", "23456", "-v", "Null"]);
  });
});
