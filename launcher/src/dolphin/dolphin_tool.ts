import { execFile } from "child_process";
import { mkdtemp, rm } from "fs/promises";
import os from "os";
import path from "path";

/** DolphinTool next to a Dolphin executable (the bundled install has both). */
export function dolphinToolNextTo(dolphinExecutable: string): string {
  const dir = path.dirname(dolphinExecutable);
  switch (process.platform) {
    case "win32":
      return path.join(dir, "DolphinTool.exe");
    case "darwin":
      // <install>/Dolphin.app/Contents/MacOS/Dolphin -> dolphin-tool in the same folder
      return path.join(dir, "dolphin-tool");
    default:
      return path.join(dir, "dolphin-tool");
  }
}

export type ExecFn = (file: string, args: string[], timeoutMs: number) => Promise<string>;

/** Runs DolphinTool (or anything) and returns its standard output; rejects with its error output. */
export const execTool: ExecFn = (file, args, timeoutMs) =>
  new Promise((resolve, reject) => {
    execFile(
      file,
      args,
      { windowsHide: true, maxBuffer: 4 * 1024 * 1024, timeout: timeoutMs },
      (err, stdout, stderr) => {
        if (err) {
          const detail = String(stderr || "").trim();
          reject(new Error(detail ? `${path.basename(file)}: ${detail}` : String(err)));
          return;
        }
        resolve(String(stdout));
      },
    );
  });

/**
 * Runs `fn` with a new, empty User folder for a DolphinTool command that sets one up (`verify`,
 * `gamedata`: UICommon::Init loads and saves its config, SYSCONF included), then deletes it.
 * Without `-u`, DolphinTool uses the player's own Dolphin's folder (Documents\Dolphin Emulator on
 * Windows), and two of these at once (two launchers, two discs) both write its SYSCONF: one stops
 * at Dolphin's "SYSCONF could not be opened" until someone answers it.
 */
export async function withScratchUserFolder<T>(fn: (userDir: string) => Promise<T>): Promise<T> {
  const dir = await mkdtemp(path.join(os.tmpdir(), "brawl-dolphintool-"));
  try {
    return await fn(dir);
  } finally {
    await rm(dir, { recursive: true, force: true, maxRetries: 3 }).catch(() => undefined);
  }
}
