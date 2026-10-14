import type { Database } from "@database/schema";
import { initTestDb } from "@database/tests/init_test_db";
import fs from "fs/promises";
import type { Kysely } from "kysely";
import os from "os";
import path from "path";
import { afterEach, beforeEach, describe, expect, it } from "vitest";

import { DatabaseReplayProvider } from "../database_replay_provider";

describe("loadFile", () => {
  let db: Kysely<Database>;
  let provider: DatabaseReplayProvider;
  let folder: string;

  beforeEach(async () => {
    db = await initTestDb();
    provider = new DatabaseReplayProvider(db);
    folder = await fs.mkdtemp(path.join(os.tmpdir(), "load-file-"));
  });

  afterEach(async () => {
    await db.destroy();
    await fs.rm(folder, { recursive: true, force: true });
  });

  // A replay opened from the desktop: the stats page's two calls load the file at once.
  it("loads a file not indexed yet from two calls at once", async () => {
    const filePath = path.join(folder, "2026-10-12 2314 Ranked G1 - A (Zelda) vs B (Lucas).brep");
    await fs.writeFile(filePath, Buffer.alloc(64));

    const [first, second] = await Promise.all([provider.loadFile(filePath), provider.loadFile(filePath)]);
    expect(first.fileName).toEqual(path.basename(filePath));
    expect(second.id).toEqual(first.id);

    // Indexed now: a later call reads it from the database.
    const third = await provider.loadFile(filePath);
    expect(third.id).toEqual(first.id);
  });
});
