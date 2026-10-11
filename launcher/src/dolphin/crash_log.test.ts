import fs from "fs";
import os from "os";
import path from "path";
import { afterEach, beforeEach, describe, expect, it } from "vitest";

import { CRASH_LOGS_KEPT, isAbnormalExit, keepCrashLog } from "./crash_log";

describe("crash log", () => {
  let dir: string;

  beforeEach(async () => {
    dir = await fs.promises.mkdtemp(path.join(os.tmpdir(), "crash-log-"));
  });

  afterEach(async () => {
    await fs.promises.rm(dir, { recursive: true, force: true });
  });

  it("treats a non-zero code or a signal as abnormal", () => {
    expect(isAbnormalExit(0, null)).toBe(false);
    expect(isAbnormalExit(1, null)).toBe(true);
    expect(isAbnormalExit(0xcfffffff, null)).toBe(true);
    expect(isAbnormalExit(null, "SIGSEGV")).toBe(true);
  });

  it("copies dolphin.log and keeps only the newest crash logs", async () => {
    const written: string[] = [];
    for (let i = 0; i < CRASH_LOGS_KEPT + 2; i++) {
      await fs.promises.writeFile(path.join(dir, "dolphin.log"), `run ${i}\n`);
      const dest = await keepCrashLog(dir, new Date(2026, 9, 11, 14, 10 + i, 5));
      expect(dest).not.toBeNull();
      written.push(path.basename(dest!));
    }
    expect(written[0]).toBe("crash-2026-10-11-141005.log");

    const left = (await fs.promises.readdir(dir)).filter((n) => n.startsWith("crash-")).sort();
    expect(left).toEqual(written.slice(-CRASH_LOGS_KEPT));
    expect(await fs.promises.readFile(path.join(dir, left[left.length - 1]), "utf-8")).toBe(
      `run ${CRASH_LOGS_KEPT + 1}\n`,
    );
    // The run's own log stays for Dolphin to rotate.
    expect(await fs.promises.readFile(path.join(dir, "dolphin.log"), "utf-8")).toBe(`run ${CRASH_LOGS_KEPT + 1}\n`);
  });

  it("does nothing without a log", async () => {
    expect(await keepCrashLog(path.join(dir, "missing"))).toBeNull();
    expect(await keepCrashLog(dir)).toBeNull();
    expect(await fs.promises.readdir(dir)).toEqual([]);
  });
});
