/**
 * The per-frame fighter state highlights need, from a `.brep`'s recorded fighter memory (FTSP/FTRM)
 * and pads (FRMS) (docs/design/replays.md §4; the prototype's timeline.py). A frame's fighter memory
 * is 2,248 bytes a player; only ~28 of them and 10 pad bytes are decoded here: each chunk's rows
 * are XORed with the row before (byte by byte), so the wanted bytes are XOR-accumulated on their own
 * and the rest is skipped.
 *
 * Fields (docs/brawl-memory-map.md): damage f32 ftOwner+0x24, stocks s32 ftOwner+0x34, status kind
 * s32 soStatusModule+0x34, animation frame f32 soMotionModule+0x40, motion kind s32
 * soMotionModule+0x58, the attacker's ftEntry id soDamageModuleImpl+0x84 (m_attackerInfo's indirect
 * entry id), the player's own id ftEntry+0x04. Pads (gfPadStatus): buttons u32 +0x0, stick s8
 * +0x30/+0x31, C-stick +0x32/+0x33, L/R analog +0x34/+0x35. All big-endian.
 *
 * A replay is a file anyone can share: sizes are checked before anything is read, every chunk's
 * CRC is checked and nothing is inflated past its stated (bounded) size.
 */
import { readFile, stat } from "node:fs/promises";
import { inflateSync } from "zlib";

import { crc32 } from "../replay_format";

const MAGIC = Buffer.from([0x89, 0x42, 0x52, 0x50, 0x0d, 0x0a, 0x1a, 0x0a]);
const HEADER_SIZE = 32;
const CHUNK_HEADER = 20;
const TRAILER_SIZE = 16;
const MAX_FILE_SIZE = 64 * 1024 * 1024;
const MAX_CHUNK_RAW = 4 * 1024 * 1024;
const MAX_JSON = 64 * 1024;
const MAX_CHUNKS = 20_000;
const FLAG_DEFLATE = 1;
const FLAG_CRITICAL = 2;
const PORTS = 4;
export const PAD_SIZE = 0x40;
/** Bytes of a pad kept per frame: buttons (0x0-0x3), sticks and triggers (0x30-0x35). */
export const PAD_KEPT = 10;
/** 2 hours at 60 frames a second: more than any game. */
const MAX_FRAMES = 2 * 60 * 60 * 60;

const fourCC = (s: string): number =>
  (s.charCodeAt(0) | (s.charCodeAt(1) << 8) | (s.charCodeAt(2) << 16) | (s.charCodeAt(3) << 24)) >>> 0;
const CHUNK_INFO = fourCC("INFO");
const CHUNK_FTSP = fourCC("FTSP");
const CHUNK_FRMS = fourCC("FRMS");
const CHUNK_FTRM = fourCC("FTRM");
const TRAILER_MAGIC = fourCC("BRPT");

export class ReplayFramesError extends Error {}

export type TimelinePlayer = {
  index: number; // among the recorded fighters
  port: number; // 1-4
  displayName: string;
  connectCode: string;
  characterId: number;
  costume: number;
  teamId: number | null;
  entryId: number;
};

/** Per player (the recorded fighters, in port order), per frame (from `firstFrame`). */
export type Timeline = {
  firstFrame: number;
  frames: number;
  players: TimelinePlayer[];
  /** The recording player's port (INFO's recorder), or null. */
  recorderPort: number | null;
  isTeams: boolean;
  damage: Float32Array[];
  stocks: Int32Array[];
  status: Int32Array[];
  attacker: Uint32Array[];
  anim: Float32Array[];
  motion: Int32Array[];
  /** PAD_KEPT bytes a frame: pad bytes 0x0-0x3, then 0x30-0x35. */
  pads: Uint8Array[];
};

/** The wanted bytes of a fighter block, by field: [block name, offset in the block, size]. */
const FIELDS = [
  ["entry", 0x04], // entry id
  ["owner", 0x24], // damage
  ["owner", 0x34], // stocks
  ["status", 0x34], // status kind
  ["damage", 0x84], // attacker's entry id
  ["motion", 0x40], // animation frame
  ["motion", 0x58], // motion kind
] as const;
const F_ENTRY = 0;
const F_DAMAGE = 1;
const F_STOCKS = 2;
const F_STATUS = 3;
const F_ATTACKER = 4;
const F_ANIM = 5;
const F_MOTION = 6;
const PAD_BYTES = [0, 1, 2, 3, 0x30, 0x31, 0x32, 0x33, 0x34, 0x35];

