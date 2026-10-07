import { INACTIVE_TEST_MODE, readTestMode } from "@common/test_mode";
import path from "path";
import { describe, expect, it } from "vitest";

import { buildNetplayDolphinArgs } from "./netplay_args";

const sd = path.resolve("/data/netplay/pponline-sd/sd.raw");
const dol = path.resolve("/data/netplay/User/Launcher/Project+ Netplay Launcher.dol");
const userArgs = ["-u", path.resolve("/data/netplay/User")];

describe("buildNetplayDolphinArgs", () => {
  it("points Dolphin at the patched SD card for this run only, then boots the launcher DOL", () => {
    expect(buildNetplayDolphinArgs({ userArgs, sdCardImage: sd, bootFile: dol, testMode: INACTIVE_TEST_MODE })).toEqual(
      [
        ...userArgs,
        "-C",
        `Dolphin.General.WiiSDCardPath=${sd}`,
        "-C",
        "Dolphin.Core.WiiSDCard=True",
        "-C",
        "Dolphin.Core.WiiSDCardEnableFolderSync=False",
        "-e",
        dol,
      ],
    );
  });

  it("leaves the SD card and boot target out when there are none", () => {
    expect(
      buildNetplayDolphinArgs({ userArgs, sdCardImage: null, bootFile: null, testMode: INACTIVE_TEST_MODE }),
    ).toEqual(userArgs);
  });

  it("makes the SD path absolute and refuses paths Dolphin would cut at '='", () => {
    const args = buildNetplayDolphinArgs({
      userArgs,
      sdCardImage: "rel/sd.raw",
      bootFile: null,
      testMode: INACTIVE_TEST_MODE,
    });
    expect(args).toContain(`Dolphin.General.WiiSDCardPath=${path.resolve("rel/sd.raw")}`);
    expect(() =>
      buildNetplayDolphinArgs({ userArgs, sdCardImage: "/a=b/sd.raw", bootFile: null, testMode: INACTIVE_TEST_MODE }),
    ).toThrow(/=/);
  });

  it("appends the harness port and extra arguments only in test mode", () => {
    const env = { PPO_HARNESS_PORT: "50123", PPO_DOLPHIN_EXTRA_ARGS: '["-v","D3D11","-C","Dolphin.DSP.Muted=True"]' };
    const tail = ["--harness-port", "50123", "-v", "D3D11", "-C", "Dolphin.DSP.Muted=True"];

    const dev = buildNetplayDolphinArgs({
      userArgs,
      sdCardImage: sd,
      bootFile: dol,
      testMode: readTestMode(env, false),
    });
    expect(dev.slice(-tail.length)).toEqual(tail);
    expect(dev.indexOf("-e")).toBeLessThan(dev.indexOf("--harness-port"));

    const packaged = buildNetplayDolphinArgs({
      userArgs,
      sdCardImage: sd,
      bootFile: dol,
      testMode: readTestMode(env, true),
    });
    expect(packaged).not.toContain("--harness-port");
    expect(packaged).not.toContain("D3D11");
    expect(packaged.slice(-2)).toEqual(["-e", dol]);

    const optedIn = buildNetplayDolphinArgs({
      userArgs,
      sdCardImage: sd,
      bootFile: dol,
      testMode: readTestMode({ ...env, PPO_TEST_MODE: "1" }, true),
    });
    expect(optedIn.slice(-tail.length)).toEqual(tail);
  });
});
