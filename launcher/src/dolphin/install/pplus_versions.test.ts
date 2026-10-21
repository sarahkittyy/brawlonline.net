import { FatImage } from "@brawl_assets/fat32";
import { Fat32Volume } from "@brawl_assets/fat32_writer";
import { makeEmptyFat32Image, pattern } from "@brawl_assets/testing/fat_image";
import { replayPPlusRelease } from "@common/pplus_releases";
import { createHash } from "crypto";
import fs from "fs";
import os from "os";
import path from "path";
import { afterEach, beforeEach, describe, expect, it } from "vitest";
import { deflateSync } from "zlib";

import { findProjectPlusCard, PPlusVersionError, PPlusVersions } from "./pplus_versions";
import type { PluginBinary } from "./sd_card";

const sha256 = (data: Buffer) => createHash("sha256").update(data).digest("hex");

// Two releases: v1 (older, for replays) and v2 (the launcher's). v2 changed the codeset and a
// fighter, added one file and removed one.
const V1 = {
  "/Project+/NETPLAY.GCT": Buffer.from("codeset v1"),
  "/Project+/pf/fighter/knuckles/FitKnuckles09.pac": pattern(3000, 1),
  "/Project+/pf/plugins/Physics.rel": Buffer.from("physics"),
  "/Project+/pf/sound/same.brstm": pattern(2000, 4),
  // (P+ really has names with ".." in them.)
  "/Project+/pf/sound/strm/2.00 a.m..brstm": pattern(1000, 5),
};
const V2 = {
  "/Project+/NETPLAY.GCT": Buffer.from("codeset v2, longer"),
  "/Project+/pf/fighter/knuckles/FitKnuckles09.pac": pattern(2900, 2),
  "/DO NOT USE THIS BUILD ON WII.txt": Buffer.from("added in v2"),
  "/Project+/pf/sound/same.brstm": pattern(2000, 4),
  "/Project+/pf/sound/strm/2.00 a.m..brstm": pattern(1000, 5),
};
const PLAYERS_OWN = pattern(5000, 3);

const manifest = (release: string, files: Record<string, Buffer>) => ({
  v: 1,
  release,
  probe: { path: "/Project+/NETPLAY.GCT", sha256: sha256(files["/Project+/NETPLAY.GCT"]) },
  files: Object.entries(files).map(([p, d]) => ({ path: p, sha256: sha256(d), size: d.length })),
});

/** The hosted folder (tools/pplus_packs/make_packs.py's layout), served by a fake fetch. */
function hosted() {
  const files = new Map<string, Buffer>();
  const m1 = Buffer.from(JSON.stringify(manifest("v1.0", V1)));
  const m2 = Buffer.from(JSON.stringify(manifest("v2.0", V2)));
  files.set("releases/v1.0.json", m1);
  files.set("releases/v2.0.json", m2);
  let raw = 0;
  let z = 0;
  for (const [p, d] of Object.entries(V1)) {
    if (sha256((V2 as Record<string, Buffer>)[p] ?? Buffer.alloc(0)) !== sha256(d)) {
      const c = deflateSync(d);
      files.set(`blobs/${sha256(d)}.zlib`, c);
      raw += d.length;
      z += c.length;
    }
  }
  const entry = (release: string, m: Buffer, bytes: number, down: number) => ({
    release,
    manifest: `releases/${release}.json`,
    sha256: sha256(m),
    files: 4,
    filesFromNewest: 0,
    bytesFromNewest: bytes,
    downloadFromNewest: down,
  });
  files.set(
    "index.json",
    Buffer.from(
      JSON.stringify({
        v: 1,
        newest: "v2.0",
        blobs: "blobs/{sha256}.zlib",
        releases: [entry("v1.0", m1, raw, z), entry("v2.0", m2, 0, 0)],
      }),
    ),
  );
  const requests: string[] = [];
  let online = true;
  const fetchImpl = async (url: string) => {
    const rel = url.replace("https://example.test/pplus/", "");
    requests.push(rel);
    if (!online) {
      throw new Error("offline");
    }
    const body = files.get(rel);
    return body ? new Response(new Uint8Array(body), { status: 200 }) : new Response("not found", { status: 404 });
  };
  return { files, fetchImpl, requests, setOnline: (v: boolean) => (online = v) };
}

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

