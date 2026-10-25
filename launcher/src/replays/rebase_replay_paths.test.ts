import type { Database } from "@database/schema";
import { initTestDb } from "@database/tests/init_test_db";
import { aMockFileWith } from "@database/tests/mocks";
import type { Kysely } from "kysely";
import path from "path";
import { afterEach, beforeEach, describe, expect, it } from "vitest";

import { rebaseReplayPaths } from "./rebase_replay_paths";

describe("rebaseReplayPaths", () => {
  let db: Kysely<Database>;

  beforeEach(async () => {
    db = await initTestDb();
  });

  afterEach(async () => {
    await db.destroy();
  });

  it("gives the moved folder's files and highlights their new paths, nothing else", async () => {
    const from = path.resolve("/Documents/Brawl Online 🎮");
    const to = path.resolve("/Games/Brawl Online/Replays");
    const other = `${from} (2)`;
    for (const folder of [from, path.join(from, "2026-10"), other]) {
      await db
        .insertInto("file")
        .values(aMockFileWith({ folder, name: "a.brep" }))
        .execute();
      await db
        .insertInto("highlight_cache")
        .values({
          path: path.join(folder, "a.brep"),
          size_bytes: 1,
          mtime_ms: 2,
          detector_version: 3,
          mode: "m",
          result: "{}",
        })
        .execute();
    }
    // The replay browser read the moved files in their new folder before this ran.
    await db
      .insertInto("file")
      .values(aMockFileWith({ folder: to, name: "a.brep" }))
      .execute();
    await db
      .insertInto("highlight_cache")
      .values({
        path: path.join(to, "a.brep"),
        size_bytes: 1,
        mtime_ms: 2,
        detector_version: 3,
        mode: "m",
        result: "{}",
      })
      .execute();

    await rebaseReplayPaths(db, { from, to });

    const folders = (await db.selectFrom("file").select("folder").orderBy("_id").execute()).map((r) => r.folder);
    expect(folders).toEqual([to, path.join(to, "2026-10"), other]);
    const cached = (await db.selectFrom("highlight_cache").select("path").execute()).map((r) => r.path).sort();
    expect(cached).toEqual(
      [path.join(to, "a.brep"), path.join(to, "2026-10", "a.brep"), path.join(other, "a.brep")].sort(),
    );
  });
});
