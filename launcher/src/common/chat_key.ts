/**
 * The key that opens the in-game chat box: Dolphin's "Activate NetPlay Chat" hotkey (T by
 * default). The launcher writes it to Dolphin.ini `[Online] ChatKey` as an input expression for
 * the hotkeys' keyboard, which Dolphin puts on the hotkey when it loads Hotkeys.ini
 * (HotkeyManagerEmu::LoadConfig). Dolphin names keys by input backend: DirectInput (Windows) and
 * Quartz (macOS) by the key's position on a US keyboard, XInput2 (Linux) by the key's symbol in
 * the current layout, unshifted.
 */

/** Dolphin's own default binding. */
export const DEFAULT_CHAT_KEY = "`T`";

type KeyNames = { label: string; win: string; mac: string; linux: string };

/** Keys other than letters and digits, by `KeyboardEvent.code`. */
const OTHER_KEYS: Record<string, KeyNames> = {
  Enter: { label: "Enter", win: "RETURN", mac: "Return", linux: "Return" },
  Tab: { label: "Tab", win: "TAB", mac: "Tab", linux: "Tab" },
  Space: { label: "Space", win: "SPACE", mac: "Space", linux: "space" },
  Backquote: { label: "`", win: "GRAVE", mac: "Tilde", linux: "grave" },
  Minus: { label: "-", win: "MINUS", mac: "-", linux: "minus" },
  Equal: { label: "=", win: "EQUALS", mac: "=", linux: "equal" },
  BracketLeft: { label: "[", win: "LBRACKET", mac: "[", linux: "bracketleft" },
  BracketRight: { label: "]", win: "RBRACKET", mac: "]", linux: "bracketright" },
  Backslash: { label: "\\", win: "BACKSLASH", mac: "\\", linux: "backslash" },
  Semicolon: { label: ";", win: "SEMICOLON", mac: ";", linux: "semicolon" },
  Quote: { label: "'", win: "APOSTROPHE", mac: "'", linux: "apostrophe" },
  Comma: { label: ",", win: "COMMA", mac: ",", linux: "comma" },
  Period: { label: ".", win: "PERIOD", mac: ".", linux: "period" },
  Slash: { label: "/", win: "SLASH", mac: "/", linux: "slash" },
};
for (let n = 1; n <= 12; n++) {
  OTHER_KEYS[`F${n}`] = { label: `F${n}`, win: `F${n}`, mac: `F${n}`, linux: `F${n}` };
}

/** XKeysymToString's names for printable ASCII other than letters and digits. */
const X_KEYSYMS: Record<string, string> = {
  " ": "space",
  "!": "exclam",
  '"': "quotedbl",
  "#": "numbersign",
  $: "dollar",
  "%": "percent",
  "&": "ampersand",
  "'": "apostrophe",
  "(": "parenleft",
  ")": "parenright",
  "*": "asterisk",
  "+": "plus",
  ",": "comma",
  "-": "minus",
  ".": "period",
  "/": "slash",
  ":": "colon",
  ";": "semicolon",
  "<": "less",
  "=": "equal",
  ">": "greater",
  "?": "question",
  "@": "at",
  "[": "bracketleft",
  "\\": "backslash",
  "]": "bracketright",
  "^": "asciicircum",
  _: "underscore",
  "`": "grave",
  "{": "braceleft",
  "|": "bar",
  "}": "braceright",
  "~": "asciitilde",
};

/** The parts of a `KeyboardEvent` the chat key is made from. */
export type ChatKeyPress = {
  code: string;
  key: string;
  ctrlKey?: boolean;
  altKey?: boolean;
  shiftKey?: boolean;
  metaKey?: boolean;
};

function backendOf(platform: string): "win" | "mac" | "linux" {
  return platform === "win32" ? "win" : platform === "darwin" ? "mac" : "linux";
}

/**
 * The `[Online] ChatKey` expression for a key press on `platform` (`process.platform`), or null
 * for a key that can't open chat (modifiers, Escape, arrows and the like, or a press with Ctrl,
 * Alt, Shift or Command held).
 */
export function chatKeyExpression(event: ChatKeyPress, platform: string): string | null {
  if (event.ctrlKey || event.altKey || event.shiftKey || event.metaKey) {
    return null;
  }
  const backend = backendOf(platform);
  const letter = /^Key([A-Z])$/.exec(event.code)?.[1];
  const digit = /^Digit([0-9])$/.exec(event.code)?.[1];
  const other = OTHER_KEYS[event.code];
  if (letter === undefined && digit === undefined && other === undefined) {
    return null;
  }
  if (backend === "linux" && event.key.length === 1 && event.key >= " " && event.key <= "~") {
    // The layout's symbol for the key (Dolphin upper-cases letters).
    const name = /[A-Za-z0-9]/.test(event.key) ? event.key.toUpperCase() : X_KEYSYMS[event.key];
    return `\`${name}\``;
  }
  return `\`${letter ?? digit ?? other![backend]}\``;
}

/**
 * What to show for a chat key expression: the key's label for a single key of any backend
 * (`RETURN`, `Return` and `` `DInput/0/Keyboard Mouse:RETURN` `` are all "Enter"), the expression
 * itself for anything else (a combination set in Dolphin's Hotkey Settings), "None" for no key.
 */
export function chatKeyLabel(expression: string): string {
  const trimmed = expression.trim();
  if (trimmed === "") {
    return "None";
  }
  const quoted = /^`([^`]*)`$/.exec(trimmed)?.[1];
  const bare = /^[A-Za-z0-9_]+$/.test(trimmed) ? trimmed : undefined;
  const qualified = quoted ?? bare;
  if (qualified === undefined) {
    return trimmed;
  }
  const name = qualified.slice(qualified.lastIndexOf(":") + 1);
  if (/^[A-Za-z0-9]$/.test(name)) {
    return name.toUpperCase();
  }
  for (const keys of Object.values(OTHER_KEYS)) {
    if (name === keys.win || name === keys.mac || name === keys.linux) {
      return keys.label;
    }
  }
  const symbol = Object.keys(X_KEYSYMS).find((char) => X_KEYSYMS[char] === name);
  return symbol !== undefined ? (symbol === " " ? "Space" : symbol) : name;
}
