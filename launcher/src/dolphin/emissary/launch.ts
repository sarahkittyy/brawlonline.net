// Starting EmissaryRecomp for Play (docs/design/brawl-recompiled.md §8.2): the same game the
// netplay Dolphin boots, set up in the recomp's own UserData before each start.
//
// - the online SD card (`pponline-sd/sd.raw`: P+ with our plugin) as `UserData/sd.raw`, a hard
//   link (never a copy of 2 GB), opened read-only (BRAWL_SD_VOLATILE_WRITES: the game's writes stay
//   in memory, as Dolphin's WiiSDCardAllowWrites=False);
// - P+'s netplay codeset from that card (`/Project+/NETPLAY.GCT` + `gc.txt`) as
//   `UserData/Codes/RSBE01.gct`: the recomp installs codes itself, there is no launcher DOL;
// - the Brawl save template (`<netplay>/pplus/NetplaySave`, the one Dolphin's netplay NAND gets) as
//   `UserData/NAND`, fresh each start: unlocks and rules are part of the match setup both
//   engines compare;
// - Config.toml: the extracted disc, and the game's own 16:9 mode (Dolphin pins SYSCONF IPL.AR on;
//   the game frames its camera, and screen KOs, by it), whatever the player chose in F10;
// - BRAWL_ONLINE_CORE: the netplay Dolphin's User folder (Config/Dolphin.ini with the [Online]
//   settings, Online/ with the account and the room hand-off files, Logs/, the replays), which the
//   recomp's in-process Brawl Online core reads as Dolphin does (docs/design/brawl-online-core.md
//   §10). Without it the recomp plays no one online.
//
// SPDX-License-Identifier: GPL-3.0-or-later

import { FatImage } from "@brawl_assets/fat32";
import fs from "fs";
import path from "path";
import { removeTree } from "utils/remove_tree";

export const NETPLAY_CODESET = "/Project+/NETPLAY.GCT";
export const NETPLAY_GECKO_CONFIG = "/Project+/gc.txt";

export type EmissaryLaunchInput = {
  /** The recomp's UserData (`<emissary-recomp>/UserData`). */
  userData: string;
  /** The extracted disc (`.../game/DATA`). */
  gameFolder: string;
  /** The online card the netplay Dolphin would boot (`pponline-sd/sd.raw`). */
  sdCardImage: string;
  /** The Brawl save template folder (`<netplay>/pplus/NetplaySave`), holding `title/...`. */
  netplaySave: string;
  /** The netplay Dolphin's User folder: the core's (BRAWL_ONLINE_CORE). */
  onlineUserFolder: string;
};

/** The User folder's Config/ files of the `-C` systems the core reads (its config layer: Dolphin.ini, GFX.ini, Logger.ini). */
const CONFIG_FILES: Record<string, string> = { Dolphin: "Dolphin.ini", GFX: "GFX.ini", Logger: "Logger.ini" };

export type ConfigIniOverride = { file: string; section: string; key: string; value: string };

/**
 * The `-C <System>.<Section>.<Key>=<Value>` settings of a Dolphin command line, which the recomp's
 * core can't be given (it reads the User folder's Config/*.ini only): the launcher writes them into
 * those files before a recomp starts: the logging Dolphin gets on every Play (LOGGING_CONFIG_ARGS,
 * so the core writes Logs/dolphin.log), and in test mode the extra arguments (the staging servers,
 * PPO_DOLPHIN_EXTRA_ARGS). Other systems (SYSCONF.) are left out.
 */
export function configIniOverrides(args: readonly string[]): ConfigIniOverride[] {
  const out: ConfigIniOverride[] = [];
  for (let i = 0; i + 1 < args.length; i++) {
    if (args[i] !== "-C" && args[i] !== "--config") {
      continue;
    }
    const m = /^([^.=]+)\.([^.=]+)\.([^=]+)=(.*)$/.exec(args[i + 1]);
    if (m && CONFIG_FILES[m[1]]) {
      out.push({ file: CONFIG_FILES[m[1]], section: m[2], key: m[3], value: m[4] });
    }
    i++;
  }
  return out;
}

