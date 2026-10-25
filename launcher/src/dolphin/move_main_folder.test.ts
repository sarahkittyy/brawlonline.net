// DolphinManager.moveMainFolder on real folders (Electron and the settings file are stand-ins).

import { defaultAppSettings } from "@settings/default_settings";
import type { SettingsManager } from "@settings/settings_manager";
import type { SettingsSchema, SettingUpdate } from "@settings/types";
import fs from "fs";
import type os from "os";
import path from "path";
import { afterAll, beforeEach, describe, expect, it, vi } from "vitest";

import { DolphinManager } from "./manager";

// Electron's folders (userData, documents) are in the case's folder. (Hoisted above the imports:
// default_settings.ts reads Documents as it loads.)
const scratch = vi.hoisted(() => {
  /* eslint-disable @typescript-eslint/no-var-requires */
  const { mkdtempSync } = require("fs") as typeof fs;
  const { tmpdir } = require("os") as typeof os;
  const { join } = require("path") as typeof path;
  /* eslint-enable @typescript-eslint/no-var-requires */
  const root = mkdtempSync(join(tmpdir(), "move-main-folder-"));
  return { root, dir: root, join };
});

vi.mock("electron", () => ({
  app: { isPackaged: false, getPath: (name: string) => scratch.join(scratch.dir, name) },
  BrowserWindow: { getFocusedWindow: () => null, getAllWindows: () => [] },
  dialog: { showMessageBox: async () => ({ response: 0 }) },
}));

const root = scratch.root;

class FakeSettings {
  settings: SettingsSchema = structuredClone(defaultAppSettings.settings);
  get() {
    return { ...defaultAppSettings, settings: this.settings };
  }
  getDolphinPath() {
    return null;
  }
  getClipsFolder() {
    return this.settings.clipsFolder ?? path.join(this.settings.rootSlpPath, "Highlights");
  }
  async updateSetting(key: keyof SettingsSchema, value: unknown) {
    (this.settings as any)[key] = value;
  }
  async updateSettings(updates: SettingUpdate[]) {
    updates.forEach((u) => ((this.settings as any)[u.key] = u.value));
  }
}

async function write(p: string, content = "x") {
  await fs.promises.mkdir(path.dirname(p), { recursive: true });
  await fs.promises.writeFile(p, content);
}
const exists = (p: string) => fs.existsSync(p);

let n = 0;
let dir: string;
let settings: FakeSettings;
let manager: DolphinManager;
let rebased: { from: string; to: string }[];

beforeEach(async () => {
  n += 1;
  dir = path.join(root, `case-${n}`);
  scratch.dir = dir;
  await fs.promises.mkdir(dir, { recursive: true });
  settings = new FakeSettings();
  manager = new DolphinManager(settings as unknown as SettingsManager);
  vi.spyOn(manager, "bundledDolphinReady").mockResolvedValue();
  vi.spyOn(manager, "installDolphin").mockResolvedValue();
  rebased = [];
  manager.setReplayPathRebaser(async (move) => void rebased.push(move));
});

afterAll(async () => {
  await fs.promises.rm(root, { recursive: true, force: true });
});

/** The game files where the launcher leaves them, and replays with a clip and a spectated game. */
async function legacyLayout() {
  const netplay = path.join(dir, "userData", "netplay");
  await write(path.join(netplay, "User", "Config", "Dolphin.ini"), "[General]\n");
  await write(path.join(netplay, "User", "Wii", "sd.raw"), "card");
  await write(path.join(netplay, "pplus", "pplus.json"), "{}");
  await write(path.join(netplay, "Dolphin.exe"), "exe");
  await write(
    path.join(netplay, "dolphin.json"),
    JSON.stringify({ version: "1", executable: "Dolphin.exe", entries: ["Dolphin.exe"], files: 1, size: 3 }),
  );
  // (The default replay folder is read once, at import: the cases share it.)
  const replays = settings.settings.rootSlpPath;
  await fs.promises.rm(replays, { recursive: true, force: true });
  const replay = path.join(replays, "2026-10", "Game_1.brep");
  await write(replay, "replay");
  await write(path.join(replays, "Highlights", "combo.mp4"), "video");
  await write(path.join(replays, "Spectated", "s.brep"), "spectated");
  settings.settings.favoriteHighlights = [`${replay}|combo|10`];
  return { netplay, replays, replay };
}

