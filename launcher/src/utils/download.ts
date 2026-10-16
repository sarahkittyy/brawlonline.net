import { createWriteStream } from "node:fs";
import { mkdir, stat, unlink } from "node:fs/promises";
import { dirname } from "node:path";

import { fileExists } from "./file_exists";

const fileSize = (file: string) =>
  stat(file).then(
    (s) => s.size,
    () => 0,
  );

/**
 * Downloads `url` to `destinationFile`. With `resume`, a `destinationFile` left by an earlier,
 * interrupted download is continued with a Range request (or started over when the server answers
 * with the whole file), and a failed download keeps what it received for the next attempt.
 */
export async function download(options: {
  url: string;
  destinationFile: string;
  onProgress?: (progress: { transferredBytes: number; totalBytes: number }) => void;
  overwrite?: boolean;
  resume?: boolean;
}): Promise<void> {
  const { url, destinationFile, onProgress, overwrite, resume } = options;

  if (!overwrite && !resume && (await fileExists(destinationFile))) {
    throw new Error(`Could not download to ${destinationFile}. File already exists!`);
  }

  // Make sure the folder exists
  await mkdir(dirname(destinationFile), { recursive: true });

  const have = resume ? await fileSize(destinationFile) : 0;
  let file: ReturnType<typeof createWriteStream> | null = null;

  try {
    const response = await fetch(url, have > 0 ? { headers: { Range: `bytes=${have}-` } } : undefined);

    // 416: what we have is not a prefix of the file (it is as long or longer). Start over.
    if (have > 0 && response.status === 416) {
      await response.body?.cancel();
      await unlink(destinationFile);
      return download(options);
    }

    if (!response.ok) {
      throw new Error(`Failed to download ${url}: HTTP ${response.status} ${response.statusText}`);
    }

    if (!response.body) {
      throw new Error(`Failed to download ${url}: Response body is empty`);
    }

    // 206 continues at `have` (Content-Range "bytes <have>-<end>/<total>"); anything else is the whole file.
    const range = /^bytes (\d+)-\d+\/(\d+)$/.exec(response.headers.get("content-range") ?? "");
    const continues = response.status === 206 && range !== null && Number(range[1]) === have;
    if (response.status === 206 && !continues) {
      throw new Error(`Failed to download ${url}: unexpected range ${response.headers.get("content-range")}`);
    }

    let transferredBytes = continues ? have : 0;
    const totalBytes = continues ? Number(range[2]) : Number(response.headers.get("content-length") ?? 0);

    const out = createWriteStream(destinationFile, { flags: continues ? "a" : "w" });
    file = out;

    const reader = response.body.getReader();

    // eslint-disable-next-line no-constant-condition
    while (true) {
      const { done, value } = await reader.read();

      if (done) {
        break;
      }

      transferredBytes += value.length;

      onProgress?.({
        transferredBytes,
        totalBytes,
      });

      if (!out.write(value)) {
        await new Promise<void>((resolve) => {
          out.once("drain", resolve);
        });
      }
    }

    out.end();

    await new Promise<void>((resolve, reject) => {
      out.once("finish", resolve);
      out.once("error", reject);
    });
  } catch (err) {
    const out = file;
    if (out && !out.destroyed) {
      // Let what was received reach the file before it is kept or deleted.
      await new Promise<void>((resolve) => out.end(() => resolve()));
    }
    out?.destroy();

    if (!resume) {
      await unlink(destinationFile).catch(() => {});
    }

    throw err;
  }
}
