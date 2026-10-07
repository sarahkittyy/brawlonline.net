// Moves a userData folder left from an earlier product name to the current one.
//
// Electron names the userData folder after the app (`<appData>/<productName>`, or
// `<productName>-dev` unpackaged, see main.ts), so renaming the product would otherwise start
// everyone on an empty profile. This must run before anything reads userData (the log file,
// settings, sessions, Dolphin User folders, Chromium's profile).
//
// SPDX-License-Identifier: GPL-3.0-or-later

import fs from "fs";
import path from "path";

export type LegacyUserDataResult =
  /** The current folder exists, or no legacy folder does: nothing to do. */
  | { kind: "none" }
  /** The legacy folder was renamed to the current one. */
  | { kind: "moved"; from: string; to: string }
  /** The rename failed; the caller should use `from` for this run. */
  | { kind: "kept"; from: string; to: string; error: string };

type Fs = Pick<typeof fs, "existsSync" | "statSync" | "renameSync">;

/**
 * @param userDataDir  the current userData folder (`app.getPath("userData")`)
 * @param legacyNames  folder names the product used before, newest first, in the same folder as
 *                     `userDataDir` and with the same suffix (e.g. `-dev`)
 */
export function migrateLegacyUserData(
  userDataDir: string,
  legacyNames: readonly string[],
  fsImpl: Fs = fs,
): LegacyUserDataResult {
  if (fsImpl.existsSync(userDataDir)) {
    return { kind: "none" };
  }
  const parent = path.dirname(userDataDir);
  for (const name of legacyNames) {
    const from = path.join(parent, name);
    if (path.resolve(from) === path.resolve(userDataDir)) {
      continue;
    }
    let isDir = false;
    try {
      isDir = fsImpl.statSync(from).isDirectory();
    } catch {
      isDir = false;
    }
    if (!isDir) {
      continue;
    }
    try {
      fsImpl.renameSync(from, userDataDir);
      return { kind: "moved", from, to: userDataDir };
    } catch (err) {
      return { kind: "kept", from, to: userDataDir, error: err instanceof Error ? err.message : String(err) };
    }
  }
  return { kind: "none" };
}

/** The legacy folder names for a run: `<name>-dev` unpackaged, as main.ts names the current one. */
export function legacyUserDataNames(legacyProductNames: readonly string[], isPackaged: boolean): string[] {
  return legacyProductNames.map((n) => (isPackaged ? n : `${n}-dev`));
}
