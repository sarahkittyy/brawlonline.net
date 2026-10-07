import path from "path";
import { describe, expect, it } from "vitest";

import { DolphinLaunchType } from "../types";
import type { DolphinPathEnv } from "./paths";
import { defaultDolphinExecutable, defaultUserTemplate, dolphinExecutableName } from "./paths";

const ctx = (over: Partial<DolphinPathEnv> = {}): DolphinPathEnv => ({
  env: {},
  isDevelopment: false,
  devRepoRoot: path.resolve("/repo/launcher"),
  userDataDir: path.resolve("/userdata"),
  platform: "win32",
  ...over,
});

describe("dolphin paths", () => {
  it("defaults to our Dolphin build next to the repo in development", () => {
    const exe = defaultDolphinExecutable(DolphinLaunchType.NETPLAY, ctx({ isDevelopment: true }));
    expect(exe).toBe(path.resolve("/repo/dolphin/build/release/x64/Binaries/Dolphin.exe"));
  });

  it("uses an install folder under userData when packaged, like Slippi", () => {
    const exe = defaultDolphinExecutable(DolphinLaunchType.PLAYBACK, ctx());
    expect(exe).toBe(path.join(path.resolve("/userdata"), "netplay", "Dolphin.exe"));
  });

  it("honours PPO_DOLPHIN_PATH", () => {
    const exe = defaultDolphinExecutable(DolphinLaunchType.NETPLAY, ctx({ env: { PPO_DOLPHIN_PATH: " /x/D.exe " } }));
    expect(exe).toBe("/x/D.exe");
  });

  it("names the binary per OS", () => {
    expect(dolphinExecutableName("win32")).toBe("Dolphin.exe");
    expect(dolphinExecutableName("linux")).toBe("dolphin-emu");
    expect(dolphinExecutableName("darwin")).toBe(path.join("Dolphin.app", "Contents", "MacOS", "Dolphin"));
  });

  it("only seeds a User folder from a template in development or when configured", () => {
    expect(defaultUserTemplate(ctx())).toBeNull();
    expect(defaultUserTemplate(ctx({ isDevelopment: true }))).toBe(path.resolve("/repo/run/template-user"));
    expect(defaultUserTemplate(ctx({ env: { PPO_DOLPHIN_USER_TEMPLATE: "/t" } }))).toBe("/t");
  });
});
