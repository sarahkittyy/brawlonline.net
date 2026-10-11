import fs from "fs";
import os from "os";
import path from "path";
import { afterEach, beforeEach, describe, expect, it, vi } from "vitest";

vi.mock("electron", () => ({ app: { getPath: () => os.tmpdir(), getName: () => "test", isPackaged: false } }));
import { getOnlineSettings, setOnlineSettings } from "./config";
import { IniFile } from "./ini_file";

let tmp: string;
let iniPath: string;

beforeEach(async () => {
  tmp = await fs.promises.mkdtemp(path.join(os.tmpdir(), "online-settings-"));
  iniPath = path.join(tmp, "Dolphin.ini");
});

afterEach(async () => {
  await fs.promises.rm(tmp, { recursive: true, force: true });
});

describe("online settings: music", () => {
  it("is on in a Dolphin.ini without the key (installs from before the setting)", async () => {
    const ini = await IniFile.init(iniPath);
    expect((await getOnlineSettings(ini)).enableMusic).toBe(true);
  });

  it("round-trips through [Online] Music", async () => {
    const ini = await IniFile.init(iniPath);
    await setOnlineSettings(ini, { enableMusic: false });
    expect(await fs.promises.readFile(iniPath, "utf8")).toMatch(/\[Online\][^[]*Music = False/);
    expect((await getOnlineSettings(await IniFile.init(iniPath))).enableMusic).toBe(false);

    await setOnlineSettings(ini, { enableMusic: true });
    expect((await getOnlineSettings(await IniFile.init(iniPath))).enableMusic).toBe(true);
  });
});
