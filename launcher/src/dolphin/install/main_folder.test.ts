import type { ManualClip } from "@replays/highlights/types";
import fs from "fs";
import os from "os";
import path from "path";
import { afterAll, beforeAll, beforeEach, describe, expect, it } from "vitest";

import { chooseMainFolder, isOwnReplayFolder, MainFolderError, rebasePath, rebaseSettings } from "./main_folder";

let root: string;
let n = 0;
let dir: string;

beforeAll(async () => {
  root = await fs.promises.mkdtemp(path.join(os.tmpdir(), "main-folder-"));
});

afterAll(async () => {
  await fs.promises.rm(root, { recursive: true, force: true });
});

beforeEach(async () => {
  n += 1;
  dir = path.join(root, `case-${n}`);
  await fs.promises.mkdir(dir, { recursive: true });
});

async function write(p: string, content = "x") {
  await fs.promises.mkdir(path.dirname(p), { recursive: true });
  await fs.promises.writeFile(p, content);
}

describe("chooseMainFolder", () => {
  it("is the folder itself when it is missing, empty or already a Brawl Online folder", async () => {
    expect(await chooseMainFolder(path.join(dir, "new"), "Brawl Online")).toBe(path.join(dir, "new"));
    await fs.promises.mkdir(path.join(dir, "empty"));
    expect(await chooseMainFolder(path.join(dir, "empty"), "Brawl Online")).toBe(path.join(dir, "empty"));
    await write(path.join(dir, "bo", "Game Files", "User", "x"));
    await write(path.join(dir, "bo", "Replays", "2026-10", "a.brep"));
    await write(path.join(dir, "bo", "desktop.ini"));
    expect(await chooseMainFolder(path.join(dir, "bo"), "Brawl Online")).toBe(path.join(dir, "bo"));
  });

  it("makes a folder of its own inside a folder that has other things", async () => {
    await write(path.join(dir, "Games", "other.txt"));
    expect(await chooseMainFolder(path.join(dir, "Games"), "Brawl Online")).toBe(
      path.join(dir, "Games", "Brawl Online"),
    );
  });

  it("refuses when that folder has other things too", async () => {
    await write(path.join(dir, "Games", "other.txt"));
    await write(path.join(dir, "Games", "Brawl Online", "mine.txt"));
    await expect(chooseMainFolder(path.join(dir, "Games"), "Brawl Online")).rejects.toThrow(MainFolderError);
  });
});

describe("rebasePath", () => {
  const move = { from: path.resolve("/x/old"), to: path.resolve("/y/new") };

  it("moves the folder and what is inside it, nothing else", () => {
    expect(rebasePath(move.from, move)).toBe(move.to);
    expect(rebasePath(path.join(move.from, "2026-10", "a.brep"), move)).toBe(path.join(move.to, "2026-10", "a.brep"));
    expect(rebasePath(`${move.from}er`, move)).toBeNull();
    expect(rebasePath(path.dirname(move.from), move)).toBeNull();
  });
});

describe("rebaseSettings", () => {
  const from = path.resolve("/data/Documents/Brawl Online");
  const to = path.resolve("/games/Brawl Online/Replays");
  const clip = (p: string): ManualClip => ({
    path: p,
    startFrame: 1,
    endFrame: 2,
    savedAt: "2026-10-25T00:00:00Z",
    date: "2026-10-25T00:00:00Z",
    players: [],
    isTeams: false,
    recorderPort: null,
  });

  it("changes the paths inside the moved folder", () => {
    const inside = path.join(from, "2026-10", "a|b.brep");
    const elsewhere = path.resolve("/elsewhere/c.brep");
    const updates = rebaseSettings(
      {
        extraSlpPaths: [path.join(from, "Spectated"), path.resolve("/slippi")],
        clipsFolder: path.join(from, "My Clips"),
        favoriteHighlights: [`${inside}|combo|120`, `${elsewhere}|lrz|5`],
        manualClips: [clip(inside), clip(elsewhere)],
      },
      [{ from, to }],
    );
    expect(updates).toEqual([
      { key: "extraSlpPaths", value: [path.join(to, "Spectated"), path.resolve("/slippi")] },
      { key: "clipsFolder", value: path.join(to, "My Clips") },
      {
        key: "favoriteHighlights",
        value: [`${path.join(to, "2026-10", "a|b.brep")}|combo|120`, `${elsewhere}|lrz|5`],
      },
      { key: "manualClips", value: [clip(path.join(to, "2026-10", "a|b.brep")), clip(elsewhere)] },
    ]);
  });

  it("changes nothing when no path is inside it", () => {
    expect(
      rebaseSettings(
        { extraSlpPaths: [path.resolve("/slippi")], clipsFolder: null, favoriteHighlights: [], manualClips: [] },
        [{ from, to }],
      ),
    ).toEqual([]);
  });
});

describe("isOwnReplayFolder", () => {
  const def = path.resolve("/data/Documents/Brawl Online");

  it("is the default replay folder or the Brawl Online folder's", () => {
    expect(isOwnReplayFolder(def, def, null)).toBe(true);
    expect(isOwnReplayFolder(path.resolve("/bo/Replays"), def, path.resolve("/bo"))).toBe(true);
  });

  it("is never a replay folder the player chose", () => {
    expect(isOwnReplayFolder(path.resolve("/slippi"), def, null)).toBe(false);
    expect(isOwnReplayFolder(path.resolve("/slippi"), def, path.resolve("/bo"))).toBe(false);
  });
});
