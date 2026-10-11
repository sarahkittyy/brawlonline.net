/**
 * The chat text cleaner (docs/chat-protocol.md §5), the same steps as Dolphin's
 * `Online::ChatText::Clean` and the server's `common::chat::clean_text`: applied to what is typed
 * before it is sent and to what arrives before it is shown.
 */

export const MAX_TEXT_BYTES = 300;
export const MAX_TEXT_CODE_POINTS = 100;
export const MAX_NAME_CODE_POINTS = 32;

/** Step 2: controls, invisible and bidi formatting, variation selectors, fillers, private use, tags, planes 15-16, noncharacters. */
function isRemoved(u: number): boolean {
  return (
    u <= 0x1f ||
    (u >= 0x7f && u <= 0x9f) ||
    u === 0xad ||
    u === 0x34f ||
    u === 0x61c ||
    u === 0x115f ||
    u === 0x1160 ||
    u === 0x17b4 ||
    u === 0x17b5 ||
    (u >= 0x180b && u <= 0x180f) ||
    (u >= 0x200b && u <= 0x200f) ||
    (u >= 0x2028 && u <= 0x202e) ||
    (u >= 0x2060 && u <= 0x206f) ||
    u === 0x3164 ||
    (u >= 0xfe00 && u <= 0xfe0f) ||
    u === 0xfeff ||
    u === 0xffa0 ||
    (u >= 0xfff0 && u <= 0xfffb) ||
    (u >= 0xe000 && u <= 0xf8ff) ||
    (u >= 0x1d173 && u <= 0x1d17a) ||
    (u >= 0xe0000 && u <= 0xe0fff) ||
    u >= 0xf0000 ||
    (u >= 0xfdd0 && u <= 0xfdef) ||
    (u & 0xfffe) === 0xfffe
  );
}

/** Step 3's whitespace. */
function isSpace(u: number): boolean {
  return (
    u === 0x20 ||
    u === 0xa0 ||
    u === 0x1680 ||
    (u >= 0x2000 && u <= 0x200a) ||
    u === 0x202f ||
    u === 0x205f ||
    u === 0x3000
  );
}

/** Step 4's combining marks. */
function isCombining(u: number): boolean {
  return (
    (u >= 0x300 && u <= 0x36f) ||
    (u >= 0x483 && u <= 0x489) ||
    (u >= 0x591 && u <= 0x5bd) ||
    (u >= 0x610 && u <= 0x61a) ||
    (u >= 0x64b && u <= 0x65f) ||
    (u >= 0xe31 && u <= 0xe3a) ||
    (u >= 0xe47 && u <= 0xe4e) ||
    (u >= 0x1ab0 && u <= 0x1aff) ||
    (u >= 0x1dc0 && u <= 0x1dff) ||
    (u >= 0x20d0 && u <= 0x20ff) ||
    (u >= 0x302a && u <= 0x302f) ||
    u === 0x3099 ||
    u === 0x309a ||
    (u >= 0xfe20 && u <= 0xfe2f)
  );
}

function utf8Length(u: number): number {
  return u < 0x80 ? 1 : u < 0x800 ? 2 : u < 0x10000 ? 3 : 4;
}

const strictUtf8 = new TextDecoder("utf-8", { fatal: true });

/** Cleans UTF-8 bytes from a peer; undefined when invalid or nothing is left. */
export function cleanBytes(bytes: Uint8Array, maxCodePoints = MAX_TEXT_CODE_POINTS): string | undefined {
  let s: string;
  try {
    s = strictUtf8.decode(bytes);
  } catch {
    return undefined;
  }
  return cleanText(s, maxCodePoints);
}

/** Cleans a string (lone surrogates count as invalid). */
export function cleanText(s: string, maxCodePoints = MAX_TEXT_CODE_POINTS): string | undefined {
  const cleaned: number[] = [];
  let space = false;
  let marks = 0;
  for (const ch of s) {
    const u = ch.codePointAt(0)!;
    if (u >= 0xd800 && u <= 0xdfff) {
      return undefined;
    }
    if (isRemoved(u)) {
      continue;
    }
    if (isSpace(u)) {
      space = true;
      marks = 0;
      continue;
    }
    if (isCombining(u)) {
      if (marks >= 2) {
        continue;
      }
      marks += 1;
    } else {
      marks = 0;
    }
    if (space && cleaned.length > 0) {
      cleaned.push(0x20);
    }
    space = false;
    cleaned.push(u);
  }
  const out: number[] = [];
  let bytes = 0;
  for (const u of cleaned) {
    if (out.length >= maxCodePoints || bytes + utf8Length(u) > MAX_TEXT_BYTES) {
      break;
    }
    out.push(u);
    bytes += utf8Length(u);
  }
  // A space the cut left at the end goes too, so cleaning twice changes nothing.
  if (out[out.length - 1] === 0x20) {
    out.pop();
  }
  return out.length > 0 ? String.fromCodePoint(...out) : undefined;
}
