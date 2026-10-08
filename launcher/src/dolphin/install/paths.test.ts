import path from "path";
import { describe, expect, it } from "vitest";

import { DolphinLaunchType } from "../types";
import type { DolphinPathEnv } from "./paths";
import {
  defaultDolphinExecutable,
  defaultPluginLocation,
  defaultUserTemplate,
  dolphinExecutableName,
  installedDolphinExecutable,
  patchedSdCardFolder,
  PLUGIN_SD_PATH,
  projectPlusStoreFolder,
} from "./paths";

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

  it("uses the installed bundle's AppDir layout on Linux", () => {
    const exe = defaultDolphinExecutable(DolphinLaunchType.NETPLAY, ctx({ platform: "linux" }));
    expect(exe).toBe(path.join(path.resolve("/userdata"), "netplay", "usr", "bin", "project-plus-dolphin"));
    expect(installedDolphinExecutable("win32")).toBe("Dolphin.exe");
    expect(projectPlusStoreFolder(path.resolve("/userdata"))).toBe(
      path.join(path.resolve("/userdata"), "netplay", "pplus"),
    );
  });

  it("honours PPO_DOLPHIN_PATH", () => {
    const exe = defaultDolphinExecutable(DolphinLaunchType.NETPLAY, ctx({ env: { PPO_DOLPHIN_PATH: " /x/D.exe " } }));
    expect(exe).toBe("/x/D.exe");
  });

  it("names the binary per OS", () => {
    expect(dolphinExecutableName("win32")).toBe("Dolphin.exe");
    expect(dolphinExecutableName("linux")).toBe("project-plus-dolphin");
    expect(dolphinExecutableName("darwin")).toBe(path.join("Dolphin.app", "Contents", "MacOS", "Dolphin"));
  });

  it("only seeds a User folder from a template in development or when configured", () => {
    expect(defaultUserTemplate(ctx())).toBeNull();
    expect(defaultUserTemplate(ctx({ isDevelopment: true }))).toBe(path.resolve("/repo/run/template-user"));
    expect(defaultUserTemplate(ctx({ env: { PPO_DOLPHIN_USER_TEMPLATE: "/t" } }))).toBe("/t");
  });

  it("takes the plugin from game-code's build in development and from resources when packaged", () => {
    const resourcesPath = path.resolve("/app/resources");
    expect(defaultPluginLocation({ ...ctx({ isDevelopment: true }), resourcesPath })).toEqual({
      binary: path.resolve("/repo/game-code/PPOnline/PPOnline.rel"),
      manifest: null,
    });
    expect(defaultPluginLocation({ ...ctx(), resourcesPath })).toEqual({
      binary: path.join(resourcesPath, "plugins", "PPOnline.rel"),
      manifest: path.join(resourcesPath, "plugins", "PPOnline.json"),
    });
    expect(defaultPluginLocation({ ...ctx({ env: { PPO_PLUGIN_PATH: " /x/P.rel " } }), resourcesPath })).toEqual({
      binary: "/x/P.rel",
      manifest: null,
    });
  });

  it("keeps the patched SD card under userData/netplay", () => {
    expect(patchedSdCardFolder(path.resolve("/userdata"))).toBe(
      path.join(path.resolve("/userdata"), "netplay", "pponline-sd"),
    );
    expect(PLUGIN_SD_PATH).toBe("/Project+/pf/plugins/PPOnline.rel");
  });
});
