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

  it("is the SHA-256 of the disc's content hash, the launcher's SHA-256 and whether there is an SD card", async () => {
    const disc = path.join(dir, "brawl.iso");
    const dol = path.join(dir, "minus.dol");
    await writeFile(disc, "disc bytes");
    await writeFile(dol, "launcher bytes");
    const parts = await computeBuildParts({ discPath: disc, launcherPath: dol, sdPath: null }, discDigest);
    expect(parts).toEqual({
      disc: "sha1-of-brawl.iso",
      launcher: sha("launcher bytes"),
      sd: false,
    });
    expect(combineBuildHash(parts)).toBe(
      sha(`${BUILD_HASH_PREFIX}disc:sha1-of-brawl.iso\nlauncher:${sha("launcher bytes")}\nsd:-\n`),
    );
  });

  it("differs when the disc or launcher differs, or a card is added, but not with the card's contents", async () => {
    const disc = path.join(dir, "brawl.iso");
    const dol = path.join(dir, "rex.elf");
    const sd = path.join(dir, "sd.raw");
    await writeFile(disc, "disc bytes");
    await writeFile(dol, "launcher one");
    await writeFile(sd, "card one");
    const hash = async (sdPath: string | null) =>
      combineBuildHash(await computeBuildParts({ discPath: disc, launcherPath: dol, sdPath }, discDigest));
    const a = await hash(sd);
    // A card the mod wrote to (played offline) is the same build: rooms play the host's card.
    await writeFile(sd, "card two");
    expect(await hash(sd)).toBe(a);
    const noCard = await hash(null);
    await writeFile(dol, "launcher two");
    const otherLauncher = await hash(sd);
    expect(new Set([a, noCard, otherLauncher]).size).toBe(3);
  });

  it("reports the bytes of the disc and the launcher and ends at the total, never reading the card", async () => {
    const disc = path.join(dir, "brawl.iso");
    const dol = path.join(dir, "rex.elf");
    await writeFile(disc, "x".repeat(1000));
    await writeFile(dol, "y".repeat(500));
    const seen: Array<[number, number]> = [];
    await computeBuildParts(
      {
        discPath: disc,
        launcherPath: dol,
        sdPath: path.join(dir, "not-read.raw"),
      },
      discDigest,
      (done, total) => seen.push([done, total]),
    );
    expect(seen.every(([, total]) => total === 1500)).toBe(true);
    expect(seen[0]).toEqual([0, 1500]);
    expect(seen).toContainEqual([1000, 1500]);
    expect(seen[seen.length - 1]).toEqual([1500, 1500]);
  });

  it("has a key that changes with the disc's size or modification time and with a card added", async () => {
    const disc = path.join(dir, "brawl.iso");
    const sd = path.join(dir, "sd.raw");
    await writeFile(disc, "disc bytes");
    await writeFile(sd, "card");
    await utimes(disc, 1_700_000_000, 1_700_000_000);
    const before = await buildHashKey({
      discPath: disc,
      launcherPath: null,
      sdPath: null,
    });
    expect(before).toMatch(/^v3\|10:1700000000000\|-\|-$/);
    const withCard = await buildHashKey({
      discPath: disc,
      launcherPath: null,
      sdPath: sd,
    });
    expect(withCard).toMatch(/^v3\|10:1700000000000\|-\|card$/);
    await writeFile(sd, "the card, written to");
    expect(await buildHashKey({ discPath: disc, launcherPath: null, sdPath: sd })).toBe(withCard);
    await utimes(disc, 1_700_000_100, 1_700_000_100);
    expect(await buildHashKey({ discPath: disc, launcherPath: null, sdPath: null })).not.toBe(before);
  });
});
