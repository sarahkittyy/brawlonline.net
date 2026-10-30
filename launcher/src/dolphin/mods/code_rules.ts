import type { ModBuildCodes, ModCode } from "./types";

/** The rules a room's codes keep (docs/design/room-codes.md), shared by the launcher's main process
 * and the Codes dialog: what Dolphin runs, and what fits a room. */

/** How many Gecko code lines Dolphin fits in the game's RAM: its code list goes between the code
 * handler (Sys/codehandler.bin, 2880 bytes from 0x80001800) and 0x80003000, 8 bytes a line
 * (GeckoCode.cpp, InstallCodeHandlerLocked); a code past that is left out. */
export const GECKO_LINES_MAX = 407;

/** As many codes and lines as Dolphin's session takes (MachineSession.cpp ParseCodes). */
export const MAX_ROOM_CODES = 256;
export const MAX_ROOM_CODE_LINES = 2048;

/** The codes go on Dolphin's command line (`--mods-room`), whose whole length Windows caps at
 * 32767 characters: they keep well under. */
export const MAX_ROOM_CODES_JSON = 20000;

const HEX_LINE = /^[0-9A-F]{8} [0-9A-F]{8}$/;
const AR_ENCRYPTED = /^[0-9A-Z]{4}-[0-9A-Z]{4}-[0-9A-Z]{5}$/;

/** A code line as Dolphin takes it: "XXXXXXXX YYYYYYYY" (upper case, one space), or an encrypted
 * Action Replay line; null for anything else (a code with options to fill in, a comment). */
export function normalizeLine(line: string, kind: ModCode["kind"]): string | null {
  const l = line.trim().toUpperCase().replace(/\s+/g, " ");
  if (HEX_LINE.test(l)) {
    return l;
  }
  return kind === "ar" && AR_ENCRYPTED.test(l) ? l : null;
}

/** Every line of the code is one Dolphin runs. */
export function isRunnable(code: Pick<ModCode, "kind" | "lines">): boolean {
  return code.lines.length > 0 && code.lines.every((l) => normalizeLine(l, code.kind) !== null);
}

/** Text pasted as a code: its lines, and whether they are Action Replay's (encrypted ones are; hex
 * pairs are either: `kind` then). Null when a line is not a code line. */
export function parsePastedCode(
  text: string,
  kind: ModCode["kind"],
): { kind: ModCode["kind"]; lines: string[] } | null {
  const raw = text
    .split(/\r?\n/)
    .map((l) => l.trim())
    .filter((l) => l);
  if (raw.length === 0) {
    return null;
  }
  const encrypted = raw.some((l) => AR_ENCRYPTED.test(l.toUpperCase()));
  const k = encrypted ? "ar" : kind;
  const lines = raw.map((l) => normalizeLine(l, k));
  return lines.every((l): l is string => l !== null) ? { kind: k, lines } : null;
}

/** Whether pasted text looks like an Action Replay code (an encrypted line). */
export function looksLikeActionReplay(text: string): boolean {
  return text.split(/\r?\n/).some((l) => AR_ENCRYPTED.test(l.trim().toUpperCase()));
}

/** Gecko code lines among these codes. */
export function geckoLines(codes: ModCode[]): number {
  return codes.reduce((n, c) => n + (c.kind === "gecko" ? c.lines.length : 0), 0);
}

/** Whether these codes fit a room: Dolphin's room limits, and the Gecko code lines the game's RAM
 * holds. */
export function fitsRoom(codes: ModCode[]): boolean {
  return (
    codes.length <= MAX_ROOM_CODES &&
    codes.reduce((n, c) => n + c.lines.length, 0) <= MAX_ROOM_CODE_LINES &&
    geckoLines(codes) <= GECKO_LINES_MAX &&
    JSON.stringify(codes.map(({ name, kind, lines }) => ({ name, kind, lines }))).length <= MAX_ROOM_CODES_JSON
  );
}

/** What a room's Dolphin runs (`--mods-room`'s "codes", MachineSession.cpp ParseCodes). */
export function roomCodes(
  codes: ModBuildCodes | undefined,
): { name: string; kind: ModCode["kind"]; lines: string[] }[] {
  return (codes?.on ?? []).map(({ name, kind, lines }) => ({ name, kind, lines }));
}
