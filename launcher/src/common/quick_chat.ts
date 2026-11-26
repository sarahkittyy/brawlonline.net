/**
 * Quick chat (docs/design/quick-chat.md; Settings > Social): 16 messages sent with a GameCube
 * controller's D-pad on the online CSS. The first press opens that direction's group of four, the
 * second sends the message in its direction. All empty by default (the D-pad then moves the CSS's
 * hand as always).
 *
 * The game reads them from `quick-chat.json` in the netplay User folder's online folder (core
 * Online/QuickChat.h): `{"version": 1, "messages": [16 strings]}`, message `4 * group + direction`,
 * directions clockwise from up. It cleans each as chat text; the launcher keeps them within chat's
 * length (100 characters, 300 bytes of UTF-8) as they are typed.
 */

export const QUICK_CHAT_FILE = "quick-chat.json";

/** The D-pad's directions, in the file's order (core QuickChat::Direction). */
export const QUICK_CHAT_DIRECTIONS = ["up", "right", "down", "left"] as const;
export type QuickChatDirection = (typeof QUICK_CHAT_DIRECTIONS)[number];

export const QUICK_CHAT_SLOTS = 16;
/** Chat's limits (core ChatText: MAX_MESSAGE_CODE_POINTS, MAX_MESSAGE_BYTES). */
export const QUICK_CHAT_MAX_CHARACTERS = 100;
export const QUICK_CHAT_MAX_BYTES = 300;

export const EMPTY_QUICK_CHAT: readonly string[] = Object.freeze(Array<string>(QUICK_CHAT_SLOTS).fill(""));

export function quickChatIndex(group: QuickChatDirection, direction: QuickChatDirection): number {
  return 4 * QUICK_CHAT_DIRECTIONS.indexOf(group) + QUICK_CHAT_DIRECTIONS.indexOf(direction);
}

// What chat's cleaner drops: C0/C1 controls, zero-width and direction marks, bidi embeddings and
// overrides, invisible operators and isolates, the BOM. The game cleans again; this only keeps the
// field honest about what will be sent.
const DROPPED_RANGES: ReadonlyArray<readonly [number, number]> = [
  [0x00, 0x1f],
  [0x7f, 0x9f],
  [0x200b, 0x200f],
  [0x202a, 0x202e],
  [0x2060, 0x2064],
  [0x2066, 0x206f],
  [0xfeff, 0xfeff],
];

function dropped(codePoint: number): boolean {
  return DROPPED_RANGES.some(([lo, hi]) => codePoint >= lo && codePoint <= hi);
}

function utf8Size(codePoint: number): number {
  return codePoint < 0x80 ? 1 : codePoint < 0x800 ? 2 : codePoint < 0x10000 ? 3 : 4;
}

/**
 * A message as it is typed: no control or invisible characters, line breaks and tabs as spaces, at
 * most chat's 100 characters and 300 bytes. Spaces are kept as typed (the game trims and collapses
 * them), so a space typed at the end isn't taken away under the cursor.
 */
export function limitQuickChatMessage(text: string): string {
  let out = "";
  let bytes = 0;
  let count = 0;
  for (const typed of text) {
    // Line breaks and tabs (a paste) as spaces.
    const ch = typed === "\r" || typed === "\n" || typed === "\t" ? " " : typed;
    const codePoint = ch.codePointAt(0) ?? 0;
    if (dropped(codePoint)) {
      continue;
    }
    const size = utf8Size(codePoint);
    if (count + 1 > QUICK_CHAT_MAX_CHARACTERS || bytes + size > QUICK_CHAT_MAX_BYTES) {
      break;
    }
    out += ch;
    bytes += size;
    count++;
  }
  return out;
}

/** Characters as chat counts them (code points). */
export function quickChatLength(text: string): number {
  return Array.from(text).length;
}

/** A stored setting as 16 messages, each within chat's length. */
export function normalizeQuickChat(messages: readonly unknown[] | null | undefined): string[] {
  const out = Array<string>(QUICK_CHAT_SLOTS).fill("");
  if (!Array.isArray(messages)) {
    return out;
  }
  for (let i = 0; i < QUICK_CHAT_SLOTS; i++) {
    const m = messages[i];
    out[i] = typeof m === "string" ? limitQuickChatMessage(m) : "";
  }
  return out;
}

/** The file's text for these messages (trimmed: what the game would make of them). */
export function quickChatFileText(messages: readonly string[]): string {
  return JSON.stringify({
    version: 1,
    messages: normalizeQuickChat(messages).map((m) => m.replace(/\s+/g, " ").trim()),
  });
}
