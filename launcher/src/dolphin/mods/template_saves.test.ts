import { mkdir, mkdtemp, readdir, rm, writeFile } from "fs/promises";
import os from "os";
import path from "path";
import { afterEach, beforeEach, describe, expect, it } from "vitest";

import { clearTemplateSeededSaves, TEMPLATE_SAVES_CLEARED } from "./template_saves";

const BRAWL = "aaaaaaaaaaaaaaaa";
const NSMB = "bbbbbbbbbbbbbbbb";

async function store(user: string, hash: string, title: string): Promise<void> {
  const data = path.join(user, "Saves", hash, "title", "00010000", title, "data");
  await mkdir(data, { recursive: true });
  await writeFile(path.join(data, "advsv0.bin"), "save");
  await writeFile(path.join(user, "Saves", hash, "fst.bin"), "meta");
}

describe("clearTemplateSeededSaves", () => {
  let user: string;
  beforeEach(async () => {
    user = await mkdtemp(path.join(os.tmpdir(), "template-saves-"));
  });
  afterEach(async () => {
    await rm(user, { recursive: true, force: true });
  });

  it("deletes the stores with a Brawl save, keeps other games' and other files, once", async () => {
    await store(user, BRAWL, "52534245");
    await store(user, NSMB, "534d4e45");
    await mkdir(path.join(user, "Saves", "not-a-build"), { recursive: true });
    expect(await clearTemplateSeededSaves(user)).toEqual([BRAWL]);
    expect((await readdir(path.join(user, "Saves"))).sort()).toEqual(
      [NSMB, TEMPLATE_SAVES_CLEARED, "not-a-build"].sort(),
    );

    // A save the fixed Dolphin made afterwards stays.
    await store(user, BRAWL, "52534245");
    expect(await clearTemplateSeededSaves(user)).toEqual([]);
    expect(await readdir(path.join(user, "Saves", BRAWL))).toContain("title");
  });

  it("with no saves yet, marks the cleanup done so later saves are kept", async () => {
    expect(await clearTemplateSeededSaves(user)).toEqual([]);
    expect(await readdir(path.join(user, "Saves"))).toEqual([TEMPLATE_SAVES_CLEARED]);
    await store(user, BRAWL, "52534245");
    expect(await clearTemplateSeededSaves(user)).toEqual([]);
  });
});
