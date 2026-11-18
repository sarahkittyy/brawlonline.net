/**
 * Custom textures in the playback Dolphin (replays, highlights, spectating, renders). It has its
 * own User folder (`<userData>/playback/User`, render: `<userData>/render/User`), so it read HD
 * textures only from that folder's `Load/Textures`, which starts empty, with Graphics' custom
 * textures off by default: a player's texture pack showed in their games and never in their
 * replays. Now, when the netplay Dolphin has custom textures on, the playback Dolphin reads the
 * netplay User folder's `Load` (`General.LoadPath`) with custom textures on: the same pack the
 * game was played with. Both are command-line settings (netplay_args.ts), never written to an ini.
 */

import { readdir } from "fs/promises";
import path from "path";

import { readHiresTexturesSetting } from "./mods/mod_textures";

export type PlaybackTextures = { loadPath: string };

async function hasEntries(dir: string): Promise<boolean> {
  return (await readdir(dir).catch(() => [])).length > 0;
}

/**
 * The netplay User folder's `Load` when the netplay Dolphin has custom textures on (its GFX.ini;
 * Dolphin's default is off), else null. Also null when the playback User folder has textures of
 * its own in its `Load/Textures` (a player who set up the playback Dolphin by hand keeps it) or is
 * the netplay one (it reads that `Load` anyway).
 */
export async function findPlaybackTextures(
  netplayUserFolder: string,
  playbackUserFolder: string,
): Promise<PlaybackTextures | null> {
  const netplayUser = path.resolve(netplayUserFolder);
  const playbackUser = path.resolve(playbackUserFolder);
  if (path.relative(netplayUser, playbackUser) === "") {
    return null;
  }
  if ((await readHiresTexturesSetting(netplayUser)) !== true) {
    return null;
  }
  if (await hasEntries(path.join(playbackUser, "Load", "Textures"))) {
    return null;
  }
  return { loadPath: path.join(netplayUser, "Load") };
}
