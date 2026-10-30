import { createHash } from "crypto";
import { mkdir, mkdtemp, readdir, readFile, rm, writeFile } from "fs/promises";
import os from "os";
import path from "path";
import { afterEach, beforeEach, describe, expect, it, vi } from "vitest";

import { BRAWL_NTSC_U_DISC } from "../brawl_disc";

const discIdentity = vi.fn(async () => BRAWL_NTSC_U_DISC);
const readDiscInfo = vi.fn(async () => ({
  gameId: "RSBE01",
  title: "Super Smash Bros. Brawl",
  revision: 1,
  platform: "wii",
  titleId: "0001000052534245",
}));
vi.mock("./disc_info", async (importOriginal) => ({
  ...(await importOriginal<Record<string, unknown>>()),
  discIdentity: (...args: unknown[]) => discIdentity(...(args as [])),
  readDiscInfo: (...args: unknown[]) => readDiscInfo(...(args as [])),
}));

import type { PublicModRoom } from "@accounts/types";
import type { SettingsManager } from "@settings/settings_manager";

import type { DolphinManager } from "../manager";
import { BUILD_HASH_VERSION, buildHashKey, combineBuildHash } from "./build_hash";
import type { ModsRoomArg } from "./mods_args";
import { ModsManager } from "./mods_manager";
import { saveStoreDir, titleDataDir } from "./save_import";
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
    hashKey: key.replace(/^v\d+\|/, "v3|"),
    ...over,
  };
}

function manager(
  builds: ModBuild[],
  settings: Record<string, unknown> = { modBuilds: builds },
  room: PublicModRoom | null = null,
  codeDatabase: (gameId: string) => Promise<string | null> = async () => null,
): {
  mods: ModsManager;
  builds: () => ModBuild[];
  settings: Record<string, unknown>;
  launches: ModsRoomArg[];
  /** Closes the Dolphins launched (their rooms' polls stop). */
  closeAll: () => Promise<void>;
} {
  const settingsManager = {
    get: () => settings,
    updateSetting: async (key: string, value: unknown) => {
      settings[key] = value;
    },
  } as unknown as SettingsManager;
  const launches: ModsRoomArg[] = [];
  const closers: Array<() => void | Promise<void>> = [];
  const dolphin = {
    netplayUserFolder: () => path.join(tmp, "netplay", "User"),
    netplayOnlineDir: () => path.join(tmp, "netplay", "User", "Online"),
    isNetplayRunning: () => false,
    launchModsDolphin: async (_build: ModBuild, arg: ModsRoomArg, onClose: () => void | Promise<void>) => {
      launches.push(arg);
      closers.push(onClose);
    },
  } as unknown as DolphinManager;
  const closeAll = async () => {
    for (const close of closers.splice(0)) {
      await close();
    }
  };
  const mods = new ModsManager(
    settingsManager,
    dolphin,
    async () => "DolphinTool",
    async () => room,
    async () => "card",
    undefined,
    // The player's own Dolphin: never this machine's.
    () => [path.join(tmp, "own Dolphin")],
    codeDatabase,
  );
  return { mods, builds: () => settings.modBuilds as ModBuild[], settings, launches, closeAll };
}

describe("rehashChanged", () => {
  it("hashes again a game an older launcher hashed, so rooms of its disc match it", async () => {
    const { mods, builds } = manager([await oldBuild("pplus")]);
    await mods.rehashChanged();
    const [build] = builds();
    expect(build.parts?.disc).toBe(BRAWL_NTSC_U_DISC);
    expect(build.hash).toBe(combineBuildHash({ disc: BRAWL_NTSC_U_DISC, launcher: null, sd: false }));
    expect(build.hashKey).toBe(await buildHashKey(build));
    expect(discIdentity).toHaveBeenCalledTimes(1);
  });

  it("runs once: not at the next start, even for a game whose files changed or that failed", async () => {
    const pplus = await oldBuild("pplus");
    const gone = await oldBuild("gone", { discPath: path.join(tmp, "unplugged", "gone.iso") });
    const first = manager([pplus, gone]);
    await first.mods.rehashChanged();
    expect(discIdentity).toHaveBeenCalledTimes(1);
    expect(first.settings.modsHashVersion).toBe(BUILD_HASH_VERSION);
    expect(first.builds()[1]).toEqual(gone);

    // The next start: P+'s disc changed since (it is hashed at its next Host or Join, not now).
    await writeFile(pplus.discPath, "changed");
    discIdentity.mockClear();
    const next = manager(first.builds(), first.settings);
    await next.mods.rehashChanged();
    expect(discIdentity).not.toHaveBeenCalled();
  });

  it("leaves a game hashed by this launcher", async () => {
    const current = await oldBuild("current");
    current.hashKey = await buildHashKey(current);
    const { mods, builds } = manager([current]);
    await mods.rehashChanged();
    expect(discIdentity).not.toHaveBeenCalled();
    expect(builds()).toEqual([current]);
  });

  it("keeps both of two games that are now one disc (Brawl Rev 1 and Rev 2)", async () => {
    const { mods, builds } = manager([await oldBuild("rev1"), await oldBuild("rev2", { revision: 2 })]);
    await mods.rehashChanged();
    expect(builds().map((b) => b.id)).toEqual(["rev1", "rev2"]);
    expect(builds().every((b) => b.parts?.disc === BRAWL_NTSC_U_DISC)).toBe(true);
  });
});

