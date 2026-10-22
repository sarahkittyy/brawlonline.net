import fs from "fs";
import os from "os";
import path from "path";
import { afterEach, beforeEach, describe, expect, it, vi } from "vitest";

vi.mock("electron", () => ({ app: { getPath: () => os.tmpdir(), getName: () => "test", isPackaged: false } }));
import { getChatKey, getOnlineSettings, setChatKey, setOnlineSettings } from "./config";
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

describe("online settings: disable chat", () => {
  it("is off in a Dolphin.ini without the key", async () => {
    const ini = await IniFile.init(iniPath);
    expect((await getOnlineSettings(ini)).disableChat).toBe(false);
  });

  it("round-trips through [Online] DisableChat", async () => {
    const ini = await IniFile.init(iniPath);
    await setOnlineSettings(ini, { disableChat: true });
    expect(await fs.promises.readFile(iniPath, "utf8")).toMatch(/\[Online\][^[]*DisableChat = True/);
    expect((await getOnlineSettings(await IniFile.init(iniPath))).disableChat).toBe(true);

    await setOnlineSettings(ini, { disableChat: false });
    expect((await getOnlineSettings(await IniFile.init(iniPath))).disableChat).toBe(false);
  });
});

describe("online settings: let others stream my games", () => {
  it("is on in a Dolphin.ini without the key", async () => {
    const ini = await IniFile.init(iniPath);
    expect((await getOnlineSettings(ini)).enableStreamGames).toBe(true);
  });

  it("round-trips through [Online] StreamGames", async () => {
    const ini = await IniFile.init(iniPath);
    await setOnlineSettings(ini, { enableStreamGames: false });
    expect(await fs.promises.readFile(iniPath, "utf8")).toMatch(/\[Online\][^[]*StreamGames = False/);
    expect((await getOnlineSettings(await IniFile.init(iniPath))).enableStreamGames).toBe(false);
    await setOnlineSettings(ini, { enableStreamGames: true });
    expect((await getOnlineSettings(await IniFile.init(iniPath))).enableStreamGames).toBe(true);
  });
});

describe("chat key", () => {
  const hotkeys = async (text?: string) => {
    const file = path.join(tmp, "Hotkeys.ini");
    if (text !== undefined) {
      await fs.promises.writeFile(file, text);
    }
    return IniFile.init(file);
  };

  it("is Dolphin's default T with no setting and no Hotkeys.ini", async () => {
    expect(getChatKey(await IniFile.init(iniPath), await hotkeys())).toBe("`T`");
  });

  it("is Hotkeys.ini's binding when the launcher never set it", async () => {
    const bound = await hotkeys("[Hotkeys]\nDevice = DInput/0/Keyboard Mouse\nGeneral/Activate NetPlay Chat = Y\n");
    expect(getChatKey(await IniFile.init(iniPath), bound)).toBe("Y");
    const unbound = await hotkeys("[Hotkeys]\nGeneral/Activate NetPlay Chat = \n");
    expect(getChatKey(await IniFile.init(iniPath), unbound)).toBe("");
  });

  it("round-trips through [Online] ChatKey, which wins over Hotkeys.ini", async () => {
    await setChatKey(await IniFile.init(iniPath), "`SLASH`");
    expect(await fs.promises.readFile(iniPath, "utf8")).toMatch(/\[Online\][^[]*ChatKey = `SLASH`/);
    const bound = await hotkeys("[Hotkeys]\nGeneral/Activate NetPlay Chat = Y\n");
    expect(getChatKey(await IniFile.init(iniPath), bound)).toBe("`SLASH`");
  });
});

describe("online settings: Project+ release", () => {
  it("writes [Online] PPlusRelease for replays to record (docs/design/pplus-versions.md)", async () => {
    const ini = await IniFile.init(iniPath);
    await setOnlineSettings(ini, { pplusRelease: "v3.2.1" });
    expect(await fs.promises.readFile(iniPath, "utf8")).toMatch(/\[Online\][^[]*PPlusRelease = v3\.2\.1/);
  });
});

describe("online settings: network for online games", () => {
  it("writes the chosen network as a forced LAN address with its prefix", async () => {
    const ini = await IniFile.init(iniPath);
    await setOnlineSettings(ini, { netplayNetwork: "10.147.17.5/24" });
    const text = await fs.promises.readFile(iniPath, "utf8");
    expect(text).toMatch(/\[Online\][^[]*ForceLanIP = True/);
    expect(text).toMatch(/\[Online\][^[]*LanIP = 10\.147\.17\.5\/24/);

    await setOnlineSettings(ini, { netplayNetwork: "" });
    const after = await fs.promises.readFile(iniPath, "utf8");
    expect(after).toMatch(/\[Online\][^[]*ForceLanIP = False/);
    expect(after).not.toMatch(/10\.147\.17\.5/);
  });

  it("leaves a Force LAN IP set by hand (no prefix) alone on Automatic", async () => {
    await fs.promises.writeFile(iniPath, "[Online]\nForceLanIP = True\nLanIP = 192.168.5.5\n");
    const ini = await IniFile.init(iniPath);
    await setOnlineSettings(ini, { netplayNetwork: "" });
    const text = await fs.promises.readFile(iniPath, "utf8");
    expect(text).toMatch(/ForceLanIP = True/);
    expect(text).toMatch(/LanIP = 192\.168\.5\.5/);
  });
});

describe("online settings: matchmaking ping limit", () => {
  it("writes [Online] MatchmakingPingLimit, and the default for a value that is not a choice", async () => {
    const ini = await IniFile.init(iniPath);
    await setOnlineSettings(ini, { pingLimit: 200 });
    expect(await fs.promises.readFile(iniPath, "utf8")).toMatch(/\[Online\][^[]*MatchmakingPingLimit = 200/);

    await setOnlineSettings(ini, { pingLimit: 75 });
    expect(await fs.promises.readFile(iniPath, "utf8")).toMatch(/\[Online\][^[]*MatchmakingPingLimit = 150/);
  });
});
