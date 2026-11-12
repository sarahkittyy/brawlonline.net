import { createHash } from "crypto";
import fs from "fs";
import os from "os";
import path from "path";
import { afterEach, beforeEach, describe, expect, it } from "vitest";
import { ZipFile } from "yazl";

import {
  EMISSARY_API,
  EmissaryCancelled,
  EmissaryRecomp,
  engineRelativePath,
  parseExtractProgress,
  parseRelease,
  unpackArchive,
} from "./emissary";
import { emissaryConfig, setTomlValue } from "./launch";
import type { EmissaryState } from "./types";

const sha256 = (b: Buffer) => createHash("sha256").update(b).digest("hex");
const DL = "https://github.com/kalomaze/EmissaryRecomp/releases/download/";

async function zipOf(files: Record<string, Buffer | string>): Promise<Buffer> {
  const zip = new ZipFile();
  for (const [name, data] of Object.entries(files)) {
    zip.addBuffer(Buffer.from(data), name);
  }
  zip.end();
  const chunks: Buffer[] = [];
  for await (const c of zip.outputStream as AsyncIterable<Buffer>) {
    chunks.push(c);
  }
  return Buffer.concat(chunks);
}

// A Windows release laid out as upstream's: BrawlRecompiled/{Brawl Recompiled.exe, engine/...}.
const releaseZip = (marker: string) =>
  zipOf({
    "BrawlRecompiled/Brawl Recompiled.exe": "upstream launcher",
    "BrawlRecompiled/README.txt": "readme",
    "BrawlRecompiled/engine/brawl-engine.exe": `engine ${marker}`,
    "BrawlRecompiled/engine/engine_usa.dll": "dll",
    "BrawlRecompiled/engine/portable.txt": "upstream portable",
    "BrawlRecompiled/engine/UserData/SD/.keep": "",
  });

function githubRelease(tag: string, zip: Buffer, digest = true) {
  return {
    tag_name: tag,
    assets: [
      { name: `BrawlRecompiled-${tag}-Linux.tar.gz`, size: 10, browser_download_url: `${DL}${tag}/l.tar.gz` },
      {
        name: `BrawlRecompiled-${tag}-Windows.zip`,
        size: zip.length,
        browser_download_url: `${DL}${tag}/BrawlRecompiled-${tag}-Windows.zip`,
        digest: digest ? `sha256:${sha256(zip)}` : undefined,
      },
    ],
  };
}

describe("parseRelease", () => {
  const zip = Buffer.from("zip");
  it("picks this platform's archive", () => {
    expect(parseRelease(githubRelease("v0.6.0", zip), "win32")).toMatchObject({
      version: "v0.6.0",
      assetName: "BrawlRecompiled-v0.6.0-Windows.zip",
      size: 3,
      sha256: sha256(zip),
    });
    expect(parseRelease(githubRelease("v0.6.0", zip), "linux").assetName).toBe("BrawlRecompiled-v0.6.0-Linux.tar.gz");
  });

  it("refuses macOS (no release yet) and downloads from elsewhere", () => {
    expect(() => parseRelease(githubRelease("v0.6.0", zip), "darwin")).toThrow(/no release for this computer/);
    const r = githubRelease("v0.6.0", zip);
    r.assets[1].browser_download_url = "https://example.com/evil.zip";
    expect(() => parseRelease(r, "win32")).toThrow(/no download for this computer/);
  });
});

describe("parseExtractProgress", () => {
  it("reads DolphinTool's percent", () => {
    expect(parseExtractProgress("Extracting: /files/menu2/sc_title.pac | 42%")).toBe(42);
    expect(parseExtractProgress("Warning: something")).toBeNull();
  });
});

describe("launch config", () => {
  it("sets keys in their sections and keeps the rest", () => {
    const text = "[video]\nresolution_multiplier = 2.25\naspect_mode = 0\n\n[audio]\nvolume = 0.5\n";
    const out = emissaryConfig(text, "C:\\x\\game\\DATA");
    expect(out).toContain('[paths]\ndvd_root = "C:/x/game/DATA"');
    expect(out).toContain("resolution_multiplier = 2.25");
    expect(out).toContain("widescreen = true");
    expect(out).toContain("aspect_mode = 1");
    expect(out).toContain("[audio]\nvolume = 0.5");
  });

  it("replaces a key already there", () => {
    expect(setTomlValue('[paths]\ndvd_root = "old"\n', "paths", "dvd_root", '"new"')).toBe(
      '[paths]\ndvd_root = "new"\n',
    );
  });

  it("keeps a wide aspect the player chose", () => {
    expect(emissaryConfig("[video]\naspect_mode = 2\n", "/g")).toContain("aspect_mode = 2");
  });
});

