import { mkdtemp, readFile, rm, writeFile } from "fs/promises";
import os from "os";
import path from "path";
import { afterEach, beforeEach, describe, expect, it } from "vitest";

import type { GameStatus, RoomRequest } from "./room_handoff";
import {
  clearRoomRequest,
  handOffRoom,
  isRoomCode,
  readGameStatus,
  requestPath,
  STATUS_FRESH_MS,
  statusPath,
  writeRoomRequest,
} from "./room_handoff";

describe("room hand-off", () => {
  let dir: string;
  beforeEach(async () => {
    dir = await mkdtemp(path.join(os.tmpdir(), "room-handoff-"));
  });
  afterEach(async () => {
    await rm(dir, { recursive: true, force: true });
  });

  const writeStatus = (s: Partial<GameStatus>) =>
    writeFile(
      statusPath(dir),
      JSON.stringify({ version: 1, state: "idle", updatedAt: new Date().toISOString(), ...s }),
    );
  const readRequest = async (): Promise<RoomRequest | null> => {
    try {
      return JSON.parse(await readFile(requestPath(dir), "utf8"));
    } catch {
      return null;
    }
  };
  const fast = { ackTimeoutMs: 300, pollMs: 20 };

  it("knows room codes", () => {
    expect(isRoomCode("KFQB")).toBe(true);
    for (const bad of ["kfqb", "KFQ", "KAQB", "KYQB", "KFQBB", ""]) {
      expect(isRoomCode(bad)).toBe(false);
    }
  });

  it("writes and clears the request", async () => {
    const req = await writeRoomRequest(dir, "KFQB", new Date("2026-10-09T12:00:00Z"));
    expect(await readRequest()).toEqual({
      version: 1,
      id: req.id,
      code: "KFQB",
      createdAt: "2026-10-09T12:00:00.000Z",
    });
    expect(await clearRoomRequest(dir, "someone-else")).toBe(false);
    expect(await readRequest()).not.toBeNull();
    expect(await clearRoomRequest(dir, req.id)).toBe(true);
    expect(await readRequest()).toBeNull();
    await expect(writeRoomRequest(dir, "nope")).rejects.toThrow();
  });

  it("reads the status: missing, stale or fresh", async () => {
    expect(await readGameStatus(dir)).toEqual({ kind: "missing" });
    await writeFile(statusPath(dir), "{not json");
    expect(await readGameStatus(dir)).toEqual({ kind: "missing" });
    await writeStatus({ state: "busy" });
    expect((await readGameStatus(dir)).kind).toBe("fresh");
    expect((await readGameStatus(dir, Date.now() + STATUS_FRESH_MS + 1000)).kind).toBe("stale");
  });

  it("refuses at once when the game is busy, without writing anything", async () => {
    await writeStatus({ state: "busy", screen: "match" });
    expect(await handOffRoom(dir, "KFQB", fast)).toEqual({ outcome: "busy" });
    expect(await readRequest()).toBeNull();
  });

  it("returns the game's answer to the request", async () => {
    await writeStatus({ state: "idle", screen: "menus" });
    // A stand-in game: answers the request the moment it appears.
    const game = (result: "accepted" | "refused", state: "idle" | "busy" = "idle") =>
      (async () => {
        for (let i = 0; i < 50; i++) {
          const req = await readRequest();
          if (req) {
            await clearRoomRequest(dir);
            await writeStatus({ state, lastRequest: { id: req.id, result, message: "Can't go there now." } });
            return;
          }
          await new Promise((resolve) => setTimeout(resolve, 10));
        }
      })();
    let [res] = await Promise.all([handOffRoom(dir, "KFQB", fast), game("accepted")]);
    expect(res).toEqual({ outcome: "accepted" });
    [res] = await Promise.all([handOffRoom(dir, "KFQB", fast), game("refused", "busy")]);
    expect(res).toEqual({ outcome: "busy" });
    // The game is idle again (its last answer said busy, which alone would refuse at once).
    await writeStatus({ state: "idle" });
    [res] = await Promise.all([handOffRoom(dir, "KFQB", fast), game("refused")]);
    expect(res).toEqual({ outcome: "refused", message: "Can't go there now." });
  });

  it("takes the request back when no game answers (an older Dolphin)", async () => {
    expect(await handOffRoom(dir, "KFQB", fast)).toEqual({ outcome: "no-response" });
    expect(await readRequest()).toBeNull();
    // A stale busy status (a game that crashed) does not block: it is treated as unknown.
    await writeStatus({ state: "busy", updatedAt: new Date(Date.now() - 60_000).toISOString() });
    expect(await handOffRoom(dir, "KFQB", fast)).toEqual({ outcome: "no-response" });
  });
});
