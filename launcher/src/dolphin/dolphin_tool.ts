import { execFile } from "child_process";
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