/** Sets `key = value` in `[section]` of a TOML text (adding either), leaving every other line as it was. */
export function setTomlValue(text: string, section: string, key: string, value: string): string {
  const lines = text.length ? text.split(/\r?\n/) : [];
  let start = -1;
  for (let i = 0; i < lines.length; i++) {
    if (lines[i].trim() === `[${section}]`) {
      start = i;
      break;
    }
  }
  const entry = `${key} = ${value}`;
  if (start < 0) {
    while (lines.length && lines[lines.length - 1].trim() === "") {
      lines.pop();
    }
    if (lines.length) {
      lines.push("");
    }
    lines.push(`[${section}]`, entry, "");
    return lines.join("\n");
  }
  let end = lines.length;
  for (let i = start + 1; i < lines.length; i++) {
    if (/^\s*\[/.test(lines[i])) {
      end = i;
      break;
    }
  }
  const keyLine = new RegExp(`^\\s*${key.replace(/[.*+?^${}()|[\]\\]/g, "\\$&")}\\s*=`);
  for (let i = start + 1; i < end; i++) {
    if (keyLine.test(lines[i])) {
      lines[i] = entry;
      return lines.join("\n");
    }
  }
  let at = end;
  while (at > start + 1 && lines[at - 1].trim() === "") {
    at--;
  }
  lines.splice(at, 0, entry);
  return lines.join("\n");
}

/** A TOML basic string. */
export const tomlString = (s: string) => JSON.stringify(s);

/** The Config.toml the recomp starts with: the player's own, with what online play needs set. */
export function emissaryConfig(existing: string, gameFolder: string): string {
  let text = setTomlValue(existing, "paths", "dvd_root", tomlString(gameFolder.replace(/\\/g, "/")));
  text = setTomlValue(text, "video", "widescreen", "true");
  // 0 is the original 4:3, which turns the game's widescreen mode off; any other shows 16:9 or more.
  if (/^\s*aspect_mode\s*=\s*0\s*$/m.test(text)) {
    text = setTomlValue(text, "video", "aspect_mode", "1");
  }
  return text.endsWith("\n") ? text : text + "\n";
}

async function linkOrReplace(target: string, link: string) {
  await fs.promises.rm(link, { force: true });
  try {
    await fs.promises.link(target, link);
  } catch (err) {
    throw new Error(
      `Could not link the SD card into EmissaryRecomp's folder (${err instanceof Error ? err.message : err}). ` +
        "It must be on the same drive as the game files.",
    );
  }
}

/** Prepares `input.userData` for a start; returns the environment the program needs. */
export async function prepareEmissaryLaunch(input: EmissaryLaunchInput): Promise<Record<string, string>> {
  const { userData } = input;
  await fs.promises.mkdir(path.join(userData, "Codes"), { recursive: true });

  const config = path.join(userData, "Config.toml");
  const existing = await fs.promises.readFile(config, "utf8").catch(() => "");
  await fs.promises.writeFile(config, emissaryConfig(existing, input.gameFolder));

  await linkOrReplace(input.sdCardImage, path.join(userData, "sd.raw"));

  const card = await FatImage.open(input.sdCardImage);
  try {
    await fs.promises.writeFile(path.join(userData, "Codes", "RSBE01.gct"), await card.readFile(NETPLAY_CODESET));
    await fs.promises.writeFile(path.join(userData, "Codes", "gc.txt"), await card.readFile(NETPLAY_GECKO_CONFIG));
  } finally {
    await card.close();
  }

  const nand = path.join(userData, "NAND");
  await removeTree(nand);
  await fs.promises.cp(input.netplaySave, nand, { recursive: true });

  return { BRAWL_SD_VOLATILE_WRITES: "1", BRAWL_ONLINE_CORE: path.resolve(input.onlineUserFolder) };
}
