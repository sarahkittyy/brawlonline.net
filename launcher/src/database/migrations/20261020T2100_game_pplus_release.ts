import type { Kysely } from "kysely";

// The Project+ release a replay was recorded on (common/pplus_releases.ts).
export async function up(db: Kysely<any>): Promise<void> {
  await db.schema.alterTable("game").addColumn("pplus_release", "text").execute();
}

export async function down(db: Kysely<any>): Promise<void> {
  await db.schema.alterTable("game").dropColumn("pplus_release").execute();
}
