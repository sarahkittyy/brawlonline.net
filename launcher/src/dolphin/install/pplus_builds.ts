// Which Project+ release a player's SD card is on. P+ writes no version on the card that tells its
// releases apart (info.xml says "3.2" on both v3.2.0 and v3.2.1), so a card is known by the sha256 of
// its /Project+/NETPLAY.GCT, the netplay codeset every release changes. pplus_build_fingerprints.json lists every
// release of P+'s GitHub repository (made by pplus_builds_cli.ts).
//
// Play refuses a card on another release than the launcher's (bug report #74, 2026-10-21: a player
// had made their P+ v3.1.5-2601 folder the game files folder, and every game desynced): the player
// is offered the launcher's release instead (pplus_release.ts replaceProjectPlusCard).
//
// SPDX-License-Identifier: GPL-3.0-or-later

import { FatImage } from "@brawl_assets/fat32";
import { createHash } from "crypto";

import BUILDS from "./pplus_build_fingerprints.json";

export type PPlusBuild = {
  /** P+'s release tag ("v3.1.5-2601"). */
  release: string;
  publishedAt: string;
  /** sha256 of the card's /Project+/NETPLAY.GCT. */
  netplayGct: string;
  /** info.xml's <version>, as P+ wrote it ("3.2"). */
  infoVersion: string | null;
};

export const PPLUS_PROBE_PATH = "/Project+/NETPLAY.GCT";
export const PPLUS_INFO_PATH = "/Project+/info.xml";

export const PPLUS_BUILDS: PPlusBuild[] = BUILDS;

/** info.xml's <version> ("3.2"), or null. */
export function readInfoVersion(xml: Buffer): string | null {
  const m = /<version>\s*([^<\s]+)\s*<\/version>/i.exec(xml.toString("utf8"));
  return m ? m[1] : null;
}

/** -1, 0 or 1 as version `a` ("3.1.5") is older than, the same as or newer than `b`, by number. */
export function compareVersions(a: string, b: string): number {
  const pa = a
    .replace(/^v/i, "")
    .split(/[.-]/)
    .map((s) => parseInt(s, 10));
  const pb = b
    .replace(/^v/i, "")
    .split(/[.-]/)
    .map((s) => parseInt(s, 10));
  for (let i = 0; i < Math.max(pa.length, pb.length); i++) {
    const x = Number.isNaN(pa[i]) ? 0 : pa[i] ?? 0;
    const y = Number.isNaN(pb[i]) ? 0 : pb[i] ?? 0;
    if (x !== y) {
      return x < y ? -1 : 1;
    }
  }
  return 0;
}

/**
 * The Project+ release `card` is on when it is not `release`, the one the launcher plays, else null.
 * A card whose NETPLAY.GCT is a release's own is that release (the first of those that share it:
 * v3.1.5-2601 has v3.1.5's game files).
 * Any other NETPLAY.GCT is a player's own codes: refused only when info.xml says an older version
 * than `release`'s ("v3.1"), so a player's codes on the right release still play. A card that
 * cannot be read is left to the other checks (null).
 */
export async function otherProjectPlusBuild(
  card: string,
  release: string,
  builds: PPlusBuild[] = PPLUS_BUILDS,
): Promise<string | null> {
  let gct: Buffer;
  let info: Buffer | null = null;
  let image: FatImage | null = null;
  try {
    image = await FatImage.open(card);
    gct = await image.readFile(PPLUS_PROBE_PATH);
    if (await image.exists(PPLUS_INFO_PATH)) {
      info = await image.readFile(PPLUS_INFO_PATH);
    }
  } catch {
    return null;
  } finally {
    await image?.close();
  }
  const ours = builds.find((b) => b.release === release);
  if (!ours) {
    return null; // a release we have no fingerprint of (a test override): nothing to compare
  }
  const hash = createHash("sha256").update(gct).digest("hex");
  const same = builds.filter((b) => b.netplayGct === hash);
  if (same.some((b) => b.release === release)) {
    return null;
  }
  if (same.length > 0) {
    return [...same].sort((a, b) => a.publishedAt.localeCompare(b.publishedAt))[0].release;
  }
  const infoVersion = info ? readInfoVersion(info) : null;
  if (infoVersion && ours.infoVersion && compareVersions(infoVersion, ours.infoVersion) < 0) {
    return `v${infoVersion.replace(/^v/i, "")}`;
  }
  return null;
}
