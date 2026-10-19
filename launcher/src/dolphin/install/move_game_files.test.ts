import fs from "fs";
import os from "os";
import path from "path";
import { afterAll, beforeAll, beforeEach, describe, expect, it } from "vitest";

import {
  chooseGameFilesTarget,
  GameFilesMoveError,
  isInInstallFolder,
  moveGameFiles,
  removeGameFiles,
} from "./move_game_files";

let root: string;
let n = 0;
let dir: string;

beforeAll(async () => {
  root = await fs.promises.mkdtemp(path.join(os.tmpdir(), "move-game-files-"));
});

afterAll(async () => {
  await fs.promises.rm(root, { recursive: true, force: true });
});

beforeEach(async () => {
  n += 1;
  dir = path.join(root, `case-${n}`);
  await fs.promises.mkdir(dir, { recursive: true });
});

async function write(p: string, content: string | Buffer) {
  await fs.promises.mkdir(path.dirname(p), { recursive: true });
  await fs.promises.writeFile(p, content);
}

const read = (p: string) => fs.promises.readFile(p, "utf8");
const exists = (p: string) =>
  fs.promises.access(p).then(
    () => true,
    () => false,
  );

/** A netplay folder as the launcher leaves it: the Dolphin build, the User folder, P+'s files, the patched card. */
async function makeNetplayFolder(netplay: string) {
  await write(path.join(netplay, "Dolphin.exe"), "exe");
  await write(path.join(netplay, "Sys", "GameSettings", "x.ini"), "sys");
  await write(
    path.join(netplay, "dolphin.json"),
    JSON.stringify({ version: "1", executable: "Dolphin.exe", entries: ["Dolphin.exe", "Sys"], files: 2, size: 6 }),
  );
  await write(path.join(netplay, "User", "Wii", "sd.raw"), Buffer.alloc(4096, 7));
  await write(path.join(netplay, "User", "Config", "Dolphin.ini"), "[General]\n");
  await write(path.join(netplay, "pplus", "pplus.json"), "{}");
  await write(path.join(netplay, "pponline-sd", "sd.raw"), Buffer.alloc(4096, 8));
  const card = await fs.promises.stat(path.join(netplay, "User", "Wii", "sd.raw"));
  await write(
    path.join(netplay, "pponline-sd", "manifest.json"),
    JSON.stringify({
      version: 1,
      source: { path: path.resolve(netplay, "User", "Wii", "sd.raw"), size: card.size, mtimeMs: card.mtimeMs },
      plugin: { sha256: "ab", size: 1, sdPath: "/Project+/pf/plugins/PPOnline.rel", from: "x" },
      updatedAt: "2026-10-12T00:00:00.000Z",
    }),
  );
}

const manifestSource = async (netplay: string) =>
  JSON.parse(await read(path.join(netplay, "pponline-sd", "manifest.json"))).source.path as string;

describe("chooseGameFilesTarget", () => {
  it("moves into an empty or missing folder", async () => {
    const current = path.join(dir, "netplay");
    await fs.promises.mkdir(path.join(dir, "empty"));
    expect(await chooseGameFilesTarget(path.join(dir, "empty"), current, "Brawl Online")).toEqual({
      folder: path.join(dir, "empty"),
      kind: "move",
    });
    expect(await chooseGameFilesTarget(path.join(dir, "new"), current, "Brawl Online")).toEqual({
      folder: path.join(dir, "new"),
      kind: "move",
    });
  });

  it("uses a copy of the game files the player made", async () => {
    // Player feedback 2026-10-12: the netplay folder copied to another drive by hand.
    await makeNetplayFolder(path.join(dir, "D", "netplay"));
    expect(
      await chooseGameFilesTarget(path.join(dir, "D", "netplay"), path.join(dir, "netplay"), "Brawl Online"),
    ).toEqual({ folder: path.join(dir, "D", "netplay"), kind: "adopt" });
  });

  it("makes a folder of its own inside a folder that has other things", async () => {
    await write(path.join(dir, "Games", "other.txt"), "x");
    expect(await chooseGameFilesTarget(path.join(dir, "Games"), path.join(dir, "netplay"), "Brawl Online")).toEqual({
      folder: path.join(dir, "Games", "Brawl Online"),
      kind: "move",
    });
    await write(path.join(dir, "Games", "Brawl Online", "other.txt"), "x");
    await expect(
      chooseGameFilesTarget(path.join(dir, "Games"), path.join(dir, "netplay"), "Brawl Online"),
    ).rejects.toThrow(GameFilesMoveError);
  });

  it("refuses the folder in use and folders inside it or around it", async () => {
    const current = path.join(dir, "data", "netplay");
    await makeNetplayFolder(current);
    await expect(chooseGameFilesTarget(current, current, "Brawl Online")).rejects.toThrow("already in");
    await expect(chooseGameFilesTarget(path.join(current, "sub"), current, "Brawl Online")).rejects.toThrow("outside");
    await expect(chooseGameFilesTarget(path.join(dir, "data"), current, "Brawl Online")).rejects.toThrow("outside");
  });
});

