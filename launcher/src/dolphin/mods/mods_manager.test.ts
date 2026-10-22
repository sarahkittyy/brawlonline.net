import { mkdtemp, rm, writeFile } from "fs/promises";
import os from "os";
import path from "path";
import { afterEach, beforeEach, describe, expect, it, vi } from "vitest";

import { BRAWL_NTSC_U_DISC } from "../brawl_disc";

const discIdentity = vi.fn(async () => BRAWL_NTSC_U_DISC);
vi.mock("./disc_info", async (importOriginal) => ({
  ...(await importOriginal<Record<string, unknown>>()),
  discIdentity: (...args: unknown[]) => discIdentity(...(args as [])),
}));

import type { SettingsManager } from "@settings/settings_manager";

import type { DolphinManager } from "../manager";
import { buildHashKey, combineBuildHash } from "./build_hash";
import { ModsManager } from "./mods_manager";
import type { ModBuild } from "./types";

let tmp: string;

beforeEach(async () => {
  tmp = await mkdtemp(path.join(os.tmpdir(), "mods-manager-"));
  discIdentity.mockClear();
});

afterEach(async () => {
  await rm(tmp, { recursive: true, force: true });
});

const OLD_DISC = "0e95949ac585f357e79fcd34b20670b5dca97ac2";

/** A game hashed by a launcher before 82db6b5e (build hash v3, the disc's contents' SHA-1). */
async function oldBuild(id: string, over: Partial<ModBuild> = {}): Promise<ModBuild> {
  const discPath = path.join(tmp, `${id}.iso`);
  await writeFile(discPath, id);
  const key = await buildHashKey({ discPath, launcherPath: null, sdPath: null });
  return {
    id,
    name: id,
    autoName: false,
    discPath,
    launcherPath: null,
    sdPath: null,
    gameId: "RSBE01",
    revision: 1,
    platform: "wii",
    hash: `${id}-v3`.padEnd(64, "0"),
    parts: { disc: OLD_DISC, launcher: null, sd: false, sdCard: null },
    hashKey: key.replace(/^v4\|/, "v3|"),
    ...over,
  };
}

function manager(builds: ModBuild[]): { mods: ModsManager; builds: () => ModBuild[] } {
  const settings: Record<string, unknown> = { modBuilds: builds };
  const settingsManager = {
    get: () => settings,
    updateSetting: async (key: string, value: unknown) => {
      settings[key] = value;
    },
  } as unknown as SettingsManager;
  const dolphin = { netplayUserFolder: () => tmp } as unknown as DolphinManager;
  const mods = new ModsManager(settingsManager, dolphin, async () => "DolphinTool");
  return { mods, builds: () => settings.modBuilds as ModBuild[] };
}

describe("rehashChanged", () => {
  it("hashes again a game an older launcher hashed, so rooms of its disc match it", async () => {
    const { mods, builds } = manager([await oldBuild("pplus")]);
    await mods.rehashChanged();
    const [build] = builds();
    expect(build.parts?.disc).toBe(BRAWL_NTSC_U_DISC);
    expect(build.hash).toBe(combineBuildHash({ disc: BRAWL_NTSC_U_DISC, launcher: null, sd: false }));
    expect(build.hashKey).toBe(await buildHashKey(build));
  });

  it("leaves a game hashed by this launcher, and one whose files are gone", async () => {
    const current = await oldBuild("current");
    current.hashKey = await buildHashKey(current);
    const gone = await oldBuild("gone", { discPath: path.join(tmp, "unplugged", "gone.iso") });
    const { mods, builds } = manager([current, gone]);
    await mods.rehashChanged();
    expect(discIdentity).not.toHaveBeenCalled();
    expect(builds()).toEqual([current, gone]);
  });

  it("keeps both of two games that are now one disc (Brawl Rev 1 and Rev 2)", async () => {
    const { mods, builds } = manager([await oldBuild("rev1"), await oldBuild("rev2", { revision: 2 })]);
    await mods.rehashChanged();
    expect(builds().map((b) => b.id)).toEqual(["rev1", "rev2"]);
    expect(builds().every((b) => b.parts?.disc === BRAWL_NTSC_U_DISC)).toBe(true);
  });
});
