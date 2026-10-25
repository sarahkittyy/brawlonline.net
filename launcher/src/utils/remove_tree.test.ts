import fs from "fs";
import os from "os";
import path from "path";
import { describe, expect, it } from "vitest";

import { removeTree } from "./remove_tree";

const held = (code: string) => Object.assign(new Error(`${code}: resource busy or locked`), { code });

describe("removeTree", () => {
  it("deletes a tree", async () => {
    const root = await fs.promises.mkdtemp(path.join(os.tmpdir(), "remove-tree-"));
    await fs.promises.mkdir(path.join(root, "a", "b", "c"), { recursive: true });
    await fs.promises.writeFile(path.join(root, "a", "b", "c", "f.bin"), "x");
    await removeTree(root);
    expect(fs.existsSync(root)).toBe(false);
  });

  it("tries again while a file is held, and only from the top", async () => {
    const calls: fs.RmOptions[] = [];
    await removeTree("x", {
      delayMs: 1,
      rm: async (_p, options) => {
        calls.push(options);
        if (calls.length < 3) {
          throw held("EBUSY");
        }
      },
    });
    expect(calls).toHaveLength(3);
    // Node's own retries run in every folder of the tree (bug report #148).
    expect(calls.every((o) => o.maxRetries === undefined)).toBe(true);
  });

  it("gives up after `tries` with the last error", async () => {
    let calls = 0;
    const removing = removeTree("x", {
      tries: 4,
      delayMs: 1,
      rm: async () => {
        calls += 1;
        throw held("EPERM");
      },
    });
    await expect(removing).rejects.toMatchObject({ code: "EPERM" });
    expect(calls).toBe(4);
  });

  it("does not try again on other errors", async () => {
    let calls = 0;
    const removing = removeTree("x", {
      delayMs: 1,
      rm: async () => {
        calls += 1;
        throw held("EIO");
      },
    });
    await expect(removing).rejects.toMatchObject({ code: "EIO" });
    expect(calls).toBe(1);
  });
});
