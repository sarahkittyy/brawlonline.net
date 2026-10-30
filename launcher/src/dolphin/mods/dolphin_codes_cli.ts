// Development CLI: makes dolphin_codes.json, Dolphin's own Gecko and Action Replay codes for each
// game (the Codes dialog's "Dolphin" list, docs/design/room-codes.md). It keeps only the code
// sections of a Dolphin checkout's Data/Sys/GameSettings inis, by file name (GP6E01, GALE01r2,
// GP6): the game settings themselves are not ours to ship (rooms pin their own, MachineSession).
//
//   npx ts-node --transpile-only src/dolphin/mods/dolphin_codes_cli.ts <Data/Sys/GameSettings> [--out <file>]
//
// SPDX-License-Identifier: GPL-3.0-or-later

import fs from "fs";
import path from "path";

const SECTIONS = new Set(["Gecko", "Gecko_Enabled", "ActionReplay", "ActionReplay_Enabled"]);

function codeSections(text: string): string {
  const out: string[] = [];
  let keep = false;
  for (const raw of text.split(/\r?\n/)) {
    const line = raw.trimEnd();
    const header = /^\[([^\]]+)\]$/.exec(line.trim());
    if (header) {
      keep = SECTIONS.has(header[1]);
      if (keep) {
        out.push(`[${header[1]}]`);
      }
      continue;
    }
    if (keep && line.trim() && !line.trim().startsWith("#")) {
      out.push(line.trim());
    }
  }
  // Only a code section header and nothing in it: nothing.
  return out.some((l) => !l.startsWith("[")) ? out.join("\n") : "";
}

function main() {
  const args = process.argv.slice(2);
  const dir = args.find((a) => !a.startsWith("--"));
  const outAt = args.indexOf("--out");
  const out = outAt >= 0 ? args[outAt + 1] : path.join(__dirname, "dolphin_codes.json");
  if (!dir) {
    throw new Error("usage: dolphin_codes_cli.ts <Data/Sys/GameSettings> [--out <file>]");
  }
  const games: Record<string, string> = {};
  for (const file of fs.readdirSync(dir).sort()) {
    if (!file.toLowerCase().endsWith(".ini")) {
      continue;
    }
    const text = codeSections(fs.readFileSync(path.join(dir, file), "utf8"));
    if (text) {
      games[file.slice(0, -4)] = text;
    }
  }
  fs.writeFileSync(out, JSON.stringify(games, null, 0) + "\n");
  console.log(`${Object.keys(games).length} games with codes -> ${out}`);
}

main();
