import fs from "fs";
import { async as AsyncStreamZip } from "node-stream-zip";
import os from "os";
import path from "path";
import { afterEach, beforeEach, describe, expect, it } from "vitest";

import {
  dolphinUserFiles,
  emissaryFiles,
  folderFiles,
  logsArchiveBuffer,
  redactDolphinIni,
  writeLogsArchive,
} from "./logs_archive";

async function readZip(zipPath: string): Promise<Record<string, string>> {
  const zip = new AsyncStreamZip({ file: zipPath });
  try {
    const out: Record<string, string> = {};
    for (const entry of Object.values(await zip.entries())) {
      out[entry.name] = (await zip.entryData(entry.name)).toString("utf-8");
    }
    return out;
  } finally {
    await zip.close();
  }
}

describe("logs archive", () => {
  let dir: string;
  const write = async (rel: string, text: string) => {
    const file = path.join(dir, rel);
    await fs.promises.mkdir(path.dirname(file), { recursive: true });
    await fs.promises.writeFile(file, text);
    return file;
  };

  beforeEach(async () => {
    dir = await fs.promises.mkdtemp(path.join(os.tmpdir(), "logs-archive-"));
  });

  afterEach(async () => {
    await fs.promises.rm(dir, { recursive: true, force: true });
  });

  it("removes only the analytics ID from Dolphin.ini", () => {
    const ini = "[Core]\r\nID = keep\r\n[Analytics]\r\nEnabled = False\r\nID = be3ed531\r\n[NetPlay]\r\nID = keep\r\n";
    expect(redactDolphinIni(ini)).toBe(
      "[Core]\r\nID = keep\r\n[Analytics]\r\nEnabled = False\r\nID = (removed)\r\n[NetPlay]\r\nID = keep\r\n",
    );
  });

  it("zips a Dolphin User folder's logs and configs, without secrets", async () => {
    const user = path.join(dir, "User");
    await write("User/Logs/dolphin.log", "this run\n");
    await write("User/Logs/dolphin.prev.log", "last run\n");
    await write("User/Config/Dolphin.ini", "[Analytics]\nID = abc\n");
    await write("User/Config/GFX.ini", "[Settings]\n");
    await write("User/Config/RetroAchievements.ini", "[Achievements]\nApiToken = secret\n");
    await write("User/Online/user.json", '{"playKey":"secret"}');
    await write("launcher/main.log", "launcher\n");

    const zipPath = path.join(dir, "logs.zip");
    await writeLogsArchive(zipPath, [
      { name: "system.txt", source: { text: "Launcher: 1.0\n" } },
      ...(await folderFiles(path.join(dir, "launcher"), "launcher")),
      ...(await folderFiles(path.join(dir, "missing"), "missing")),
      ...(await dolphinUserFiles(user, "netplay-dolphin")),
    ]);

    expect(await readZip(zipPath)).toEqual({
      "system.txt": "Launcher: 1.0\n",
      "launcher/main.log": "launcher\n",
      "netplay-dolphin/Logs/dolphin.log": "this run\n",
      "netplay-dolphin/Logs/dolphin.prev.log": "last run\n",
      "netplay-dolphin/Config/Dolphin.ini": "[Analytics]\nID = (removed)\n",
      "netplay-dolphin/Config/GFX.ini": "[Settings]\n",
    });
    expect(fs.existsSync(`${zipPath}.partial`)).toBe(false);
  });

  it("keeps the end of a big file and skips files that are gone", async () => {
    const big = await write("big.log", "old old old\nnew lines\n");
    const zipPath = path.join(dir, "logs.zip");
    await writeLogsArchive(
      zipPath,
      [
        { name: "big.log", source: { path: big } },
        { name: "empty.log", source: { path: await write("empty.log", "") } },
        { name: "gone.log", source: { path: path.join(dir, "gone.log") } },
      ],
      10,
    );
    expect(await readZip(zipPath)).toEqual({ "big.log": "new lines\n", "empty.log": "" });
  });

  it("makes the same zip in memory, for a bug report", async () => {
    const big = await write("big.log", "old old old\nnew lines\n");
    const zip = await logsArchiveBuffer(
      [
        { name: "system.txt", source: { text: "Launcher: 1.0\n" } },
        { name: "big.log", source: { path: big } },
      ],
      10,
    );
    const zipPath = path.join(dir, "memory.zip");
    await fs.promises.writeFile(zipPath, zip);
    expect(await readZip(zipPath)).toEqual({ "system.txt": "Launcher: 1.0\n", "big.log": "new lines\n" });
  });

  it("has EmissaryRecomp's last runs and its core's dumps whole", async () => {
    const userData = path.join(dir, "emissary-recomp", "UserData");
    const netplay = path.join(dir, "User");
    const setTime = async (p: string, seconds: number) => fs.promises.utimes(p, seconds, seconds);
    for (let i = 1; i <= 4; i++) {
      await write(`emissary-recomp/UserData/Logs/brawl_${i}_pid${i}/console.log`, `run ${i}\n`);
      await setTime(path.join(userData, "Logs", `brawl_${i}_pid${i}`), 1_000_000 + i);
      await write(`User/Logs/recomp-${i}.dmp`, i === 4 ? "x".repeat(50) : `dump ${i}`);
      await setTime(path.join(netplay, "Logs", `recomp-${i}.dmp`), 1_000_000 + i);
    }
    await write("emissary-recomp/UserData/Config.toml", "[audio]\nmuted = true\n");
    await write("User/Logs/dolphin.log", "dolphin\n");

    const files = await emissaryFiles(userData, netplay, "emissary-recomp", 20);
    expect(files.map((f) => f.name).sort()).toEqual([
      "emissary-recomp/Config.toml",
      // The newest three runs; the newest dump is too big to go in whole.
      "emissary-recomp/Logs/brawl_2_pid2/console.log",
      "emissary-recomp/Logs/brawl_3_pid3/console.log",
      "emissary-recomp/Logs/brawl_4_pid4/console.log",
      "emissary-recomp/dumps/recomp-2.dmp",
      "emissary-recomp/dumps/recomp-3.dmp",
    ]);
    // Not again (cut to its end) with the netplay Dolphin's logs.
    expect((await dolphinUserFiles(netplay, "netplay-dolphin")).map((f) => f.name)).toEqual([
      "netplay-dolphin/Logs/dolphin.log",
    ]);
    expect(await emissaryFiles(path.join(dir, "none"), path.join(dir, "none"), "x", 20)).toEqual([]);
  });
});
