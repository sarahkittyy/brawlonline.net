import { BRAWL_NTSC_U_DISC } from "../brawl_disc";

/**
 * Names for common discs and mods (docs/design/brawl-mods.md §4). Only names: an unknown disc or
 * mod plays the same way, under the name the player gave it.
 *
 * Discs by their part in a build (`disc_info.ts` discIdentity: a Wii disc's game data, in any
 * format); mods by the SHA-256 of their launcher file (their SD card is not part of a build: rooms
 * play the host's).
 */

export const KNOWN_DISCS: Readonly<Record<string, string>> = {
  [BRAWL_NTSC_U_DISC]: "Super Smash Bros. Brawl (USA)",
  "8703334493028a1dbd8a72ce59914b2eed8d2ddd": "New Super Mario Bros. Wii (USA, Rev 2)",
  "479693e6d3100011f5941e40afaa6143e8750c1a": "Mario Kart Wii (USA)",
};

/** The launcher's SHA-256 -> the mod's name. */
export const KNOWN_MODS: Readonly<Record<string, string>> = {
  // Brawl Minus v5.1.0 (Dolphin), as downloaded: Launcher/Minus Launcher.dol.
  aa96c0e3d5589fb09663231a41e6368bc8fd1bab6a7bcdfd44f05e7c0ecc0793: "Brawl Minus 5.1.0",
  // REX Release 0½ Hotfix 1 (Dolphin), as downloaded: user/Launcher/*.elf.
  "405d964ad21fdd3b7eaadc3043baae763e64a0ff1fe785155162943a3600df38": "REX 0½ Hotfix 1",
  "5a8a5364e60aca38f06846bf1ba38dd68a8b3a54f1a4dd0005a2e62a8bcb9048": "REX 0½ Hotfix 1 (offline launcher)",
};

export function knownDiscName(discSha1: string | null | undefined): string | null {
  return (discSha1 && KNOWN_DISCS[discSha1]) || null;
}

export function knownModName(launcherSha256: string | null | undefined): string | null {
  return (launcherSha256 && KNOWN_MODS[launcherSha256]) || null;
}
