import { mkdir, mkdtemp, rm, writeFile } from "fs/promises";
import os from "os";
import path from "path";
import { afterEach, beforeEach, describe, expect, it } from "vitest";

import { findPlaybackTextures } from "./playback_textures";

let root: string;
let netplayUser: string;
let playbackUser: string;

const put = async (rel: string, data = "x") => {
  const p = path.join(root, rel);
  await mkdir(path.dirname(p), { recursive: true });
  await writeFile(p, data);
  return p;
};

beforeEach(async () => {
  root = await mkdtemp(path.join(os.tmpdir(), "playback-textures-"));
  netplayUser = path.join(root, "netplay", "User");
  playbackUser = path.join(root, "playback", "User");
  await mkdir(path.join(netplayUser, "Load", "Textures"), { recursive: true });
  await mkdir(path.join(playbackUser, "Load", "Textures"), { recursive: true });
});

afterEach(async () => {
  await rm(root, { recursive: true, force: true });
});

describe("findPlaybackTextures", () => {
  it("shares the netplay User folder's Load when Play has custom textures on", async () => {
    await put("netplay/User/Load/Textures/RSBE01/tex.png");
    await put("netplay/User/Config/GFX.ini", "[Hardware]\r\nVSync = False\r\n[Settings]\r\nHiresTextures = True\r\n");
    expect(await findPlaybackTextures(netplayUser, playbackUser)).toEqual({
      loadPath: path.join(netplayUser, "Load"),
    });
  });

  it("leaves Play's setting alone when it is off or unset (Dolphin's default is off)", async () => {
    await put("netplay/User/Load/Textures/RSBE01/tex.png");
    expect(await findPlaybackTextures(netplayUser, playbackUser)).toBeNull();
    await put("netplay/User/Config/GFX.ini", "[Settings]\nHiresTextures = False\n");
    expect(await findPlaybackTextures(netplayUser, playbackUser)).toBeNull();
    await put("netplay/User/Config/GFX.ini", "[Enhancements]\nHiresTextures = True\n");
    expect(await findPlaybackTextures(netplayUser, playbackUser)).toBeNull();
  });

  it("keeps textures a player put in the playback User folder itself", async () => {
    await put("netplay/User/Config/GFX.ini", "[Settings]\nHiresTextures = True\n");
    await put("playback/User/Load/Textures/RSBE01/tex.png");
    expect(await findPlaybackTextures(netplayUser, playbackUser)).toBeNull();
  });

  it("never passes a User folder its own Load", async () => {
    await put("netplay/User/Config/GFX.ini", "[Settings]\nHiresTextures = True\n");
    expect(await findPlaybackTextures(netplayUser, netplayUser)).toBeNull();
  });
});
