// First-run extraction of the launcher theme assets from the user's own Brawl disc and Project+ SD card.
// Writes PNGs, TrueType fonts, glyph sheets, WAV sound effects and a manifest.json into `cacheDir`. Nothing is bundled.
//
// SPDX-License-Identifier: GPL-3.0-or-later

import fs from "fs";
import path from "path";

import type { FontRole, Slice, SoundRole, TextureCandidate, TextureRole } from "./catalog";
import {
  FONT_ROLES,
  parseStockTextureName,
  SOUND_ROLES,
  STOCK_CHARACTERS,
  STOCK_FILES,
  stockTextureName,
  TEXTURE_ROLES,
} from "./catalog";
import { convertRfnt } from "./font/convert";
import type { BrresTexture } from "./formats/brres";
import { decodeTex0, listBrresTextures, parseBrres } from "./formats/brres";
import type { FoundTexture } from "./formats/container";
import { findTextures, walk } from "./formats/container";
import { parseRfnt } from "./formats/rfnt";
import { encodePng } from "./png";
import { SoundLibrary } from "./sounds";
import type { GameFileSource } from "./sources";
import { DiscFolderSource, DiscIsoSource, SdSource } from "./sources";
import { encodeWav } from "./wav";

export type Origin = "disc" | "sd";

/**
 * Role keys (stable):
 *  textures: background, backgroundTile, button, buttonSelected, panel, cursor
 *  fonts:    menuFont, titleFont, smallFont
 *  stocks:   character keys from catalog.STOCK_CHARACTERS (e.g. "mario", "roy", "knuckles")
 *  sounds:   chat-message, invite (sounds/<role>.wav, 16-bit PCM)
 * All paths are relative to the cache directory and use forward slashes.
 */
export type AssetManifest = {
  schemaVersion: 1;
  extractedAt: string;
  sources: { disc: string; sd?: string };
  textures: Record<
    string,
    {
      file: string;
      width: number;
      height: number;
      origin: Origin;
      archive: string;
      entry: string;
      texture: string;
      /** GX source format, e.g. "IA4". */
      format: string;
      /** True when every pixel is grey (R=G=B): the game tints these at runtime. */
      greyscale: boolean;
      slice?: Slice;
    }
  >;
  fonts: Record<
    string,
    {
      /** TrueType file for CSS @font-face. */
      file: string;
      family: string;
      /** Glyph sheet PNGs (white glyphs with alpha). */
      sheets: string[];
      /** FontMetrics JSON (cell layout, baseline, per-glyph widths). */
      metrics: string;
      origin: Origin;
      archive: string;
      entry: string;
      glyphCount: number;
      /** Source pixel height of the em square: render at multiples of this CSS px size for crisp output. */
      pixelSize: number;
    }
  >;
  stocks: Record<string, { file: string; origin: Origin; name: string; costumes?: string[] }>;
  /** Absent in manifests written before sounds were extracted (extractMissingSounds fills it in). */
  sounds?: Record<
    string,
    {
      /** 16-bit PCM WAV. */
      file: string;
      origin: Origin;
      durationMs: number;
      /** BRSAR symbol name and sound id. */
      sound: string;
      soundId: number;
      /** Where the audio came from: the BRSAR path, or the P+ .sawnd path. */
      source: string;
    }
  >;
  missing: string[];
  warnings: string[];
};

export type DiscInput = { kind: "folder"; path: string } | { kind: "iso"; path: string; dolphinToolPath: string };

export type ExtractOptions = {
  disc: DiscInput;
  sdRawPath?: string;
  cacheDir: string;
  onProgress?: (done: number, total: number, label: string) => void;
  /** Also export every costume's stock icon (stocks/<key>/<n>.png). Default false. */
  allCostumes?: boolean;
  /** Scratch directory for ISO extraction (defaults to a new OS temp dir). */
  tmpDir?: string;
};

