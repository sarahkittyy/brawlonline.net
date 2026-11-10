import { FatImage } from "@brawl_assets/fat32";
import { Fat32Volume } from "@brawl_assets/fat32_writer";
import { makeEmptyFat32Image, pattern } from "@brawl_assets/testing/fat_image";
import { createHash } from "crypto";
import fs from "fs";
import os from "os";
import path from "path";
import { afterAll, beforeAll, beforeEach, describe, expect, it } from "vitest";
import { ZipFile } from "yazl";

import { PLUGIN_SD_PATH } from "../install/paths";
import {
  cardPathOf,
  parseRelease,
  patchBootToTraining,
  PROJECT_PUNCH_API,
  ProjectPunch,
  ProjectPunchCancelled,
} from "./project_punch";
import type { ProjectPunchState } from "./types";

const sha256 = (b: Buffer) => createHash("sha256").update(b).digest("hex");

// A codeset with BootToCSS v5.4's default case somewhere in it.
const BOOT = [0x41a0ff84, 0x38951b54, 0x38a00000, 0x48000038, 0x38951c94, 0x38a00014];
function codeset(seed: number): Buffer {
  const b = pattern(4096, seed);
  BOOT.forEach((w, i) => b.writeUInt32BE(w, 0x400 + i * 4));
  return b;
}

async function zipOf(files: Record<string, Buffer>): Promise<Buffer> {
  const zip = new ZipFile();
  for (const [name, data] of Object.entries(files)) {
    zip.addBuffer(data, name);
  }
  zip.end();
  const chunks: Buffer[] = [];
  for await (const c of zip.outputStream as AsyncIterable<Buffer>) {
    chunks.push(c);
  }
  return Buffer.concat(chunks);
}

async function readOnCard(image: string, p: string): Promise<Buffer | null> {
  const fat = await FatImage.open(image);
  try {
    return (await fat.exists(p)) ? await fat.readFile(p) : null;
  } finally {
    await fat.close();
  }
}

type FakeRelease = { tag: string; zip: Buffer; digest?: string };

function fakeGitHub(releases: () => FakeRelease) {
  const calls: string[] = [];
  const fetchImpl = async (url: string) => {
    calls.push(url);
    const r = releases();
    const name = `ProjectPunch-${r.tag.slice(1)}.zip`;
    if (url.startsWith(PROJECT_PUNCH_API)) {
      return new Response(
        JSON.stringify({
          tag_name: r.tag,
          assets: [
            {
              name,
              size: r.zip.length,
              digest: r.digest ?? `sha256:${sha256(r.zip)}`,
              browser_download_url: `https://github.com/WispSSBM/ProjectPunch/releases/download/${r.tag}/${name}`,
            },
          ],
        }),
        { status: 200 },
      );
    }
    if (url.endsWith(name)) {
      return new Response(new Uint8Array(r.zip), { status: 200 });
    }
    return new Response("", { status: 404 });
  };
  return { fetchImpl, calls };
}

let root: string;
let n = 0;

beforeAll(async () => {
  root = await fs.promises.mkdtemp(path.join(os.tmpdir(), "project-punch-"));
});

afterAll(async () => {
  await fs.promises.rm(root, { recursive: true, force: true });
});

describe("helpers", () => {
  it("points BootToCSS's default case at Training", () => {
    const data = codeset(1);
    const out = patchBootToTraining(data)!;
    expect(out.readUInt32BE(0x404)).toBe(0x38951870);
    expect(Buffer.compare(out.subarray(0, 0x404), data.subarray(0, 0x404))).toBe(0);
    expect(Buffer.compare(out.subarray(0x408), data.subarray(0x408))).toBe(0);
    expect(data.readUInt32BE(0x404)).toBe(0x38951b54);
    expect(patchBootToTraining(pattern(4096, 2))).toBeNull();
    // Patched already (an update of the kept card): found, unchanged.
    expect(patchBootToTraining(out)).toEqual(out);
  });

  it("maps zip entries under Project+/ to the card", () => {
    expect(cardPathOf("Project+/pf/plugins/ProjectPunch.rel")).toBe("/Project+/pf/plugins/ProjectPunch.rel");
    expect(cardPathOf("ProjectPunch-1.3.1/Project+/ppunch/actions.bin")).toBe("/Project+/ppunch/actions.bin");
    expect(cardPathOf("README.md")).toBeNull();
    expect(cardPathOf("Project+/../evil.rel")).toBeNull();
    expect(cardPathOf("Project+/")).toBeNull();
  });

  it("takes only a zip from the repo's own release downloads", () => {
    const asset = {
      name: "ProjectPunch-1.3.1.zip",
      size: 37697,
      digest: "sha256:" + "a".repeat(64),
      browser_download_url: "https://github.com/WispSSBM/ProjectPunch/releases/download/v1.3.1/ProjectPunch-1.3.1.zip",
    };
    expect(parseRelease({ tag_name: "v1.3.1", assets: [asset] })).toMatchObject({
      version: "v1.3.1",
      size: 37697,
      sha256: "a".repeat(64),
    });
    expect(() =>
      parseRelease({ tag_name: "v1.3.1", assets: [{ ...asset, browser_download_url: "https://evil.example/x.zip" }] }),
    ).toThrow();
    expect(() => parseRelease({ message: "Not Found" })).toThrow();
  });
});

