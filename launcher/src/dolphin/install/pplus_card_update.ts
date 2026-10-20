// Brings a player's P+ SD card (<netplay User>/Wii/sd.raw) up to the pinned P+ release when P+
// changed its game files. The card is never replaced as a whole: only the files P+ itself changed
// between two releases are written (taken from the new release's card) and the files it removed
// are deleted, so anything else on the card stays as the player has it.
//
// Players on different P+ releases cannot play each other (v3.2.1 changed gameplay codes, fighters
// and stages; a mixed match desyncs), so every card that is on the previous release is updated.
// A card on neither release (a player's own build) is left alone.
//
// The update is made on a copy of the card, checked (every written file reads back with P+'s
// sha256, the FAT is consistent, the card reads as the new release) and only then renamed over it.
//
// SPDX-License-Identifier: GPL-3.0-or-later

import { FatImage } from "@brawl_assets/fat32";
import { Fat32Volume } from "@brawl_assets/fat32_writer";
import { createHash } from "crypto";
import fs from "fs";
import type { ByteProgress } from "utils/copy_file";
import { copyFileWithProgress } from "utils/copy_file";

import V3_2_1 from "./pplus_card_update_v3.2.1.json";

export type PPlusCardUpdate = {
  /** P+ release tags. */
  from: string;
  to: string;
  /** A file whose sha256 tells which of the two releases a card is on. */
  probe: { path: string; from: string; to: string };
  /** The files P+ changed or added, with their sha256 in `to`. */
  write: { path: string; sha256: string }[];
  /** The files P+ removed. */
  remove: string[];
};

/**
 * One entry per P+ release whose card differs from the one before it. Made from the two release
 * zips' user/Wii/sd.raw (every file's sha256 compared; v3.2.0 -> v3.2.1: 134 written, 12 removed).
 */
export const PPLUS_CARD_UPDATES: PPlusCardUpdate[] = [V3_2_1];

export function cardUpdateTo(version: string): PPlusCardUpdate | undefined {
  return PPLUS_CARD_UPDATES.find((u) => u.to === version);
}

const sha256 = (data: Buffer) => createHash("sha256").update(data).digest("hex");

/** Which side of `update` the card is on: `from`, `to`, or `other` (also when it cannot be read). */
export async function cardRelease(card: string, update: PPlusCardUpdate): Promise<"from" | "to" | "other"> {
  let image: FatImage | null = null;
  try {
    image = await FatImage.open(card);
    const hash = sha256(await image.readFile(update.probe.path));
    return hash === update.probe.to ? "to" : hash === update.probe.from ? "from" : "other";
  } catch {
    return "other";
  } finally {
    await image?.close();
  }
}

/**
 * Updates `card` from `update.from` to `update.to`, with the files read from `newCard` (the `to`
 * release's own sd.raw). Returns what it found: `updated`, `current` (already on `to`) or `other`
 * (on neither release: not touched).
 */
export async function updateProjectPlusCard({
  card,
  newCard,
  update,
  onProgress,
  log = () => undefined,
}: {
  card: string;
  newCard: string;
  update: PPlusCardUpdate;
  /** Bytes of the card copied, then files written (as bytes of the update). */
  onProgress?: ByteProgress;
  log?: (message: string) => void;
}): Promise<"updated" | "current" | "other"> {
  const state = await cardRelease(card, update);
  if (state === "to") {
    return "current";
  }
  if (state === "other") {
    log(`${card} is on neither Project+ ${update.from} nor ${update.to}; it is left as it is`);
    return "other";
  }

  const partial = `${card}.update.partial`;
  await fs.promises.rm(partial, { force: true });
  const { size: cardSize } = await fs.promises.stat(card);
  const source = await FatImage.open(newCard);
  let volume: Fat32Volume | null = null;
  try {
    // Read everything first: a wrong source must not leave a half-written copy behind.
    const files: { path: string; data: Buffer }[] = [];
    for (const { path: p, sha256: want } of update.write) {
      const data = await source.readFile(p);
      if (sha256(data) !== want) {
        throw new Error(`Project+ ${update.to}'s ${p} is not the expected file`);
      }
      files.push({ path: p, data });
    }
    const updateBytes = files.reduce((sum, f) => sum + f.data.length, 0);
    const total = cardSize + updateBytes;

    await copyFileWithProgress(card, partial, { onProgress: (current) => onProgress?.(current, total) });
    volume = await Fat32Volume.open(partial, { writable: true });
    let done = cardSize;
    for (const f of files) {
      await volume.writeFile(f.path, f.data);
      done += f.data.length;
      onProgress?.(done, total);
    }
    let removed = 0;
    for (const p of update.remove) {
      if (await volume.delete(p)) {
        removed++;
      }
    }
    await volume.flush();
    const problems = await volume.check();
    if (problems.length > 0) {
      throw new Error(`The updated SD card is inconsistent: ${problems.slice(0, 3).join("; ")}`);
    }
    for (const f of files) {
      if (sha256(await volume.readFile(f.path)) !== sha256(f.data)) {
        throw new Error(`${f.path} did not read back from the updated SD card`);
      }
    }
    await volume.close();
    volume = null;
    if ((await cardRelease(partial, update)) !== "to") {
      throw new Error(`The updated SD card does not read as Project+ ${update.to}`);
    }
    await fs.promises.rename(partial, card);
    log(
      `Updated ${card} from Project+ ${update.from} to ${update.to}: ${files.length} files written, ${removed} removed`,
    );
    return "updated";
  } catch (err) {
    await volume?.close().catch(() => undefined);
    await fs.promises.rm(partial, { force: true });
    throw err;
  } finally {
    await source.close();
  }
}