const posix = (...p: string[]) => p.join("/");

/** Caches parsed archives per (origin, file). */
class ArchiveCache {
  private files = new Map<string, Promise<Buffer | null>>();
  private textures = new Map<string, FoundTexture[]>();

  constructor(private readonly disc: GameFileSource, private readonly sd: GameFileSource | null) {}

  source(origin: Origin) {
    return origin === "sd" ? this.sd : this.disc;
  }

  read(origin: Origin, file: string): Promise<Buffer | null> {
    const src = this.source(origin);
    if (!src) {
      return Promise.resolve(null);
    }
    const key = `${origin}:${file}`;
    let p = this.files.get(key);
    if (!p) {
      p = src.read(file);
      this.files.set(key, p);
    }
    return p;
  }

  async textureList(origin: Origin, file: string): Promise<FoundTexture[] | null> {
    const key = `${origin}:${file}`;
    const cached = this.textures.get(key);
    if (cached) {
      return cached;
    }
    const buf = await this.read(origin, file);
    if (!buf) {
      return null;
    }
    const list = findTextures(buf);
    this.textures.set(key, list);
    return list;
  }

  private soundLibrary: SoundLibrary | null = null;

  get sounds(): SoundLibrary {
    this.soundLibrary ??= new SoundLibrary((origin, file) => this.read(origin, file));
    return this.soundLibrary;
  }
}

function matches(t: FoundTexture, c: TextureCandidate, checkEntry: boolean) {
  return (
    t.tex.name === c.texture &&
    (!checkEntry || t.brresPath === c.entry) &&
    (c.width === undefined || t.tex.width === c.width) &&
    (c.height === undefined || t.tex.height === c.height)
  );
}

async function findCandidate(cache: ArchiveCache, c: TextureCandidate): Promise<FoundTexture | null> {
  const list = await cache.textureList(c.origin, c.file);
  if (!list) {
    return null;
  }
  // Exact entry first; then the same texture anywhere in the archive (P+ sometimes reorders archives).
  return list.find((t) => matches(t, c, true)) ?? list.find((t) => matches(t, c, false)) ?? null;
}

function isGreyscale(rgba: Uint8Array) {
  for (let i = 0; i < rgba.length; i += 4) {
    if (rgba[i + 3] !== 0 && (rgba[i] !== rgba[i + 1] || rgba[i + 1] !== rgba[i + 2])) {
      return false;
    }
  }
  return true;
}

async function writeFile(root: string, rel: string, data: Buffer | string) {
  const full = path.join(root, ...rel.split("/"));
  await fs.promises.mkdir(path.dirname(full), { recursive: true });
  await fs.promises.writeFile(full, data);
}

function decodeTexture(t: BrresTexture) {
  return decodeTex0(t.tex, t.palette);
}

async function extractTextureRole(
  role: TextureRole,
  cache: ArchiveCache,
  cacheDir: string,
  manifest: AssetManifest,
): Promise<boolean> {
  for (const c of role.candidates) {
    if (!cache.source(c.origin)) {
      continue;
    }
    const found = await findCandidate(cache, c);
    if (!found) {
      continue;
    }
    const img = decodeTexture(found);
    const file = posix("textures", `${role.role}.png`);
    await writeFile(cacheDir, file, encodePng(img.width, img.height, img.rgba));
    manifest.textures[role.role] = {
      file,
      width: img.width,
      height: img.height,
      origin: c.origin,
      archive: c.file,
      entry: found.brresPath,
      texture: found.tex.name,
      format: found.tex.formatName,
      greyscale: isGreyscale(img.rgba),
      ...(role.slice ? { slice: role.slice } : {}),
    };
    return true;
  }
  return false;
}

