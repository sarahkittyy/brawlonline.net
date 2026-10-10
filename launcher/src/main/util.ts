/* eslint import/prefer-default-export: off, import/no-mutable-exports: off */
import { app } from "electron";
import { existsSync } from "node:fs";
import { mkdir, rm } from "node:fs/promises";
import path from "path";
import { URL } from "url";

export let resolveHtmlPath: (htmlFileName: string) => string;

if (process.env.NODE_ENV === "development") {
  const port = process.env.PORT || 1212;
  resolveHtmlPath = (htmlFileName: string) => {
    const url = new URL(`http://localhost:${port}`);
    url.pathname = htmlFileName;
    return url.href;
  };
} else {
  resolveHtmlPath = (htmlFileName: string) => {
    return `file://${path.resolve(__dirname, "../renderer/", htmlFileName)}`;
  };
}

export const getAssetPath = (...paths: string[]): string => {
  const resourcesPath = app.isPackaged
    ? path.join(process.resourcesPath, "assets")
    : path.join(__dirname, "../../assets");
  return path.resolve(path.join(resourcesPath, ...paths));
};

// The window icon on Windows and Linux (macOS uses the bundle's icon.icns). Undefined where the
// assets aren't next to the build (an unpackaged production build).
export const getWindowIcon = (): string | undefined => {
  const icon = getAssetPath("icon.png");
  return existsSync(icon) ? icon : undefined;
};

export async function clearTempFolder() {
  const tmpDir = path.join(app.getPath("userData"), "temp");
  await rm(tmpDir, { recursive: true, force: true });
  await mkdir(tmpDir, { recursive: true });
}
