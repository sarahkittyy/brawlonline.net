// A moved replay folder (DolphinManager.moveMainFolder, main_folder.ts) keeps its rows in
// replays.sqlite: the files' folders and the highlight cache's paths get the new path, so nothing
// is read again (the move keeps sizes and modification times, which the highlight cache checks).
//
// SPDX-License-Identifier: GPL-3.0-or-later

import type { Database } from "@database/schema";
import type { FolderMove } from "@dolphin/install/main_folder";
import type { Kysely } from "kysely";
import { sql } from "kysely";
import path from "path";

type PathColumn = "folder" | "path";

const withSep = (p: string) => (p.endsWith(path.sep) ? p : p + path.sep);

// (SQLite's substr counts characters, not UTF-16 units as JS's length does.)
const chars = (s: string) => [...s].length;

/** `column` is a path inside `dir`. */
const inside = (column: PathColumn, dir: string) =>
  sql<boolean>`substr(${sql.ref(column)}, 1, ${chars(withSep(dir))}) = ${withSep(dir)}`;

/** `column` is `dir` or a path inside it. */
const under = (column: PathColumn, dir: string) =>
  sql<boolean>`(${sql.ref(column)} = ${dir} or ${inside(column, dir)})`;

/** Rows of files at or under `move.from` get the same path under `move.to`. */
export async function rebaseReplayPaths(db: Kysely<Database>, move: FolderMove): Promise<void> {
  const from = path.resolve(move.from);
  const to = path.resolve(move.to);
  // `<from>/<rest>` -> `<to>/<rest>`
  const rebased = (column: PathColumn) =>
    sql<string>`${withSep(to)} || substr(${sql.ref(column)}, ${chars(withSep(from)) + 1})`;
  await db.transaction().execute(async (trx) => {
    // Rows already under the new path are the same files read there since they moved: the moved
    // rows replace them.
    await trx.deleteFrom("file").where(under("folder", to)).execute();
    await trx.deleteFrom("highlight_cache").where(under("path", to)).execute();
    await trx.updateTable("file").set({ folder: to }).where("folder", "=", from).execute();
    await trx
      .updateTable("file")
      .set({ folder: rebased("folder") })
      .where(inside("folder", from))
      .execute();
    await trx
      .updateTable("highlight_cache")
      .set({ path: rebased("path") })
      .where(inside("path", from))
      .execute();
  });
}