describe("ProjectPunch", () => {
  let dir: string;
  let source: string;
  let release: FakeRelease;
  let pplusRelease: string;
  let states: ProjectPunchState[];

  const v1 = async () =>
    zipOf({
      "Project+/pf/plugins/ProjectPunch.rel": pattern(5000, 11),
      "Project+/ppunch/actions.bin": pattern(300, 12),
      "ProjectPunch.map": pattern(100, 13),
      "README.md": Buffer.from("readme"),
    });

  const make = (extra: Partial<ConstructorParameters<typeof ProjectPunch>[0]> = {}) => {
    const gh = fakeGitHub(() => release);
    const pp = new ProjectPunch({
      root: path.join(dir, "project-punch"),
      sourceCard: () => source,
      pplusRelease: () => pplusRelease,
      otherPPlusBuild: async () => null,
      fetch: gh.fetchImpl as never,
      clone: async () => {
        throw new Error("no clone");
      },
      onChange: (s) => states.push(s),
      ...extra,
    });
    return { pp, gh };
  };

  beforeEach(async () => {
    dir = path.join(root, String(n++));
    await fs.promises.mkdir(dir, { recursive: true });
    source = path.join(dir, "sd.raw");
    await makeEmptyFat32Image(source, { clusters: 70000 });
    const vol = await Fat32Volume.open(source, { writable: true });
    await vol.writeFile("/Project+/BOOST.GCT", codeset(3));
    await vol.writeFile("/Project+/NETBOOST.GCT", codeset(4));
    await vol.writeFile("/Project+/pf/plugins/AsyncRSP.rel", pattern(700, 5));
    // Should never be there, and never reach Project Punch's card if it is.
    await vol.writeFile(PLUGIN_SD_PATH, pattern(900, 6));
    await vol.close();
    release = { tag: "v1.3.0", zip: await v1() };
    pplusRelease = "v3.2.1";
    states = [];
  });

  it("makes its own card from P+'s: the release's files, Training boot, no online plugin", async () => {
    const before = sha256(await fs.promises.readFile(source));
    const { pp } = make();
    expect((await pp.state({ refresh: true })).installed).toBeNull();
    const s = await pp.install();

    expect(s.installed?.version).toBe("v1.3.0");
    expect(s.upToDate).toBe(true);
    expect(s.installed?.bootsToTraining).toBe(true);
    expect(sha256(await fs.promises.readFile(source))).toBe(before);
    const card = pp.cardImage;
    expect(await readOnCard(card, "/Project+/pf/plugins/ProjectPunch.rel")).toEqual(pattern(5000, 11));
    expect(await readOnCard(card, "/Project+/ppunch/actions.bin")).toEqual(pattern(300, 12));
    expect(await readOnCard(card, "/Project+/pf/plugins/AsyncRSP.rel")).toEqual(pattern(700, 5));
    expect(await readOnCard(card, PLUGIN_SD_PATH)).toBeNull();
    expect(await readOnCard(card, "/README.md")).toBeNull();
    expect((await readOnCard(card, "/Project+/BOOST.GCT"))!.readUInt32BE(0x404)).toBe(0x38951870);
    expect((await readOnCard(card, "/Project+/NETBOOST.GCT"))!.readUInt32BE(0x404)).toBe(0x38951870);
    expect(states.some((x) => x.job?.phase === "card")).toBe(true);
    expect(states[states.length - 1].job).toBeNull();
    expect(await pp.cardForPlay()).toBe(card);
  });

  it("updates in place: new files written, files the release dropped deleted, the card kept", async () => {
    const { pp } = make();
    await pp.install();
    // Something the game wrote on the card (Project Punch's settings) survives an update.
    const vol = await Fat32Volume.open(pp.cardImage, { writable: true });
    await vol.writeFile("/Project+/ppunch/settings.bin", pattern(64, 20));
    await vol.close();

    release = {
      tag: "v1.3.1",
      zip: await zipOf({ "Project+/pf/plugins/ProjectPunch.rel": pattern(5200, 21) }),
    };
    const s1 = await pp.state({ refresh: true });
    expect(s1.upToDate).toBe(false);
    expect(s1.needsCard).toBe(false);
    expect(s1.downloadBytes).toBe(release.zip.length);
    states = [];
    const s2 = await pp.install();

    expect(s2.installed?.version).toBe("v1.3.1");
    expect(s2.installed?.bootsToTraining).toBe(true);
    expect(states.some((x) => x.job?.kind === "update")).toBe(true);
    expect(await readOnCard(pp.cardImage, "/Project+/pf/plugins/ProjectPunch.rel")).toEqual(pattern(5200, 21));
    expect(await readOnCard(pp.cardImage, "/Project+/ppunch/actions.bin")).toBeNull();
    expect(await readOnCard(pp.cardImage, "/Project+/ppunch/settings.bin")).toEqual(pattern(64, 20));
    expect((await readOnCard(pp.cardImage, "/Project+/BOOST.GCT"))!.readUInt32BE(0x404)).toBe(0x38951870);
  });

  it("makes the card again for a new Project+ release", async () => {
    const { pp } = make();
    await pp.install();
    const vol = await Fat32Volume.open(pp.cardImage, { writable: true });
    await vol.writeFile("/marker.bin", pattern(10, 30));
    await vol.close();
    pplusRelease = "v3.2.2";
    const s = await pp.state();
    expect(s.needsCard).toBe(true);
    expect(s.upToDate).toBe(false);
    await expect(pp.cardForPlay()).rejects.toThrow(/another Project\+ version/);
    expect(s.downloadBytes).toBe(release.zip.length + (await fs.promises.stat(source)).size);
    await pp.install();
    expect(await readOnCard(pp.cardImage, "/marker.bin")).toBeNull();
    expect((await pp.state()).installed?.pplusRelease).toBe("v3.2.2");
  });

  it("refuses a download whose sha256 is not GitHub's", async () => {
    release = { tag: "v1.3.0", zip: await v1(), digest: `sha256:${"0".repeat(64)}` };
    const { pp } = make();
    await expect(pp.install()).rejects.toThrow(/damaged/);
    const s = await pp.state();
    expect(s.installed).toBeNull();
    expect(s.error).toMatch(/damaged/);
    expect(fs.existsSync(pp.cardImage)).toBe(false);
  });

  it("stops on Cancel while the card is copied, leaving nothing behind", async () => {
    const { pp } = make();
    const done = pp.install();
    // Cancel once the copy is under way.
    await new Promise<void>((resolve) => {
      const t = setInterval(() => {
        if (states.some((s) => s.job?.phase === "card")) {
          clearInterval(t);
          pp.cancel();
          resolve();
        }
      }, 1);
    });
    await expect(done).rejects.toBeInstanceOf(ProjectPunchCancelled);
    const s = await pp.state();
    expect(s.installed).toBeNull();
    expect(s.error).toBeNull();
    expect(s.job).toBeNull();
    expect(fs.existsSync(pp.cardImage)).toBe(false);
    expect(fs.existsSync(`${pp.cardImage}.partial`)).toBe(false);
    expect(fs.existsSync(path.join(dir, "project-punch", "download"))).toBe(false);
  });

  it("says when GitHub can't be reached, and plays what it has", async () => {
    const { pp } = make();
    await pp.install();
    const offline = new ProjectPunch({
      root: path.join(dir, "project-punch"),
      sourceCard: () => source,
      pplusRelease: () => pplusRelease,
      otherPPlusBuild: async () => null,
      fetch: async () => {
        throw new Error("getaddrinfo ENOTFOUND api.github.com");
      },
    });
    const s = await offline.state({ refresh: true });
    expect(s.latest).toBeNull();
    expect(s.latestError).toMatch(/ENOTFOUND/);
    expect(s.upToDate).toBe(true);
    expect(await offline.cardForPlay()).toBe(pp.cardImage);
  });

  it("refuses to update or remove while its game runs, then removes everything", async () => {
    const { pp } = make();
    await pp.install();
    pp.setRunning(true);
    await expect(pp.remove()).rejects.toThrow(/Close/);
    await expect(pp.install()).rejects.toThrow(/Close/);
    pp.setRunning(false);
    await pp.remove();
    expect(fs.existsSync(path.join(dir, "project-punch"))).toBe(false);
    expect((await pp.state()).installed).toBeNull();
  });
});
