import { createHash } from "crypto";
import fs from "fs";
import os from "os";
import path from "path";
import { afterAll, beforeAll, describe, expect, it, vi } from "vitest";

import { copyFileWithProgress, hashFileWithProgress, throttleProgress } from "./copy_file";

let root: string;
let n = 0;

beforeAll(async () => {
  root = await fs.promises.mkdtemp(path.join(os.tmpdir(), "copy-file-"));
});

afterAll(async () => {
  await fs.promises.rm(root, { recursive: true, force: true });
});

function data(size: number, seed = 1): Buffer {
  const b = Buffer.alloc(size);
  let x = seed;
  for (let i = 0; i < size; i++) {
    x = (x * 1103515245 + 12345) >>> 0;
    b[i] = x >>> 24;
  }
  return b;
}

async function source(size: number, seed = 1): Promise<{ file: string; content: Buffer }> {
  const file = path.join(root, `src-${n++}.bin`);
  const content = data(size, seed);
  await fs.promises.writeFile(file, content);
  return { file, content };
}

const dest = () => path.join(root, `dst-${n++}.bin`);

const noClone = async () => {
  throw Object.assign(new Error("cannot clone"), { code: "ENOTSUP" });
};

/** Every value is >= the one before, never past the total, and the last one is the total. */
function expectMonotonicToTotal(calls: [number, number][], total: number) {
  expect(calls.length).toBeGreaterThan(0);
  calls.forEach(([done, t], i) => {
    expect(t).toBe(total);
    expect(done).toBeLessThanOrEqual(t);
    if (i > 0) {
      expect(done).toBeGreaterThanOrEqual(calls[i - 1][0]);
    }
  });
  expect(calls[calls.length - 1]).toEqual([total, total]);
}

