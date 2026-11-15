import type { Database } from "@database/schema";
import type { Kysely } from "kysely";
import { afterEach, beforeEach, describe, expect, it } from "vitest";

import { FileRepository } from "../repositories/file_repository";
import { GameRepository } from "../repositories/game_repository";
import { PlayerRepository } from "../repositories/player_repository";
import { initTestDb } from "./init_test_db";
import { aMockFileWith, aMockGameWith, aMockPlayerWith } from "./mocks";

describe("PlayerRepository.searchPlayers", () => {
  let db: Kysely<Database>;
  let n = 0;

  beforeEach(async () => {
    db = await initTestDb();
  });

  afterEach(async () => {
    await db.destroy();
  });

  /** A game of these players (port 1, 2, ...) started at `start`. */
  const addGame = async (start: string, ...players: { code: string; name: string; character: number }[]) => {
    const file = await FileRepository.insertFile(db, aMockFileWith({ folder: "/replays", name: `g${n++}.slp` }));
    const game = await GameRepository.insertGame(db, aMockGameWith(file._id, { start_time: start }));
    await PlayerRepository.insertPlayer(
      db,
      ...players.map((p, i) =>
        aMockPlayerWith(game._id, {
          port: i + 1,
          connect_code: p.code,
          display_name: p.name,
          tag: "",
          character_id: p.character,
          character_color: 0,
        }),
      ),
    );
  };

  const sara = (character: number, name = "sarah") => ({ code: "SARA#694", name, character });
  const impy = { code: "IMPY#5", name: "IMPOSSIBLE", character: 22 };
  const kof = { code: "TST#67", name: "KoF_comatoast", character: 27 };

  it("suggests the players with the most games first, with their latest name and most played character", async () => {
    await addGame("2026-11-01T10:00:00Z", sara(20, "old name"), impy);
    await addGame("2026-11-02T10:00:00Z", sara(20), impy);
    await addGame("2026-11-03T10:00:00Z", sara(7), kof);

    const all = await PlayerRepository.searchPlayers(db, "", 8);
    expect(all.map((p) => [p.connect_code, Number(p.games)])).toEqual([
      ["SARA#694", 3],
      ["IMPY#5", 2],
      ["TST#67", 1],
    ]);
    expect(all[0].display_name).toBe("sarah");
    expect(all[0].character_id).toBe(20);
  });

  it("matches the code or the name in any case, and a % or _ only as itself", async () => {
    await addGame("2026-11-01T10:00:00Z", sara(20), impy);
    await addGame("2026-11-02T10:00:00Z", sara(20), kof);

    expect((await PlayerRepository.searchPlayers(db, "comA", 8)).map((p) => p.connect_code)).toEqual(["TST#67"]);
    expect((await PlayerRepository.searchPlayers(db, "#5", 8)).map((p) => p.connect_code)).toEqual(["IMPY#5"]);
    expect(await PlayerRepository.searchPlayers(db, "%", 8)).toEqual([]);
    expect((await PlayerRepository.searchPlayers(db, "f_c", 8)).map((p) => p.connect_code)).toEqual(["TST#67"]);
    expect(await PlayerRepository.searchPlayers(db, "k_f", 8)).toEqual([]);
  });

  it("leaves out players without a connect code and keeps to the limit", async () => {
    await addGame("2026-11-01T10:00:00Z", sara(20), { code: "", name: "local", character: 1 }, impy, kof);

    const two = await PlayerRepository.searchPlayers(db, "", 2);
    expect(two).toHaveLength(2);
    expect((await PlayerRepository.searchPlayers(db, "local", 8)).length).toBe(0);
  });
});
