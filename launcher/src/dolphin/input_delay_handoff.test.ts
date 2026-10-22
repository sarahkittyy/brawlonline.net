import { mkdtemp, readdir, readFile, rm } from "fs/promises";
import os from "os";
import path from "path";
import { afterEach, beforeEach, describe, expect, it } from "vitest";

import {
  clearInputDelayRequest,
  INPUT_DELAY_REQUEST_FILE,
  inputDelayRequestPath,
  writeInputDelayRequest,
} from "./input_delay_handoff";

describe("input delay hand-off", () => {
  let dir: string;
  beforeEach(async () => {
    dir = await mkdtemp(path.join(os.tmpdir(), "input-delay-"));
  });
  afterEach(async () => {
    await rm(dir, { recursive: true, force: true });
  });

  it("writes the request the game reads, and a newer one replaces it", async () => {
    const now = new Date("2026-10-20T12:00:00.000Z");
    const first = await writeInputDelayRequest(dir, 4, now);
    const second = await writeInputDelayRequest(dir, 0, now);
    expect(second.id).not.toBe(first.id);
    const onDisk = JSON.parse(await readFile(inputDelayRequestPath(dir), "utf8"));
    expect(onDisk).toEqual({ version: 1, id: second.id, inputDelay: 0, createdAt: "2026-10-20T12:00:00.000Z" });
    // No temporary file is left behind.
    expect(await readdir(dir)).toEqual([INPUT_DELAY_REQUEST_FILE]);
  });

  it("refuses what the game would ignore", async () => {
    for (const bad of [-3, 10, 2.5, Number.NaN]) {
      await expect(writeInputDelayRequest(dir, bad)).rejects.toThrow();
    }
    expect(await readdir(dir)).toEqual([]);
  });

  it("takes the profiles: -1 Aggressive, 0 Normal, -2 Smooth", async () => {
    for (const profile of [-1, 0, -2]) {
      const req = await writeInputDelayRequest(dir, profile);
      expect(JSON.parse(await readFile(inputDelayRequestPath(dir), "utf8")).inputDelay).toBe(profile);
      expect(req.inputDelay).toBe(profile);
    }
  });

  it("clears a request, and nothing when there is none", async () => {
    await writeInputDelayRequest(dir, 2);
    await clearInputDelayRequest(dir);
    await clearInputDelayRequest(dir);
    expect(await readdir(dir)).toEqual([]);
  });
});