describe("a new Wii game's save", () => {
  const BRAWL = "0001000052534245";
  const brawlData = (root: string) => path.join(root, "Wii", "title", "00010000", "52534245", "data");

  /** A Brawl mod's Dolphin folder: its launcher, maybe its SD card, and its own Brawl save. */
  async function modFolder(name: string, save: string | null): Promise<{ launcherPath: string; sdPath: string }> {
    const user = path.join(tmp, name, "User");
    const launcherPath = path.join(user, "Launcher", "Netplay Launcher.dol");
    await mkdir(path.dirname(launcherPath), { recursive: true });
    await writeFile(launcherPath, name);
    await mkdir(brawlData(user), { recursive: true });
    if (save !== null) {
      await writeFile(path.join(brawlData(user), "autosv0.bin"), save);
    }
    const sdPath = path.join(user, "Wii", "sd.raw");
    await writeFile(sdPath, "sd");
    return { launcherPath, sdPath };
  }

  async function newBuild(id: string, over: Partial<ModBuild>): Promise<ModBuild> {
    const build = await oldBuild(id, { titleId: BRAWL, ...over });
    return { ...build, hash: null, parts: null, hashKey: null };
  }

  const store = (hash: string) => titleDataDir(saveStoreDir(path.join(tmp, "netplay", "User"), hash), BRAWL);

  it("starts from the save in the mod's Dolphin folder (bug report #144)", async () => {
    const { mods, builds } = manager([await newBuild("cobalt", await modFolder("Cobalt 1.33b", "my rules"))]);
    await mods.hashBuild("cobalt");
    expect(await readFile(path.join(store(builds()[0].hash!), "autosv0.bin"), "utf8")).toBe("my rules");
  });

  it("leaves a save the store has, and a mod folder with no save", async () => {
    const { mods, builds } = manager([
      await newBuild("cobalt", await modFolder("Cobalt", "theirs")),
      await newBuild("fresh", { launcherPath: (await modFolder("Fresh", null)).launcherPath }),
    ]);
    const hash = combineBuildHash({
      disc: BRAWL_NTSC_U_DISC,
      launcher: createHash("sha256").update("Cobalt").digest("hex"),
      sd: true,
    });
    await mkdir(store(hash), { recursive: true });
    await writeFile(path.join(store(hash), "autosv0.bin"), "from rooms");
    await mods.hashBuild("cobalt");
    await mods.hashBuild("fresh");
    const [cobalt, fresh] = builds();
    expect(cobalt.hash).toBe(hash);
    expect(await readFile(path.join(store(hash), "autosv0.bin"), "utf8")).toBe("from rooms");
    expect(await readdir(store(fresh.hash!)).catch(() => [])).toEqual([]);
  });

  it("starts a plain disc from the save in the player's own Dolphin, not Brawl (bug report #193)", async () => {
    const TVC = "0001000053544b45";
    const own = path.join(tmp, "own Dolphin", "Wii", "title", "00010000");
    await mkdir(path.join(own, "53544b45", "data"), { recursive: true });
    await writeFile(path.join(own, "53544b45", "data", "save.dat"), "everyone unlocked");
    await mkdir(path.join(own, "52534245", "data"), { recursive: true });
    await writeFile(path.join(own, "52534245", "data", "autosv0.bin"), "NoTap");
    // (The mocked disc check gives both discs one hash: one list each.)
    const brawlList = manager([await newBuild("brawl", {})]);
    await brawlList.mods.hashBuild("brawl");
    expect(await readdir(store(brawlList.builds()[0].hash!)).catch(() => [])).toEqual([]);
    const { mods, builds } = manager([await newBuild("tvc", { gameId: "STKE08", titleId: TVC })]);
    await mods.hashBuild("tvc");
    const [tvc] = builds();
    const tvcStore = titleDataDir(saveStoreDir(path.join(tmp, "netplay", "User"), tvc.hash!), TVC);
    expect(await readFile(path.join(tvcStore, "save.dat"), "utf8")).toBe("everyone unlocked");
    // The edit window offers it only while the store's save is another one.
    expect((await mods.saveInfo("tvc"))?.ownDolphin).toBeNull();
    await writeFile(path.join(tvcStore, "save.dat"), "new save from a room");
    expect((await mods.saveInfo("tvc"))?.ownDolphin?.folder).toBe(path.join(own, "53544b45", "data"));
  });

  it("never starts from P+'s save (a card in the netplay User folder)", async () => {
    const pplus = path.join(tmp, "netplay", "User");
    await mkdir(brawlData(pplus), { recursive: true });
    await writeFile(path.join(brawlData(pplus), "autosv0.bin"), "NoTap");
    const sdPath = path.join(pplus, "Wii", "sd.raw");
    await writeFile(sdPath, "sd");
    const { mods, builds } = manager([await newBuild("brawl", { sdPath })]);
    await mods.hashBuild("brawl");
    expect(await readdir(store(builds()[0].hash!)).catch(() => [])).toEqual([]);
  });
});