const PLUGIN: PluginBinary = {
  data: Buffer.from("plugin bytes"),
  sha256: sha256(Buffer.from("plugin bytes")),
  source: "test",
};

describe("PPlusVersions", () => {
  let dir: string;
  let userCard: string;
  let host: ReturnType<typeof hosted>;
  let versions: PPlusVersions;

  beforeEach(async () => {
    dir = await fs.promises.mkdtemp(path.join(os.tmpdir(), "pplus-versions-"));
    userCard = path.join(dir, "User", "Wii", "sd.raw");
    await fs.promises.mkdir(path.dirname(userCard), { recursive: true });
    await card(userCard, { ...V2, "/Project+/pf/fighter/mario/FitMario00.pac": PLAYERS_OWN });
    host = hosted();
    versions = new PPlusVersions({
      root: path.join(dir, "pplus-versions"),
      currentRelease: "v2.0",
      baseUrl: "https://example.test/pplus/",
      fetch: host.fetchImpl,
    });
  });

  afterEach(async () => {
    await fs.promises.rm(dir, { recursive: true, force: true });
  });

  it("plans the files that turn the current release's card into the older one's", async () => {
    const plan = await versions.plan("v1.0");
    expect(plan.write.map((f) => f.path).sort()).toEqual([
      "/Project+/NETPLAY.GCT",
      "/Project+/pf/fighter/knuckles/FitKnuckles09.pac",
      "/Project+/pf/plugins/Physics.rel",
    ]);
    expect(plan.remove).toEqual(["/DO NOT USE THIS BUILD ON WII.txt"]);
  });

  it("downloads only the differing files, then makes a card of the older release with the player's own files and the plugin", async () => {
    const before = await versions.state("v1.0");
    expect(before).toMatchObject({ filesReady: false, cardReady: false, playable: false });
    expect(before.downloadBytes).toBeGreaterThan(0);
    await expect(versions.card("v1.0", userCard, PLUGIN)).rejects.toThrow(PPlusVersionError);

    const progress: number[] = [];
    expect(await versions.download("v1.0", (c) => progress.push(c))).toBe(3);
    expect(host.requests.filter((r) => r.startsWith("blobs/"))).toHaveLength(3);
    // (In compressed bytes: what the index says the download is.)
    const index = JSON.parse(host.files.get("index.json")!.toString());
    expect(progress[progress.length - 1]).toBe(index.releases[0].downloadFromNewest);
    expect((await versions.state("v1.0")).filesReady).toBe(true);
    // Nothing more to fetch the second time.
    expect(await versions.download("v1.0")).toBe(0);

    const result = await versions.card("v1.0", userCard, PLUGIN);
    expect(result.action).toBe("copied");
    expect(await read(result.image, "/Project+/NETPLAY.GCT")).toEqual(V1["/Project+/NETPLAY.GCT"]);
    expect(await read(result.image, "/Project+/pf/fighter/knuckles/FitKnuckles09.pac")).toEqual(
      V1["/Project+/pf/fighter/knuckles/FitKnuckles09.pac"],
    );
    expect(await read(result.image, "/Project+/pf/plugins/Physics.rel")).toEqual(
      V1["/Project+/pf/plugins/Physics.rel"],
    );
    expect(await read(result.image, "/DO NOT USE THIS BUILD ON WII.txt")).toBeNull();
    expect(await read(result.image, "/Project+/pf/fighter/mario/FitMario00.pac")).toEqual(PLAYERS_OWN);
    // The player's own card is untouched.
    expect(await read(userCard, "/Project+/NETPLAY.GCT")).toEqual(V2["/Project+/NETPLAY.GCT"]);
    expect((await versions.state("v1.0")).cardReady).toBe(true);
    expect((await versions.card("v1.0", userCard, PLUGIN)).action).toBe("unchanged");
  });

  it("keeps the old release's files when a card update overwrites them: no download afterwards", async () => {
    const oldCard = path.join(dir, "old.raw");
    await card(oldCard, V1);
    const paths = [
      "/Project+/NETPLAY.GCT",
      "/Project+/pf/fighter/knuckles/FitKnuckles09.pac",
      "/Project+/pf/plugins/Physics.rel",
    ];
    expect(await versions.keepReplacedFiles(oldCard, paths)).toBe(3);
    expect((await versions.state("v1.0")).filesReady).toBe(true);
    expect(await versions.download("v1.0")).toBe(0);
    expect(host.requests.filter((r) => r.startsWith("blobs/"))).toHaveLength(0);
  });

  it("takes the files from a Project+ folder of that release on this PC", async () => {
    const folder = path.join(dir, "Project+ v1 Netplay", "User", "Wii");
    await fs.promises.mkdir(folder, { recursive: true });
    await card(path.join(folder, "sd.raw"), V1);
    const found = await findProjectPlusCard(path.join(dir, "Project+ v1 Netplay"));
    expect(found).toBe(path.join(folder, "sd.raw"));
    expect(await versions.importFromCard("v1.0", found!)).toEqual({ needed: 3, found: 3, missing: 0 });
    expect((await versions.state("v1.0")).filesReady).toBe(true);
    // A card of another release gives nothing.
    const other = new PPlusVersions({
      root: path.join(dir, "other"),
      currentRelease: "v2.0",
      baseUrl: "https://example.test/pplus/",
      fetch: host.fetchImpl,
    });
    expect((await other.importFromCard("v1.0", userCard)).found).toBe(0);
  });

  it("refuses a damaged download and works offline from what it has", async () => {
    const blob = [...host.files.keys()].find((k) => k.startsWith("blobs/"))!;
    host.files.set(blob, deflateSync(Buffer.from("not the file")));
    await expect(versions.download("v1.0")).rejects.toMatchObject({ code: "damaged" });

    host.setOnline(false);
    const offline = new PPlusVersions({
      root: path.join(dir, "pplus-versions"),
      currentRelease: "v2.0",
      baseUrl: "https://example.test/pplus/",
      fetch: host.fetchImpl,
    });
    // The index and manifests on disk still say what is needed.
    expect((await offline.plan("v1.0")).write).toHaveLength(3);
    await expect(offline.download("v1.0")).rejects.toMatchObject({ code: "offline" });
  });

  it("does not make a card from a card that is not on the launcher's release", async () => {
    const oldCard = path.join(dir, "old.raw");
    await card(oldCard, V1);
    await versions.download("v1.0");
    await expect(versions.card("v1.0", oldCard, PLUGIN)).rejects.toMatchObject({ code: "card-unknown" });
  });

  it("removes a release's card and files", async () => {
    await versions.download("v1.0");
    await versions.card("v1.0", userCard, PLUGIN);
    await versions.remove("v1.0");
    const state = await versions.state("v1.0");
    expect(state).toMatchObject({ filesReady: false, cardReady: false });
    expect(fs.existsSync(versions.cardDir("v1.0"))).toBe(false);
  });
});

describe("replayPPlusRelease", () => {
  it("takes build.pplus, else the launcher build that recorded it", () => {
    expect(replayPPlusRelease({ pplus: "v3.2.1", dolphin: "v0.1.50" })).toBe("v3.2.1");
    expect(replayPPlusRelease({ dolphin: "v0.1.70" })).toBe("v3.2.0");
    expect(replayPPlusRelease({ dolphin: "v0.1.89" })).toBe("v3.2.0");
    expect(replayPPlusRelease({ dolphin: "v0.1.90" })).toBe("v3.2.1");
    expect(replayPPlusRelease({ dolphin: "v0.1.120" })).toBe("v3.2.1");
    // A development build (a commit hash): unknown.
    expect(replayPPlusRelease({ dolphin: "ec0a8220" })).toBeNull();
    expect(replayPPlusRelease({ pplus: "3.2.0; rm -rf", dolphin: "x" })).toBeNull();
  });
});