describe("the launcher's install folder", () => {
  // A player report 2026-10-19: an update deleted the custom textures, Dolphin settings, hotkeys
  // and controller settings. Every update deletes the install folder (NSIS: RMDir /r $INSTDIR).
  it("is never a target, nor a folder inside it", async () => {
    const install = path.join(dir, "Brawl Online");
    await write(path.join(install, "Brawl Online.exe"), "x");
    const current = path.join(dir, "netplay");
    await expect(chooseGameFilesTarget(install, current, "Brawl Online", install)).rejects.toThrow("install folder");
    await expect(chooseGameFilesTarget(path.join(install, "game"), current, "Brawl Online", install)).rejects.toThrow(
      "install folder",
    );
    await expect(
      chooseGameFilesTarget(path.join(install, "resources"), current, "Brawl Online", install),
    ).rejects.toThrow(GameFilesMoveError);
  });

  it("a folder next to it or around it is fine", async () => {
    const install = path.join(dir, "Games", "brawl-online");
    await write(path.join(install, "Brawl Online.exe"), "x");
    expect(
      await chooseGameFilesTarget(path.join(dir, "Games"), path.join(dir, "netplay"), "Brawl Online", install),
    ).toEqual({ folder: path.join(dir, "Games", "Brawl Online"), kind: "move" });
    expect(isInInstallFolder(path.join(dir, "Games"), install)).toBe(false);
    expect(isInInstallFolder(path.join(install, "x", "netplay"), install)).toBe(true);
    expect(isInInstallFolder(install, install)).toBe(true);
    expect(isInInstallFolder(install, null)).toBe(false);
  });
});

describe("moveGameFiles", () => {
  it("renames the data on one drive and points the patched card at the moved one", async () => {
    const from = path.join(dir, "netplay");
    const to = path.join(dir, "D", "Brawl Online");
    await makeNetplayFolder(from);
    const result = await moveGameFiles({ from, to });
    expect(result.renamed.sort()).toEqual(["User", "pplus", "pponline-sd"]);
    expect(result.copied).toEqual([]);
    expect(await read(path.join(to, "User", "Config", "Dolphin.ini"))).toBe("[General]\n");
    expect(await exists(path.join(to, "pplus", "pplus.json"))).toBe(true);
    expect(await manifestSource(to)).toBe(path.resolve(to, "User", "Wii", "sd.raw"));
    // The Dolphin build stays for removeGameFiles; the bundled one is installed into `to`.
    expect(await exists(path.join(from, "Dolphin.exe"))).toBe(true);
    expect(await exists(path.join(to, "Dolphin.exe"))).toBe(false);
    expect(await exists(path.join(from, "User"))).toBe(false);
  });

  it("copies between drives with progress, keeping the sources until the old folder is removed", async () => {
    const from = path.join(dir, "netplay");
    const to = path.join(dir, "D", "Brawl Online");
    await makeNetplayFolder(from);
    const exdev = async () => {
      throw Object.assign(new Error("cross-device link not permitted"), { code: "EXDEV" });
    };
    const seen: [number, number][] = [];
    const result = await moveGameFiles({ from, to, rename: exdev, onProgress: (d, t) => seen.push([d, t]) });
    expect(result.copied.sort()).toEqual(["User", "pplus", "pponline-sd"]);
    const total =
      4096 +
      "[General]\n".length +
      2 +
      4096 +
      (await fs.promises.stat(path.join(from, "pponline-sd", "manifest.json"))).size;
    expect(seen[0]).toEqual([0, total]);
    expect(seen[seen.length - 1]).toEqual([total, total]);
    for (let i = 1; i < seen.length; i++) {
      expect(seen[i][0]).toBeGreaterThanOrEqual(seen[i - 1][0]);
    }
    expect(
      Buffer.compare(await fs.promises.readFile(path.join(to, "User", "Wii", "sd.raw")), Buffer.alloc(4096, 7)),
    ).toBe(0);
    expect(await manifestSource(to)).toBe(path.resolve(to, "User", "Wii", "sd.raw"));
    expect(await exists(path.join(from, "User", "Wii", "sd.raw"))).toBe(true);

    await removeGameFiles(from);
    expect(await exists(from)).toBe(false);
  });

  it("puts everything back when a move fails half-way", async () => {
    const from = path.join(dir, "netplay");
    const to = path.join(dir, "D", "Brawl Online");
    await makeNetplayFolder(from);
    let calls = 0;
    const flaky = async (a: string, b: string) => {
      calls += 1;
      if (calls === 2) {
        throw Object.assign(new Error("in use"), { code: "EBUSY" });
      }
      await fs.promises.rename(a, b);
    };
    await expect(moveGameFiles({ from, to, rename: flaky })).rejects.toThrow("in use");
    for (const name of ["User", "pplus", "pponline-sd"]) {
      expect(await exists(path.join(from, name))).toBe(true);
      expect(await exists(path.join(to, name))).toBe(false);
    }
    expect(await manifestSource(from)).toBe(path.resolve(from, "User", "Wii", "sd.raw"));
  });
});

describe("removeGameFiles", () => {
  it("removes only the launcher's files and keeps a folder the player also uses", async () => {
    const games = path.join(dir, "Games");
    await makeNetplayFolder(games);
    await write(path.join(games, "mine.txt"), "player's");
    await removeGameFiles(games);
    expect(await fs.promises.readdir(games)).toEqual(["mine.txt"]);
  });
});
