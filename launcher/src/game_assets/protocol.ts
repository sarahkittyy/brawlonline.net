import type { CustomScheme } from "electron";
import { net, protocol } from "electron";
import path from "path";
import { pathToFileURL } from "url";

import { GAME_ASSET_SCHEME } from "./types";

/**
 * The scheme's privileges, registered with the launcher's own (main/app_scheme.ts) before the app
 * is ready: Electron takes one registerSchemesAsPrivileged call.
 */
export const GAME_ASSET_SCHEME_PRIVILEGES: CustomScheme = {
  scheme: GAME_ASSET_SCHEME,
  // stream: <audio> elements play the extracted sound effects (sounds/*.wav) from this scheme.
  privileges: { standard: true, secure: true, supportFetchAPI: true, corsEnabled: true, stream: true },
};

/** Resolves `<scheme>://<host>/<rel>` inside `root`, refusing anything outside it or another host. */
export function resolveSchemePath(root: string, requestUrl: string, host: string): string | null {
  const url = new URL(requestUrl);
  if (url.hostname !== host) {
    return null;
  }
  const rel = decodeURIComponent(url.pathname).replace(/^\/+/, "");
  const base = path.resolve(root);
  const full = path.resolve(base, rel);
  if (full !== base && !full.startsWith(base + path.sep)) {
    return null;
  }
  return full;
}

/** Resolves `game-asset://cache/<rel>` inside `cacheDir`, refusing anything outside it. */
export function resolveAssetPath(cacheDir: string, requestUrl: string): string | null {
  return resolveSchemePath(cacheDir, requestUrl, "cache");
}

/** Serves the extracted asset cache to the renderer. Call after the app is ready. */
export function handleGameAssetProtocol(getCacheDir: () => string) {
  protocol.handle(GAME_ASSET_SCHEME, async (request) => {
    const file = resolveAssetPath(getCacheDir(), request.url);
    if (!file) {
      return new Response("Not found", { status: 404 });
    }
    try {
      return await net.fetch(pathToFileURL(file).toString());
    } catch {
      return new Response("Not found", { status: 404 });
    }
  });
}
