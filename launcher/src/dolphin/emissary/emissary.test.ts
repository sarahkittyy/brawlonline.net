import { createHash } from "crypto";
import fs from "fs";
import os from "os";
import path from "path";
import { afterEach, beforeEach, describe, expect, it } from "vitest";
import { ZipFile } from "yazl";

import {
  compareTags,
  EMISSARY_API,
  EmissaryCancelled,
  EmissaryRecomp,
  engineRelativePath,
  localReleasesFetch,
  parseExtractProgress,
  parseManifest,
  parseRelease,
  releaseFeatures,
  unpackArchive,
} from "./emissary";
import { configIniOverrides, emissaryConfig, setTomlValue } from "./launch";
import type { EmissaryJob, EmissaryManifest, EmissaryState } from "./types";
import { EMISSARY_PROTOCOL } from "./types";

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
const releaseZip = (marker: string, online = true) =>
  zipOf({
    "BrawlRecompiled/Brawl Recompiled.exe": "upstream launcher",
    "BrawlRecompiled/README.txt": "readme",
    "BrawlRecompiled/engine/brawl-engine.exe": `engine ${marker}`,
    "BrawlRecompiled/engine/engine_usa.dll": online ? "dll BRAWL_SD_VOLATILE_WRITES BRAWL_ONLINE_CORE" : "dll",
    "BrawlRecompiled/engine/portable.txt": "upstream portable",
    "BrawlRecompiled/engine/UserData/SD/.keep": "",
  });

// A release from EmissaryRecomp's CI: WiiCompiled.exe with Brawl Online's core, its DLLs, Sys/,
// wii_bootstrap/ and a portable.txt + UserData of its own, in one top folder.
const ciZip = (marker: string, { core = true } = {}) =>
  zipOf({
    "EmissaryRecomp/WiiCompiled.exe": `exe ${marker} BRAWL_SD_VOLATILE_WRITES${core ? " BRAWL_ONLINE_CORE" : ""}`,
    "EmissaryRecomp/SDL3.dll": "sdl",
    "EmissaryRecomp/Sys/files.txt": "sys",
    "EmissaryRecomp/wii_bootstrap/shared2/x": "x",
    "EmissaryRecomp/portable.txt": "theirs",
    "EmissaryRecomp/UserData/Config.toml": "theirs",
  });

