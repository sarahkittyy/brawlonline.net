import { createHash } from "crypto";
import { mkdtemp, rm, utimes, writeFile } from "fs/promises";
import os from "os";
import path from "path";
import { afterEach, beforeEach, describe, expect, it } from "vitest";

import { BUILD_HASH_PREFIX, buildHashKey, combineBuildHash, computeBuildHash } from "./build_hash";

const sha = (data: string) => createHash("sha256").update(data).digest("hex");

describe("build hash", () => {
  let dir: string;
  beforeEach(async () => {
    dir = await mkdtemp(path.join(os.tmpdir(), "mods-hash-"));
  });
  afterEach(async () => {
    await rm(dir, { recursive: true, force: true });
  });

  it("is the SHA-256 of the parts' SHA-256s, a missing part as '-'", async () => {
    const disc = path.join(dir, "brawl.iso");
    const dol = path.join(dir, "minus.dol");
    await writeFile(disc, "disc bytes");
    await writeFile(dol, "launcher bytes");
    const expected = sha(`${BUILD_HASH_PREFIX}disc:${sha("disc bytes")}\nlauncher:${sha("launcher bytes")}\nsd:-\n`);
    expect(await computeBuildHash({ discPath: disc, launcherPath: dol, sdPath: null })).toBe(expected);
    expect(combineBuildHash({ disc: sha("disc bytes"), launcher: sha("launcher bytes"), sd: null })).toBe(expected);
  });

  it("differs when any part differs, or a part is added", async () => {
    const disc = path.join(dir, "brawl.iso");
    const sd = path.join(dir, "sd.raw");
    await writeFile(disc, "disc bytes");
    await writeFile(sd, "card one");
    const a = await computeBuildHash({ discPath: disc, launcherPath: null, sdPath: sd });
    await writeFile(sd, "card two");
    const b = await computeBuildHash({ discPath: disc, launcherPath: null, sdPath: sd });
    const c = await computeBuildHash({ discPath: disc, launcherPath: null, sdPath: null });
    expect(new Set([a, b, c]).size).toBe(3);
  });

  it("reports the bytes of all parts and ends at the total", async () => {
    const disc = path.join(dir, "brawl.iso");
    const sd = path.join(dir, "sd.raw");
    await writeFile(disc, "x".repeat(1000));
    await writeFile(sd, "y".repeat(500));
    const seen: Array<[number, number]> = [];
    await computeBuildHash({ discPath: disc, launcherPath: null, sdPath: sd }, (done, total) =>
      seen.push([done, total]),
    );
    expect(seen.every(([, total]) => total === 1500)).toBe(true);
    expect(seen[seen.length - 1]).toEqual([1500, 1500]);
  });

  it("has a key that changes with a part's size or modification time", async () => {
    const disc = path.join(dir, "brawl.iso");
    await writeFile(disc, "disc bytes");
    await utimes(disc, 1_700_000_000, 1_700_000_000);
    const before = await buildHashKey({ discPath: disc, launcherPath: null, sdPath: null });
    expect(before).toMatch(/^10:1700000000000\|-\|-$/);
    await utimes(disc, 1_700_000_100, 1_700_000_100);
    expect(await buildHashKey({ discPath: disc, launcherPath: null, sdPath: null })).not.toBe(before);
  });
});