const obj = (v: unknown): Record<string, unknown> =>
  v !== null && typeof v === "object" && !Array.isArray(v) ? (v as Record<string, unknown>) : {};
const int = (v: unknown, min: number, max: number): number | null =>
  typeof v === "number" && Number.isInteger(v) && v >= min && v <= max ? v : null;
const str = (v: unknown, max: number): string => (typeof v === "string" ? Array.from(v).slice(0, max).join("") : "");

function parseJson(raw: Buffer): Record<string, unknown> {
  try {
    return obj(JSON.parse(raw.toString("utf8")));
  } catch {
    return {};
  }
}

type Chunk = { type: number; raw: Buffer };

/** The file's chunks, CRC-checked and inflated (a damaged chunk ends the list: what came before stays). */
function* chunks(data: Buffer): Generator<Chunk> {
  if (data.length < HEADER_SIZE || !data.subarray(0, 8).equals(MAGIC)) {
    throw new ReplayFramesError("not a Brawl Online replay");
  }
  if (data.readUInt16LE(8) !== 1) {
    throw new ReplayFramesError("replay format is newer than this launcher");
  }
  const headerSize = data.readUInt32LE(12);
  if (headerSize < HEADER_SIZE || headerSize > 4096) {
    throw new ReplayFramesError("damaged replay header");
  }
  let end = data.length;
  if (data.length >= headerSize + TRAILER_SIZE) {
    const t = data.subarray(data.length - TRAILER_SIZE);
    if (t.readUInt32LE(0) === TRAILER_MAGIC && t.readUInt32LE(12) === crc32(t.subarray(0, 12))) {
      end -= TRAILER_SIZE;
    }
  }
  let off = headerSize;
  for (let n = 0; n < MAX_CHUNKS && end - off >= CHUNK_HEADER; n++) {
    const type = data.readUInt32LE(off);
    const flags = data.readUInt32LE(off + 4);
    const storedLen = data.readUInt32LE(off + 8);
    const rawLen = data.readUInt32LE(off + 12);
    const crc = data.readUInt32LE(off + 16);
    const start = off + CHUNK_HEADER;
    if (storedLen > end - start || rawLen > MAX_CHUNK_RAW || flags & ~(FLAG_DEFLATE | FLAG_CRITICAL)) {
      return;
    }
    const stored = data.subarray(start, start + storedLen);
    off = start + storedLen;
    if (crc32(stored) !== crc) {
      return;
    }
    let raw: Buffer = stored;
    if (flags & FLAG_DEFLATE) {
      try {
        raw = inflateSync(stored, { maxOutputLength: rawLen });
      } catch {
        return;
      }
    }
    if (raw.length !== rawLen) {
      return;
    }
    yield { type, raw };
  }
}

const popcount = (ports: number): number => {
  let n = 0;
  for (let p = 0; p < PORTS; p++) {
    n += (ports >> p) & 1;
  }
  return n;
};

/** Grows a typed array's buffer (frames arrive a chunk at a time). */
class Column<T extends Float32Array | Int32Array | Uint32Array | Uint8Array> {
  data: T;
  constructor(private readonly make: (n: number) => T, private readonly width = 1) {
    this.data = make(64 * 60 * width);
  }
  ensure(frames: number) {
    if (frames * this.width > this.data.length) {
      const next = this.make(Math.max(frames * this.width, this.data.length * 2));
      next.set(this.data);
      this.data = next;
    }
  }
  take(frames: number): T {
    return this.data.slice(0, frames * this.width) as T;
  }
}

/**
 * The timeline of a replay's bytes. Throws ReplayFramesError when it has no fighter memory
 * (a damaged or an older file).
 */
