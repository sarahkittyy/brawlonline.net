import type { Kysely } from "kysely";

// Highlights and L+R+Z clips found in a replay (src/replays/highlights), by file and detector version.
export async function up(db: Kysely<any>): Promise<void> {
  await db.schema
    .createTable("highlight_cache")
    .addColumn("path", "text", (col) => col.primaryKey())
    .addColumn("size_bytes", "integer", (col) => col.notNull())
    .addColumn("mtime_ms", "integer", (col) => col.notNull())
    .addColumn("detector_version", "integer", (col) => col.notNull())
    .addColumn("mode", "text", (col) => col.notNull())
    .addColumn("result", "text", (col) => col.notNull())
    .execute();
}

export async function down(db: Kysely<any>): Promise<void> {
  await db.schema.dropTable("highlight_cache").execute();
}
