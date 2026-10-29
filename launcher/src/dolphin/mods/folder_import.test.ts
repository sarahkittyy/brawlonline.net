import { mkdir, mkdtemp, rm, writeFile } from "fs/promises";
import os from "os";
import path from "path";
import { afterEach, beforeEach, describe, expect, it } from "vitest";

import { cleanFolderName, findModParts, pickLauncher } from "./folder_import";

describe("mod folder import", () => {
  let dir: string;
  beforeEach(async () => {
    dir = await mkdtemp(path.join(os.tmpdir(), "mods-folder-"));
  });
  afterEach(async () => {
    await rm(dir, { recursive: true, force: true });
  });

  const touch = async (rel: string) => {
    const p = path.join(dir, rel);
    await mkdir(path.dirname(p), { recursive: true });
    await writeFile(p, "x");
    return p;
  };

  it("finds REX's layout (nested in the archive's folder), preferring the netplay launcher", async () => {
    const root = "REX Release 0½ Hotfix 1 - Dolphin";
    await touch(`${root}/Dolphin.exe`);
    await touch(`${root}/Sys/codehandler.bin`);
    await touch(`${root}/user/Launcher/REX Offline Launcher.elf`);
    const netplay = await touch(`${root}/user/Launcher/REX Netplay Launcher.elf`);
    const sd = await touch(`${root}/user/Wii/sd.raw`);
    const parts = await findModParts(path.join(dir, root));
    expect(parts).toEqual({ name: "REX Release 0½ Hotfix 1", launcherPath: netplay, sdPath: sd });
  });

  it("finds Brawl Minus's layout and an archive folder holding one Dolphin folder", async () => {
    const dol = await touch("Brawl Minus v5.1.0 (Dolphin)/windows/Launcher/Minus Launcher.dol");
    await touch("Brawl Minus v5.1.0 (Dolphin)/windows/Sys/GameSettings/RSBE01.ini");
    const parts = await findModParts(path.join(dir, "Brawl Minus v5.1.0 (Dolphin)"));
    expect(parts.launcherPath).toBe(dol);
    expect(parts.sdPath).toBeNull();
    expect(parts.name).toBe("Brawl Minus v5.1.0");
  });

  it("finds Brawl Online's own P+ from its game files folder or its User folder", async () => {
    const netplay = await touch("netplay/User/Launcher/Project+ Netplay Launcher.dol");
    await touch("netplay/User/Launcher/Project+ Offline Launcher.dol");
    const sd = await touch("netplay/User/Wii/sd.raw");
    // The card with our plugin, which Play boots: never the one picked.
    await touch("netplay/pponline-sd/sd.raw");
    await touch("netplay/Sys/codehandler.bin");
    // (Windows and macOS find our `User` as P+'s `user`: the same file.)
    const same = (p: string | null) => p?.toLowerCase();
    for (const folder of ["netplay", "netplay/User"]) {
      const parts = await findModParts(path.join(dir, folder));
      expect(same(parts.launcherPath)).toBe(same(netplay));
      expect(same(parts.sdPath)).toBe(same(sd));
    }
  });

  it("says when there is nothing to play", async () => {
    await touch("empty/readme.txt");
    await expect(findModParts(path.join(dir, "empty"))).rejects.toThrow(/No mod launcher/);
  });

  it("names and picks", () => {
    expect(pickLauncher(["a/b.txt"])).toBeNull();
    expect(pickLauncher(["a/z.dol", "a/b.elf"])).toBe("a/b.elf");
    expect(cleanFolderName("C:/x/Project+ v3.1.5 - Dolphin")).toBe("Project+ v3.1.5");
  });
});
