import { readdir } from "fs/promises";
import path from "path";

// Dolphin's own replay folder, <netplay User folder>/Replays: where Dolphin saves a game when the
// replay folder refuses the file (ReplayRecorder.cpp OwnReplayDir; bug report #96, Windows'
// Controlled folder access or an antivirus guarding Documents). Its replays are listed once there
// are any, as an extra folder.

const EXTENSION = ".brep";

export function ownReplayFolder(netplayUserFolder: string): string {
  return path.join(netplayUserFolder, "Replays");
}

function samePathOrInside(folder: string, parent: string): boolean {
  const norm = (p: string) => {
    const resolved = path.resolve(p);
    return process.platform === "win32" ? resolved.toLowerCase() : resolved;
  };
  const relative = path.relative(norm(parent), norm(folder));
  return relative === "" || (!relative.startsWith("..") && !path.isAbsolute(relative));
}

// A replay in the folder or one level down (the monthly folders).
async function holdsReplays(folder: string): Promise<boolean> {
  const entries = await readdir(folder, { withFileTypes: true }).catch(() => []);
  if (entries.some((e) => e.isFile() && e.name.toLowerCase().endsWith(EXTENSION))) {
    return true;
  }
  for (const entry of entries.filter((e) => e.isDirectory())) {
    const inner = await readdir(path.join(folder, entry.name), { withFileTypes: true }).catch(() => []);
    if (inner.some((e) => e.isFile() && e.name.toLowerCase().endsWith(EXTENSION))) {
      return true;
    }
  }
  return false;
}

/** The extra folders with Dolphin's own one added, or null when nothing changes. */
export async function withOwnReplayFolder(
  netplayUserFolder: string,
  rootSlpPath: string,
  extraSlpPaths: readonly string[],
): Promise<string[] | null> {
  const folder = ownReplayFolder(netplayUserFolder);
  if ([rootSlpPath, ...extraSlpPaths].some((listed) => samePathOrInside(folder, listed))) {
    return null;
  }
  if (!(await holdsReplays(folder))) {
    return null;
  }
  return [...extraSlpPaths, folder];
}
