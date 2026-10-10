import fs from "fs";
import { async as AsyncStreamZip } from "node-stream-zip";
import os from "os";
import path from "path";
import { afterEach, beforeEach, describe, expect, it } from "vitest";

import { dolphinUserFiles, folderFiles, redactDolphinIni, writeLogsArchive } from "./logs_archive";

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
});