describe("joinByCode", () => {
  /** A game just added: not checked yet (its check runs in the background). */
  async function unchecked(id: string, over: Partial<ModBuild> = {}): Promise<ModBuild> {
    return { ...(await oldBuild(id, over)), hash: null, parts: null, hashKey: null };
  }

  const brawlRoom = (build: Partial<PublicModRoom["build"]> = {}): PublicModRoom => ({
    code: "BKSD",
    build: {
      hash: combineBuildHash({ disc: BRAWL_NTSC_U_DISC, launcher: null, sd: false }),
      name: "Brawl",
      disc: BRAWL_NTSC_U_DISC,
      discLabel: "RSBE01",
      ...build,
    },
    hostName: "ANGE",
    players: 1,
    open: 4,
  });

  it("checks a game not checked yet and joins with it", async () => {
    const { mods } = manager([await unchecked("brawl")], undefined, brawlRoom());
    const join = vi.spyOn(mods, "join").mockResolvedValue({ outcome: "joining" });
    expect(await mods.joinByCode({ uid: "u", code: "bksd" })).toEqual({ outcome: "joining" });
    expect(join).toHaveBeenCalledWith({ buildId: "brawl", code: "BKSD", uid: "u" });
  });

  it("names the player's game when only its SD card keeps it out (bug report #158)", async () => {
    const { mods } = manager([await unchecked("rex")], undefined, brawlRoom({ name: "REX", sd: "card" }));
    const join = vi.spyOn(mods, "join");
    expect(await mods.joinByCode({ uid: "u", code: "BKSD" })).toEqual({
      outcome: "no-game",
      name: "REX",
      hash: brawlRoom().build.hash,
      discLabel: "RSBE01",
      closest: { buildId: "rex", part: "sd", roomHasCard: true },
    });
    expect(join).not.toHaveBeenCalled();
  });

  it("names the player's game on another disc of the room's game", async () => {
    discIdentity.mockResolvedValueOnce("ff".repeat(20));
    const { mods } = manager([await unchecked("dump")], undefined, brawlRoom());
    const result = await mods.joinByCode({ uid: "u", code: "BKSD" });
    expect(result).toMatchObject({ outcome: "no-game", closest: { buildId: "dump", part: "disc" } });
  });

  it("says there is no game when the player has none of the room's, without reading other games", async () => {
    const { mods } = manager([await unchecked("mkwii", { gameId: "RMCE01" })], undefined, brawlRoom());
    const result = await mods.joinByCode({ uid: "u", code: "BKSD" });
    expect(result).toEqual({ outcome: "no-game", name: "Brawl", hash: brawlRoom().build.hash, discLabel: "RSBE01" });
    expect(discIdentity).not.toHaveBeenCalled();
  });
});

