import { diskSpaceMessage, formatBytes, isDiskSpaceError, withoutIpcPrefix } from "@common/disk_space";
import fs from "fs";
import os from "os";
import path from "path";
import { describe, expect, it } from "vitest";

import {
  asDiskSpaceError,
  DISK_SPACE_MARGIN,
  diskPlace,
  DiskSpaceError,
  ensureFreeSpace,
  freeDiskBytes,
  isDiskFull,
} from "./disk_space";

const enospc = (p?: string) =>
  Object.assign(new Error(`ENOSPC: no space left on device, write`), { code: "ENOSPC", ...(p ? { path: p } : {}) });

describe("disk-full messages", () => {
  it("name the drive, what needs the room, the sizes, and what to do", () => {
    expect(diskSpaceMessage({ what: "Dolphin", place: "C:", needBytes: 4.2e9, freeBytes: 1.25e9 })).toBe(
      "Not enough disk space on C: for Dolphin: 4.2 GB needed, 1.3 GB free. Free up space on C:, then restart the launcher.",
    );
    expect(diskSpaceMessage({ what: "Dolphin's set-up", place: "C:", needBytes: null, freeBytes: 12e6 })).toBe(
      "Not enough disk space on C: for Dolphin's set-up: the disk is full (12 MB free). Free up space on C:, then restart the launcher.",
    );
    expect(diskSpaceMessage({ what: "the game", place: "D:", needBytes: null, freeBytes: null })).toBe(
      "Not enough disk space on D: for the game: the disk is full. Free up space on D:, then restart the launcher.",
    );
  });

  it("are recognised through IPC, and the IPC prefix comes off", () => {
    const ipc = new Error(
      "Error invoking remote method 'downloadDolphin': DiskSpaceError: Not enough disk space on C: for Dolphin: 1 GB needed",
    );
    expect(isDiskSpaceError(ipc)).toBe(true);
    expect(isDiskSpaceError(new Error("Error invoking remote method 'downloadDolphin': Error: EPERM"))).toBe(false);
    expect(isDiskSpaceError(undefined)).toBe(false);
    expect(withoutIpcPrefix(ipc.message)).toBe("Not enough disk space on C: for Dolphin: 1 GB needed");
    expect(withoutIpcPrefix("Error invoking remote method 'x': Error: No netplay Dolphin found")).toBe(
      "No netplay Dolphin found",
    );
    expect(withoutIpcPrefix("Dolphin is already open")).toBe("Dolphin is already open");
    expect(formatBytes(2114556060)).toBe("2.1 GB");
  });
});

describe("diskPlace", () => {
  it("is the drive letter on Windows and the folder's disk elsewhere", () => {
    expect(diskPlace(String.raw`C:\Users\p\AppData\Roaming\Brawl Online\netplay`, "win32")).toBe("C:");
    expect(diskPlace("e:/games/netplay", "win32")).toBe("E:");
    const share = String.raw`\\server\share\netplay`;
    expect(diskPlace(share, "win32")).toBe(`the disk of ${share}`);
    expect(diskPlace("/home/p/.config/Brawl Online/netplay", "linux")).toBe(
      "the disk of /home/p/.config/Brawl Online/netplay",
    );
  });
});

describe("ensureFreeSpace", () => {
  it("refuses below the need plus the margin, and passes at it or when the space is unknown", async () => {
    const need = 1e9;
    const dir = "/x";
    await expect(ensureFreeSpace(dir, need, "Dolphin", async () => need + DISK_SPACE_MARGIN - 1)).rejects.toThrow(
      DiskSpaceError,
    );
    await expect(ensureFreeSpace(dir, need, "Dolphin", async () => need + DISK_SPACE_MARGIN)).resolves.toBeUndefined();
    await expect(ensureFreeSpace(dir, need, "Dolphin", async () => null)).resolves.toBeUndefined();
    await expect(ensureFreeSpace(dir, 0, "Dolphin", async () => 0)).resolves.toBeUndefined();
  });

  it("reads the free space of a folder that does not exist yet from its parent", async () => {
    const free = await freeDiskBytes(path.join(os.tmpdir(), `no-such-${process.pid}`, "netplay", "User"));
    expect(free).toBeGreaterThan(0);
    expect(fs.existsSync(path.join(os.tmpdir(), `no-such-${process.pid}`))).toBe(false);
  });
});

describe("asDiskSpaceError", () => {
  it("words ENOSPC for the player and leaves other errors alone", async () => {
    const free = async () => 5e6;
    const dir = path.join(os.tmpdir(), "netplay");
    const out = await asDiskSpaceError(enospc(), dir, "Dolphin's set-up", { freeBytes: free });
    expect(out).toBeInstanceOf(DiskSpaceError);
    expect((out as Error).message).toContain("for Dolphin's set-up: the disk is full (5 MB free).");

    const sized = await asDiskSpaceError(enospc(path.join(dir, "sd.raw.partial")), dir, "the SD card copy", {
      needBytes: 2_097_152_000,
      freeBytes: free,
    });
    expect((sized as Error).message).toContain(`${formatBytes(2_097_152_000 + DISK_SPACE_MARGIN)} needed, 5 MB free`);

    const other = new Error("EPERM: operation not permitted");
    expect(await asDiskSpaceError(other, dir, "Dolphin", { freeBytes: free })).toBe(other);
    // Already worded: kept, not wrapped again.
    const worded = new DiskSpaceError("Not enough disk space on C: for Dolphin: 1 GB needed");
    expect(await asDiskSpaceError(worded, dir, "the game", { freeBytes: free })).toBe(worded);
  });

  it("knows ENOSPC by its code or its message", () => {
    expect(isDiskFull(enospc())).toBe(true);
    expect(isDiskFull(new Error("write ENOSPC"))).toBe(true);
    expect(isDiskFull(new Error("EIO: i/o error, read"))).toBe(false);
    expect(isDiskFull(null)).toBe(false);
  });
});
