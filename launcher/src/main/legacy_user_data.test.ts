import fs from "fs";
import os from "os";
import path from "path";
import { afterEach, beforeEach, describe, expect, it } from "vitest";

import { legacyUserDataNames, migrateLegacyUserData } from "./legacy_user_data";

describe("migrateLegacyUserData", () => {
  let appData: string;
  const current = () => path.join(appData, "Brawl Online-dev");
  const legacy = () => path.join(appData, "PlusOnline-dev");

  beforeEach(async () => {
    appData = await fs.promises.mkdtemp(path.join(os.tmpdir(), "legacy-user-data-"));
  });

  afterEach(async () => {
    await fs.promises.rm(appData, { recursive: true, force: true });
  });

  it("renames the legacy folder, keeping its contents", async () => {
    await fs.promises.mkdir(path.join(legacy(), "netplay"), { recursive: true });
    await fs.promises.writeFile(path.join(legacy(), "Settings"), "{}");

    expect(migrateLegacyUserData(current(), ["PlusOnline-dev"])).toEqual({
      kind: "moved",
      from: legacy(),
      to: current(),
    });
    expect(fs.existsSync(legacy())).toBe(false);
    expect(await fs.promises.readFile(path.join(current(), "Settings"), "utf8")).toBe("{}");
    expect(fs.existsSync(path.join(current(), "netplay"))).toBe(true);
  });

  it("leaves both alone when the current folder exists", async () => {
    await fs.promises.mkdir(legacy());
    await fs.promises.mkdir(current());

    expect(migrateLegacyUserData(current(), ["PlusOnline-dev"])).toEqual({ kind: "none" });
    expect(fs.existsSync(legacy())).toBe(true);
  });

  it("does nothing without a legacy folder, or when the legacy name is a file", async () => {
    expect(migrateLegacyUserData(current(), ["PlusOnline-dev"])).toEqual({ kind: "none" });
    await fs.promises.writeFile(legacy(), "not a folder");
    expect(migrateLegacyUserData(current(), ["PlusOnline-dev"])).toEqual({ kind: "none" });
    expect(fs.existsSync(current())).toBe(false);
  });

  it("reports a failed rename so the caller can keep using the legacy folder", async () => {
    await fs.promises.mkdir(legacy());
    const failing = {
      existsSync: fs.existsSync,
      statSync: fs.statSync,
      renameSync: () => {
        throw new Error("EBUSY: resource busy or locked");
      },
    } as unknown as Parameters<typeof migrateLegacyUserData>[2];

    expect(migrateLegacyUserData(current(), ["PlusOnline-dev"], failing)).toEqual({
      kind: "kept",
      from: legacy(),
      to: current(),
      error: "EBUSY: resource busy or locked",
    });
    expect(fs.existsSync(legacy())).toBe(true);
  });
});

describe("legacyUserDataNames", () => {
  it("adds main.ts's -dev suffix when unpackaged", () => {
    expect(legacyUserDataNames(["PlusOnline"], false)).toEqual(["PlusOnline-dev"]);
    expect(legacyUserDataNames(["PlusOnline"], true)).toEqual(["PlusOnline"]);
  });
});