function githubRelease(tag: string, zip: Buffer, digest = true, manifest = false) {
  return {
    tag_name: tag,
    assets: [
      ...(manifest ? [{ name: "manifest.json", size: 80, browser_download_url: `${DL}${tag}/manifest.json` }] : []),
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
  let latest: { tag: string; zip: Buffer; manifest?: EmissaryManifest };
  let downloads: number;
  let failDownloads: boolean;
  let extracts: number;
  let isoValid: boolean;

  const fetchImpl = async (url: string) => {
    if (url === `${EMISSARY_API}/latest`) {
      return new Response(JSON.stringify(githubRelease(latest.tag, latest.zip, true, latest.manifest != null)), {
        status: 200,
      });
    }
    if (url.endsWith("/manifest.json") && latest.manifest) {
      return new Response(JSON.stringify(latest.manifest), { status: 200 });
    }
    if (url.endsWith("-Windows.zip")) {
      downloads++;
      if (failDownloads) {
        return new Response("", { status: 503 });
      }
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
    downloads = 0;
    failDownloads = false;
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
    // The switch runs in the background: wait for it (a busy disk makes it slow).
    const old = path.join(em.root, "versions", "v0.6.0");
    for (let i = 0; i < 200 && ((await em.stagedVersion()) != null || fs.existsSync(old)); i++) {
      await new Promise((resolve) => setTimeout(resolve, 50));
    }
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

  it("doesn't play a release without online play (v0.6.0)", async () => {
    latest = { tag: "v0.6.0", zip: await releaseZip("0.6.0", false) };
    const em = make();
    const s = await em.install();
    expect(s.status).toBe("ready");
    expect(s.installed?.playsOnline).toBe(false);
    await expect(em.engineForPlay()).rejects.toThrow(/can't play online yet/);

    latest = { tag: "v0.7.0", zip: await releaseZip("0.7.0") };
    expect((await em.install()).installed?.playsOnline).toBe(true);
    expect(await em.engineForPlay()).toContain("v0.7.0");
  });

  it("installs a CI release (WiiCompiled with the core, its manifest) and plays it", async () => {
    const manifest = { version: "v0.7.0", brawlOnline: "0.2.6", protocol: EMISSARY_PROTOCOL };
    latest = { tag: "v0.7.0", zip: await ciZip("0.7.0"), manifest };
    const em = make();
    const s = await em.install();
    expect(s.installed).toMatchObject({ version: "v0.7.0", playsOnline: true, brawlOnline: "0.2.6" });
    const version = path.join(em.root, "versions", "v0.7.0");
    expect(await em.engineForPlay()).toBe(path.join(version, "WiiCompiled.exe"));
    expect(fs.existsSync(path.join(version, "Sys", "files.txt"))).toBe(true);
    expect(fs.existsSync(path.join(version, "wii_bootstrap", "shared2", "x"))).toBe(true);
    // The release's own portable root would hide ours: only <emissary-recomp>/portable.txt stays.
    expect(fs.existsSync(path.join(version, "portable.txt"))).toBe(false);
    expect(fs.existsSync(path.join(version, "UserData"))).toBe(false);
    expect(fs.existsSync(path.join(em.root, "portable.txt"))).toBe(true);
  });

  it("doesn't play a release without the core, or one for another launcher protocol", async () => {
    latest = { tag: "v0.7.0", zip: await ciZip("0.7.0", { core: false }) };
    const em = make();
    expect((await em.install()).installed?.playsOnline).toBe(false);
    expect(await em.prepareForPlay()).toEqual({ fallback: { reason: "noOnline" } });

    const other = { version: "v0.8.0", brawlOnline: "0.3.0", protocol: EMISSARY_PROTOCOL + 1 };
    latest = { tag: "v0.8.0", zip: await ciZip("0.8.0"), manifest: other };
    await em.remove();
    const s = await em.install();
    expect(s.installed).toMatchObject({ version: "v0.8.0", playsOnline: false });
    await expect(em.engineForPlay()).rejects.toThrow(/can't play online yet/);
  });

  it("keeps the release that plays when the latest is for another launcher protocol", async () => {
    const fits = { version: "v0.7.0", brawlOnline: "x", protocol: EMISSARY_PROTOCOL };
    latest = { tag: "v0.7.0", zip: await ciZip("0.7.0"), manifest: fits };
    const em = make();
    await em.install();
    const other = { version: "v0.8.0", brawlOnline: "y", protocol: 99 };
    latest = { tag: "v0.8.0", zip: await ciZip("0.8.0"), manifest: other };
    const s = await em.state({ refresh: true });
    expect(s.upToDate).toBe(true);
    expect(s.status).toBe("ready");
    expect(await em.prepareForPlay()).toMatchObject({ version: "v0.7.0" });
    expect(downloads).toBe(1);
  });

  it("downloads a newer release before Play starts the game, with its progress", async () => {
    latest = { tag: "v0.7.0", zip: await ciZip("0.7.0") };
    const em = make();
    await em.install();
    latest = { tag: "v0.7.1", zip: await ciZip("0.7.1") };
    const jobs: EmissaryJob[] = [];
    // The last answer is older than a minute: Play asks again and finds v0.7.1.
    (em as unknown as { releaseAt: number }).releaseAt = 0;
    const played = await em.prepareForPlay((job) => jobs.push(job));
    expect(played).toMatchObject({ version: "v0.7.1" });
    expect("exe" in played && played.exe).toContain(path.join("v0.7.1", "WiiCompiled.exe"));
    expect(jobs.some((j) => j.kind === "update" && j.phase === "download")).toBe(true);
    expect(fs.existsSync(path.join(em.root, "versions", "v0.7.0"))).toBe(false);
  });

  it("falls back to Dolphin when the update can't be downloaded", async () => {
    latest = { tag: "v0.7.0", zip: await ciZip("0.7.0") };
    const em = make();
    await em.install();
    latest = { tag: "v0.7.1", zip: await ciZip("0.7.1") };
    failDownloads = true;
    (em as unknown as { releaseAt: number }).releaseAt = 0;
    expect(await em.prepareForPlay()).toEqual({
      fallback: { reason: "updateFailed", message: expect.stringMatching(/HTTP 503/) },
    });
    // Nothing half-installed: the old one is still there.
    expect((await em.state()).installed?.version).toBe("v0.7.0");
  });

  it("plays the installed release when GitHub can't be reached", async () => {
    latest = { tag: "v0.7.0", zip: await ciZip("0.7.0") };
    let offline = false;
    const em = new EmissaryRecomp({
      root: path.join(dir, "emissary-recomp"),
      platform: "win32",
      isoPath: () => iso,
      isoValid: () => true,
      dolphinTool: () => "x",
      fetch: async (url) => (offline ? Promise.reject(new Error("offline")) : fetchImpl(url)),
      extract: async (_t, _i, out) => {
        await fs.promises.mkdir(path.join(out, "DATA", "sys"), { recursive: true });
        await fs.promises.writeFile(path.join(out, "DATA", "sys", "main.dol"), "dol");
      },
    });
    await em.install();
    offline = true;
    (em as unknown as { releaseAt: number }).releaseAt = 0;
    expect(await em.prepareForPlay()).toMatchObject({ version: "v0.7.0" });
  });

  it("says why Dolphin plays when it is not ready", async () => {
    const em = make();
    expect(await em.prepareForPlay()).toEqual({ fallback: { reason: "status", status: "notInstalled" } });
    latest = { tag: "v0.7.0", zip: await ciZip("0.7.0") };
    await em.install();
    iso = path.join(dir, "other.iso");
    await fs.promises.writeFile(iso, "another disc");
    expect(await em.prepareForPlay()).toEqual({ fallback: { reason: "status", status: "needsFiles" } });
  });

  it("plays a release staged while the game ran", async () => {
    latest = { tag: "v0.7.0", zip: await ciZip("0.7.0") };
    const em = make();
    await em.install();
    latest = { tag: "v0.7.1", zip: await ciZip("0.7.1") };
    em.setRunning(true);
    await em.install();
    expect(await em.stagedVersion()).toBe("v0.7.1");
    // Closed without setRunning(false) having switched yet: Play switches.
    (em as unknown as { running: boolean }).running = false;
    expect(await em.prepareForPlay()).toMatchObject({ version: "v0.7.1" });
    expect(await em.stagedVersion()).toBeNull();
    expect(downloads).toBe(2);
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

describe("release manifest and features", () => {
  it("reads manifest.json", () => {
    expect(parseManifest({ version: "v0.7.0", brawlOnline: "0.2.6", protocol: 1 })).toEqual({
      version: "v0.7.0",
      brawlOnline: "0.2.6",
      protocol: 1,
    });
    expect(parseManifest({ version: "v0.7.0", protocol: 1 })).toBeNull();
    expect(parseManifest({ version: "v0.7.0", brawlOnline: "x", protocol: "1" })).toBeNull();
    expect(parseManifest(null)).toBeNull();
  });

  it("finds the manifest asset", () => {
    const r = parseRelease(githubRelease("v0.7.0", Buffer.from("z"), true, true), "win32");
    expect(r.manifestUrl).toBe(`${DL}v0.7.0/manifest.json`);
    expect(parseRelease(githubRelease("v0.6.0", Buffer.from("z")), "win32").manifestUrl).toBeNull();
  });

  it("finds the switches across read chunks", async () => {
    const dir = await fs.promises.mkdtemp(path.join(os.tmpdir(), "emissary-f-"));
    try {
      const big = Buffer.alloc(4 * 1024 * 1024 - 5, 0x41);
      await fs.promises.writeFile(
        path.join(dir, "WiiCompiled.exe"),
        Buffer.concat([big, Buffer.from("BRAWL_ONLINE_CORE..BRAWL_SD_VOLATILE_WRITES")]),
      );
      expect(await releaseFeatures(dir)).toEqual({ readOnlyCard: true, core: true });
      await fs.promises.writeFile(path.join(dir, "WiiCompiled.exe"), "BRAWL_ONLINE_IPC BRAWL_SD_VOLATILE_WRITES");
      expect(await releaseFeatures(dir)).toEqual({ readOnlyCard: true, core: false });
    } finally {
      await fs.promises.rm(dir, { recursive: true, force: true });
    }
  });

  it("orders tags by their numbers", () => {
    expect(["v0.7.10", "v0.7.9", "v0.6.0"].sort(compareTags)).toEqual(["v0.6.0", "v0.7.9", "v0.7.10"]);
  });
});

describe("local releases (PPO_EMISSARY_RELEASES)", () => {
  it("serves the highest tag's assets as GitHub would", async () => {
    const dir = await fs.promises.mkdtemp(path.join(os.tmpdir(), "emissary-l-"));
    try {
      for (const tag of ["v0.7.0", "v0.7.10"]) {
        await fs.promises.mkdir(path.join(dir, tag));
        await fs.promises.writeFile(path.join(dir, tag, `EmissaryRecomp-${tag}-Windows.zip`), `zip ${tag}`);
        await fs.promises.writeFile(
          path.join(dir, tag, "manifest.json"),
          JSON.stringify({ version: tag, brawlOnline: "dev", protocol: 1 }),
        );
      }
      const f = localReleasesFetch(dir);
      const release = parseRelease(await (await f(`${EMISSARY_API}/latest`)).json(), "win32");
      expect(release).toMatchObject({ version: "v0.7.10", size: "zip v0.7.10".length });
      expect(await (await f(release.url)).text()).toBe("zip v0.7.10");
      expect(await (await f(release.manifestUrl!)).json()).toMatchObject({ version: "v0.7.10" });
      expect(parseRelease(await (await f(`${EMISSARY_API}/tags/v0.7.0`)).json(), "win32").version).toBe("v0.7.0");
      expect((await f(`${EMISSARY_API}/tags/v9`)).status).toBe(404);
    } finally {
      await fs.promises.rm(dir, { recursive: true, force: true });
    }
  });
});

describe("configIniOverrides", () => {
  it("takes the -C settings of the ini files the core reads and leaves the rest", () => {
    expect(
      configIniOverrides([
        "-C",
        "Dolphin.Online.MatchmakingHost=192.168.0.226",
        "-C",
        "Dolphin.Online.AccountsUrl=http://192.168.0.226:18080",
        "-C",
        "Logger.Options.WriteToFile=True",
        "--harness-port",
        "5000",
        "-C",
        "SYSCONF.IPL.AR=True",
      ]),
    ).toEqual([
      { file: "Dolphin.ini", section: "Online", key: "MatchmakingHost", value: "192.168.0.226" },
      { file: "Dolphin.ini", section: "Online", key: "AccountsUrl", value: "http://192.168.0.226:18080" },
      { file: "Logger.ini", section: "Options", key: "WriteToFile", value: "True" },
    ]);
  });
});
