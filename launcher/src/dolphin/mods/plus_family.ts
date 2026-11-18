import { FatImage } from "@brawl_assets/fat32";
import type { PluginVariant } from "@dolphin/install/paths";
import { PLUGIN_FILE_NAME, PLUGIN_VARIANTS } from "@dolphin/install/paths";
import crypto from "crypto";
import fs from "fs";
import path from "path";

import catalog from "../../../../server/config/games.json";

/**
 * Mods of Project+ that play in the P+ online flow (docs/design/pplus-mods.md): P+'s own online
 * menus, matchmaking, rooms and gameplay rollback, with our plugin on the mod's SD card, instead
 * of a Brawl Mods room (the whole machine rolled back, the launcher holding the room).
 *
 * A P+Ex build (Cobalt, Project+ Ex) boots from P+'s own launcher files and differs in its folder
 * on the card; REX boots from launcher files of its own (the game's `launcherPath`). So a build is
 * known by its card (that folder and its codesets) and the file it boots. The key below is the
 * one Dolphin sends to mm (Core/Online/GameBuild.h) and mm's games table lists
 * (server/config/games.json): only the releases listed there play, and two releases never meet.
 */
export type PlusFamilyBuild = {
  /** mm's id of the build (server/config/games.json). */
  id: string;
  name: string;
  /** Its folder on the SD card (P+'s is "Project+"). */
  folder: string;
  /** The hash parts of its keys, as mm's table lists them: one per file it may boot (P+'s Netplay
   * Launcher, which Play boots, and its Offline Launcher; or the build's own two). */
  codes: readonly string[];
  /** Which of our plugin's files goes on its card (the table's `plugin`; Project+'s own for none):
   * a build with another kind's plugin has its online modes locked by the plugin itself. */
  plugin: PluginVariant;
  /** It boots its own launcher (the game's `launcherPath`) instead of P+'s Netplay Launcher, and
   * plays online with Dolphin's MMU emulation off, whatever its own game settings say (REX ships
   * them on; docs/design/pplus-mods.md §3). */
  ownLauncher?: true;
};

/**
 * The supported builds: mm's games table itself (server/config/games.json, bundled with the
 * launcher), without its first entry, Project+, which Play starts. One list for mm and the
 * launcher: a build is added there (docs/design/pplus-mods.md section 6). What mm alone reads of
 * an entry (its stage lists, its modes, its layout) is left out.
 */
export const PLUS_FAMILY_BUILDS: readonly PlusFamilyBuild[] = (
  catalog.games as {
    id: string;
    name: string;
    folder: string;
    codes?: string[];
    ownLauncher?: boolean;
    plugin?: string;
  }[]
)
  .slice(1)
  .map((g) => ({
    id: g.id,
    name: g.name,
    folder: g.folder,
    codes: g.codes ?? [],
    plugin: pluginVariant(g.plugin),
    ...(g.ownLauncher ? { ownLauncher: true as const } : {}),
  }));

/** An entry's `plugin` as one of the plugin's variants (Project+'s own for none; mm checks the name). */
function pluginVariant(name: string | undefined): PluginVariant {
  return PLUGIN_VARIANTS.find((v) => v === name) ?? "pplus";
}

/** The codesets of a build's folder, in the key's order. */
export const CODESETS = ["NETPLAY.GCT", "NETBOOST.GCT", "RSBE01.GCT", "BOOST.GCT"] as const;

const sha256 = (data: Buffer | string) => crypto.createHash("sha256").update(data).digest("hex");

/**
 * The key of a build: `<folder>:<32 hex digits>`, the digits from the SHA-256 of "boot=<sha256>\n"
 * and "<NAME>=<sha256>\n" per codeset ("-" for a file that is not there). Empty for no folder.
 */
export function gameKey(folder: string, bootSha256: string | null, codesetSha256: readonly (string | null)[]): string {
  if (!folder) {
    return "";
  }
  const text =
    `boot=${bootSha256 || "-"}\n` + CODESETS.map((name, i) => `${name}=${codesetSha256[i] || "-"}\n`).join("");
  return `${folder}:${sha256(text).slice(0, 32)}`;
}

/** Where our plugin goes on a build's card (sy_core loads every plugin of the folder). */
export function plusFamilyPluginPath(folder: string): string {
  return `/${folder}/pf/plugins/${PLUGIN_FILE_NAME}`;
}

/** The launcher-managed copy of a mod's card with the plugin: `<netplay>/pponline-sd-<id>`. */
export function plusFamilySdCardFolder(netplayDir: string, id: string): string {
  return path.join(netplayDir, `pponline-sd-${id}`);
}