async function extractFontRole(role: FontRole, cache: ArchiveCache, cacheDir: string, manifest: AssetManifest) {
  for (const c of role.candidates) {
    const buf = await cache.read(c.origin, c.file);
    if (!buf) {
      continue;
    }
    let data: Buffer | null = null;
    for (const node of walk(buf)) {
      if (node.kind === "rfnt" && node.path === c.entry) {
        data = node.data;
        break;
      }
    }
    if (!data) {
      continue;
    }
    const font = parseRfnt(data);
    const dir = posix("fonts", role.role);
    const converted = convertRfnt(font, {
      family: role.family,
      weightClass: role.weightClass,
      sheetFileName: (i) => posix(dir, `sheet_${i}.png`),
    });
    const ttfFile = posix("fonts", `${role.role}.ttf`);
    const metricsFile = posix("fonts", `${role.role}.metrics.json`);
    await writeFile(cacheDir, ttfFile, converted.ttf);
    for (let i = 0; i < converted.sheets.length; i++) {
      await writeFile(cacheDir, converted.metrics.sheetFiles[i], converted.sheets[i]);
    }
    await writeFile(cacheDir, metricsFile, JSON.stringify(converted.metrics));
    manifest.fonts[role.role] = {
      file: ttfFile,
      family: role.family,
      sheets: converted.metrics.sheetFiles,
      metrics: metricsFile,
      origin: c.origin,
      archive: c.file,
      entry: c.entry,
      glyphCount: converted.glyphCount,
      pixelSize: converted.metrics.height,
    };
    return true;
  }
  return false;
}

/** Writes sounds/<role>.wav from the first candidate found. Errors of one candidate become warnings. */
async function extractSoundRole(role: SoundRole, cache: ArchiveCache, cacheDir: string, manifest: AssetManifest) {
  for (const c of role.candidates) {
    if (!cache.source(c.origin)) {
      continue;
    }
    let loaded;
    try {
      loaded = await cache.sounds.load(c);
    } catch (err: any) {
      manifest.warnings.push(`sound ${role.role} (${c.origin}:${c.sound}): ${err?.message ?? err}`);
      continue;
    }
    if (!loaded || loaded.decoded.channels.every((ch) => ch.length === 0)) {
      continue;
    }
    const file = posix("sounds", `${role.role}.wav`);
    await writeFile(cacheDir, file, encodeWav(loaded.decoded.sampleRate, loaded.decoded.channels));
    (manifest.sounds ??= {})[role.role] = {
      file,
      origin: c.origin,
      durationMs: loaded.durationMs,
      sound: loaded.sound.name,
      soundId: loaded.sound.id,
      source: loaded.source,
    };
    return true;
  }
  return false;
}

async function extractSounds(
  cache: ArchiveCache,
  cacheDir: string,
  manifest: AssetManifest,
  step: (label: string) => void = () => undefined,
) {
  manifest.sounds ??= {};
  for (const role of SOUND_ROLES) {
    try {
      if (!(await extractSoundRole(role, cache, cacheDir, manifest))) {
        manifest.missing.push(`sound:${role.role}`);
      }
    } catch (err: any) {
      manifest.missing.push(`sound:${role.role}`);
      manifest.warnings.push(`sound ${role.role}: ${err?.message ?? err}`);
    }
    step(`sound ${role.role}`);
  }
}

