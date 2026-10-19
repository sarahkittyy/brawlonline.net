/**
 * When the Slippi Launcher is packaged as an AppImage, the AppImage runtime and AppRun
 * script set environment variables (APPDIR, APPIMAGE, LD_LIBRARY_PATH, QT_PLUGIN_PATH,
 * etc.) that point into the launcher's temporary FUSE mount at /tmp/.mount_SlippiXXX/.
 *
 * When the launcher spawns a Dolphin AppImage as a child process, these env vars leak
 * into Dolphin's environment. Dolphin then resolves libraries, Qt plugins, and icons
 * from the launcher's mount point instead of its own, causing two distinct problems:
 *
 *   1. Slow file picker — Dolphin's Qt file dialog searches thousands of nonexistent
 *      icon paths under the launcher's mount, making it extremely slow to open.
 *   2. Mainline fails to launch — QT_PLUGIN_PATH pointing to the launcher's Qt
 *      plugins can prevent Dolphin from finding its bundled xcb platform plugin.
 *
 * This module cleans the inherited environment before spawning Dolphin by:
 *   - Deleting single-value AppImage vars (APPDIR, APPIMAGE, etc.)
 *   - Filtering out AppImage mount path segments from colon-separated path vars
 *     (LD_LIBRARY_PATH, QT_PLUGIN_PATH, PATH, etc.) while preserving user-set values.
 *
 * The ideal root fix would be for the AppImage ecosystem to stop using environment
 * variables for library/plugin discovery (using patchelf rpath or ld-linux
 * --library-path instead), but that change has been stalled upstream since 2017.
 * In the meantime, this sanitization is the pragmatic fix on the launcher side.
 *
 * Steam's overlay: Steam starts a non-Steam game with gameoverlayrenderer.so in
 * LD_PRELOAD, which crashes Electron's sandboxed zygote in the AppImage. The AppImage's
 * launcher binary is a wrapper (.erb/scripts/linux-steam-wrapper.js) that takes the
 * overlay out and saves the original LD_PRELOAD in PPO_STEAM_LD_PRELOAD; Dolphin gets
 * that value back, so it has the Steam overlay as a Steam game does.
 */

const appImageEnvVars = new Set(["APPDIR", "APPIMAGE", "ARGV0", "OWD"]);

export const STEAM_LD_PRELOAD_VAR = "PPO_STEAM_LD_PRELOAD";

const appImagePathVars = [
  "LD_LIBRARY_PATH",
  "QT_PLUGIN_PATH",
  "QT_QPA_PLATFORM_PLUGIN_PATH",
  "QML2_IMPORT_PATH",
  "PATH",
  "XDG_DATA_DIRS",
];

function isAppImagePath(segment: string, appDir: string): boolean {
  if (appDir && segment.includes(appDir)) {
    return true;
  }
  return segment.includes("/tmp/.mount_");
}

function filterPathVar(val: string | undefined, appDir: string): string | undefined {
  if (!val) {
    return val;
  }
  const parts = val.split(":");
  const filtered = parts.filter((part) => !isAppImagePath(part, appDir));
  return filtered.length > 0 ? filtered.join(":") : undefined;
}

export function sanitizeAppImageEnv(): NodeJS.ProcessEnv {
  if (process.platform !== "linux") {
    return process.env;
  }

  const appDir = process.env.APPDIR ?? "";
  const env: NodeJS.ProcessEnv = {};

  for (const key of Object.keys(process.env)) {
    if (appImageEnvVars.has(key)) {
      continue;
    }
    env[key] = process.env[key];
  }

  for (const varName of appImagePathVars) {
    if (varName in env) {
      const cleaned = filterPathVar(env[varName], appDir);
      if (cleaned !== undefined) {
        env[varName] = cleaned;
      } else {
        delete env[varName];
      }
    }
  }

  const steamPreload = env[STEAM_LD_PRELOAD_VAR];
  delete env[STEAM_LD_PRELOAD_VAR];
  if (steamPreload) {
    env.LD_PRELOAD = steamPreload;
  }

  return env;
}
