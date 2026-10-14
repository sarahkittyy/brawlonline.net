import { describe, expect, it } from "vitest";

import { chatKeyExpression, chatKeyLabel, DEFAULT_CHAT_KEY } from "./chat_key";

describe("chatKeyExpression", () => {
  it("names a key the way each platform's Dolphin input backend does", () => {
    const y = { code: "KeyY", key: "y" };
    expect(chatKeyExpression(y, "win32")).toBe("`Y`");
    expect(chatKeyExpression(y, "darwin")).toBe("`Y`");
    expect(chatKeyExpression(y, "linux")).toBe("`Y`");

    const slash = { code: "Slash", key: "/" };
    expect(chatKeyExpression(slash, "win32")).toBe("`SLASH`");
    expect(chatKeyExpression(slash, "darwin")).toBe("`/`");
    expect(chatKeyExpression(slash, "linux")).toBe("`slash`");

    const enter = { code: "Enter", key: "Enter" };
    expect(chatKeyExpression(enter, "win32")).toBe("`RETURN`");
    expect(chatKeyExpression(enter, "darwin")).toBe("`Return`");
    expect(chatKeyExpression(enter, "linux")).toBe("`Return`");

    expect(chatKeyExpression({ code: "Digit1", key: "1" }, "win32")).toBe("`1`");
    expect(chatKeyExpression({ code: "F5", key: "F5" }, "linux")).toBe("`F5`");
    expect(chatKeyExpression({ code: "Backquote", key: "`" }, "darwin")).toBe("`Tilde`");
  });

  it("goes by position on Windows and macOS, by the layout's symbol on Linux", () => {
    // Z on a German keyboard sits where US keyboards have Y.
    const germanZ = { code: "KeyY", key: "z" };
    expect(chatKeyExpression(germanZ, "win32")).toBe("`Y`");
    expect(chatKeyExpression(germanZ, "darwin")).toBe("`Y`");
    expect(chatKeyExpression(germanZ, "linux")).toBe("`Z`");
    // The 1 key of a French keyboard is & unshifted.
    expect(chatKeyExpression({ code: "Digit1", key: "&" }, "linux")).toBe("`ampersand`");
    // A symbol XInput2 doesn't name by ASCII: the US layout's name.
    expect(chatKeyExpression({ code: "Semicolon", key: "ö" }, "linux")).toBe("`semicolon`");
  });

  it("refuses modifiers, Escape, arrows and keys pressed with a modifier", () => {
    expect(chatKeyExpression({ code: "Escape", key: "Escape" }, "win32")).toBeNull();
    expect(chatKeyExpression({ code: "ShiftLeft", key: "Shift" }, "win32")).toBeNull();
    expect(chatKeyExpression({ code: "ArrowUp", key: "ArrowUp" }, "win32")).toBeNull();
    expect(chatKeyExpression({ code: "KeyT", key: "t", ctrlKey: true }, "win32")).toBeNull();
    expect(chatKeyExpression({ code: "KeyT", key: "T", shiftKey: true }, "linux")).toBeNull();
  });
});

describe("chatKeyLabel", () => {
  it("shows one label for every backend's name of a key", () => {
    expect(chatKeyLabel(DEFAULT_CHAT_KEY)).toBe("T");
    expect(chatKeyLabel("T")).toBe("T");
    for (const name of ["`RETURN`", "`Return`", "RETURN", "`DInput/0/Keyboard Mouse:RETURN`"]) {
      expect(chatKeyLabel(name)).toBe("Enter");
    }
    for (const name of ["`SLASH`", "`/`", "`slash`"]) {
      expect(chatKeyLabel(name)).toBe("/");
    }
    expect(chatKeyLabel("`Tilde`")).toBe("`");
    expect(chatKeyLabel("`ampersand`")).toBe("&");
    expect(chatKeyLabel("`space`")).toBe("Space");
  });

  it("shows anything else as it is, and no key as None", () => {
    expect(chatKeyLabel("@(`Ctrl`+`T`)")).toBe("@(`Ctrl`+`T`)");
    expect(chatKeyLabel("")).toBe("None");
  });
});
