import { describe, expect, it } from "vitest";

import {
  limitQuickChatMessage,
  normalizeQuickChat,
  QUICK_CHAT_SLOTS,
  quickChatFileText,
  quickChatIndex,
  quickChatLength,
} from "./quick_chat";

describe("quick chat messages", () => {
  it("are numbered group then direction, clockwise from up", () => {
    expect(quickChatIndex("up", "up")).toBe(0);
    expect(quickChatIndex("up", "left")).toBe(3);
    expect(quickChatIndex("right", "up")).toBe(4);
    expect(quickChatIndex("left", "left")).toBe(15);
  });

  it("stay within chat's 100 characters", () => {
    expect(limitQuickChatMessage("a".repeat(150))).toBe("a".repeat(100));
    // 100 kana are 300 bytes: all fit; an emoji is one character of 4 bytes.
    expect(quickChatLength(limitQuickChatMessage("ね".repeat(120)))).toBe(100);
    expect(quickChatLength(limitQuickChatMessage("😀".repeat(120)))).toBe(75);
  });

  it("drop what chat would drop, and keep spaces as typed", () => {
    const rlo = String.fromCodePoint(0x202e);
    const zwsp = String.fromCodePoint(0x200b);
    expect(limitQuickChatMessage(`gg${rlo} wp${zwsp}\u0007`)).toBe("gg wp");
    expect(limitQuickChatMessage("one\nmore ")).toBe("one more ");
  });

  it("are always 16 in the setting and the file", () => {
    expect(normalizeQuickChat(undefined)).toHaveLength(QUICK_CHAT_SLOTS);
    expect(normalizeQuickChat(["gg", 3, null])).toEqual(["gg", ...Array(15).fill("")]);
    const file = JSON.parse(quickChatFileText(["  gg   wp  "]));
    expect(file.version).toBe(1);
    expect(file.messages).toHaveLength(16);
    expect(file.messages[0]).toBe("gg wp");
  });
});
