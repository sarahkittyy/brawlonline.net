import { createHash } from "crypto";
import { mkdtemp, rm, utimes, writeFile } from "fs/promises";
import os from "os";
import path from "path";
import { afterEach, beforeEach, describe, expect, it } from "vitest";

import { BUILD_HASH_PREFIX, buildHashKey, combineBuildHash, computeBuildParts } from "./build_hash";

const sha = (data: string) => createHash("sha256").update(data).digest("hex");
// Stands in for DolphinTool's SHA-1 of the disc's contents.
const discDigest = async (p: string) => `sha1-of-${path.basename(p)}`;

describe("build hash", () => {
  let dir: string;
  beforeEach(async () => {
    dir = await mkdtemp(path.join(os.tmpdir(), "mods-hash-"));
  });
  afterEach(async () => {
    await rm(dir, { recursive: true, force: true });
  });

  it("is the SHA-256 of the disc's content hash and the other parts' SHA-256s, a missing part as '-'", async () => {
    const disc = path.join(dir, "brawl.iso");
    const dol = path.join(dir, "minus.dol");
    await writeFile(disc, "disc bytes");
    await writeFile(dol, "launcher bytes");
    const parts = await computeBuildParts({ discPath: disc, launcherPath: dol, sdPath: null }, discDigest);
    expect(parts).toEqual({ disc: "sha1-of-brawl.iso", launcher: sha("launcher bytes"), sd: null });
    expect(combineBuildHash(parts)).toBe(
      sha(`${BUILD_HASH_PREFIX}disc:sha1-of-brawl.iso\nlauncher:${sha("launcher bytes")}\nsd:-\n`),
    );
  });

  it("differs when any part differs, or a part is added", async () => {
    const disc = path.join(dir, "brawl.iso");
    const sd = path.join(dir, "sd.raw");
    await writeFile(disc, "disc bytes");
    await writeFile(sd, "card one");
    const a = combineBuildHash(await computeBuildParts({ discPath: disc, launcherPath: null, sdPath: sd }, discDigest));
    await writeFile(sd, "card two");
    const b = combineBuildHash(await computeBuildParts({ discPath: disc, launcherPath: null, sdPath: sd }, discDigest));
    const c = combineBuildHash(
      await computeBuildParts({ discPath: disc, launcherPath: null, sdPath: null }, discDigest),
    );
    expect(new Set([a, b, c]).size).toBe(3);
  });

  it("reports the bytes of all parts and ends at the total", async () => {
    const disc = path.join(dir, "brawl.iso");
    const sd = path.join(dir, "sd.raw");
    await writeFile(disc, "x".repeat(1000));
    await writeFile(sd, "y".repeat(500));
    const seen: Array<[number, number]> = [];
    await computeBuildParts({ discPath: disc, launcherPath: null, sdPath: sd }, discDigest, (done, total) =>
      seen.push([done, total]),
    );
    expect(seen.every(([, total]) => total === 1500)).toBe(true);
    expect(seen[0]).toEqual([0, 1500]);
    expect(seen).toContainEqual([1000, 1500]);
    expect(seen[seen.length - 1]).toEqual([1500, 1500]);
  });

  it("has a key that changes with a part's size or modification time", async () => {
    const disc = path.join(dir, "brawl.iso");
    await writeFile(disc, "disc bytes");
    await utimes(disc, 1_700_000_000, 1_700_000_000);
    const before = await buildHashKey({ discPath: disc, launcherPath: null, sdPath: null });
    expect(before).toMatch(/^v2\|10:1700000000000\|-\|-$/);
    await utimes(disc, 1_700_000_100, 1_700_000_100);
    expect(await buildHashKey({ discPath: disc, launcherPath: null, sdPath: null })).not.toBe(before);
  });
});
