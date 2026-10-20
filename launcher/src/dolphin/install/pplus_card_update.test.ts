import { FatImage } from "@brawl_assets/fat32";
import { Fat32Volume } from "@brawl_assets/fat32_writer";
import { makeEmptyFat32Image, pattern } from "@brawl_assets/testing/fat_image";
import { createHash } from "crypto";
import fs from "fs";
import os from "os";
import path from "path";
import { afterEach, beforeEach, describe, expect, it } from "vitest";

import type { PPlusCardUpdate } from "./pplus_card_update";
import { cardRelease, cardUpdateTo, PPLUS_CARD_UPDATES, updateProjectPlusCard } from "./pplus_card_update";
import { PPLUS_RELEASE } from "./pplus_release";

const sha256 = (data: Buffer) => createHash("sha256").update(data).digest("hex");

const OLD_CODES = Buffer.from("codeset 3.2.0");
const NEW_CODES = Buffer.from("codeset 3.2.1, longer");
const OLD_FIGHTER = pattern(3000, 1);
const NEW_FIGHTER = pattern(2900, 2);
const ADDED = Buffer.from("do not use this build on wii");
const PLAYERS_OWN = pattern(5000, 3);

const UPDATE: PPlusCardUpdate = {
  from: "v1",
  to: "v2",
  probe: { path: "/Project+/NETPLAY.GCT", from: sha256(OLD_CODES), to: sha256(NEW_CODES) },
  write: [
    { path: "/Project+/NETPLAY.GCT", sha256: sha256(NEW_CODES) },
    { path: "/Project+/pf/fighter/knuckles/FitKnuckles09.pac", sha256: sha256(NEW_FIGHTER) },
    { path: "/DO NOT USE THIS BUILD ON WII.txt", sha256: sha256(ADDED) },
  ],
  remove: ["/Project+/pf/plugins/Physics.rel", "/apps/project+/boot.dol"],
};

async function card(file: string, files: Record<string, Buffer>) {
  await makeEmptyFat32Image(file);
  const vol = await Fat32Volume.open(file, { writable: true });
  for (const [p, data] of Object.entries(files)) {
    await vol.writeFile(p, data);
  }
  await vol.close();
}

async function read(file: string, p: string): Promise<Buffer | null> {
  const image = await FatImage.open(file);
  try {
    return (await image.exists(p)) ? await image.readFile(p) : null;
  } finally {
    await image.close();
  }
}

