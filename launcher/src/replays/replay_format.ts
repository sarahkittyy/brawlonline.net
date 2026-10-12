/**
 * Brawl Online replay files (`.brep`, docs/design/replays.md §4), as Dolphin writes them
 * (dolphin/Source/Core/Core/Rollback/ReplayFile.cpp): a 32-byte header, then chunks
 * `{u32 type, u32 flags, u32 stored length, u32 raw length, u32 CRC-32}` + the stored bytes, then
 * a 16-byte trailer once the game has ended. The indexer reads only the metadata: the INFO
 * chunk (always the first, JSON) and the result (ENDG, JSON), which the trailer points at.
 *
 * A replay is a file anyone can share, so everything here is bounded: sizes are checked before
 * anything is read, every chunk's CRC is checked, JSON is parsed only from a checked chunk, and
 * text that ends up on screen is cut to length (React escapes it).
 *
 * The indexer stores Slippi's `ParsedFileInfo` shape (GameStartType/MetadataType); ours fills the
 * fields the browser and the database use.
 */
import type { GameStartType, MetadataType } from "@slippi/slippi-js/node";
import { open, stat } from "node:fs/promises";
import path from "path";
import { inflateSync } from "zlib";

/** Our replay files. */
export const REPLAY_EXTENSIONS: readonly string[] = [".brep"];

export function isReplayFileName(fileName: string): boolean {
  return REPLAY_EXTENSIONS.includes(path.extname(fileName).toLowerCase());
}

const MAGIC = Buffer.from([0x89, 0x42, 0x52, 0x50, 0x0d, 0x0a, 0x1a, 0x0a]);
const HEADER_SIZE = 32;
const CHUNK_HEADER = 20;
const TRAILER_SIZE = 16;
const MAX_FILE_SIZE = 64 * 1024 * 1024;
const MAX_INFO = 64 * 1024;
const MAX_ENDG = 16 * 1024;
const FLAG_DEFLATE = 1;
const FLAG_CRITICAL = 2;
const MAX_TEXT = 64;
/** Slippi counts frames from -123 (the countdown); the browser shows durations that way. */
const SLIPPI_FIRST_FRAME = -123;

const fourCC = (s: string): number =>
  s.charCodeAt(0) | (s.charCodeAt(1) << 8) | (s.charCodeAt(2) << 16) | (s.charCodeAt(3) << 24);
const CHUNK_INFO = fourCC("INFO") >>> 0;
const CHUNK_ENDG = fourCC("ENDG") >>> 0;
const TRAILER_MAGIC = fourCC("BRPT") >>> 0;

let crcTable: Uint32Array | null = null;
export function crc32(data: Uint8Array): number {
  if (!crcTable) {
    crcTable = new Uint32Array(256);
    for (let n = 0; n < 256; n++) {
      let c = n;
      for (let k = 0; k < 8; k++) {
        c = c & 1 ? 0xedb88320 ^ (c >>> 1) : c >>> 1;
      }
      crcTable[n] = c >>> 0;
    }
  }
  let crc = 0xffffffff;
  for (let i = 0; i < data.length; i++) {
    crc = crcTable[(crc ^ data[i]) & 0xff] ^ (crc >>> 8);
  }
  return (crc ^ 0xffffffff) >>> 0;
}

/** The metadata a replay carries (INFO), as the indexer and the launcher use it. */
export type ReplayPlayer = {
  port: number; // 1-4
  uid: string;
  displayName: string;
  connectCode: string;
  characterId: number; // gmCharacterKind
  costume: number;
  teamId: number | null;
};

export type ReplayMeta = {
  startTime: string | null;
  mode: string; // "ranked" | "unranked" | "direct" | "teams" | "room" | "unknown"
  matchId: string;
  gameIndex: number;
  isTeams: boolean;
  stageId: number | null;
  players: ReplayPlayer[];
  recorderPort: number | null;
  startFrame: number;
  stocks: number | null;
  minutes: number | null;
  sim: number | null;
  game: string;
  dolphin: string;
};

