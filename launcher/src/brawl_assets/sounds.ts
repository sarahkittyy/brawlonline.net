// Looks sound effects up in the disc's sound archive (and Project+'s SD card group replacements) and decodes them.
//
// SPDX-License-Identifier: GPL-3.0-or-later

import type { SoundCandidate } from "./catalog";
import { BRSAR_FILE } from "./catalog";
import type { Brsar, BrsarFileData, BrsarGroup, BrsarSound } from "./formats/brsar";
import { fileGroups, findSound, parseBrsar, readBrsarFile, SoundType } from "./formats/brsar";
import type { DecodedSound } from "./formats/rwsd";
import { decodeRwsdSound, isRwsd, parseRwsd } from "./formats/rwsd";
import type { Sawnd } from "./formats/sawnd";
import { parseSawnd, sawndFile, sawndPath } from "./formats/sawnd";

type Origin = "disc" | "sd";

export type LoadedSound = {
  sound: BrsarSound;
  group: BrsarGroup;
  origin: Origin;
  /** Where the bytes came from: the BRSAR path, or the .sawnd path on the SD card. */
  source: string;
  decoded: DecodedSound;
  durationMs: number;
};

type Archive = { buf: Buffer; brsar: Brsar };

export function soundDurationMs(d: DecodedSound): number {
  const frames = Math.max(0, ...d.channels.map((c) => c.length));
  return Math.round((frames / d.sampleRate) * 1000);
}

/**
 * Resolves sound candidates against the game files. `read` returns a file of the disc (relative to DATA/files) or of
 * the P+ SD card (relative to /Project+/pf/), or null; it should cache, as the archive is read more than once.
 */
export class SoundLibrary {
  private archives = new Map<Origin, Promise<Archive | null>>();
  private sawnds = new Map<number, Promise<Sawnd | null>>();

  constructor(private readonly read: (origin: Origin, file: string) => Promise<Buffer | null>) {}

  /** The parsed sound archive of a source (null if that source has none). */
  archive(origin: Origin): Promise<Archive | null> {
    let p = this.archives.get(origin);
    if (!p) {
      p = this.read(origin, BRSAR_FILE).then((buf) => (buf ? { buf, brsar: parseBrsar(buf) } : null));
      this.archives.set(origin, p);
    }
    return p;
  }

  /** P+'s replacement of a sound group (null when the SD card does not replace it). */
  private sawnd(group: BrsarGroup): Promise<Sawnd | null> {
    let p = this.sawnds.get(group.id);
    if (!p) {
      p = this.read("sd", sawndPath(group.id)).then((buf) => (buf ? parseSawnd(buf, group) : null));
      this.sawnds.set(group.id, p);
    }
    return p;
  }

  /**
   * Loads a candidate. "disc": the disc's archive. "sd": the sound's group as P+ replaces it (sfx/<group>.sawnd), or
   * an archive on the SD card itself; null when the SD card changes nothing for this sound.
   * Throws when the sound exists but cannot be decoded.
   */
  async load(c: SoundCandidate): Promise<LoadedSound | null> {
    const sdArchive = c.origin === "sd" ? await this.archive("sd") : null;
    const info = sdArchive ?? (await this.archive("disc"));
    if (!info) {
      return null;
    }
    const sound = findSound(info.brsar, c.sound);
    if (!sound) {
      return null;
    }
    if (sound.soundType !== SoundType.Wave) {
      throw new Error(`${c.sound} is not a wave sound`);
    }
    let data: BrsarFileData | null = null;
    let source = BRSAR_FILE;
    if (c.origin === "sd") {
      for (const group of fileGroups(info.brsar, sound.fileId)) {
        const sawnd = await this.sawnd(group);
        data = sawnd && sawndFile(sawnd, group.id, sound.fileId);
        if (data) {
          source = sawndPath(group.id);
          break;
        }
      }
      if (!data && sdArchive) {
        data = readBrsarFile(sdArchive.buf, sdArchive.brsar, sound.fileId);
      }
    } else {
      data = readBrsarFile(info.buf, info.brsar, sound.fileId);
    }
    if (!data) {
      return null;
    }
    if (!isRwsd(data.header)) {
      throw new Error(`${c.sound}: file ${sound.fileId} is not an RWSD`);
    }
    const decoded = decodeRwsdSound(parseRwsd(data.header), data.waveData, sound.index);
    return {
      sound,
      group: info.brsar.groups[data.groupId],
      origin: c.origin,
      source,
      decoded,
      durationMs: soundDurationMs(decoded),
    };
  }
}
