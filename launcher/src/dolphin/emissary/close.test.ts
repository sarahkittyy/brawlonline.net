import fs from "fs";
import os from "os";
import path from "path";
import { afterEach, beforeEach, describe, expect, it } from "vitest";

import { GAME_STATUS_FILE } from "../room_handoff";
import { closeEmissary, emissaryIdle } from "./close";

describe("emissaryIdle", () => {
  let dir: string;
  const status = async (s: object) =>
    fs.promises.writeFile(
      path.join(dir, GAME_STATUS_FILE),
      JSON.stringify({ version: 1, updatedAt: new Date().toISOString(), pid: 42, ...s }),
    );

  beforeEach(async () => {
    dir = await fs.promises.mkdtemp(path.join(os.tmpdir(), "emissary-close-"));
  });

  afterEach(async () => {
    await fs.promises.rm(dir, { recursive: true, force: true });
  });

  it("is idle in the menus, an online character select or a room between games", async () => {
    for (const screen of ["menus", "online-css", "room", "other"]) {
      await status({ state: "idle", screen });
      expect(await emissaryIdle(dir, 42)).toEqual({ idle: true });
    }
  });

  it("is busy in a match, a search, a set's session or single-player", async () => {
    for (const screen of ["match", "searching", "online-css", "single-player"]) {
      await status({ state: "busy", screen });
      expect(await emissaryIdle(dir, 42)).toEqual({ idle: false, why: `its game is busy (${screen})` });
    }
  });

  it("is not idle without a fresh status of its own", async () => {
    expect((await emissaryIdle(dir, 42)).idle).toBe(false);
    await status({ state: "idle", screen: "menus", pid: 7 });
    expect(await emissaryIdle(dir, 42)).toMatchObject({ idle: false, why: expect.stringMatching(/pid 7/) });
    await status({ state: "idle", screen: "menus", updatedAt: new Date(Date.now() - 60_000).toISOString() });
    expect(await emissaryIdle(dir, 42)).toMatchObject({ idle: false, why: expect.stringMatching(/stale/) });
  });
});

describe("closeEmissary", () => {
  let t = 0;
  const opts = { sleep: async (ms: number) => void (t += ms), now: () => t, platform: "win32" as const };

  beforeEach(() => {
    t = 0;
  });

  it("asks it to close and waits for it", async () => {
    let open = true;
    const asked: number[] = [];
    let killed = false;
    const closed = await closeEmissary(
      9,
      () => open,
      () => (killed = true),
      {
        ...opts,
        ask: async (pid) => {
          asked.push(pid);
          setTimeout(() => (open = false), 0);
        },
        sleep: async () => new Promise((resolve) => setTimeout(resolve, 1)),
      },
    );
    expect(closed).toBe(true);
    expect(asked).toEqual([9]);
    expect(killed).toBe(false);
  });

  it("ends it when it doesn't close in time", async () => {
    let open = true;
    let killed = false;
    const closed = await closeEmissary(
      9,
      () => open,
      () => {
        killed = true;
        open = false;
      },
      { ...opts, ask: async () => undefined, timeoutMs: 1000 },
    );
    expect(closed).toBe(true);
    expect(killed).toBe(true);
    expect(t).toBeGreaterThanOrEqual(1000);
  });

  it("does nothing when it is not open", async () => {
    expect(
      await closeEmissary(
        9,
        () => false,
        () => undefined,
        opts,
      ),
    ).toBe(true);
  });
});