/** The result (ENDG), or null while the game is in progress or after a crash. */
export type ReplayEnd = {
  endMethod: string;
  lastFrame: number;
  finalFrame: number;
  winnerPort: number | null; // 1-4; null: none or a draw
  draw: boolean;
  players: { port: number; stocks: number; damage: number; place: number | null; isWinner: boolean }[];
};

export type ReplayHeaderInfo = {
  uid: string; // hex, the same in every player's file of the game
  meta: ReplayMeta;
  end: ReplayEnd | null;
  complete: boolean; // the trailer is there: the game ended and the file is whole
};

export class ReplayFormatError extends Error {}

const num = (v: unknown, min: number, max: number): number | null =>
  typeof v === "number" && Number.isFinite(v) && Number.isInteger(v) && v >= min && v <= max ? v : null;
const text = (v: unknown, max = MAX_TEXT): string =>
  typeof v === "string"
    ? Array.from(v)
        .filter((c) => {
          const cp = c.codePointAt(0) ?? 0;
          return (
            cp >= 0x20 &&
            cp !== 0x7f &&
            !(cp >= 0x80 && cp < 0xa0) &&
            !(cp >= 0x202a && cp <= 0x202e) &&
            !(cp >= 0x2066 && cp <= 0x2069)
          );
        })
        .slice(0, max)
        .join("")
    : "";
const obj = (v: unknown): Record<string, unknown> =>
  v !== null && typeof v === "object" && !Array.isArray(v) ? (v as Record<string, unknown>) : {};

/** A chunk at `offset` of `buf`: its type, flags and raw bytes (CRC-checked, inflated), or null. */
function readChunk(
  buf: Buffer,
  offset: number,
  maxRaw: number,
): { type: number; flags: number; raw: Buffer; end: number } | null {
  if (offset + CHUNK_HEADER > buf.length) {
    return null;
  }
  const type = buf.readUInt32LE(offset);
  const flags = buf.readUInt32LE(offset + 4);
  const storedLen = buf.readUInt32LE(offset + 8);
  const rawLen = buf.readUInt32LE(offset + 12);
  const crc = buf.readUInt32LE(offset + 16);
  const start = offset + CHUNK_HEADER;
  if (storedLen > buf.length - start || rawLen > maxRaw || flags & ~(FLAG_DEFLATE | FLAG_CRITICAL)) {
    return null;
  }
  const stored = buf.subarray(start, start + storedLen);
  if (crc32(stored) !== crc) {
    return null;
  }
  let raw: Buffer = stored;
  if (flags & FLAG_DEFLATE) {
    try {
      raw = inflateSync(stored, { maxOutputLength: maxRaw });
    } catch {
      return null;
    }
  }
  if (raw.length !== rawLen) {
    return null;
  }
  return { type, flags, raw, end: start + storedLen };
}

function parseJson(raw: Buffer): Record<string, unknown> | null {
  try {
    const v = JSON.parse(raw.toString("utf8"));
    return v !== null && typeof v === "object" && !Array.isArray(v) ? v : null;
  } catch {
    return null;
  }
}

function parseMeta(info: Record<string, unknown>): ReplayMeta {
  const players: ReplayPlayer[] = [];
  const list = Array.isArray(info.players) ? info.players.slice(0, 4) : [];
  for (const p of list) {
    const o = obj(p);
    const port = num(o.port, 1, 4);
    if (port == null || players.some((q) => q.port === port)) {
      continue;
    }
    players.push({
      port,
      uid: text(o.uid, 128),
      displayName: text(o.displayName, 32),
      connectCode: text(o.connectCode, 16),
      characterId: num(o.characterId, 0, 0xff) ?? 0xff,
      costume: num(o.costume, 0, 0xff) ?? 0,
      teamId: num(o.teamId, 0, 2),
    });
  }
  players.sort((a, b) => a.port - b.port);
  const rules = obj(info.rules);
  const build = obj(info.build);
  const session = obj(info.session);
  const recorder = obj(info.recorder);
  const startTime = text(info.startTime, 40);
  return {
    startTime: startTime && !Number.isNaN(Date.parse(startTime)) ? startTime : null,
    mode: text(info.mode, 16) || "unknown",
    matchId: text(info.matchId, 128),
    gameIndex: num(info.gameIndex, 0, 255) ?? 1,
    isTeams: info.isTeams === true,
    stageId: num(info.stageId, 0, 0xffff),
    players,
    recorderPort: num(recorder.port, 1, 4),
    startFrame: num(session.startFrame, 0, 1000) ?? 120,
    stocks: num(rules.stocks, 1, 99),
    minutes: num(rules.timeMinutes, 0, 99),
    sim: num(build.sim, 0, 0xffffffff),
    game: text(build.game, 8),
    dolphin: text(build.dolphin, 64),
  };
}

