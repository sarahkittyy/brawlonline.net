import { mkdtemp, rm, writeFile } from "node:fs/promises";
import os from "os";
import path from "path";
import { describe, expect, it } from "vitest";

import { isReplayFileName, readReplayFileInfo } from "./replay_format";

describe("replay format", () => {
  it("lists our .rep and .json replays, not Slippi .slp files", () => {
    expect(isReplayFileName("Game_20261006T120000.rep")).toBe(true);
    expect(isReplayFileName("debug.JSON")).toBe(true);
    expect(isReplayFileName("Game_20231030T2041.slp")).toBe(false);
  });

  it("reads name, size and date only", async () => {
    const dir = await mkdtemp(path.join(os.tmpdir(), "ppo-rep-"));
    try {
      await writeFile(path.join(dir, "a.rep"), Buffer.alloc(42));
      const info = await readReplayFileInfo(dir, "a.rep");
      expect(info.filename).toBe("a.rep");
      expect(info.sizeBytes).toBe(42);
      expect(info.birthTime).toBeTruthy();
      expect(info.settings).toBeUndefined();
    } finally {
      await rm(dir, { recursive: true, force: true });
    }
  });
});