describe("Brawl Online's own Project+", () => {
  const user = () => path.join(tmp, "netplay", "User");
  const dol = () => path.join(user(), "Launcher", "Project+ Netplay Launcher.dol");
  const card = () => path.join(user(), "Wii", "sd.raw");

  async function installPPlus(): Promise<string> {
    await mkdir(path.dirname(dol()), { recursive: true });
    await writeFile(dol(), "launcher");
    await mkdir(path.dirname(card()), { recursive: true });
    await writeFile(card(), "card");
    const iso = path.join(tmp, "brawl.iso");
    await writeFile(iso, "brawl");
    return iso;
  }

  /** The list once refreshed, with the check it started done. */
  async function refreshed(m: ReturnType<typeof manager>): Promise<ModBuild[]> {
    await m.mods.refreshBuiltIn();
    const builtIn = m.builds().find((b) => b.builtIn);
    if (builtIn) {
      await m.mods.hashBuild(builtIn.id);
    }
    return m.builds();
  }

  it("is in the list once P+ is installed and the Brawl disc set, first, and checked", async () => {
    const other = await oldBuild("nsmb", { gameId: "SMNE01" });
    other.hashKey = await buildHashKey(other);
    const m = manager([other], { modBuilds: [other], settings: { isoPath: null } });
    expect(await refreshed(m)).toEqual([other]);

    const iso = await installPPlus();
    expect(await refreshed(m)).toEqual([other]);
    (m.settings.settings as Record<string, unknown>).isoPath = iso;
    const [pplus, ...rest] = await refreshed(m);
    expect(rest).toEqual([other]);
    expect(pplus).toMatchObject({
      name: "Project+ v3.2.1",
      builtIn: true,
      autoName: false,
      discPath: iso,
      launcherPath: dol(),
      sdPath: card(),
      gameId: "RSBE01",
      titleId: "0001000052534245",
    });
    expect(pplus.parts).toMatchObject({ disc: BRAWL_NTSC_U_DISC, sd: true, sdCard: "card" });

    // Nothing changed: left as it is.
    readDiscInfo.mockClear();
    expect(await refreshed(m)).toEqual([pplus, other]);
    expect(readDiscInfo).not.toHaveBeenCalled();
  });

  it("follows the Brawl disc, and goes while there is none (back with the same save store)", async () => {
    const iso = await installPPlus();
    const m = manager([], { modBuilds: [], settings: { isoPath: iso } });
    const [first] = await refreshed(m);
    const rev2 = path.join(tmp, "brawl-rev2.rvz");
    await writeFile(rev2, "rev2");
    (m.settings.settings as Record<string, unknown>).isoPath = rev2;
    const [moved] = await refreshed(m);
    expect(moved).toMatchObject({ id: first.id, discPath: rev2, builtIn: true });
    expect(moved.hashKey).not.toBe(first.hashKey);
    // The same disc's game data: the same hash, so the same save.
    expect(moved.hash).toBe(first.hash);

    (m.settings.settings as Record<string, unknown>).isoPath = null;
    expect(await refreshed(m)).toEqual([]);
    (m.settings.settings as Record<string, unknown>).isoPath = rev2;
    const [back] = await refreshed(m);
    expect(back.hash).toBe(first.hash);
  });

  it("is the game the player added from the game files folder (Add mod folder)", async () => {
    const iso = await installPPlus();
    const added: ModBuild = {
      id: "added",
      name: "netplay",
      discPath: iso,
      launcherPath: dol(),
      sdPath: card(),
      hash: null,
      hashKey: null,
    };
    const m = manager([added], { modBuilds: [added], settings: { isoPath: iso } });
    const builds = await refreshed(m);
    expect(builds).toHaveLength(1);
    expect(builds[0]).toMatchObject({ id: "added", name: "Project+ v3.2.1", builtIn: true });
  });

  it("stays beside a copy of P+ the player added from elsewhere (the same hash)", async () => {
    const iso = await installPPlus();
    const elsewhere = path.join(tmp, "P+ Dolphin", "User");
    await mkdir(path.join(elsewhere, "Launcher"), { recursive: true });
    await mkdir(path.join(elsewhere, "Wii"), { recursive: true });
    await writeFile(path.join(elsewhere, "Launcher", "Project+ Netplay Launcher.dol"), "launcher");
    await writeFile(path.join(elsewhere, "Wii", "sd.raw"), "card");
    const m = manager([], { modBuilds: [], settings: { isoPath: iso } });
    const [pplus] = await refreshed(m);
    const copy = await m.mods.addBuild({
      name: "",
      discPath: iso,
      launcherPath: path.join(elsewhere, "Launcher", "Project+ Netplay Launcher.dol"),
      sdPath: path.join(elsewhere, "Wii", "sd.raw"),
    });
    // The player's copy is the duplicate: it goes, P+ stays.
    await expect(m.mods.hashBuild(copy.id)).rejects.toThrow(/already have this game: Project\+ v3.2.1/);
    expect(m.builds().map((b) => b.id)).toEqual([pplus.id]);
  });

  it("cannot be removed or have its files changed", async () => {
    const iso = await installPPlus();
    const m = manager([], { modBuilds: [], settings: { isoPath: iso } });
    const [pplus] = await refreshed(m);
    await expect(m.mods.removeBuild(pplus.id)).rejects.toThrow(/built into Brawl Online/);
    await expect(m.mods.updateBuild(pplus.id, { sdPath: null })).rejects.toThrow(/Brawl Online's own/);
    expect(m.builds()).toEqual([pplus]);
  });

  it("never has its SD card overwritten by a room's", async () => {
    const iso = await installPPlus();
    const m = manager([], { modBuilds: [], settings: { isoPath: iso } });
    const [pplus] = await refreshed(m);
    expect(await m.mods.join({ buildId: pplus.id, code: "BKSD", uid: "u", sdSync: "overwrite" })).toEqual({
      outcome: "joining",
    });
    expect(m.launches).toEqual([expect.objectContaining({ join: "BKSD", sd_sync: "yes" })]);
    // Dolphin's own question too.
    await mkdir(path.join(user(), "Online"), { recursive: true });
    await m.mods.request({ op: "sd-sync", choice: "overwrite" });
    const request = JSON.parse(await readFile(path.join(user(), "Online", "mods-request.json"), "utf8"));
    expect(request).toMatchObject({ op: "sd-sync", choice: "yes" });
    await m.closeAll();
  });

  it("leaves another game's overwrite as the player chose", async () => {
    const iso = await installPPlus();
    const rex: ModBuild = {
      id: "rex",
      name: "REX",
      discPath: iso,
      launcherPath: null,
      sdPath: card(),
      hash: null,
      hashKey: null,
    };
    const m = manager([rex], { modBuilds: [rex], settings: { isoPath: null } });
    await m.mods.join({ buildId: "rex", code: "BKSD", uid: "u", sdSync: "overwrite" });
    expect(m.launches).toEqual([expect.objectContaining({ sd_sync: "overwrite" })]);
    await m.closeAll();
  });
});

describe("room codes", () => {
  /** A game ini in a Dolphin User folder's GameSettings. */
  async function gameIni(user: string, name: string, lines: string[]): Promise<void> {
    await mkdir(path.join(user, "GameSettings"), { recursive: true });
    await writeFile(path.join(user, "GameSettings", `${name}.ini`), lines.join("\r\n"));
  }
  const own = () => path.join(tmp, "own Dolphin");
  const netplay = () => path.join(tmp, "netplay", "User");
  const hostArg = (arg: ModsRoomArg) => arg as Extract<ModsRoomArg, { host: true }>;

  async function marioParty(over: Partial<ModBuild> = {}): Promise<ModBuild> {
    return oldBuild("mp6", { gameId: "GP6E01", revision: 0, platform: "gamecube", ...over });
  }

  it("starts with the codes on in the player's Dolphins, and its room runs them", async () => {
    await gameIni(own(), "GP6E01", [
      "[Gecko]",
      "$Faster Boot Time [gamemasterplc]",
      "204cdc74 386003e8",
      "*Skips the intro",
      "$Off",
      "04001238 00000002",
      "[Gecko_Enabled]",
      "$Faster Boot Time",
    ]);
    // Brawl Online's own Dolphin, by the 3-letter name; and a code of Dolphin's list turned on there.
    await gameIni(netplay(), "GP6", [
      "[ActionReplay]",
      "$Marker",
      "04002F04 CAFEBABE",
      "[ActionReplay_Enabled]",
      "$Marker",
      "[Gecko_Enabled]",
      "$QOL - Instant Text Display",
    ]);
    const m = manager([await marioParty()]);
    await m.mods.host({ buildId: "mp6", public: true });
    expect(hostArg(m.launches[0]).codes).toEqual([
      { name: "Marker", kind: "ar", lines: ["04002F04 CAFEBABE"] },
      { name: "Faster Boot Time", kind: "gecko", lines: ["204CDC74 386003E8"] },
      {
        name: "QOL - Instant Text Display",
        kind: "gecko",
        lines: [expect.stringMatching(/^[0-9A-F]{8} [0-9A-F]{8}$/)],
      },
    ]);
    // Kept with the game: the player's Dolphin changing later changes nothing.
    expect(m.builds()[0].codes?.on.map((c) => c.source)).toEqual(["own", "own", "dolphin"]);
    await m.closeAll();
  });

  it("starts a Brawl game with none (its inis are an old P+ netplay setup's)", async () => {
    await gameIni(own(), "RSBE01", ["[Gecko]", "+$Netplay Code", "04001234 00000001"]);
    const m = manager([await oldBuild("rex")]);
    await m.mods.host({ buildId: "rex", public: true });
    expect(m.launches[0]).not.toHaveProperty("codes");
    expect(m.builds()[0].codes).toEqual({ on: [], yours: [] });
    await m.closeAll();
  });

  it("runs none for a mod booted by its launcher (its own Gecko code handler is where Dolphin's goes)", async () => {
    const launcherPath = path.join(tmp, "minus.dol");
    await writeFile(launcherPath, "launcher");
    const on = [{ name: "Old", kind: "gecko" as const, lines: ["04000000 00000001"], source: "yours" as const }];
    const m = manager([await oldBuild("minus", { launcherPath, codes: { on, yours: [] } })]);
    await m.mods.host({ buildId: "minus", public: true });
    expect(m.launches[0]).not.toHaveProperty("codes");
    await m.closeAll();
    await expect(m.mods.setCodes("minus", { on, yours: [] })).rejects.toThrow(/its own codes/);
  });

  it("lists the player's Dolphin's, Dolphin's own and the database's", async () => {
    await gameIni(own(), "GP6E01", ["[Gecko]", "$Mine", "04000004 00000001"]);
    const database = [
      "GP6E01",
      "Mario Party 6",
      "",
      "Moon Jump [someone]",
      "28000000 00000000",
      "Hold A",
      "",
      "Bad [x]",
      "2834XXXX YYYYZZZZ",
      "",
    ].join("\r\n");
    const m = manager([await marioParty()], undefined, null, async (id) => (id === "GP6E01" ? database : null));
    const { codes, lists } = await m.mods.codeLists("mp6");
    expect(codes).toEqual({ on: [], yours: [] });
    expect(lists.own.map((c) => c.name)).toEqual(["Mine"]);
    expect(lists.ownFiles).toEqual([path.join(own(), "GameSettings", "GP6E01.ini")]);
    // Dolphin's list for Mario Party 6 (dolphin_codes.json).
    expect(lists.dolphin.length).toBeGreaterThan(50);
    expect(lists.dolphin.find((c) => c.name === "QOL - Auto Advance Text Boxes")?.notes).toContain("without pushing A");
    expect(await m.mods.databaseCodes("mp6")).toEqual([
      { name: "Moon Jump", kind: "gecko", lines: ["28000000 00000000"], author: "someone", notes: "Hold A" },
      { name: "Bad", kind: "gecko", lines: ["2834XXXX YYYYZZZZ"], author: "x" },
    ]);
  });

  it("keeps the codes checked, and not while the game's room is open", async () => {
    const m = manager([await marioParty({ codes: { on: [], yours: [] } })]);
    const mine = { name: "Turn count 30 \u0007", kind: "gecko" as const, lines: ["0426b6a0  0000001e"] };
    const kept = await m.mods.setCodes("mp6", {
      yours: [mine],
      on: [
        { ...mine, source: "yours" },
        // Lines to fill in: not run.
        { name: "Bad", kind: "gecko", lines: ["2834XXXX YYYYZZZZ"], source: "database" },
      ],
    });
    const clean = { name: "Turn count 30", kind: "gecko", lines: ["0426B6A0 0000001E"] };
    expect(kept).toEqual({ yours: [clean], on: [{ ...clean, source: "yours" }] });
    expect(m.builds()[0].codes).toEqual(kept);
    await m.mods.host({ buildId: "mp6", public: true });
    expect(hostArg(m.launches[0]).codes).toEqual([clean]);
    await expect(m.mods.setCodes("mp6", { on: [], yours: [] })).rejects.toThrow(/Leave the room/);
    await m.closeAll();
    await m.mods.setCodes("mp6", { on: [], yours: [] });
  });
});