function parseEnd(endg: Record<string, unknown>): ReplayEnd {
  const winner = endg.winnerPort;
  const players: ReplayEnd["players"] = [];
  for (const p of Array.isArray(endg.players) ? endg.players.slice(0, 4) : []) {
    const o = obj(p);
    const port = num(o.port, 1, 4);
    if (port == null) {
      continue;
    }
    players.push({
      port,
      stocks: num(o.stocks, -1000, 1000) ?? 0,
      damage: typeof o.damage === "number" && Number.isFinite(o.damage) ? o.damage : 0,
      place: num(o.place, 1, 4),
      isWinner: o.isWinner === true,
    });
  }
  return {
    endMethod: text(endg.endMethod, 16) || "unknown",
    lastFrame: num(endg.lastFrame, -1, 1_000_000) ?? -1,
    finalFrame: num(endg.finalFrame, -1, 1_000_000) ?? -1,
    winnerPort: num(winner, 1, 4),
    draw: winner === "draw",
    players,
  };
}

/**
 * The metadata of a replay from its first bytes (`head`, at least the header and the INFO
 * chunk) and its last bytes (`tail`, the trailer and the ENDG chunk before it), with the file's
 * size. Throws ReplayFormatError when it is not a replay this launcher can read.
 */
export function parseReplayHeader(head: Buffer, tail: Buffer, size: number): ReplayHeaderInfo {
  if (size > MAX_FILE_SIZE) {
    throw new ReplayFormatError("the file is too large to be a replay");
  }
  if (head.length < HEADER_SIZE || !head.subarray(0, 8).equals(MAGIC)) {
    throw new ReplayFormatError("not a Brawl Online replay");
  }
  const major = head.readUInt16LE(8);
  const headerSize = head.readUInt32LE(12);
  if (major !== 1) {
    throw new ReplayFormatError(`replay format ${major} is newer than this launcher`);
  }
  if (headerSize < HEADER_SIZE || headerSize > 4096) {
    throw new ReplayFormatError("damaged replay header");
  }
  const uid = head.subarray(16, 32).toString("hex");
  const first = readChunk(head, headerSize, MAX_INFO);
  if (!first || first.type !== CHUNK_INFO) {
    throw new ReplayFormatError("damaged replay (no metadata)");
  }
  const info = parseJson(first.raw);
  if (!info) {
    throw new ReplayFormatError("damaged replay (bad metadata)");
  }
  const meta = parseMeta(info);

  // The trailer: "BRPT", the ENDG chunk's offset, the last frame, CRC-32 of these 12 bytes.
  let end: ReplayEnd | null = null;
  let complete = false;
  if (tail.length >= TRAILER_SIZE && size >= HEADER_SIZE + TRAILER_SIZE) {
    const t = tail.subarray(tail.length - TRAILER_SIZE);
    if (t.readUInt32LE(0) === TRAILER_MAGIC && t.readUInt32LE(12) === crc32(t.subarray(0, 12))) {
      const endgOffset = t.readUInt32LE(4);
      const tailStart = size - tail.length;
      if (endgOffset >= tailStart && endgOffset < size - TRAILER_SIZE) {
        const c = readChunk(tail.subarray(0, tail.length - TRAILER_SIZE), endgOffset - tailStart, MAX_ENDG);
        const endg = c && c.type === CHUNK_ENDG ? parseJson(c.raw) : null;
        if (endg && c && c.end === tail.length - TRAILER_SIZE) {
          end = parseEnd(endg);
          complete = true;
        }
      }
    }
  }
  return { uid, meta, end, complete };
}

