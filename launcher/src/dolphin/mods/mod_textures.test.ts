import { mkdir, mkdtemp, rm, writeFile } from "fs/promises";
import os from "os";
import path from "path";
import { afterEach, beforeEach, describe, expect, it } from "vitest";

import { findModTextures, hasTexturePack, textureGameIds } from "./mod_textures";

let root: string;
let netplayUser: string;

const put = async (rel: string, data = "x") => {
  const p = path.join(root, rel);
  await mkdir(path.dirname(p), { recursive: true });
  await writeFile(p, data);
  return p;
};

beforeEach(async () => {
  root = await mkdtemp(path.join(os.tmpdir(), "mod-textures-"));
  netplayUser = path.join(root, "Brawl Online", "Game Files", "User");
  await mkdir(netplayUser, { recursive: true });
});

afterEach(async () => {
  await rm(root, { recursive: true, force: true });
});

describe("hasTexturePack", () => {
  it("takes Dolphin's folder names and marker files for the game", async () => {
    await put("a/RSBE01/tex1_64x64_0123456789abcdef_14.png");
    await put("b/RSB/tex.png");
    await put("c/My Pack/ids/RSBE01.txt", "");
    await put("d/Pack/all.txt", "");
    await put("e/RMCE01/tex.png");
    const textures = (dir: string) => path.join(root, dir);
    expect(await hasTexturePack(textures("a"), "RSBE01")).toBe(true);
    expect(await hasTexturePack(textures("b"), "RSBE01")).toBe(true);
    expect(await hasTexturePack(textures("c"), "RSBE01")).toBe(true);
    expect(await hasTexturePack(textures("d"), "RSBE01")).toBe(true);
    expect(await hasTexturePack(textures("e"), "RSBE01")).toBe(false);
    expect(await hasTexturePack(textures("missing"), "RSBE01")).toBe(false);
  });

  it("takes the ID Dolphin gives a launcher boot of the disc", async () => {
    await put("f/ID-REX Netplay Launcher-RSBE01/tex.png");
    const launcher = path.join(root, "REX", "REX Netplay Launcher.elf");
    expect(textureGameIds("RSBE01", launcher)).toEqual(["ID-REX Netplay Launcher-RSBE01", "RSBE01"]);
    expect(textureGameIds("RSBE01", null)).toEqual(["RSBE01"]);
    expect(await hasTexturePack(path.join(root, "f"), "RSBE01", launcher)).toBe(true);
    expect(await hasTexturePack(path.join(root, "f"), "RSBE01")).toBe(false);
  });
});

describe("findModTextures", () => {
  it("finds the Load folder of the Dolphin folder a mod's launcher or SD card is in", async () => {
    await put("REX/User/Load/Textures/RSBE01/tex.png");
    const launcher = await put("REX/User/Launcher/REX Netplay Launcher.elf");
    const sd = await put("REX/User/Wii/sd.raw");
    const load = path.join(root, "REX", "User", "Load");
    expect(await findModTextures({ launcherPath: launcher, sdPath: null, gameId: "RSBE01" }, netplayUser)).toEqual({
      loadPath: load,
    });
    expect(await findModTextures({ launcherPath: null, sdPath: sd, gameId: "RSBE01" }, netplayUser)).toEqual({
      loadPath: load,
    });

    await put("Minus/user/Load/Textures/RSB/tex.png");
    const minus = await put("Minus/Launcher/minus.dol");
    // The same folder as "User/Load" on Windows and macOS, which don't tell the case apart.
    const found = await findModTextures({ launcherPath: minus, sdPath: null, gameId: "RSBE01" }, netplayUser);
    expect(found?.loadPath.toLowerCase()).toBe(path.join(root, "Minus", "user", "Load").toLowerCase());
  });

  it("leaves the netplay User folder's Load when the mod folder has no pack for the game", async () => {
    await put("Cobalt/User/Load/Textures/RMCE01/tex.png");
    const launcher = await put("Cobalt/User/Launcher/cobalt.dol");
    expect(await findModTextures({ launcherPath: launcher, sdPath: null, gameId: "RSBE01" }, netplayUser)).toBeNull();
    expect(await findModTextures({ launcherPath: launcher, sdPath: null, gameId: null }, netplayUser)).toBeNull();
    expect(await findModTextures({ launcherPath: null, sdPath: null, gameId: "RSBE01" }, netplayUser)).toBeNull();
  });

  it("leaves it when that Dolphin has custom textures off", async () => {
    await put("REX/User/Load/Textures/RSBE01/tex.png");
    await put("REX/User/Config/GFX.ini", "[Hardware]\r\nVSync = False\r\n[Settings]\r\nHiresTextures = False\r\n");
    const launcher = await put("REX/User/Launcher/rex.elf");
    expect(await findModTextures({ launcherPath: launcher, sdPath: null, gameId: "RSBE01" }, netplayUser)).toBeNull();

    await put("REX/User/Config/GFX.ini", "[Settings]\nHiresTextures = True\n");
    expect(await findModTextures({ launcherPath: launcher, sdPath: null, gameId: "RSBE01" }, netplayUser)).toEqual({
      loadPath: path.join(root, "REX", "User", "Load"),
    });
  });

  it("never points at the netplay User folder's own Load (a build of P+'s card)", async () => {
    await put("Brawl Online/Game Files/User/Load/Textures/RSBE01/tex.png");
    const sd = await put("Brawl Online/Game Files/User/Wii/sd.raw");
    expect(await findModTextures({ launcherPath: null, sdPath: sd, gameId: "RSBE01" }, netplayUser)).toBeNull();
  });
});