async function extractStocks(
  cache: ArchiveCache,
  cacheDir: string,
  manifest: AssetManifest,
  allCostumes: boolean,
  progress: (label: string) => void,
) {
  const byOrigin = new Map<Origin, Map<string, BrresTexture>>();
  for (const origin of ["sd", "disc"] as Origin[]) {
    const spec = STOCK_FILES[origin];
    const buf = await cache.read(origin, spec.file);
    if (!buf) {
      continue;
    }
    const m = new Map<string, BrresTexture>();
    try {
      for (const t of listBrresTextures(parseBrres(buf))) {
        m.set(t.tex.name, t);
      }
    } catch (err: any) {
      manifest.warnings.push(`stocks: cannot parse ${origin}:${spec.file}: ${err?.message ?? err}`);
      continue;
    }
    byOrigin.set(origin, m);
  }
  if (byOrigin.size === 0) {
    manifest.missing.push("stocks");
    return;
  }
  const usable = (t: BrresTexture | undefined): t is BrresTexture => !!t && t.tex.width >= 16 && t.tex.height >= 16;
  // With a P+ SD card the roster is P+'s (no Pokemon Trainer); without one, P+-only characters are not expected.
  const hasSd = !!cache.source("sd");
  for (const ch of STOCK_CHARACTERS) {
    progress(`stock ${ch.key}`);
    if ((ch.pplusOnly && !hasSd) || (ch.discOnly && hasSd)) {
      continue;
    }
    const origins: Origin[] = ch.pplusOnly ? ["sd"] : ch.discOnly ? ["disc"] : ["sd", "disc"];
    let done = false;
    for (const origin of origins) {
      const textures = byOrigin.get(origin);
      const t = textures?.get(stockTextureName(origin, ch.id, 1));
      if (!usable(t)) {
        continue;
      }
      const img = decodeTexture(t);
      const file = posix("stocks", `${ch.key}.png`);
      await writeFile(cacheDir, file, encodePng(img.width, img.height, img.rgba));
      const entry: AssetManifest["stocks"][string] = { file, origin, name: ch.name };
      if (allCostumes) {
        entry.costumes = [];
        const costumes = [...textures!.values()]
          .map((x) => ({ x, parsed: parseStockTextureName(origin, x.tex.name) }))
          .filter((e) => e.parsed && e.parsed.id === ch.id && usable(e.x))
          .sort((a, b) => a.parsed!.costume - b.parsed!.costume);
        for (const { x, parsed } of costumes) {
          const ci = decodeTexture(x);
          const cf = posix("stocks", ch.key, `${parsed!.costume}.png`);
          await writeFile(cacheDir, cf, encodePng(ci.width, ci.height, ci.rgba));
          entry.costumes.push(cf);
        }
      }
      manifest.stocks[ch.key] = entry;
      done = true;
      break;
    }
    if (!done) {
      manifest.missing.push(`stock:${ch.key}`);
    }
  }
}

async function openDisc(disc: DiscInput, tmpDir?: string): Promise<GameFileSource> {
  if (disc.kind === "folder") {
    return DiscFolderSource.open(disc.path);
  }
  return DiscIsoSource.open({ isoPath: disc.path, dolphinToolPath: disc.dolphinToolPath, tmpDir });
}

export async function extractBrawlAssets(opts: ExtractOptions): Promise<AssetManifest> {
  const disc = await openDisc(opts.disc, opts.tmpDir);
  let sd: GameFileSource | null = null;
  try {
    if (opts.sdRawPath) {
      sd = await SdSource.open(opts.sdRawPath);
    }
    return await runExtraction(disc, sd, opts);
  } finally {
    await sd?.close();
    await disc.close();
  }
}

