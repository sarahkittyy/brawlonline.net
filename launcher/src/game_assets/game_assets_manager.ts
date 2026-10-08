import electronLog from "electron-log";
import { existsSync } from "fs";
import { mkdir, readFile, rm, stat, writeFile } from "node:fs/promises";
import path from "path";

import type { GameAssetManifest, GameAssetsState } from "./types";
import { GameAssetsStatus } from "./types";

const log = electronLog.scope("game_assets");

export type DiscSource = { kind: "folder"; path: string } | { kind: "iso"; path: string; dolphinToolPath: string };

export type ExtractOptions = {
  disc: DiscSource;
  sdRawPath?: string;
  cacheDir: string;
  onProgress?: (done: number, total: number, label: string) => void;
};

export type ExtractFn = (opts: ExtractOptions) => Promise<GameAssetManifest>;

/** Where the assets come from right now; null when no disc is set (fallback look). */
export type SourceResolver = () => Promise<{ disc: DiscSource; sdRawPath?: string } | null>;

/** Bump when the catalog or decoders change in a way that needs a re-extraction. */
export const EXTRACTION_VERSION = 1;

const SOURCES_FILE = "sources.json";
const MANIFEST_FILE = "manifest.json";

type SourceStamp = {
  version: number;
  disc: { kind: string; path: string; size?: number; mtimeMs?: number };
  sd?: { path: string; size: number; mtimeMs: number };
};

/**
 * Owns the extracted asset cache: decides when to (re)extract, runs the
 * extractor, and reports the state to the renderer.
 */
export class GameAssetsManager {
  private state: GameAssetsState = { status: GameAssetsStatus.NONE, manifest: null };
  private running: Promise<GameAssetsState> | null = null;
  private listeners = new Set<(state: GameAssetsState) => void>();

  constructor(
    readonly cacheDir: string,
    private readonly resolveSources: SourceResolver,
    private readonly extract: ExtractFn,
  ) {}

  getState(): GameAssetsState {
    return this.state;
  }

  onStateChange(listener: (state: GameAssetsState) => void): () => void {
    this.listeners.add(listener);
    return () => this.listeners.delete(listener);
  }

  /**
   * Loads the cached manifest if it still matches the current sources, and
   * extracts otherwise. With no disc set it clears to the fallback look.
   */
  async refresh(options: { force?: boolean } = {}): Promise<GameAssetsState> {
    if (this.running) {
      return this.running;
    }
    this.running = this._refresh(Boolean(options.force)).finally(() => {
      this.running = null;
    });
    return this.running;
  }

  private async _refresh(force: boolean): Promise<GameAssetsState> {
    const sources = await this.resolveSources();
    if (!sources) {
      this._setState({ status: GameAssetsStatus.NONE, manifest: null });
      return this.state;
    }

    const stamp = await makeStamp(sources.disc, sources.sdRawPath);
    if (!force) {
      const cached = await this._loadCached(stamp);
      if (cached) {
        this._setState({ status: GameAssetsStatus.READY, manifest: cached });
        return this.state;
      }
    }

    this._setState({
      status: GameAssetsStatus.EXTRACTING,
      manifest: this.state.manifest,
      progress: { done: 0, total: 1, label: "" },
    });
    try {
      // Start from an empty cache so assets from another disc never linger.
      await rm(this.cacheDir, { recursive: true, force: true });
      await mkdir(this.cacheDir, { recursive: true });
      const manifest = await this.extract({
        disc: sources.disc,
        sdRawPath: sources.sdRawPath,
        cacheDir: this.cacheDir,
        onProgress: (done, total, label) => {
          this._setState({ ...this.state, progress: { done, total, label } });
        },
      });
      await writeFile(path.join(this.cacheDir, SOURCES_FILE), JSON.stringify(stamp, null, 2));
      if (manifest.missing.length > 0) {
        log.warn(`Assets not found: ${manifest.missing.join(", ")}`);
      }
      this._setState({ status: GameAssetsStatus.READY, manifest });
    } catch (err) {
      const message = err instanceof Error ? err.message : String(err);
      log.error(`Asset extraction failed: ${message}`);
      this._setState({ status: GameAssetsStatus.ERROR, manifest: null, error: message });
    }
    return this.state;
  }

  private async _loadCached(stamp: SourceStamp): Promise<GameAssetManifest | null> {
    try {
      const saved = JSON.parse(await readFile(path.join(this.cacheDir, SOURCES_FILE), "utf8")) as SourceStamp;
      if (JSON.stringify(saved) !== JSON.stringify(stamp)) {
        return null;
      }
      return JSON.parse(await readFile(path.join(this.cacheDir, MANIFEST_FILE), "utf8")) as GameAssetManifest;
    } catch {
      return null;
    }
  }

  private _setState(state: GameAssetsState) {
    this.state = state;
    for (const listener of this.listeners) {
      listener(state);
    }
  }
}

async function makeStamp(disc: DiscSource, sdRawPath?: string): Promise<SourceStamp> {
  const stamp: SourceStamp = { version: EXTRACTION_VERSION, disc: { kind: disc.kind, path: disc.path } };
  if (disc.kind === "iso") {
    const s = await stat(disc.path);
    stamp.disc.size = s.size;
    stamp.disc.mtimeMs = s.mtimeMs;
  }
  if (sdRawPath && existsSync(sdRawPath)) {
    const s = await stat(sdRawPath);
    stamp.sd = { path: sdRawPath, size: s.size, mtimeMs: s.mtimeMs };
  }
  return stamp;
}
