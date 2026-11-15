import { chunk } from "@common/chunk";
import type { Kysely } from "kysely";
import { sql } from "kysely";

import type { Database, NewPlayer, PlayerRecord } from "../schema";

type DB = Kysely<Database>;

// SQLite has a limit of 999 parameters per query
const SQLITE_PARAM_LIMIT_BATCH_SIZE = 500;

/** A player of the replays (by connect code): the name and character of their most games. */
export type ReplayPlayerRecord = {
  connect_code: string;
  display_name: string | null;
  character_id: number | null;
  character_color: number | null;
  games: number;
};

export class PlayerRepository {
  /**
   * The players with a connect code whose code, name or tag has `query` (any case), the most games first: the
   * Replays and Highlights player search's suggestions.
   */
  static async searchPlayers(db: DB, query: string, limit: number): Promise<ReplayPlayerRecord[]> {
    const like = `%${query.replace(/[\\%_]/g, (c) => `\\${c}`)}%`;
    const result = await sql<ReplayPlayerRecord>`
      WITH m AS MATERIALIZED (
        SELECT p.connect_code AS code, p.display_name AS name, p.character_id AS cid, p.character_color AS color,
               g.start_time AS t
        FROM player p JOIN game g ON g._id = p.game_id
        WHERE p.connect_code IS NOT NULL AND p.connect_code != ''
          AND (p.connect_code LIKE ${like} ESCAPE '\\' OR p.display_name LIKE ${like} ESCAPE '\\'
               OR p.tag LIKE ${like} ESCAPE '\\')
      ),
      c AS (SELECT code, COUNT(*) AS games FROM m GROUP BY code ORDER BY games DESC LIMIT ${limit})
      SELECT c.code AS connect_code, c.games AS games,
        (SELECT name FROM m WHERE m.code = c.code AND name != '' ORDER BY t DESC LIMIT 1) AS display_name,
        c.main AS character_id,
        (SELECT color FROM m WHERE m.code = c.code AND m.cid IS c.main GROUP BY color ORDER BY COUNT(*) DESC LIMIT 1)
          AS character_color
      FROM (
        SELECT c.*, (SELECT cid FROM m WHERE m.code = c.code GROUP BY cid ORDER BY COUNT(*) DESC LIMIT 1) AS main FROM c
      ) c ORDER BY c.games DESC
    `.execute(db);
    return result.rows;
  }

  static async insertPlayer(db: DB, ...player: NewPlayer[]): Promise<PlayerRecord[]> {
    return db.insertInto("player").values(player).returningAll().execute();
  }

  static async findAllPlayersByGame(db: DB, ...gameId: number[]): Promise<Map<number, PlayerRecord[]>> {
    const gameIdToPlayersMap = new Map<number, PlayerRecord[]>();

    // Batch the query to stay under SQLite's 999 parameter limit
    // This ensures the code works regardless of how many game IDs are passed
    const batches = chunk(gameId, SQLITE_PARAM_LIMIT_BATCH_SIZE);

    for (const batch of batches) {
      const query = db.selectFrom("player").where("game_id", "in", batch).orderBy(["player.game_id", "player.port"]);
      const playerRecords = await query.selectAll().execute();
      const totalRecords = playerRecords.length;

      // We care about performance so use a for-loop for speed
      for (let i = 0; i < totalRecords; i++) {
        const player = playerRecords[i];
        const gameId = player.game_id;
        const players = gameIdToPlayersMap.get(gameId) ?? [];
        players.push(player);
        gameIdToPlayersMap.set(gameId, players);
      }
    }

    return gameIdToPlayersMap;
  }
}