describe("updateProjectPlusCard", () => {
  let dir: string;
  let oldCard: string;
  let newCard: string;

  beforeEach(async () => {
    dir = await fs.promises.mkdtemp(path.join(os.tmpdir(), "pplus-card-"));
    oldCard = path.join(dir, "sd.raw");
    newCard = path.join(dir, "sd-v2.raw");
    await card(oldCard, {
      "/Project+/NETPLAY.GCT": OLD_CODES,
      "/Project+/pf/fighter/knuckles/FitKnuckles09.pac": OLD_FIGHTER,
      "/Project+/pf/plugins/Physics.rel": Buffer.from("physics"),
      "/apps/project+/boot.dol": Buffer.from("boot"),
      "/Project+/pf/fighter/mario/FitMario00.pac": PLAYERS_OWN, // a player's own costume
      "/Project+/rp/rp_261019_1200_00.bin": Buffer.from("a replay"),
    });
    await card(newCard, {
      "/Project+/NETPLAY.GCT": NEW_CODES,
      "/Project+/pf/fighter/knuckles/FitKnuckles09.pac": NEW_FIGHTER,
      "/DO NOT USE THIS BUILD ON WII.txt": ADDED,
      "/Project+/pf/fighter/mario/FitMario00.pac": pattern(5000, 4),
    });
  });

  afterEach(async () => {
    await fs.promises.rm(dir, { recursive: true, force: true });
  });

  it("writes only the release's changed files and keeps the rest of the card", async () => {
    expect(await cardRelease(oldCard, UPDATE)).toBe("from");
    const progress: number[] = [];
    const logs: string[] = [];
    const result = await updateProjectPlusCard({
      card: oldCard,
      newCard,
      update: UPDATE,
      onProgress: (current, total) => progress.push(current / total),
      log: (m) => logs.push(m),
    });
    expect(result).toBe("updated");
    expect(await cardRelease(oldCard, UPDATE)).toBe("to");
    expect(await read(oldCard, "/Project+/NETPLAY.GCT")).toEqual(NEW_CODES);
    expect(await read(oldCard, "/Project+/pf/fighter/knuckles/FitKnuckles09.pac")).toEqual(NEW_FIGHTER);
    expect(await read(oldCard, "/DO NOT USE THIS BUILD ON WII.txt")).toEqual(ADDED);
    expect(await read(oldCard, "/Project+/pf/plugins/Physics.rel")).toBeNull();
    expect(await read(oldCard, "/apps/project+/boot.dol")).toBeNull();
    // Not in the update: the player's file and the game's replay stay.
    expect(await read(oldCard, "/Project+/pf/fighter/mario/FitMario00.pac")).toEqual(PLAYERS_OWN);
    expect(await read(oldCard, "/Project+/rp/rp_261019_1200_00.bin")).toEqual(Buffer.from("a replay"));
    expect(progress[progress.length - 1]).toBe(1);
    expect(logs.join("\n")).toContain("3 files written, 2 removed");
    expect(fs.existsSync(`${oldCard}.update.partial`)).toBe(false);
    const vol = await Fat32Volume.open(oldCard);
    try {
      expect(await vol.check()).toEqual([]);
    } finally {
      await vol.close();
    }
  });

  it("leaves a card that is already on the release alone", async () => {
    await updateProjectPlusCard({ card: oldCard, newCard, update: UPDATE });
    const before = await fs.promises.readFile(oldCard);
    expect(await updateProjectPlusCard({ card: oldCard, newCard, update: UPDATE })).toBe("current");
    expect((await fs.promises.readFile(oldCard)).equals(before)).toBe(true);
  });

  it("leaves a card on neither release alone", async () => {
    const other = path.join(dir, "other.raw");
    await card(other, { "/Project+/NETPLAY.GCT": Buffer.from("a player's own codeset") });
    const before = await fs.promises.readFile(other);
    const logs: string[] = [];
    expect(await updateProjectPlusCard({ card: other, newCard, update: UPDATE, log: (m) => logs.push(m) })).toBe(
      "other",
    );
    expect((await fs.promises.readFile(other)).equals(before)).toBe(true);
    expect(logs.join("\n")).toContain("left as it is");
  });

  it("does not touch the card when the new release's files are not the expected ones", async () => {
    const wrong = path.join(dir, "wrong.raw");
    await card(wrong, {
      "/Project+/NETPLAY.GCT": NEW_CODES,
      "/Project+/pf/fighter/knuckles/FitKnuckles09.pac": pattern(2900, 9),
      "/DO NOT USE THIS BUILD ON WII.txt": ADDED,
    });
    const before = await fs.promises.readFile(oldCard);
    await expect(updateProjectPlusCard({ card: oldCard, newCard: wrong, update: UPDATE })).rejects.toThrow(
      /FitKnuckles09\.pac is not the expected file/,
    );
    expect((await fs.promises.readFile(oldCard)).equals(before)).toBe(true);
    expect(fs.existsSync(`${oldCard}.update.partial`)).toBe(false);
  });
});

describe("PPLUS_CARD_UPDATES", () => {
  it("has the update to the pinned release, whose probe is one of the files it writes", () => {
    const update = cardUpdateTo(PPLUS_RELEASE.version);
    expect(update).toBeDefined();
    expect(update!.write.find((w) => w.path === update!.probe.path)?.sha256).toBe(update!.probe.to);
    expect(update!.probe.from).not.toBe(update!.probe.to);
  });

  it("lists every path once, in absolute form", () => {
    for (const update of PPLUS_CARD_UPDATES) {
      const paths = [...update.write.map((w) => w.path), ...update.remove];
      expect(new Set(paths).size).toBe(paths.length);
      for (const p of paths) {
        expect(p.startsWith("/")).toBe(true);
      }
    }
  });
});