/** Reads a replay's metadata from the file (its first 64 KiB and last 16 KiB). */
export async function readReplayHeader(fullPath: string): Promise<ReplayHeaderInfo> {
  const handle = await open(fullPath, "r");
  try {
    const { size } = await handle.stat();
    if (size > MAX_FILE_SIZE) {
      throw new ReplayFormatError("the file is too large to be a replay");
    }
    const headLen = Math.min(size, HEADER_SIZE + CHUNK_HEADER + MAX_INFO);
    const head = Buffer.alloc(headLen);
    await handle.read(head, 0, headLen, 0);
    const tailLen = Math.min(size, CHUNK_HEADER + MAX_ENDG + TRAILER_SIZE);
    const tail = Buffer.alloc(tailLen);
    await handle.read(tail, 0, tailLen, size - tailLen);
    return parseReplayHeader(head, tail, size);
  } finally {
    await handle.close();
  }
}

/** What the indexer stores about a replay file (Slippi's `ParsedFileInfo` shape). */
export type ReplayFileInfo = {
  filename: string;
  sizeBytes: number;
  birthTime: string | undefined;
  settings: GameStartType | undefined;
  metadata: MetadataType | undefined;
  winnerIndices: number[];
};

/** ReplayHeaderInfo in the shapes of Slippi's indexer (GameStartType, MetadataType). */
export function toSlippiShapes(h: ReplayHeaderInfo): Pick<ReplayFileInfo, "settings" | "metadata" | "winnerIndices"> {
  const m = h.meta;
  const settings = {
    slpVersion: `brep-${h.meta.sim ?? 0}`,
    isTeams: m.isTeams,
    isPAL: false,
    stageId: m.stageId ?? undefined,
    gameMode: 8, // online
    timerType: undefined,
    startingTimerSeconds: m.minutes ? m.minutes * 60 : undefined,
    players: m.players.map((p) => ({
      playerIndex: p.port - 1,
      port: p.port,
      characterId: p.characterId,
      characterColor: p.costume,
      startStocks: m.stocks ?? undefined,
      type: 0,
      teamId: p.teamId ?? undefined,
      controllerFix: null,
      nametag: "",
      displayName: p.displayName,
      connectCode: p.connectCode,
      userId: p.uid,
    })),
    matchInfo: {
      matchId: m.matchId || undefined,
      sessionId: m.matchId || undefined,
      gameNumber: m.gameIndex,
      tiebreakerNumber: 0,
    },
  } as unknown as GameStartType;
  const lastFrame = h.end && h.end.lastFrame >= 0 ? h.end.lastFrame + m.startFrame + SLIPPI_FIRST_FRAME : undefined;
  const metadata = {
    startAt: m.startTime ?? undefined,
    lastFrame,
    playedOn: "brawlonline",
  } as unknown as MetadataType;
  // A team battle: every player on the winner's team won.
  const winnerPort = h.end?.winnerPort ?? null;
  const winnerTeam = m.isTeams ? m.players.find((p) => p.port === winnerPort)?.teamId : null;
  const winnerIndices =
    winnerPort == null
      ? []
      : winnerTeam != null
      ? m.players.filter((p) => p.teamId === winnerTeam).map((p) => p.port - 1)
      : [winnerPort - 1];
  return { settings, metadata, winnerIndices };
}

export async function readReplayFileInfo(folder: string, filename: string): Promise<ReplayFileInfo> {
  const fullPath = path.join(folder, filename);
  let sizeBytes = 0;
  let birthTime: string | undefined = undefined;
  try {
    const info = await stat(fullPath);
    sizeBytes = info.size;
    birthTime = info.birthtime.toISOString();
  } catch {
    // Keep the defaults; the file is still listed.
  }
  try {
    const header = await readReplayHeader(fullPath);
    return { filename, sizeBytes, birthTime, ...toSlippiShapes(header) };
  } catch {
    // Not readable as a replay (damaged, or from a newer launcher): listed by name and date.
    return { filename, sizeBytes, birthTime, settings: undefined, metadata: undefined, winnerIndices: [] };
  }
}