describe("moveMainFolder", () => {
  it("moves the game files and the launcher's replay folder into the chosen folder", async () => {
    const { netplay, replays, replay } = await legacyLayout();
    const picked = path.join(dir, "D", "Brawl");

    const result = await manager.moveMainFolder(picked);

    expect(result).toEqual({
      folder: picked,
      gameFiles: "moved",
      replays: "moved",
      replaysError: null,
      leftBehind: [],
    });
    const gameFiles = path.join(picked, "Game Files");
    const newReplays = path.join(picked, "Replays");
    expect(exists(path.join(gameFiles, "User", "Wii", "sd.raw"))).toBe(true);
    expect(exists(path.join(gameFiles, "pplus", "pplus.json"))).toBe(true);
    expect(exists(path.join(netplay, "User"))).toBe(false);
    expect(exists(path.join(newReplays, "2026-10", "Game_1.brep"))).toBe(true);
    expect(exists(path.join(newReplays, "Highlights", "combo.mp4"))).toBe(true);
    expect(exists(path.join(newReplays, "Spectated", "s.brep"))).toBe(true);
    expect(exists(replays)).toBe(false);
    expect(settings.settings.mainFolder).toBe(picked);
    expect(settings.settings.gameFilesFolder).toBe(gameFiles);
    expect(settings.settings.rootSlpPath).toBe(newReplays);
    expect(settings.get().settings.clipsFolder).toBeNull();
    expect(settings.settings.favoriteHighlights).toEqual([
      `${path.join(newReplays, path.relative(replays, replay))}|combo|10`,
    ]);
    expect(rebased).toEqual([
      { from: netplay, to: gameFiles },
      { from: replays, to: newReplays },
    ]);
  });

  it("makes a Brawl Online folder inside a folder that has other things", async () => {
    await legacyLayout();
    await write(path.join(dir, "Games", "other.txt"));

    const result = await manager.moveMainFolder(path.join(dir, "Games"));

    expect(result?.folder).toBe(path.join(dir, "Games", "Brawl Online"));
    expect(exists(path.join(dir, "Games", "other.txt"))).toBe(true);
    expect(exists(path.join(dir, "Games", "Brawl Online", "Game Files", "User"))).toBe(true);
  });

  it("leaves a replay folder the player chose where it is", async () => {
    await legacyLayout();
    const mine = path.join(dir, "My Replays");
    await write(path.join(mine, "a.brep"));
    settings.settings.rootSlpPath = mine;
    const picked = path.join(dir, "D", "Brawl");

    const result = await manager.moveMainFolder(picked);

    expect(result).toMatchObject({ gameFiles: "moved", replays: "kept" });
    expect(settings.settings.rootSlpPath).toBe(mine);
    expect(exists(path.join(mine, "a.brep"))).toBe(true);
    expect(exists(path.join(picked, "Replays"))).toBe(false);
  });

  it("moves everything again from one Brawl Online folder to another", async () => {
    await legacyLayout();
    const first = path.join(dir, "first");
    const second = path.join(dir, "second");
    await manager.moveMainFolder(first);

    const result = await manager.moveMainFolder(second);

    expect(result).toMatchObject({ folder: second, gameFiles: "moved", replays: "moved" });
    expect(exists(path.join(second, "Game Files", "User", "Wii", "sd.raw"))).toBe(true);
    expect(exists(path.join(second, "Replays", "2026-10", "Game_1.brep"))).toBe(true);
    expect(exists(first)).toBe(false);
    expect(settings.settings.rootSlpPath).toBe(path.join(second, "Replays"));
    await expect(manager.moveMainFolder(second)).rejects.toThrow(/already in/);
  });

  it("moves nothing when a part of it can't go there", async () => {
    const { netplay, replays } = await legacyLayout();
    const picked = path.join(dir, "D", "Brawl");
    await write(path.join(picked, "Replays", "someone else's.brep"));

    await expect(manager.moveMainFolder(picked)).rejects.toThrow(/not empty/);

    expect(exists(path.join(netplay, "User", "Wii", "sd.raw"))).toBe(true);
    expect(exists(path.join(replays, "2026-10", "Game_1.brep"))).toBe(true);
    expect(settings.settings.mainFolder).toBeNull();
    expect(settings.settings.gameFilesFolder).toBeNull();
  });

  it("refuses a folder inside the replay folder", async () => {
    const { netplay, replays } = await legacyLayout();

    await expect(manager.moveMainFolder(path.join(replays, "BO"))).rejects.toThrow(/your replays are there/);

    expect(exists(path.join(netplay, "User"))).toBe(true);
  });
});
