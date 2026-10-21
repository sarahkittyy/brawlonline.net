import { Fat32Volume } from "@brawl_assets/fat32_writer";
import { makeEmptyFat32Image } from "@brawl_assets/testing/fat_image";
import { createHash } from "crypto";
import fs from "fs";
import os from "os";
import path from "path";
import { afterEach, beforeEach, describe, expect, it } from "vitest";

import type { PPlusBuild } from "./pplus_builds";
import { compareVersions, otherProjectPlusBuild, PPLUS_BUILDS, readInfoVersion } from "./pplus_builds";
import { launcherDolPath, PPLUS_LAUNCHER_DOL_ENTRIES, PPLUS_RELEASE, setAsideProjectPlusCard } from "./pplus_release";

const sha256 = (data: Buffer) => createHash("sha256").update(data).digest("hex");
const info = (version: string) =>
  Buffer.from(`<?xml version="1.0" encoding="UTF-8"?>\n<app>\n    <version>${version}</version>\n</app>\n`);

const CODES = {
  old: Buffer.from("codeset 3.1.5"),
  oldPatch: Buffer.from("codeset 3.1.5-2601"),
  ours: Buffer.from("codeset 3.2.1"),
  newer: Buffer.from("codeset 3.2.1b"),
};

const BUILDS: PPlusBuild[] = [
  { release: "v3.1.5", publishedAt: "2025-10-12", netplayGct: sha256(CODES.old), infoVersion: "3.1" },
  // shares its codes with v3.1.5: named as the first of the two
  { release: "v3.1.5-2601", publishedAt: "2026-01-27", netplayGct: sha256(CODES.old), infoVersion: "3.1" },
  { release: "v3.2.1", publishedAt: "2026-10-19", netplayGct: sha256(CODES.ours), infoVersion: "3.2" },
  { release: "v3.2.1b", publishedAt: "2026-10-21", netplayGct: sha256(CODES.newer), infoVersion: "3.2" },
];

async function card(file: string, files: Record<string, Buffer>) {
  await makeEmptyFat32Image(file);
  const vol = await Fat32Volume.open(file, { writable: true });
  for (const [p, data] of Object.entries(files)) {
    await vol.writeFile(p, data);
  }
  await vol.close();
}

describe("otherProjectPlusBuild", () => {
  let dir: string;
  let sd: string;

  beforeEach(async () => {
    dir = await fs.promises.mkdtemp(path.join(os.tmpdir(), "pplus-builds-"));
    sd = path.join(dir, "sd.raw");
  });

  afterEach(async () => {
    await fs.promises.rm(dir, { recursive: true, force: true });
  });

  it("passes a card on the launcher's release", async () => {
    await card(sd, { "/Project+/NETPLAY.GCT": CODES.ours, "/Project+/info.xml": info("3.2") });
    expect(await otherProjectPlusBuild(sd, "v3.2.1", BUILDS)).toBeNull();
  });

  it("names an older release by its codes (the first of those that share them)", async () => {
    await card(sd, { "/Project+/NETPLAY.GCT": CODES.old, "/Project+/info.xml": info("3.1") });
    expect(await otherProjectPlusBuild(sd, "v3.2.1", BUILDS)).toBe("v3.1.5");
  });

  it("names a newer release too (mixed releases desync either way)", async () => {
    await card(sd, { "/Project+/NETPLAY.GCT": CODES.newer, "/Project+/info.xml": info("3.2") });
    expect(await otherProjectPlusBuild(sd, "v3.2.1", BUILDS)).toBe("v3.2.1b");
  });

  it("names an unknown older card by its info.xml version", async () => {
    await card(sd, { "/Project+/NETPLAY.GCT": Buffer.from("codeset 3.0.5"), "/Project+/info.xml": info("3.0.5") });
    expect(await otherProjectPlusBuild(sd, "v3.2.1", BUILDS)).toBe("v3.0.5");
  });

  it("leaves a player's own codes on the launcher's version alone", async () => {
    await card(sd, { "/Project+/NETPLAY.GCT": Buffer.from("my own codes"), "/Project+/info.xml": info("3.2") });
    expect(await otherProjectPlusBuild(sd, "v3.2.1", BUILDS)).toBeNull();
    await card(sd, { "/Project+/NETPLAY.GCT": Buffer.from("my own codes") });
    expect(await otherProjectPlusBuild(sd, "v3.2.1", BUILDS)).toBeNull();
  });

  it("leaves unreadable cards and unknown launcher releases to the other checks", async () => {
    expect(await otherProjectPlusBuild(path.join(dir, "missing.raw"), "v3.2.1", BUILDS)).toBeNull();
    await fs.promises.writeFile(sd, "not a card");
    expect(await otherProjectPlusBuild(sd, "v3.2.1", BUILDS)).toBeNull();
    await card(sd, { "/Project+/NETPLAY.GCT": CODES.old });
    expect(await otherProjectPlusBuild(sd, "v9.9.9", BUILDS)).toBeNull();
  });
});

