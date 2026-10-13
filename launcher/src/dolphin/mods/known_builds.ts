/**
 * Names for common discs and mods (docs/design/brawl-mods.md §4). Only names: an unknown disc or
 * mod plays the same way, under the name the player gave it.
 *
 * Discs by the SHA-1 of their contents (`disc_info.ts`, the dump hash); mods by the SHA-256 of
 * their launcher file and of their SD card image ("-" for none). A mod that writes to its own SD
 * card (playing it offline in its own Dolphin) gets another SD hash and is no longer named here.
 */

export const KNOWN_DISCS: Readonly<Record<string, string>> = {
  "0e95949ac585f357e79fcd34b20670b5dca97ac2": "Super Smash Bros. Brawl (USA, Rev 1)",
  "59432f150bf6b871ab378bb5b28e92005e0862f6": "Super Smash Bros. Brawl (USA, Rev 2)",
  "9ac7242a5feebe1d0a6e2775c21f482c471b2240": "New Super Mario Bros. Wii (USA, Rev 2)",
};

/** "<launcher sha256>|<sd sha256 or ->" -> the mod's name. */
export const KNOWN_MODS: Readonly<Record<string, string>> = {
  // Brawl Minus v5.1.0 (Dolphin), as downloaded: Launcher/Minus Launcher.dol, User/Wii/sd.raw.
  "aa96c0e3d5589fb09663231a41e6368bc8fd1bab6a7bcdfd44f05e7c0ecc0793|577a886ceb0a7f04f3f5c1f88de08a9bffd8d38e563965ff7d3dad79a2bdccce":
    "Brawl Minus 5.1.0",
  // REX Release 0½ Hotfix 1 (Dolphin), as downloaded: user/Launcher/*.elf, user/Wii/sd.raw.
  "405d964ad21fdd3b7eaadc3043baae763e64a0ff1fe785155162943a3600df38|e3e94c337f96c6265c655df046bb3652507f78d8509d85751d291a350fa3cdcb":
    "REX 0½ Hotfix 1",
  "5a8a5364e60aca38f06846bf1ba38dd68a8b3a54f1a4dd0005a2e62a8bcb9048|e3e94c337f96c6265c655df046bb3652507f78d8509d85751d291a350fa3cdcb":
    "REX 0½ Hotfix 1 (offline launcher)",
};

export function knownDiscName(discSha1: string | null | undefined): string | null {
  return (discSha1 && KNOWN_DISCS[discSha1]) || null;
}

export function knownModName(
  launcherSha256: string | null | undefined,
  sdSha256: string | null | undefined,
): string | null {
  if (!launcherSha256) {
    return null;
  }
  return KNOWN_MODS[`${launcherSha256}|${sdSha256 ?? "-"}`] ?? null;
}
