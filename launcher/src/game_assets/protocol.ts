import { net, protocol } from "electron";
import path from "path";
import { pathToFileURL } from "url";

import { GAME_ASSET_SCHEME } from "./types";

/** Must run before the app is ready (Electron requirement for privileged schemes). */
export function registerGameAssetScheme() {
  protocol.registerSchemesAsPrivileged([
    {
      scheme: GAME_ASSET_SCHEME,
      privileges: { standard: true, secure: true, supportFetchAPI: true, corsEnabled: true },
    },
  ]);
}

/** Resolves `game-asset://cache/<rel>` inside `cacheDir`, refusing anything outside it. */
export function resolveAssetPath(cacheDir: string, requestUrl: string): string | null {
  const url = new URL(requestUrl);
  if (url.hostname !== "cache") {
    return null;
  }
  const rel = decodeURIComponent(url.pathname).replace(/^\/+/, "");
  const root = path.resolve(cacheDir);
  const full = path.resolve(root, rel);
  if (full !== root && !full.startsWith(root + path.sep)) {
    return null;
  }
  return full;
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
