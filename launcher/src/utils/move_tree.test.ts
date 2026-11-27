import fs from "fs";
import os from "os";
import path from "path";
import { describe, expect, it } from "vitest";

import { moveTree } from "./move_tree";

const held = (code: string) => Object.assign(new Error(`${code}: operation not permitted, rename`), { code });

async function tree(): Promise<{ root: string; from: string; to: string }> {
  const root = await fs.promises.mkdtemp(path.join(os.tmpdir(), "move-tree-"));
  const from = path.join(root, "v1.partial", "EmissaryRecomp");
  await fs.promises.mkdir(path.join(from, "bin"), { recursive: true });
  await fs.promises.writeFile(path.join(from, "WiiCompiled.exe"), "exe");
  await fs.promises.writeFile(path.join(from, "bin", "a.dll"), "dll");
  return { root, from, to: path.join(root, "v1") };
}

describe("moveTree", () => {
  it("renames a folder", async () => {
    const { from, to } = await tree();
    await moveTree(from, to);
    expect(fs.readFileSync(path.join(to, "bin", "a.dll"), "utf8")).toBe("dll");
    expect(fs.existsSync(from)).toBe(false);
  });

  it("tries again while a file in it is held", async () => {
    const { from, to } = await tree();
    let calls = 0;
    await moveTree(from, to, {
      delayMs: 1,
      rename: async (a, b) => {
        calls += 1;
        if (calls < 3) {
          throw held("EPERM");
        }
        await fs.promises.rename(a, b);
      },
    });
    expect(calls).toBe(3);
    expect(fs.existsSync(path.join(to, "WiiCompiled.exe"))).toBe(true);
  });

  it("gives up with the rename's error without copyFallback", async () => {
    const { from, to } = await tree();
    let calls = 0;
    const moving = moveTree(from, to, {
      tries: 4,
      delayMs: 1,
      rename: async () => {
        calls += 1;
        throw held("EPERM");
      },
    });
    await expect(moving).rejects.toMatchObject({ code: "EPERM" });
    expect(calls).toBe(4);
    expect(fs.existsSync(to)).toBe(false);
  });

  it("copies the folder when the rename keeps failing (an antivirus holds the new exe)", async () => {
    const { from, to } = await tree();
    const logged: string[] = [];
    await moveTree(from, to, {
      tries: 2,
      delayMs: 1,
      copyFallback: true,
      log: (m) => logged.push(m),
      rename: async () => {
        throw held("EPERM");
      },
    });
    expect(fs.readFileSync(path.join(to, "WiiCompiled.exe"), "utf8")).toBe("exe");
    expect(fs.readFileSync(path.join(to, "bin", "a.dll"), "utf8")).toBe("dll");
    expect(fs.existsSync(from)).toBe(false);
    expect(logged.some((m) => m.includes("copying it instead"))).toBe(true);
  });

  it("leaves no half copy when the copy fails", async () => {
    const { from, to } = await tree();
    const moving = moveTree(from, to, {
      tries: 1,
      copyFallback: true,
      rename: async () => {
        throw held("EPERM");
      },
      cp: async () => {
        await fs.promises.mkdir(to);
        throw held("EIO");
      },
    });
    await expect(moving).rejects.toMatchObject({ code: "EIO" });
    expect(fs.existsSync(to)).toBe(false);
    expect(fs.existsSync(from)).toBe(true);
  });

  it("does not try again on other errors", async () => {
    let calls = 0;
    const moving = moveTree("x", "y", {
      delayMs: 1,
      copyFallback: true,
      rename: async () => {
        calls += 1;
        throw held("ENOENT");
      },
    });
    await expect(moving).rejects.toMatchObject({ code: "ENOENT" });
    expect(calls).toBe(1);
  });
});