describe("pplus_build_fingerprints.json", () => {
  it("knows the launcher's release", () => {
    expect(PPLUS_BUILDS.some((b) => b.release === PPLUS_RELEASE.version)).toBe(true);
  });
  it("has one fingerprint per release", () => {
    const releases = PPLUS_BUILDS.map((b) => b.release);
    expect(new Set(releases).size).toBe(releases.length);
    for (const b of PPLUS_BUILDS) {
      expect(b.netplayGct).toMatch(/^[0-9a-f]{64}$/);
    }
  });
});

describe("helpers", () => {
  it("reads info.xml's version", () => {
    expect(readInfoVersion(info("3.2"))).toBe("3.2");
    expect(readInfoVersion(Buffer.from("<app></app>"))).toBeNull();
  });
  it("compares versions by number", () => {
    expect(compareVersions("3.1.5", "3.2")).toBe(-1);
    expect(compareVersions("3.2", "v3.2.0")).toBe(0);
    expect(compareVersions("3.10", "3.9")).toBe(1);
  });
});

describe("setAsideProjectPlusCard", () => {
  let user: string;

  beforeEach(async () => {
    user = await fs.promises.mkdtemp(path.join(os.tmpdir(), "pplus-aside-"));
    await fs.promises.mkdir(path.join(user, "Wii"), { recursive: true });
    await fs.promises.writeFile(path.join(user, "Wii", "sd.raw"), "old card");
    for (const entry of PPLUS_LAUNCHER_DOL_ENTRIES) {
      await fs.promises.mkdir(path.dirname(launcherDolPath(user, entry)), { recursive: true });
      await fs.promises.writeFile(launcherDolPath(user, entry), "old dol");
    }
  });

  afterEach(async () => {
    await fs.promises.rm(user, { recursive: true, force: true });
  });

  it("keeps the card under its release's name and deletes the release's DOLs", async () => {
    const kept = await setAsideProjectPlusCard(user, "v3.1.5-2601");
    expect(kept).toBe(path.join(user, "Wii", "sd (Project+ v3.1.5-2601).raw"));
    expect(await fs.promises.readFile(kept, "utf8")).toBe("old card");
    expect(fs.existsSync(path.join(user, "Wii", "sd.raw"))).toBe(false);
    for (const entry of PPLUS_LAUNCHER_DOL_ENTRIES) {
      expect(fs.existsSync(launcherDolPath(user, entry))).toBe(false);
    }
  });

  it("never overwrites a card kept before", async () => {
    await setAsideProjectPlusCard(user, "v3.1.5");
    await fs.promises.writeFile(path.join(user, "Wii", "sd.raw"), "second card");
    const kept = await setAsideProjectPlusCard(user, "v3.1.5");
    expect(kept).toBe(path.join(user, "Wii", "sd (Project+ v3.1.5) 2.raw"));
    expect(await fs.promises.readFile(path.join(user, "Wii", "sd (Project+ v3.1.5).raw"), "utf8")).toBe("old card");
  });
});
