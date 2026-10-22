import { mkdir, mkdtemp, rm, writeFile } from "fs/promises";
import os from "os";
import path from "path";
import { afterEach, beforeEach, describe, expect, it } from "vitest";

import { ownReplayFolder, withOwnReplayFolder } from "./own_replay_folder";

describe("withOwnReplayFolder", () => {
  let tmp: string;
  let user: string;
  const root = path.join("C:", "Users", "player", "Documents", "Brawl Online");

  beforeEach(async () => {
    tmp = await mkdtemp(path.join(os.tmpdir(), "own-replays-"));
    user = path.join(tmp, "User");
  });

  afterEach(async () => {
    await rm(tmp, { recursive: true, force: true });
  });

  it("lists the folder once Dolphin saved a replay in a monthly folder there", async () => {
    const month = path.join(ownReplayFolder(user), "2026-10");
    await mkdir(month, { recursive: true });
    await writeFile(path.join(month, "2026-10-21 2102 Ranked G1 - a (Mario) vs b (Marth).brep"), "");
    expect(await withOwnReplayFolder(user, root, ["D:\\more"])).toEqual(["D:\\more", ownReplayFolder(user)]);
  });

  it("lists a replay saved straight in the folder", async () => {
    await mkdir(ownReplayFolder(user), { recursive: true });
    await writeFile(path.join(ownReplayFolder(user), "game.brep"), "");
    expect(await withOwnReplayFolder(user, root, [])).toEqual([ownReplayFolder(user)]);
  });

  it("changes nothing without replays there", async () => {
    expect(await withOwnReplayFolder(user, root, [])).toBeNull();
    await mkdir(path.join(ownReplayFolder(user), "2026-10"), { recursive: true });
    await writeFile(path.join(ownReplayFolder(user), "2026-10", "notes.txt"), "");
    expect(await withOwnReplayFolder(user, root, [])).toBeNull();
  });

  it("changes nothing when the folder is already listed, or is the replay folder or inside it", async () => {
    await mkdir(ownReplayFolder(user), { recursive: true });
    await writeFile(path.join(ownReplayFolder(user), "game.brep"), "");
    expect(await withOwnReplayFolder(user, root, [ownReplayFolder(user)])).toBeNull();
    expect(await withOwnReplayFolder(user, ownReplayFolder(user), [])).toBeNull();
    expect(await withOwnReplayFolder(user, tmp, [])).toBeNull();
  });
});