/** A supported build's card, its key and the file it boots. */
export type PlusFamilyCard = { build: PlusFamilyBuild; key: string; bootFile: string };

/**
 * Which supported mod of P+ a game is, or null: no card, a card that can't be read, P+'s own card,
 * another mod, or another release of a supported one. The game's card (`sdPath`) booted from P+'s
 * Netplay Launcher (`pplusBootFile`), or for a build with launchers of its own from the game's
 * (`launcherPath`). The files are only read.
 */
export async function identifyPlusFamilyCard(
  game: { sdPath: string | null; launcherPath?: string | null },
  pplusBootFile: string,
): Promise<PlusFamilyCard | null> {
  if (!game.sdPath) {
    return null;
  }
  let fat: FatImage | null = null;
  try {
    const bootSha = new Map<string, string>();
    fat = await FatImage.open(game.sdPath);
    for (const build of PLUS_FAMILY_BUILDS) {
      const bootFile = build.ownLauncher ? game.launcherPath : pplusBootFile;
      if (!bootFile) {
        continue;
      }
      const dir = await fat.stat(`/${build.folder}`);
      if (!dir?.isDirectory) {
        continue;
      }
      if (!bootSha.has(bootFile)) {
        bootSha.set(bootFile, sha256(await fs.promises.readFile(bootFile)));
      }
      const codesets: (string | null)[] = [];
      for (const name of CODESETS) {
        const entry = await fat.stat(`/${build.folder}/${name}`);
        codesets.push(entry && !entry.isDirectory ? sha256(await fat.readFile(`/${build.folder}/${name}`)) : null);
      }
      const key = gameKey(build.folder, bootSha.get(bootFile)!, codesets);
      if (build.codes.some((code) => key === `${build.folder}:${code}`)) {
        return { build, key, bootFile };
      }
    }
    return null;
  } catch {
    return null;
  } finally {
    await fat?.close().catch(() => undefined);
  }
}

/**
 * The mod a replay was recorded on, from its game build (`<folder>:<hash>`, replay_format.ts
 * `gameBuild`): the supported build's name, else the folder; null for Project+ itself and for
 * recordings that name no build.
 */
export function replayModName(gameBuild: string | null | undefined): string | null {
  const at = gameBuild?.indexOf(":") ?? -1;
  if (!gameBuild || at <= 0) {
    return null;
  }
  const folder = gameBuild.slice(0, at);
  if (folder === "Project+") {
    return null;
  }
  const code = gameBuild.slice(at + 1);
  return PLUS_FAMILY_BUILDS.find((b) => b.folder === folder && b.codes.includes(code))?.name ?? folder;
}

/**
 * What a replay plays on, from its header: `release` null is the launcher's own Project+ release
 * (`current`), else the release or the mod it was recorded on. `mod`: a mod of Project+'s, which
 * no card of the playback Dolphin has, so it is not played or rendered at all, on any release.
 */
export function replayPlaysOn(
  meta: { gameBuild?: string | null; pplusRelease: string | null },
  current: string,
): { release: string | null; mod: boolean } {
  const mod = replayModName(meta.gameBuild);
  if (mod != null) {
    return { release: mod, mod: true };
  }
  return { release: meta.pplusRelease === current ? null : meta.pplusRelease, mod: false };
}

/**
 * Dolphin's game settings for a build that boots its own launcher (`ownLauncher`), written into
 * the User folder before it starts: `GameSettings/ID-<boot file's name>.ini` (Dolphin names an
 * executable's settings so, ConfigManager.cpp) with MMU emulation off. A game settings file
 * outranks the command line, and REX's own turn it on: its online play runs with it off (the
 * game plugin's Wi-Fi bypass reads through null pointers that only an MMU-off Dolphin forgives).
 * Null for any other build.
 */
export function plusFamilyGameSettings(card: PlusFamilyCard): { file: string; text: string } | null {
  if (!card.build.ownLauncher) {
    return null;
  }
  const name = path.basename(card.bootFile);
  const dot = name.lastIndexOf(".");
  return {
    file: path.join("GameSettings", `ID-${dot > 0 ? name.slice(0, dot) : name}.ini`),
    text: "[Core]\nCPUThread = True\nMMU = False\n",
  };
}

/** The supported build with mm's id `id`, or null. */
export function plusFamilyBuild(id: string): PlusFamilyBuild | null {
  return PLUS_FAMILY_BUILDS.find((b) => b.id === id) ?? null;
}
