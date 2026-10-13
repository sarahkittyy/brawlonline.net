import type { Expression, ExpressionBuilder, Kysely, SqlBool } from "kysely";
import { sql } from "kysely";
import path from "node:path";

import type { Database, FileRecord, NewFile } from "../schema";

type DB = Kysely<Database>;

/**
 * `file.folder` is `folder` or a folder under it (a replay folder's month folders): an exact,
 * case-sensitive prefix match on the path with its separator.
 */
export const inFolderTree =
  (folder: string) =>
  (eb: ExpressionBuilder<Database, "file">): Expression<SqlBool> => {
    const prefix = folder.endsWith(path.sep) ? folder : folder + path.sep;
    return eb.or([
      eb("file.folder", "=", folder),
      eb(sql<number>`instr(${sql.ref("file.folder")}, ${prefix})`, "=", 1),
    ]);
  };

export class FileRepository {
  static async insertFile(db: DB, file: NewFile) {
    return db.insertInto("file").values(file).returningAll().executeTakeFirstOrThrow();
  }

  static async findAllFilesInFolder(db: DB, folder: string): Promise<Pick<FileRecord, "_id" | "name">[]> {
    const query = db.selectFrom("file").where("folder", "=", folder);
    const records = await query.select(["_id", "name"]).execute();
    return records;
  }

  /** The folders under `folder` (not itself) that have files in the database. */
  static async findFoldersUnder(db: DB, folder: string): Promise<string[]> {
    const records = await db
      .selectFrom("file")
      .where(inFolderTree(folder))
      .where("folder", "!=", folder)
      .select("folder")
      .distinct()
      .execute();
    return records.map((r) => r.folder);
  }

  static async deleteFileById(db: DB, ...ids: number[]) {
    return await db.deleteFrom("file").where("_id", "in", ids).execute();
  }

  static async findTotalSizeByFolder(db: DB, folder: string): Promise<number> {
    const query = db
      .selectFrom("file")
      .where("folder", "=", folder)
      .select((eb) => eb.fn.sum<number>("file.size_bytes").as("total_size"));
    const res = await query.executeTakeFirst();
    return res?.total_size ?? 0;
  }
}