/** Runs the extraction against already-open sources (exported for tests and tooling). */
export async function runExtraction(
  disc: GameFileSource,
  sd: GameFileSource | null,
  opts: Pick<ExtractOptions, "cacheDir" | "onProgress" | "allCostumes">,
): Promise<AssetManifest> {
  const manifest: AssetManifest = {
    schemaVersion: 1,
    extractedAt: new Date().toISOString(),
    sources: { disc: disc.label, ...(sd ? { sd: sd.label } : {}) },
    textures: {},
    fonts: {},
    stocks: {},
    sounds: {},
    missing: [],
    warnings: [],
  };
  await fs.promises.mkdir(opts.cacheDir, { recursive: true });
  const cache = new ArchiveCache(disc, sd);
  const total = TEXTURE_ROLES.length + FONT_ROLES.length + STOCK_CHARACTERS.length + SOUND_ROLES.length + 1;
  let done = 0;
  const step = (label: string) => opts.onProgress?.(++done, total, label);
  opts.onProgress?.(0, total, "starting");

  for (const role of TEXTURE_ROLES) {
    try {
      if (!(await extractTextureRole(role, cache, opts.cacheDir, manifest))) {
        manifest.missing.push(`texture:${role.role}`);
      }
    } catch (err: any) {
      manifest.missing.push(`texture:${role.role}`);
      manifest.warnings.push(`texture ${role.role}: ${err?.message ?? err}`);
    }
    step(`texture ${role.role}`);
  }
  for (const role of FONT_ROLES) {
    try {
      if (!(await extractFontRole(role, cache, opts.cacheDir, manifest))) {
        manifest.missing.push(`font:${role.role}`);
      }
    } catch (err: any) {
      manifest.missing.push(`font:${role.role}`);
      manifest.warnings.push(`font ${role.role}: ${err?.message ?? err}`);
    }
    step(`font ${role.role}`);
  }
  try {
    await extractStocks(cache, opts.cacheDir, manifest, !!opts.allCostumes, step);
  } catch (err: any) {
    manifest.missing.push("stocks");
    manifest.warnings.push(`stocks: ${err?.message ?? err}`);
  }
  await extractSounds(cache, opts.cacheDir, manifest, step);
  done = total - 1;
  await writeFile(opts.cacheDir, "manifest.json", JSON.stringify(manifest, null, 2));
  step("done");
  return manifest;
}

/** Reads a previously written manifest (null when absent or from another schema version). */
export async function readManifest(cacheDir: string): Promise<AssetManifest | null> {
  try {
    const m = JSON.parse(await fs.promises.readFile(path.join(cacheDir, "manifest.json"), "utf8"));
    return m?.schemaVersion === 1 ? (m as AssetManifest) : null;
  } catch {
    return null;
  }
}

/**
 * True when the manifest's sounds do not match the catalog: none extracted yet, a role neither extracted nor recorded
 * missing (added since), or a role taken under a sound the catalog no longer lists for it (changed since).
 */
export function soundsOutdated(manifest: { sounds?: Record<string, { sound?: string }>; missing?: string[] }) {
  const sounds = manifest.sounds;
  if (!sounds) {
    return true;
  }
  return SOUND_ROLES.some((role) => {
    const s = sounds[role.role];
    return s
      ? !role.candidates.some((c) => c.sound === s.sound)
      : !(manifest.missing ?? []).includes(`sound:${role.role}`);
  });
}

/**
 * Brings the sound effects of a cache in line with the catalog (see soundsOutdated): a cache extracted before the
 * launcher had them, or with a sound the catalog has replaced since, without extracting everything again. Returns the
 * updated manifest, the unchanged one when its sounds are current, or null when there is no readable manifest in
 * `cacheDir`.
 */
export async function extractMissingSounds(
  opts: Pick<ExtractOptions, "disc" | "sdRawPath" | "cacheDir" | "tmpDir">,
): Promise<AssetManifest | null> {
  const manifest = await readManifest(opts.cacheDir);
  if (!manifest || !soundsOutdated(manifest)) {
    return manifest;
  }
  delete manifest.sounds;
  manifest.warnings = (manifest.warnings ?? []).filter((w) => !w.startsWith("sound "));
  manifest.missing = (manifest.missing ?? []).filter((m) => !m.startsWith("sound:"));
  const disc = await openDisc(opts.disc, opts.tmpDir);
  let sd: GameFileSource | null = null;
  try {
    if (opts.sdRawPath) {
      sd = await SdSource.open(opts.sdRawPath);
    }
    await extractSounds(new ArchiveCache(disc, sd), opts.cacheDir, manifest);
  } finally {
    await sd?.close();
    await disc.close();
  }
  await writeFile(opts.cacheDir, "manifest.json", JSON.stringify(manifest, null, 2));
  return manifest;
}
