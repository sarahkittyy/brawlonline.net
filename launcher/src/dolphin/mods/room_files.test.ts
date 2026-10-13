import { mkdtemp, readdir, readFile, rm, writeFile } from "fs/promises";
import os from "os";
import path from "path";
import { afterEach, beforeEach, describe, expect, it } from "vitest";

import { normalizeRoomCode } from "./mods_manager";
import {
  clearModRoomFiles,
  isFreshStatus,
  MODS_REQUEST_FILE,
  MODS_STATUS_FILE,
  readModRoomStatus,
  writeModRoomRequest,
} from "./room_files";

describe("the room hand-off files", () => {
  let dir: string;
  beforeEach(async () => {
    dir = await mkdtemp(path.join(os.tmpdir(), "mods-files-"));
  });
  afterEach(async () => {
    await rm(dir, { recursive: true, force: true });
  });

  it("writes a version 1 request with an id, and no temporary file is left", async () => {
    const id = await writeModRoomRequest(dir, { op: "kick", slot: 3 });
    const body = JSON.parse(await readFile(path.join(dir, MODS_REQUEST_FILE), "utf8"));
    expect(body).toMatchObject({ version: 1, id, op: "kick", slot: 3 });
    expect(typeof body.createdAt).toBe("string");
    expect(await readdir(dir)).toEqual([MODS_REQUEST_FILE]);
  });

  it("reads Dolphin's status, and nothing for a missing, broken or other-version file", async () => {
    expect(await readModRoomStatus(dir)).toBeNull();
    await writeFile(path.join(dir, MODS_STATUS_FILE), "{not json");
    expect(await readModRoomStatus(dir)).toBeNull();
    await writeFile(path.join(dir, MODS_STATUS_FILE), JSON.stringify({ version: 2 }));
    expect(await readModRoomStatus(dir)).toBeNull();
    const status = {
      version: 1,
      kind: "host",
      state: "in",
      error: "",
      room: { code: "KFQB", public: true, build: { hash: "ab", name: "x" }, host: 1, you: 1, slots: [] },
      session: { phase: "running", players: 1 },
      lastRequest: null,
      pid: 7,
      updatedAt: new Date().toISOString(),
    };
    await writeFile(path.join(dir, MODS_STATUS_FILE), JSON.stringify(status));
    expect(await readModRoomStatus(dir)).toEqual(status);
  });

  it("calls a status fresh for 15 s", () => {
    const now = Date.parse("2026-10-12T20:00:20Z");
    const at = (iso: string) => ({ updatedAt: iso } as Parameters<typeof isFreshStatus>[0]);
    expect(isFreshStatus(at("2026-10-12T20:00:10Z"), now)).toBe(true);
    expect(isFreshStatus(at("2026-10-12T20:00:00Z"), now)).toBe(false);
    expect(isFreshStatus(at("not a date"), now)).toBe(false);
  });

  it("clears both files", async () => {
    await writeFile(path.join(dir, MODS_STATUS_FILE), "{}");
    await writeFile(path.join(dir, MODS_REQUEST_FILE), "{}");
    await clearModRoomFiles(dir);
    expect(await readdir(dir)).toEqual([]);
    await clearModRoomFiles(dir);
  });
});

describe("normalizeRoomCode", () => {
  it("takes letters and digits in any case, and nothing else", () => {
    expect(normalizeRoomCode(" kfqb ")).toBe("KFQB");
    expect(normalizeRoomCode("K")).toBeNull();
    expect(normalizeRoomCode("KF QB")).toBeNull();
    expect(normalizeRoomCode("ABCD#1")).toBeNull();
  });
});
