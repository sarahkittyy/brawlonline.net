import path from "path";

/**
 * Windows answers CreateProcess with "access denied" (Node: spawn EPERM; UNKNOWN for some policy
 * refusals) when an antivirus or another security program blocks or holds the executable; EACCES
 * is "needs administrator" there and "not executable" on macOS/Linux. The bare "spawn EPERM" told
 * the player nothing (Sarah's board, 2026-10-29: "i got this error trying to rebind controls?").
 * Returns the error to show instead, or null for any other error.
 */
export function spawnRefusedError(
  err: unknown,
  executablePath: string,
  platform: NodeJS.Platform = process.platform,
): Error | null {
  const code = (err as NodeJS.ErrnoException | null)?.code;
  const detail = err instanceof Error ? err.message : String(err);
  const name = path.basename(executablePath);
  if (platform === "win32" && (code === "EPERM" || code === "UNKNOWN")) {
    return new Error(
      `Windows refused to start Dolphin: an antivirus or other security program is probably blocking ` +
        `${name}. Allow Brawl Online in it (restore ${name} if it was quarantined), then try again. ` +
        `(${executablePath}: ${detail})`,
    );
  }
  if (platform === "win32" && code === "EACCES") {
    return new Error(
      `Windows wants administrator rights to start Dolphin: untick "Run this program as an ` +
        `administrator" in ${name}'s Properties > Compatibility, then try again. (${executablePath}: ${detail})`,
    );
  }
  if (code === "EACCES") {
    return new Error(
      `Dolphin could not be started: ${executablePath} is not executable. Check the Dolphin ` +
        `Executable setting, or reinstall the launcher. (${detail})`,
    );
  }
  return null;
}
