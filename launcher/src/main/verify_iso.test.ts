import { IsoValidity } from "@common/types";
import { mkdtemp, rm, writeFile } from "node:fs/promises";
import os from "os";
import path from "path";
import { afterEach, beforeEach, describe, expect, it } from "vitest";

import { checkIsoHeader, md5ToValidity, verifyIso, verifyIsoCached, verifyIsoDetailed } from "./verify_iso";

function header(id: string, revision: number): Buffer {
  const b = Buffer.alloc(64);
  b.write(id, 0, "latin1");
  b.writeUInt8(revision, 7);
  return b;
}

describe("verifyIso", () => {
  let dir: string;
  beforeEach(async () => {
    dir = await mkdtemp(path.join(os.tmpdir(), "ppo-iso-"));
  });
  afterEach(async () => {
    await rm(dir, { recursive: true, force: true });
  });

  it("accepts exactly the NTSC-U Brawl Rev 1 and Rev 2 MD5s", () => {
    expect(md5ToValidity("d18726e6dfdc8bdbdad540b561051087")).toBe(IsoValidity.VALID);
    expect(md5ToValidity("52CE7160CED2505AD5E397477D0EA4FE")).toBe(IsoValidity.VALID);
    expect(md5ToValidity("00000000000000000000000000000000")).toBe(IsoValidity.INVALID);
  });

  it("rejects other games and revisions from the header without hashing", async () => {
    const melee = path.join(dir, "melee.iso");
    await writeFile(melee, header("GALE01", 2));
    expect(await checkIsoHeader(melee)).toBe(false);
    const rev0 = path.join(dir, "rev0.iso");
    await writeFile(rev0, header("RSBE01", 0));
    expect(await checkIsoHeader(rev0)).toBe(false);
    const rev2 = path.join(dir, "rev2.iso");
    await writeFile(rev2, header("RSBE01", 2));
    expect(await checkIsoHeader(rev2)).toBe(true);
  });

  it("rejects a Brawl header whose contents do not match, never answering unknown", async () => {
    const fake = path.join(dir, "modded.iso");
    await writeFile(fake, header("RSBE01", 1));
    expect(await verifyIso(fake)).toBe(IsoValidity.INVALID);
    const cache = path.join(dir, "cache.json");
    const first = await verifyIsoCached(fake, cache);
    expect(first.valid).toBe(IsoValidity.INVALID);
    expect(first.cached).toBe(false);
    const again = await verifyIsoCached(fake, cache);
    expect(again.valid).toBe(IsoValidity.INVALID);
    expect(again.cached).toBe(true);
    expect(again.detail).toContain(first.detail);
  });

  it("says why an image was rejected", async () => {
    const melee = path.join(dir, "melee.iso");
    await writeFile(melee, header("GALE01", 2));
    const wrongGame = await verifyIsoDetailed(melee);
    expect(wrongGame.valid).toBe(IsoValidity.INVALID);
    expect(wrongGame.detail).toBe(
      '64 bytes, header [47 41 4c 45 30 31 00 02] (game ID "GALE01", revision 2) is not RSBE01 revision 1 or 2',
    );

    const fake = path.join(dir, "modded.iso");
    await writeFile(fake, header("RSBE01", 1));
    const wrongHash = await verifyIsoDetailed(fake);
    expect(wrongHash.valid).toBe(IsoValidity.INVALID);
    expect(wrongHash.detail).toMatch(
      /^64 bytes, header \[52 53 42 45 30 31 00 01\] .* MD5 [0-9a-f]{32} is not an accepted image$/,
    );

    const empty = path.join(dir, "empty.iso");
    await writeFile(empty, Buffer.alloc(0));
    expect((await verifyIsoDetailed(empty)).detail).toBe(
      '0 bytes, header [] (game ID "") is not RSBE01 revision 1 or 2',
    );
  });

  // Hashing an 8.5 GB image takes a while: opt in with PPO_TEST_REAL_ISO=<path to a Brawl ISO>.
  it.skipIf(!process.env.PPO_TEST_REAL_ISO)(
    "accepts a real Brawl image",
    async () => {
      const result = await verifyIsoDetailed(process.env.PPO_TEST_REAL_ISO as string);
      expect(result.valid).toBe(IsoValidity.VALID);
      expect(result.detail).toMatch(/^8511160320 bytes, MD5 [0-9a-f]{32} \(NTSC-U Rev [12]\)$/);
    },
    600000,
  );
});