export function decodeTimeline(data: Buffer): Timeline {
  let info: Record<string, unknown> = {};
  let spec: Record<string, number> | null = null;
  let block = 0;
  let fighterPorts = 0;
  let fighterFrames = 0;
  let firstFrame = -1;
  let padFrames = 0;
  let padPorts = 0;
  let cols: {
    damage: Column<Float32Array>;
    stocks: Column<Int32Array>;
    status: Column<Int32Array>;
    attacker: Column<Uint32Array>;
    anim: Column<Float32Array>;
    motion: Column<Int32Array>;
    entry: Column<Uint32Array>;
  }[] = [];
  const pads: Column<Uint8Array>[] = Array.from(
    { length: PORTS },
    () => new Column((n) => new Uint8Array(n), PAD_KEPT),
  );
  let fieldOffsets: number[] = [];

  for (const { type, raw } of chunks(data)) {
    if (type === CHUNK_INFO && raw.length <= MAX_JSON) {
      info = parseJson(raw);
    } else if (type === CHUNK_FTSP && raw.length <= MAX_JSON) {
      const blocks = obj(parseJson(raw)).blocks;
      spec = {};
      for (const b of Array.isArray(blocks) ? blocks : []) {
        const o = obj(b);
        const name = str(o.name, 32);
        const offset = int(o.offset, 0, 1 << 16);
        const size = int(o.size, 0, 1 << 16);
        if (name && offset != null && size != null) {
          spec[name] = offset;
          spec[`${name}.size`] = size;
        }
      }
    } else if (type === CHUNK_FRMS) {
      if (raw.length < 8) {
        break;
      }
      const first = raw.readUInt32LE(0);
      const count = raw.readUInt16LE(4);
      const ports = raw[6] & 0xf;
      if (padFrames === 0) {
        firstFrame = first;
        padPorts = ports;
      }
      if (first !== firstFrame + padFrames || ports !== padPorts || padFrames + count > MAX_FRAMES) {
        break;
      }
      if (8 + popcount(ports) * count * PAD_SIZE > raw.length) {
        break;
      }
      let off = 8;
      for (let port = 0; port < PORTS; port++) {
        if (!((ports >> port) & 1)) {
          continue;
        }
        const col = pads[port];
        col.ensure(padFrames + count);
        const acc = new Uint8Array(PAD_KEPT);
        for (let f = 0; f < count; f++) {
          const row = off + f * PAD_SIZE;
          const dst = (padFrames + f) * PAD_KEPT;
          for (let k = 0; k < PAD_KEPT; k++) {
            acc[k] ^= raw[row + PAD_BYTES[k]];
            col.data[dst + k] = acc[k];
          }
        }
        off += count * PAD_SIZE;
      }
      padFrames += count;
    } else if (type === CHUNK_FTRM) {
      if (raw.length < 16 || !spec) {
        break;
      }
      const first = raw.readUInt32LE(0);
      const count = raw.readUInt32LE(4);
      const ports = raw[8] & 0xf;
      const blockSize = raw.readUInt32LE(12);
      if (fighterFrames === 0) {
        block = blockSize;
        fighterPorts = ports;
        fieldOffsets = FIELDS.map(([name, at]) => {
          const base = spec?.[name];
          const size = spec?.[`${name}.size`] ?? 0;
          if (base == null || at + 4 > size || base + at + 4 > block) {
            throw new ReplayFramesError(`no ${name} block in the fighter memory`);
          }
          return base + at;
        });
        cols = Array.from({ length: popcount(ports) }, () => ({
          damage: new Column((n) => new Float32Array(n)),
          stocks: new Column((n) => new Int32Array(n)),
          status: new Column((n) => new Int32Array(n)),
          attacker: new Column((n) => new Uint32Array(n)),
          anim: new Column((n) => new Float32Array(n)),
          motion: new Column((n) => new Int32Array(n)),
          entry: new Column((n) => new Uint32Array(n)),
        }));
      }
      if (
        blockSize !== block ||
        ports !== fighterPorts ||
        first !== firstFrame + fighterFrames ||
        fighterFrames + count > MAX_FRAMES ||
        16 + cols.length * count * block > raw.length
      ) {
        break;
      }
      let off = 16;
      const acc = Buffer.alloc(FIELDS.length * 4);
      for (const c of cols) {
        for (const col of Object.values(c)) {
          col.ensure(fighterFrames + count);
        }
        acc.fill(0);
        for (let f = 0; f < count; f++) {
          const row = off + f * block;
          for (let k = 0; k < FIELDS.length; k++) {
            const src = row + fieldOffsets[k];
            const a = k * 4;
            acc[a] ^= raw[src];
            acc[a + 1] ^= raw[src + 1];
            acc[a + 2] ^= raw[src + 2];
            acc[a + 3] ^= raw[src + 3];
          }
          const i = fighterFrames + f;
          c.entry.data[i] = acc.readUInt32BE(F_ENTRY * 4);
          c.damage.data[i] = acc.readFloatBE(F_DAMAGE * 4);
          c.stocks.data[i] = acc.readInt32BE(F_STOCKS * 4);
          c.status.data[i] = acc.readInt32BE(F_STATUS * 4);
          c.attacker.data[i] = acc.readUInt32BE(F_ATTACKER * 4);
          c.anim.data[i] = acc.readFloatBE(F_ANIM * 4);
          c.motion.data[i] = acc.readInt32BE(F_MOTION * 4);
        }
        off += count * block;
      }
      fighterFrames += count;
    }
  }
  if (!fighterFrames || !cols.length) {
    throw new ReplayFramesError("the replay has no fighter memory");
  }

  const frames = fighterFrames;
  const recorded: number[] = [];
  for (let p = 0; p < PORTS; p++) {
    if ((fighterPorts >> p) & 1) {
      recorded.push(p);
    }
  }
  const infoPlayers = new Map<number, Record<string, unknown>>();
  for (const p of Array.isArray(info.players) ? info.players.slice(0, PORTS) : []) {
    const o = obj(p);
    const port = int(o.port, 1, PORTS);
    if (port != null) {
      infoPlayers.set(port, o);
    }
  }
  const players = recorded.map((port0, index): TimelinePlayer => {
    const meta = infoPlayers.get(port0 + 1) ?? {};
    return {
      index,
      port: port0 + 1,
      displayName: str(meta.displayName, 32) || `P${port0 + 1}`,
      connectCode: str(meta.connectCode, 16),
      characterId: int(meta.characterId, 0, 0xff) ?? 0xff,
      costume: int(meta.costume, 0, 0xff) ?? 0,
      teamId: int(meta.teamId, 0, 2),
      entryId: entryId(cols[index].entry.take(frames)),
    };
  });
  const emptyPads = new Uint8Array(frames * PAD_KEPT);
  return {
    firstFrame,
    frames,
    players,
    recorderPort: int(obj(info.recorder).port, 1, PORTS),
    isTeams: info.isTeams === true,
    damage: cols.map((c) => c.damage.take(frames)),
    stocks: cols.map((c) => c.stocks.take(frames)),
    status: cols.map((c) => c.status.take(frames)),
    attacker: cols.map((c) => c.attacker.take(frames)),
    anim: cols.map((c) => c.anim.take(frames)),
    motion: cols.map((c) => c.motion.take(frames)),
    // (The pads cover the same frames: FTRM and FRMS are written together.)
    pads: recorded.map((p) => (padFrames >= frames ? pads[p].take(frames) : emptyPads)),
  };
}

/**
 * A fighter's ftEntry id: the most common nonzero value's low 16 bits win (zeros where the entry
 * did not resolve), and the first value with them is the id (as timeline.py).
 */
function entryId(ids: Uint32Array): number {
  const counts = new Map<number, number>();
  for (const id of ids) {
    if (id !== 0) {
      counts.set(id & 0xffff, (counts.get(id & 0xffff) ?? 0) + 1);
    }
  }
  let best = -1;
  let bestCount = 0;
  for (const [low, n] of counts) {
    if (n > bestCount || (n === bestCount && low < best)) {
      best = low;
      bestCount = n;
    }
  }
  if (best < 0) {
    return 0;
  }
  for (const id of ids) {
    if (id !== 0 && (id & 0xffff) === best) {
      return id;
    }
  }
  return 0;
}

/** Reads and decodes a replay file. */
export async function readTimeline(fullPath: string): Promise<Timeline> {
  const { size } = await stat(fullPath);
  if (size > MAX_FILE_SIZE) {
    throw new ReplayFramesError("the file is too large to be a replay");
  }
  return decodeTimeline(await readFile(fullPath));
}