describe("copyFileWithProgress", () => {
  it("copies in chunks with monotonic progress that reaches the total", async () => {
    const { file, content } = await source(10 * 1000 + 7);
    const out = dest();
    const calls: [number, number][] = [];
    const how = await copyFileWithProgress(file, out, {
      chunkSize: 1000,
      clone: noClone,
      onProgress: (d, t) => calls.push([d, t]),
    });
    expect(how).toBe("copied");
    expect(await fs.promises.readFile(out)).toEqual(content);
    expect(calls[0]).toEqual([0, content.length]);
    // 0, then one report per chunk: 10 full chunks and the 7-byte tail.
    expect(calls.length).toBe(12);
    expectMonotonicToTotal(calls, content.length);
  });

  it("uses the copy-on-write clone when the file system has one, reporting it as done at once", async () => {
    const { file, content } = await source(5000);
    const out = dest();
    const cloned: string[] = [];
    const calls: [number, number][] = [];
    const how = await copyFileWithProgress(file, out, {
      chunkSize: 1000,
      clone: async (s, d) => {
        cloned.push(s);
        await fs.promises.copyFile(s, d);
      },
      onProgress: (d, t) => calls.push([d, t]),
    });
    expect(how).toBe("cloned");
    expect(cloned).toEqual([file]);
    expect(calls).toEqual([[5000, 5000]]);
    expect(await fs.promises.readFile(out)).toEqual(content);
  });

  it("falls back to the chunked copy when the clone fails, also after it left a file behind", async () => {
    const { file, content } = await source(3000);
    const out = dest();
    const calls: [number, number][] = [];
    const how = await copyFileWithProgress(file, out, {
      chunkSize: 1024,
      clone: async (_s, d) => {
        await fs.promises.writeFile(d, "half a clone, longer than nothing at all");
        throw Object.assign(new Error("EXDEV"), { code: "EXDEV" });
      },
      onProgress: (d, t) => calls.push([d, t]),
    });
    expect(how).toBe("copied");
    expect(await fs.promises.readFile(out)).toEqual(content);
    expectMonotonicToTotal(calls, 3000);
  });

  it("works with the platform's own clone (on Windows: no clone, so a chunked copy)", async () => {
    const { file, content } = await source(200_000);
    const out = dest();
    const calls: [number, number][] = [];
    const how = await copyFileWithProgress(file, out, {
      chunkSize: 64 * 1024,
      onProgress: (d, t) => calls.push([d, t]),
    });
    expect(["cloned", "copied"]).toContain(how);
    if (process.platform === "win32") {
      expect(how).toBe("copied");
    }
    expect(await fs.promises.readFile(out)).toEqual(content);
    expectMonotonicToTotal(calls, content.length);
  });

  it("flushes to disk along the way when asked, with the same result", async () => {
    const { file, content } = await source(10_000, 3);
    const out = dest();
    const calls: [number, number][] = [];
    const how = await copyFileWithProgress(file, out, {
      chunkSize: 1000,
      clone: noClone,
      syncEvery: 3000,
      onProgress: (d, t) => calls.push([d, t]),
    });
    expect(how).toBe("copied");
    expect(await fs.promises.readFile(out)).toEqual(content);
    expectMonotonicToTotal(calls, content.length);
  });

  it("replaces an existing destination completely", async () => {
    const { file, content } = await source(1500);
    const out = dest();
    await fs.promises.writeFile(out, data(9000, 7));
    await copyFileWithProgress(file, out, { chunkSize: 512, clone: noClone });
    expect(await fs.promises.readFile(out)).toEqual(content);
  });

  it("copies an empty file", async () => {
    const { file } = await source(0);
    const out = dest();
    const calls: [number, number][] = [];
    await copyFileWithProgress(file, out, { clone: noClone, onProgress: (d, t) => calls.push([d, t]) });
    expect((await fs.promises.stat(out)).size).toBe(0);
    expect(calls).toEqual([[0, 0]]);
  });

  it("stops when progress throws, leaving a partial file that is not held open and an untouched source", async () => {
    const { file, content } = await source(10_000);
    const out = dest();
    let calls = 0;
    await expect(
      copyFileWithProgress(file, out, {
        chunkSize: 1000,
        clone: noClone,
        onProgress: () => {
          if (++calls === 4) {
            throw new Error("cancelled");
          }
        },
      }),
    ).rejects.toThrow("cancelled");
    const partial = (await fs.promises.stat(out)).size;
    expect(partial).toBeGreaterThan(0);
    expect(partial).toBeLessThan(content.length);
    expect(await fs.promises.readFile(file)).toEqual(content);
    // Both handles were closed (Windows refuses to delete a file that is still open).
    await fs.promises.rm(out);
    await fs.promises.rm(file);
  });

  it("keeps what was there of a file that shrank while it was copied", async () => {
    const { file, content } = await source(10_000);
    const out = dest();
    const calls: [number, number][] = [];
    await copyFileWithProgress(file, out, {
      chunkSize: 1000,
      clone: noClone,
      onProgress: (d, t) => {
        calls.push([d, t]);
        if (calls.length === 3) {
          fs.truncateSync(file, 3500);
        }
      },
    });
    const { size } = await fs.promises.stat(out);
    expect(size).toBeLessThan(content.length);
    expect(calls[calls.length - 1]).toEqual([size, size]);
  });

  it("fails when the reads end before the end of a file that is still whole (bug report #230)", async () => {
    const { file, content } = await source(10_000);
    const out = dest();
    const open = fs.promises.open.bind(fs.promises);
    const spy = vi.spyOn(fs.promises, "open").mockImplementation(async (p, flags, mode) => {
      const fh = await open(p, flags, mode);
      if (p === file) {
        // Reads past 6000 bytes come back empty, as at the end of the file.
        const read = fh.read.bind(fh) as (b: Buffer, o: number, l: number, pos: number) => Promise<unknown>;
        Object.assign(fh, {
          read: (b: Buffer, o: number, l: number, pos: number) =>
            pos >= 6000 ? Promise.resolve({ bytesRead: 0, buffer: b }) : read(b, o, l, pos),
        });
      }
      return fh;
    });
    try {
      await expect(copyFileWithProgress(file, out, { chunkSize: 1000, clone: noClone })).rejects.toThrow(
        /stopped after 6000 of its 10000 bytes/,
      );
    } finally {
      spy.mockRestore();
    }
    expect(await fs.promises.readFile(file)).toEqual(content);
  });

  it("fails without a destination when the source is missing", async () => {
    const out = dest();
    await expect(copyFileWithProgress(path.join(root, "nope.bin"), out, { clone: noClone })).rejects.toThrow(/ENOENT/);
    expect(fs.existsSync(out)).toBe(false);
  });
});

describe("hashFileWithProgress", () => {
  it("hashes like crypto and reports the bytes read up to the size", async () => {
    const { file, content } = await source(100_003);
    for (const algorithm of ["md5", "sha256"]) {
      const calls: [number, number][] = [];
      const digest = await hashFileWithProgress(file, algorithm, (d, t) => calls.push([d, t]), 10_000);
      expect(digest).toBe(createHash(algorithm).update(content).digest("hex"));
      expect(calls[0]).toEqual([0, content.length]);
      expect(calls.length).toBe(12);
      expectMonotonicToTotal(calls, content.length);
    }
  });
});

describe("throttleProgress", () => {
  it("passes the first and last values and at most one per interval in between", () => {
    let now = 0;
    const seen: number[] = [];
    const p = throttleProgress(
      (d) => seen.push(d),
      100,
      () => now,
    );
    p(0, 10);
    for (let i = 1; i < 10; i++) {
      now += 30;
      p(i, 10);
    }
    p(10, 10);
    expect(seen[0]).toBe(0);
    expect(seen[seen.length - 1]).toBe(10);
    expect(seen).toEqual([0, 4, 8, 10]);
  });
});