describe("EmissaryRecomp", () => {
  let dir: string;
  let iso: string;
  let latest: { tag: string; zip: Buffer };
  let extracts: number;
  let isoValid: boolean;

  const fetchImpl = async (url: string) => {
    if (url === `${EMISSARY_API}/latest`) {
      return new Response(JSON.stringify(githubRelease(latest.tag, latest.zip)), { status: 200 });
    }
    if (url.endsWith("-Windows.zip")) {
      return new Response(new Uint8Array(latest.zip), { status: 200 });
    }
    return new Response("", { status: 404 });
  };

  const make = (states: EmissaryState[] = []) =>
    new EmissaryRecomp({
      root: path.join(dir, "emissary-recomp"),
      platform: "win32",
      isoPath: () => iso,
      isoValid: () => isoValid,
      dolphinTool: () => "DolphinTool.exe",
      fetch: fetchImpl,
      unpack: unpackArchive,
      extract: async (_tool, _iso, out, onPercent) => {
        extracts++;
        await fs.promises.mkdir(path.join(out, "DATA", "sys"), { recursive: true });
        await fs.promises.writeFile(path.join(out, "DATA", "sys", "main.dol"), "dol");
        onPercent(100);
      },
      onChange: (s) => states.push(s),
    });

  beforeEach(async () => {
    dir = await fs.promises.mkdtemp(path.join(os.tmpdir(), "emissary-"));
    iso = path.join(dir, "brawl.iso");
    await fs.promises.writeFile(iso, "disc");
    latest = { tag: "v0.6.0", zip: await releaseZip("0.6.0") };
    extracts = 0;
    isoValid = true;
  });

  afterEach(async () => {
    await fs.promises.rm(dir, { recursive: true, force: true });
  });

  it("needs the disc, then installs the program and the game files", async () => {
    isoValid = false;
    const em = make();
    expect((await em.state({ refresh: true })).status).toBe("needsDisc");
    isoValid = true;
    const before = await em.state({ refresh: true });
    expect(before.status).toBe("notInstalled");
    expect(before.downloadBytes).toBe(latest.zip.length);

    const after = await em.install();
    expect(after.status).toBe("ready");
    expect(after.installed?.version).toBe("v0.6.0");
    expect(extracts).toBe(1);

    const exe = await em.engineForPlay();
    expect(exe).toBe(path.join(em.root, "versions", "v0.6.0", engineRelativePath("win32")));
    // Only the game: no upstream launcher, its portable.txt and UserData are ours, one level up.
    const version = path.join(em.root, "versions", "v0.6.0");
    expect(fs.existsSync(path.join(version, "Brawl Recompiled.exe"))).toBe(false);
    expect(fs.existsSync(path.join(version, "engine", "portable.txt"))).toBe(false);
    expect(fs.existsSync(path.join(version, "engine", "UserData"))).toBe(false);
    expect(fs.existsSync(path.join(em.root, "portable.txt"))).toBe(true);
    expect(fs.existsSync(path.join(em.gameFolder, "sys", "main.dol"))).toBe(true);
  });

  it("updates in place when closed, stages the update while the game runs", async () => {
    const em = make();
    await em.install();
    latest = { tag: "v0.6.1", zip: await releaseZip("0.6.1") };
    expect((await em.state({ refresh: true })).status).toBe("updateAvailable");

    em.setRunning(true);
    const staged = await em.install();
    expect(staged.staged).toBe("v0.6.1");
    expect(staged.installed?.version).toBe("v0.6.0");
    expect(extracts).toBe(1);

    em.setRunning(false);
    await new Promise((resolve) => setTimeout(resolve, 50));
    const after = await em.state();
    expect(after.installed?.version).toBe("v0.6.1");
    expect(after.staged).toBeNull();
    expect(after.status).toBe("ready");
    expect(fs.existsSync(path.join(em.root, "versions", "v0.6.0"))).toBe(false);
  });

  it("extracts the game files again for another disc image", async () => {
    const em = make();
    await em.install();
    iso = path.join(dir, "other.iso");
    await fs.promises.writeFile(iso, "another disc");
    expect((await em.state()).status).toBe("needsFiles");
    await em.install();
    expect(extracts).toBe(2);
    expect((await em.state()).status).toBe("ready");
  });

  it("refuses a damaged download and leaves nothing installed", async () => {
    const em = make();
    const good = latest.zip;
    latest = { tag: "v0.6.0", zip: good };
    const realFetch = fetchImpl;
    const broken = new EmissaryRecomp({
      root: em.root,
      platform: "win32",
      isoPath: () => iso,
      isoValid: () => true,
      dolphinTool: () => "x",
      fetch: async (url) => {
        const res = await realFetch(url);
        if (url.endsWith(".zip")) {
          const b = Buffer.from(await res.arrayBuffer());
          b[b.length - 1] ^= 0xff;
          return new Response(new Uint8Array(b), { status: 200 });
        }
        return res;
      },
    });
    await expect(broken.install()).rejects.toThrow(/damaged/);
    const s = await broken.state();
    expect(s.installed).toBeNull();
    expect(s.error).toMatch(/damaged/);
    expect(fs.existsSync(path.join(em.root, "download"))).toBe(false);
  });

  it("cancels the game files and removes everything", async () => {
    let em: EmissaryRecomp | null = null;
    em = new EmissaryRecomp({
      root: path.join(dir, "emissary-recomp"),
      platform: "win32",
      isoPath: () => iso,
      isoValid: () => true,
      dolphinTool: () => "x",
      fetch: fetchImpl,
      extract: async (_t, _i, _o, _p, signal) => {
        em!.cancel();
        if (signal.aborted) {
          throw new EmissaryCancelled();
        }
      },
    });
    await expect(em.install()).rejects.toBeInstanceOf(EmissaryCancelled);
    const s = await em.state();
    expect(s.status).toBe("needsFiles");
    expect(s.error).toBeNull();

    await em.remove();
    expect(fs.existsSync(em.root)).toBe(false);
    expect((await em.state()).status).toBe("notInstalled");
  });

  it("can't be removed while the game runs", async () => {
    const em = make();
    await em.install();
    em.setRunning(true);
    await expect(em.remove()).rejects.toThrow(/Close EmissaryRecomp/);
    expect((await em.state()).status).toBe("running");
  });

  it("is unavailable on macOS", async () => {
    const em = new EmissaryRecomp({
      root: path.join(dir, "emissary-recomp"),
      platform: "darwin",
      isoPath: () => iso,
      isoValid: () => true,
      dolphinTool: () => "x",
      fetch: fetchImpl,
    });
    const s = await em.state({ refresh: true });
    expect(s.status).toBe("unavailable");
    expect(s.supported).toBe(false);
    await expect(em.install()).rejects.toThrow(/no release for this computer/);
  });
});
