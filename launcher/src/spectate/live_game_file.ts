import { crc32 } from "@replays/replay_format";
import type { FileHandle } from "fs/promises";
import { open } from "fs/promises";
import { inflateSync } from "zlib";

/**
 * A spectated game written as the replay it arrives as (docs/design/spectating.md §3.3): the
 * .brep header, then each chunk exactly as mm relays it (the 20-byte chunk header and the stored
 * bytes, docs/spectate-protocol.md), then, once the game has ended, the trailer. The playback
 * Dolphin follows the file as it grows (`mirror`); it reads it as it reads any replay, with every
 * check, so nothing here is trusted beyond what it takes to write the trailer. Once ended, the
 * file is a whole replay (dolphin/Source/Core/Core/Rollback/ReplayFile.h).
 */

const MAGIC = Buffer.from([0x89, 0x42, 0x52, 0x50, 0x0d, 0x0a, 0x1a, 0x0a]);
const HEADER_SIZE = 32;
const CHUNK_HEADER_SIZE = 20;
const FLAG_DEFLATE = 1;
/** ReplayFile.h's MAX_FRAME_CHUNK: a raw FRMS is no larger. */
const MAX_FRAME_CHUNK = 1 << 20;

export class LiveGameFile {
  private at = HEADER_SIZE;
  private endgAt = -1;
  private lastFrame = -1;
  private done = false;

  private constructor(private readonly handle: FileHandle, readonly path: string) {}

  /** Creates (or replaces) `path` with the header for the game `uid` (32 hex characters). */
  static async create(path: string, uid: string): Promise<LiveGameFile> {
    if (!/^[0-9a-f]{32}$/i.test(uid)) {
      throw new Error("bad game id");
    }
    const header = Buffer.alloc(HEADER_SIZE);
    MAGIC.copy(header, 0);
    header.writeUInt16LE(1, 8); // format major
    header.writeUInt16LE(0, 10); // minor
    header.writeUInt32LE(HEADER_SIZE, 12);
    Buffer.from(uid, "hex").copy(header, 16);
    const handle = await open(path, "w");
    await handle.write(header);
    return new LiveGameFile(handle, path);
  }

  /** The bytes written so far. */
  get size(): number {
    return this.at;
  }

  /** Ended or closed: nothing more is written. */
  get ended(): boolean {
    return this.done;
  }

  /** Appends one chunk as relayed. A chunk that isn't one (its header and length disagree) is refused. */
  async append(chunk: Buffer): Promise<void> {
    if (this.done) {
      throw new Error("the game has ended");
    }
    if (chunk.length < CHUNK_HEADER_SIZE || chunk.readUInt32LE(8) !== chunk.length - CHUNK_HEADER_SIZE) {
      throw new Error("not a replay chunk");
    }
    const type = chunk.toString("latin1", 0, 4);
    if (type === "FRMS") {
      const last = frmsLastFrame(chunk);
      if (last !== null) {
        this.lastFrame = last;
      }
    } else if (type === "ENDG") {
      this.endgAt = this.at;
    }
    await this.handle.write(chunk);
    this.at += chunk.length;
  }

  /** The game's end: the trailer (when its ENDG came), and the file is closed. */
  async end(): Promise<void> {
    if (this.done) {
      return;
    }
    this.done = true;
    if (this.endgAt >= 0 && this.lastFrame >= 0) {
      const trailer = Buffer.alloc(16);
      trailer.write("BRPT", 0, "latin1");
      trailer.writeUInt32LE(this.endgAt, 4);
      trailer.writeUInt32LE(this.lastFrame, 8);
      trailer.writeUInt32LE(crc32(trailer.subarray(0, 12)), 12);
      await this.handle.write(trailer);
      this.at += trailer.length;
    }
    await this.handle.close();
  }

  /** Closes the file where it is (a stream left before the game's end). */
  async close(): Promise<void> {
    if (!this.done) {
      this.done = true;
      await this.handle.close();
    }
  }
}

/** An FRMS chunk's last frame (`first_frame + count - 1`), or null if it doesn't read. */
export function frmsLastFrame(chunk: Buffer): number | null {
  try {
    const flags = chunk.readUInt32LE(4);
    const rawLen = chunk.readUInt32LE(12);
    if (rawLen < 8 || rawLen > MAX_FRAME_CHUNK) {
      return null;
    }
    const stored = chunk.subarray(CHUNK_HEADER_SIZE);
    const raw = flags & FLAG_DEFLATE ? inflateSync(stored, { maxOutputLength: MAX_FRAME_CHUNK }) : stored;
    if (raw.length < 8) {
      return null;
    }
    const first = raw.readUInt32LE(0);
    const count = raw.readUInt16LE(4);
    return count > 0 ? first + count - 1 : null;
  } catch {
    return null;
  }
}
